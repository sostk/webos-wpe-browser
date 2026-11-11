// SPDX-License-Identifier: GPL-3.0-or-later
#include "media_server.hpp"

#include "wpe_media_ipc.h"
#include "wnk/log.hpp"
#include "wnk/media/ndl_backend.hpp"

#include <cerrno>
#include <chrono>
#include <condition_variable>
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
    std::vector<uint8_t> payload;
    std::atomic<uint64_t> units{0}, full_waits{0};
    // Access units go to a queue fed to the decoder by their own thread, as a media player's feeder does: a
    // decoder refusing data ("buffer full", e.g. while paused) then never holds up the control messages behind
    // them (play, pause, seek). The sink is synchronised to the pipeline clock, so the queue stays short.
    struct Unit {
        wnk::media::Packet packet;
        uint64_t generation;
    };
    std::mutex qm;
    std::condition_variable qcv;
    std::deque<Unit> queue;
    uint64_t generation = 0;  // qm; a flush or load drops what was queued before it
    bool stop = false;
    std::thread feeder([&] {
        std::unique_lock<std::mutex> ql(qm);
        for (;;) {
            qcv.wait(ql, [&] { return stop || !queue.empty(); });
            if (stop) return;
            Unit &u = queue.front();
            const uint64_t g = u.generation;
            ql.unlock();
            bool full = false;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (owner_.load() == id && backend_ && (use_lg_ || ndl_loaded_)) {
                    full = use_lg_ ? lg_.feed_video(u.packet.data.data(), u.packet.data.size(), u.packet.pts_us) ==
                                         LgMediaPlayer::Feed::Full
                                   : backend_->feed(u.packet) == wnk::media::MediaBackend::Feed::Full;
                }
            }
            ql.lock();
            if (full && generation == g) {
                ++full_waits;
                qcv.wait_for(ql, std::chrono::milliseconds(10), [&] { return stop || generation != g; });
                continue;
            }
            if (!queue.empty() && queue.front().generation == g) queue.pop_front();
            ++units;
        }
    });
    const auto drop_queued = [&] {
        std::lock_guard<std::mutex> ql(qm);
        queue.clear();
        ++generation;
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
    for (;;) {
        WpebMediaHeader h;
        if (!read_all(fd, &h, sizeof(h)) || h.size > WPEB_MEDIA_MAX_PAYLOAD) break;
        payload.resize(h.size);
        if (h.size && !read_all(fd, payload.data(), h.size)) break;
        if (h.type == WPEB_MEDIA_UNLOAD) {
            drop_queued();
            release();
            continue;
        }
        if (h.type == WPEB_MEDIA_VIDEO) {
            Unit u;
            u.packet.type = wnk::media::TrackType::Video;
            u.packet.pts_us = h.pts_us;
            u.packet.data.assign(payload.begin(), payload.end());
            std::lock_guard<std::mutex> ql(qm);
            u.generation = generation;
            queue.push_back(std::move(u));
            qcv.notify_all();
            continue;
        }
        if (h.type == WPEB_MEDIA_LOAD || h.type == WPEB_MEDIA_FLUSH) drop_queued();
        std::lock_guard<std::mutex> lock(mutex_);
        if (!backend_) break;
        auto &b = *backend_;
        switch (h.type) {
        case WPEB_MEDIA_LOAD: {
            if (h.size < sizeof(WpebMediaLoad)) break;
            WpebMediaLoad l;
            std::memcpy(&l, payload.data(), sizeof(l));
            l.window_id[sizeof(l.window_id) - 1] = '\0';
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
            if (l.window_id[0] && LgMediaPlayer::available()) {
                const LgMediaPlayer::Codec c = l.codec == WPEB_MEDIA_H264 ? LgMediaPlayer::Codec::H264
                                               : l.codec == WPEB_MEDIA_HEVC ? LgMediaPlayer::Codec::Hevc
                                               : l.codec == WPEB_MEDIA_VP9  ? LgMediaPlayer::Codec::Vp9
                                                                            : LgMediaPlayer::Codec::Av1;
                use_lg_ = lg_.load("org.sostk.wpebrowser", l.window_id, c, static_cast<int>(l.width),
                                   static_cast<int>(l.height), l.fps_milli / 1000.0, &error);
                if (!use_lg_) WNK_LOGW(kCat, "stream %llu: LG media pipeline: %s; trying NDL", static_cast<unsigned long long>(id), error.c_str());
            }
            if (!use_lg_) {
                error.clear();
                ndl_loaded_ = b.load(f, &error);
            }
            if (use_lg_ || ndl_loaded_)
                WNK_LOGI(kCat, "stream %llu: %s for %s %ux%u @ %.2f fps%s%s", static_cast<unsigned long long>(id),
                         use_lg_ ? "LG media pipeline" : "NDL", codec_label(l.codec), l.width, l.height, l.fps_milli / 1000.0,
                         l.window_id[0] ? ", window " : "", l.window_id);
            else
                WNK_LOGW(kCat, "stream %llu: decoder load failed: %s", static_cast<unsigned long long>(id), error.c_str());
            break;
        }
        case WPEB_MEDIA_FLUSH:
            if (owner_.load() == id) {
                std::string error;
                if (use_lg_) lg_.flush();
                else if (ndl_loaded_) b.flush(&error);
            }
            break;
        case WPEB_MEDIA_AREA: {
            if (h.size < sizeof(WpebMediaArea)) break;
            WpebMediaArea a;
            std::memcpy(&a, payload.data(), sizeof(a));
            if (a.width > 0 && a.height > 0 && (owner_.load() == id || owner_.load() == 0)) {
                if (!use_lg_) b.set_area(wnk::Rect{static_cast<float>(a.x), static_cast<float>(a.y), static_cast<float>(a.width),
                                     static_cast<float>(a.height)});
            }
            break;
        }
        case WPEB_MEDIA_PAUSE:
            if (owner_.load() == id && h.size >= sizeof(uint32_t)) {
                uint32_t paused;
                std::memcpy(&paused, payload.data(), sizeof(paused));
                if (use_lg_) lg_.pause(paused != 0);
                else if (ndl_loaded_) b.pause(paused != 0);
            }
            break;
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
