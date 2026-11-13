// SPDX-License-Identifier: GPL-3.0-or-later
#include "media_server.hpp"

#include "wpe_media_ipc.h"
#include "wnk/log.hpp"
#include "wnk/media/ndl_backend.hpp"

#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <deque>
#include <vector>

#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace browser {
namespace {

constexpr const char *kCat = "media";

bool read_all(int fd, void *data, size_t size) {
    auto *p = static_cast<uint8_t *>(data);
    while (size) {
        const ssize_t n = ::recv(fd, p, size, 0);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return false;
        p += n;
        size -= static_cast<size_t>(n);
    }
    return true;
}

const char *codec_label(uint32_t c) {
    switch (c) {
    case WPEB_MEDIA_H264: return "H.264";
    case WPEB_MEDIA_HEVC: return "HEVC";
    case WPEB_MEDIA_VP9: return "VP9";
    case WPEB_MEDIA_AV1: return "AV1";
    default: return "?";
    }
}

}  // namespace

MediaServer::MediaServer() = default;

MediaServer::~MediaServer() { stop(); }

void MediaServer::set_foreground(bool foreground) { wnk::media::NdlBackend::set_app_foreground(foreground); }

bool MediaServer::start(const std::string &path, std::string *why) {
    if (running()) return true;
    if (!wnk::media::NdlBackend::available(why)) return false;
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    if (path.size() >= sizeof(addr.sun_path)) {
        *why = "socket path too long";
        return false;
    }
    std::memcpy(addr.sun_path, path.c_str(), path.size() + 1);
    ::unlink(path.c_str());
    const int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0 || ::bind(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0 || ::listen(fd, 4) != 0 ||
        ::pipe2(wake_, O_CLOEXEC) != 0) {
        *why = std::string("media socket: ") + std::strerror(errno);
        if (fd >= 0) ::close(fd);
        return false;
    }
    path_ = path;
    listen_fd_ = fd;
    backend_ = std::make_unique<wnk::media::NdlBackend>();
    acceptor_ = std::thread([this] { accept_loop(); });
    WNK_LOGI(kCat, "hardware video service on %s", path.c_str());
    return true;
}

void MediaServer::stop() {
    if (!running()) return;
    const char byte = 1;
    (void)!::write(wake_[1], &byte, 1);
    if (acceptor_.joinable()) acceptor_.join();
    ::close(listen_fd_);
    listen_fd_ = -1;
    ::unlink(path_.c_str());
    // Connections end with the web process (the engine stops first); give their threads a moment.
    for (int i = 0; i < 100 && connections_.load() > 0; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(20));
    std::lock_guard<std::mutex> lock(mutex_);
    lg_.unload();
    use_lg_ = ndl_loaded_ = false;
    if (connections_.load() == 0) backend_.reset();  // releases LG's decoder
    else backend_.release();                          // a thread still uses it: leaked at exit, NDL quits atexit
    ::close(wake_[0]);
    ::close(wake_[1]);
    wake_[0] = wake_[1] = -1;
}

void MediaServer::accept_loop() {
    for (;;) {
        pollfd fds[2] = {{listen_fd_, POLLIN, 0}, {wake_[0], POLLIN, 0}};
        if (::poll(fds, 2, -1) < 0) {
            if (errno == EINTR) continue;
            return;
        }
        if (fds[1].revents) return;
        if (!(fds[0].revents & POLLIN)) continue;
        const int fd = ::accept4(listen_fd_, nullptr, nullptr, SOCK_CLOEXEC);
        if (fd < 0) continue;
        const uint64_t id = next_id_++;
        ++connections_;
        std::thread([this, fd, id] {
            serve(fd, id);
            ::close(fd);
            --connections_;
        }).detach();
    }
}

void MediaServer::serve(int fd, uint64_t id) {
    WNK_LOGI(kCat, "stream %llu connected", static_cast<unsigned long long>(id));
    using Clock = std::chrono::steady_clock;
    std::vector<uint8_t> payload;
    std::atomic<uint64_t> units{0}, full_waits{0};
    // Access units and audio frames go to queues fed to the decoder by their own thread, as a media player's feeder
    // does: a decoder refusing data ("buffer full", e.g. while paused) never holds up the control messages behind
    // them (play, pause, seek), and a full video buffer never holds up the audio (or the other way round): LG's
    // pipeline waits for both before it plays.
    struct Unit {
        wnk::media::Packet packet;
        uint64_t generation;
        bool eos = false;  // the stream's end, behind its last unit (pts: last, so the other stream goes first)
    };
    std::mutex qm;
    std::condition_variable qcv;
    std::deque<Unit> queues[2];  // [0] video, [1] audio
    uint64_t generation = 0;     // qm; a flush or load drops what was queued before it
    bool stop = false;
    bool ready = false;          // qm; the decoder is loaded (units wait until then)
    std::thread feeder([&] {
        Clock::time_point blocked_until[2] = {};
        bool ended[2] = {};          // feeder thread; the streams whose end was reached, in eos_generation
        bool pushed_eos = false;
        uint64_t eos_generation = 0;
        std::unique_lock<std::mutex> ql(qm);
        for (;;) {
            qcv.wait(ql, [&] { return stop || (ready && (!queues[0].empty() || !queues[1].empty())); });
            if (stop) return;
            const auto now = Clock::now();
            int pick = -1;
            for (int t = 0; t < 2; ++t) {
                if (queues[t].empty() || blocked_until[t] > now) continue;
                if (pick < 0 || queues[t].front().packet.pts_us < queues[pick].front().packet.pts_us) pick = t;
            }
            if (pick < 0) {
                const uint64_t g = generation;
                qcv.wait_for(ql, std::chrono::milliseconds(10), [&] { return stop || generation != g; });
                continue;
            }
            Unit &u = queues[pick].front();
            const uint64_t g = u.generation;
            if (u.eos) {
                queues[pick].pop_front();
                if (eos_generation != g) ended[0] = ended[1] = pushed_eos = false, eos_generation = g;
                ended[pick] = true;
                ql.unlock();
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    // LG's pipeline ends once per stretch of playback (a flush or load starts a new generation):
                    // when the video has ended and so has its audio, if LG plays that too.
                    if (owner_.load() == id && use_lg_ && !pushed_eos && ended[0] && (ended[1] || !lg_.has_audio())) {
                        lg_.end_of_stream();
                        pushed_eos = true;
                    }
                }
                ql.lock();
                continue;
            }
            ql.unlock();
            bool full = false;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (owner_.load() == id && backend_ && (use_lg_ || ndl_loaded_)) {
                    if (use_lg_) {
                        if (pick == 0 || lg_.has_audio())
                            full = lg_.feed(pick == 1, u.packet.data.data(), u.packet.data.size(), u.packet.pts_us) ==
                                   LgMediaPlayer::Feed::Full;
                    } else if (pick == 0) {
                        full = backend_->feed(u.packet) == wnk::media::MediaBackend::Feed::Full;
                    }
                }
            }
            ql.lock();
            if (full && generation == g) {
                ++full_waits;
                blocked_until[pick] = Clock::now() + std::chrono::milliseconds(10);
                continue;
            }
            if (!queues[pick].empty() && queues[pick].front().generation == g) queues[pick].pop_front();
            if (pick == 0) ++units;
        }
    });
    const auto drop_queued = [&] {
        std::lock_guard<std::mutex> ql(qm);
        queues[0].clear();
        queues[1].clear();
        ++generation;
        qcv.notify_all();
    };
    const auto set_ready = [&](bool r) {
        std::lock_guard<std::mutex> ql(qm);
        ready = r;
        qcv.notify_all();
    };
    const auto release = [&] {
        uint64_t mine = id;
        if (owner_.compare_exchange_strong(mine, 0)) {
            std::lock_guard<std::mutex> lock(mutex_);
            lg_.unload();
            if (backend_ && ndl_loaded_) backend_->unload();
            use_lg_ = ndl_loaded_ = false;
            WNK_LOGI(kCat, "stream %llu released the decoder after %llu access units (%llu waits for room)",
                     static_cast<unsigned long long>(id), static_cast<unsigned long long>(units.load()),
                     static_cast<unsigned long long>(full_waits.load()));
        }
    };

    // The video's Load waits for the audio side's answer (AUDIO_FORMAT or AUDIO_NONE), as LG's pipeline takes both
    // codecs at once, unless the stream has no audio or the answer does not come within kAudioWait.
    constexpr auto kAudioWait = std::chrono::milliseconds(1000);
    enum class AudioState { Unknown, None, Hardware } audio_state = AudioState::Unknown;
    WpebMediaAudio audio_format{};
    std::vector<uint8_t> audio_header;  // Opus: the OpusHead
    bool no_audio_track = false;
    bool load_pending = false;
    WpebMediaLoad pending{};
    Clock::time_point load_at{};

    const auto do_load = [&](const WpebMediaLoad &l) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!backend_) return;
        auto &b = *backend_;
        wnk::media::StreamFormat f;
        wnk::media::VideoFormat v;
        v.codec = l.codec == WPEB_MEDIA_H264 ? wnk::media::CodecId::H264
                  : l.codec == WPEB_MEDIA_HEVC ? wnk::media::CodecId::Hevc
                  : l.codec == WPEB_MEDIA_VP9  ? wnk::media::CodecId::Vp9
                                               : wnk::media::CodecId::Av1;
        v.width = static_cast<int>(l.width);
        v.height = static_cast<int>(l.height);
        v.frame_rate = l.fps_milli / 1000.0;
        f.video = v;
        std::string error;
        // One decoder per app: the newest stream takes it (another tab or element loses its picture).
        lg_.unload();
        if (ndl_loaded_) b.unload();
        use_lg_ = ndl_loaded_ = false;
        owner_ = id;
        units = 0;
        full_waits = 0;
        const bool with_audio = audio_state == AudioState::Hardware;
        if (l.window_id[0] && LgMediaPlayer::available()) {
            const LgMediaPlayer::Codec c = l.codec == WPEB_MEDIA_H264 ? LgMediaPlayer::Codec::H264
                                           : l.codec == WPEB_MEDIA_HEVC ? LgMediaPlayer::Codec::Hevc
                                           : l.codec == WPEB_MEDIA_VP9  ? LgMediaPlayer::Codec::Vp9
                                                                        : LgMediaPlayer::Codec::Av1;
            LgMediaPlayer::Audio a;
            a.codec = audio_format.codec == WPEB_MEDIA_OPUS ? LgMediaPlayer::Audio::Codec::Opus : LgMediaPlayer::Audio::Codec::Aac;
            a.rate = static_cast<int>(audio_format.rate);
            a.channels = static_cast<int>(audio_format.channels);
            a.header = audio_header;
            use_lg_ = lg_.load("org.sostk.wpebrowser", l.window_id, c, static_cast<int>(l.width),
                               static_cast<int>(l.height), l.fps_milli / 1000.0, with_audio ? &a : nullptr, &error);
            if (use_lg_) lg_.set_hdr(l.hdr);
            if (!use_lg_) WNK_LOGW(kCat, "stream %llu: LG media pipeline: %s; trying NDL", static_cast<unsigned long long>(id), error.c_str());
        }
        if (!use_lg_) {
            error.clear();
            ndl_loaded_ = b.load(f, &error);
        }
        if (use_lg_ || ndl_loaded_)
            WNK_LOGI(kCat, "stream %llu: %s for %s %ux%u @ %.2f fps%s%s%s", static_cast<unsigned long long>(id),
                     use_lg_ ? "LG media pipeline" : "NDL", codec_label(l.codec), l.width, l.height, l.fps_milli / 1000.0,
                     !(use_lg_ && with_audio) ? "" : audio_format.codec == WPEB_MEDIA_OPUS ? " with its Opus audio" : " with its AAC audio", l.window_id[0] ? ", window " : "", l.window_id);
        else
            WNK_LOGW(kCat, "stream %llu: decoder load failed: %s", static_cast<unsigned long long>(id), error.c_str());
    };
    const auto maybe_load = [&] {
        if (!load_pending) return;
        const bool waited = Clock::now() - load_at >= kAudioWait;
        if (audio_state == AudioState::Unknown && !no_audio_track && !waited) return;
        if (audio_state == AudioState::Unknown && waited)
            WNK_LOGW(kCat, "stream %llu: no word from the audio side: video only", static_cast<unsigned long long>(id));
        load_pending = false;
        do_load(pending);
        set_ready(true);
    };

    for (;;) {
        pollfd pfd{fd, POLLIN, 0};
        const int pr = ::poll(&pfd, 1, load_pending ? 100 : -1);
        if (pr < 0 && errno == EINTR) continue;
        if (pr < 0) break;
        if (pr == 0) {
            maybe_load();
            continue;
        }
        WpebMediaHeader h;
        if (!read_all(fd, &h, sizeof(h)) || h.size > WPEB_MEDIA_MAX_PAYLOAD) break;
        payload.resize(h.size);
        if (h.size && !read_all(fd, payload.data(), h.size)) break;
        if (h.type == WPEB_MEDIA_LOAD || h.type == WPEB_MEDIA_UNLOAD || h.type == WPEB_MEDIA_FLUSH)
            WNK_LOGI(kCat, "stream %llu: %s", static_cast<unsigned long long>(id),
                     h.type == WPEB_MEDIA_LOAD ? "load" : h.type == WPEB_MEDIA_UNLOAD ? "unload" : "flush");
        if (h.type == WPEB_MEDIA_UNLOAD) {
            drop_queued();
            set_ready(false);
            load_pending = false;
            release();
            continue;
        }
        if (h.type == WPEB_MEDIA_VIDEO || h.type == WPEB_MEDIA_AUDIO) {
            // A held Load also times out while units keep arriving: poll() never goes idle when a page streams
            // without pause (YouTube's TV app), and the Load waited forever (TV, 2026-11-14).
            if (load_pending) maybe_load();
            const bool audio = h.type == WPEB_MEDIA_AUDIO;
            if (audio && audio_state != AudioState::Hardware) continue;
            Unit u;
            u.packet.type = audio ? wnk::media::TrackType::Audio : wnk::media::TrackType::Video;
            u.packet.pts_us = h.pts_us;
            u.packet.data.assign(payload.begin(), payload.end());
            std::lock_guard<std::mutex> ql(qm);
            u.generation = generation;
            queues[audio ? 1 : 0].push_back(std::move(u));
            qcv.notify_all();
            continue;
        }
        switch (h.type) {
        case WPEB_MEDIA_LOAD: {
            if (h.size < sizeof(WpebMediaLoad)) break;
            drop_queued();
            set_ready(false);
            std::memcpy(&pending, payload.data(), sizeof(pending));
            pending.window_id[sizeof(pending.window_id) - 1] = '\0';
            load_pending = true;
            load_at = Clock::now();
            maybe_load();
            break;
        }
        case WPEB_MEDIA_AUDIO_FORMAT:
            if (h.size >= sizeof(WpebMediaAudio)) {
                std::memcpy(&audio_format, payload.data(), sizeof(audio_format));
                if (audio_format.header_size > h.size - sizeof(WpebMediaAudio)) break;
                audio_header.assign(payload.begin() + sizeof(WpebMediaAudio),
                                    payload.begin() + sizeof(WpebMediaAudio) + audio_format.header_size);
                audio_state = AudioState::Hardware;
                WNK_LOGI(kCat, "stream %llu: %s %u Hz, %u channels goes with the video", static_cast<unsigned long long>(id),
                         audio_format.codec == WPEB_MEDIA_OPUS ? "Opus" : "AAC", audio_format.rate, audio_format.channels);
                maybe_load();
            }
            break;
        case WPEB_MEDIA_EOS: {
            if (h.size < sizeof(uint32_t)) break;
            uint32_t audio;
            std::memcpy(&audio, payload.data(), sizeof(audio));
            if (audio && audio_state != AudioState::Hardware) break;
            WNK_LOGI(kCat, "stream %llu: end of the %s", static_cast<unsigned long long>(id), audio ? "audio" : "video");
            Unit u;
            u.packet.pts_us = INT64_MAX;
            u.eos = true;
            std::lock_guard<std::mutex> ql(qm);
            u.generation = generation;
            queues[audio ? 1 : 0].push_back(std::move(u));
            qcv.notify_all();
            break;
        }
        case WPEB_MEDIA_AUDIO_NONE:
            audio_state = AudioState::None;
            maybe_load();
            break;
        case WPEB_MEDIA_TRACKS:
            if (h.size >= sizeof(uint32_t)) {
                uint32_t has_audio;
                std::memcpy(&has_audio, payload.data(), sizeof(has_audio));
                no_audio_track = has_audio == 0;
                maybe_load();
            }
            break;
        case WPEB_MEDIA_FLUSH: {
            drop_queued();
            std::lock_guard<std::mutex> lock(mutex_);
            if (owner_.load() == id) {
                std::string error;
                if (use_lg_) lg_.flush();
                else if (ndl_loaded_ && backend_) backend_->flush(&error);
            }
            break;
        }
        case WPEB_MEDIA_AREA: {
            if (h.size < sizeof(WpebMediaArea)) break;
            WpebMediaArea a;
            std::memcpy(&a, payload.data(), sizeof(a));
            std::lock_guard<std::mutex> lock(mutex_);
            if (backend_ && a.width > 0 && a.height > 0 && (owner_.load() == id || owner_.load() == 0) && !use_lg_)
                backend_->set_area(wnk::Rect{static_cast<float>(a.x), static_cast<float>(a.y), static_cast<float>(a.width),
                                             static_cast<float>(a.height)});
            break;
        }
        case WPEB_MEDIA_PAUSE: {
            if (h.size < sizeof(uint32_t)) break;
            uint32_t paused;
            std::memcpy(&paused, payload.data(), sizeof(paused));
            std::lock_guard<std::mutex> lock(mutex_);
            if (owner_.load() == id) {
                if (use_lg_) lg_.pause(paused != 0);
                else if (ndl_loaded_ && backend_) backend_->pause(paused != 0);
            }
            break;
        }
        case WPEB_MEDIA_VOLUME: {
            if (h.size < sizeof(WpebMediaVolume)) break;
            WpebMediaVolume v;
            std::memcpy(&v, payload.data(), sizeof(v));
            std::lock_guard<std::mutex> lock(mutex_);
            if (owner_.load() == id && use_lg_) lg_.set_volume(v.permille, v.muted != 0);
            break;
        }
        default:
            break;
        }
    }
    {
        std::lock_guard<std::mutex> ql(qm);
        stop = true;
        qcv.notify_all();
    }
    feeder.join();
    release();
    WNK_LOGI(kCat, "stream %llu closed", static_cast<unsigned long long>(id));
}

}  // namespace browser
