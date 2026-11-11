// SPDX-License-Identifier: GPL-3.0-or-later
#include "lg_media_player.hpp"

#include "wnk/log.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <new>
#include <thread>

#if defined(WNK_SDL_WEBOS)
#include <StarfishMediaAPIs.h>
#endif

namespace browser {
namespace {
constexpr const char *kCat = "lgmedia";

#if defined(WNK_SDL_WEBOS)
const char *codec_name(LgMediaPlayer::Codec c) {
    switch (c) {
    case LgMediaPlayer::Codec::H264: return "H264";
    case LgMediaPlayer::Codec::Hevc: return "H265";
    case LgMediaPlayer::Codec::Vp9: return "VP9";
    case LgMediaPlayer::Codec::Av1: return "AV1";
    }
    return "H264";
}

void event_callback(int type, int64_t num, const char *str, void *data) {
    static_cast<LgMediaPlayer *>(data)->on_event(type, num, str);
}

// The firmware's object is larger than the header's declaration says (its padding is a guess on LG's side): give it
// room and never free the block, as a late callback can still carry its address after the destructor.
StarfishMediaAPIs *new_api() {
    void *block = nullptr;
    if (posix_memalign(&block, 16, 64 * 1024) != 0) return nullptr;
    std::memset(block, 0, 64 * 1024);
    return new (block) StarfishMediaAPIs(nullptr);
}
#endif
}  // namespace

LgMediaPlayer::~LgMediaPlayer() { unload(); }

bool LgMediaPlayer::available() {
#if defined(WNK_SDL_WEBOS)
    return true;
#else
    return false;
#endif
}

bool LgMediaPlayer::load(const std::string &app_id, const std::string &window_id, Codec codec, int width, int height,
                         double fps, const Audio *audio, std::string *error) {
#if defined(WNK_SDL_WEBOS)
    unload();
    int max_w = 3840, max_h = 2160;
    int32_t dw = 0, dh = 0, dfps = 0;
    if (smp::util::getMaxVideoResolution(codec_name(codec), &dw, &dh, &dfps) && dw > 0 && dh > 0) max_w = dw, max_h = dh;
    // H.264 that fits FHD is declared as FHD (larger declarations are refused by some firmware).
    if (codec == Codec::H264 && width > 0 && width <= 1920 && height <= 1088) max_w = 1920, max_h = 1080;
    const int max_fps = fps > 30.5 ? 60 : 30;
    // With audio, as the kit's StarfishBackend loads it (verified on this TV): the pipeline syncs the video to the
    // audio (audioSync), with a larger audio buffer and without low-delay mode.
    const bool with_audio = audio != nullptr;
    char payload[2048];
    std::snprintf(payload, sizeof payload,
                  "{\"args\":[{\"mediaTransportType\":\"BUFFERSTREAM\",\"option\":{\"appId\":\"%s\",\"windowId\":\"%s\","
                  "\"externalStreamingInfo\":{\"contents\":{\"codec\":{\"video\":\"%s\"%s},\"esInfo\":{\"pauseAtDecodeTime\":false,"
                  "\"ptsToDecode\":0,\"seperatedPTS\":true},\"format\":\"RAW\",\"provider\":\"wpebrowser\"},"
                  "\"streamQualityInfo\":true,\"streamQualityInfoNonFlushable\":true,\"audioSync\":%s,\"restartStreaming\":false,"
                  "\"bufferingCtrInfo\":{\"bufferMaxLevel\":0,\"bufferMinLevel\":0,\"preBufferByte\":0,\"qBufferLevelAudio\":0,"
                  "\"qBufferLevelVideo\":0,\"srcBufferLevelAudio\":{\"minimum\":1,\"maximum\":%d},"
                  "\"srcBufferLevelVideo\":{\"minimum\":1,\"maximum\":8388608}}},\"needAudio\":%s,\"queryPosition\":false,"
                  "\"lowDelayMode\":%s,\"transmission\":{\"contentsType\":\"LIVE\"},\"adaptiveStreaming\":{\"audioOnly\":false,"
                  "\"adaptiveResolution\":true,\"maxWidth\":%d,\"maxHeight\":%d,\"maxFrameRate\":%d}}}]}",
                  app_id.c_str(), window_id.c_str(), codec_name(codec), with_audio ? ",\"audio\":\"AAC\"" : "",
                  with_audio ? "true" : "false", with_audio ? 1048576 : 32768, with_audio ? "true" : "false",
                  with_audio ? "false" : "true", max_w, max_h, max_fps);
    api_ = new_api();
    if (api_ == nullptr) {
        *error = "out of memory";
        return false;
    }
    load_completed_ = false;
    playing_ = paused_ = false;
    seek_pending_ = true;
    fed_ = full_ = last_pts_us_ = audio_fed_ = 0;
    has_audio_ = with_audio;
    frames_ = 0;
    stats_at_ms_ = 0;
    if (!api_->Load(payload, &event_callback, this)) {
        *error = "StarfishMediaAPIs::Load refused the stream";
        api_->~StarfishMediaAPIs();
        api_ = nullptr;
        return false;
    }
    WNK_LOGI(kCat, "Load %s %dx%d @ %.2f (envelope %dx%d@%d)%s, window %s, media id %s", codec_name(codec), width, height,
             fps, max_w, max_h, max_fps, with_audio ? " + AAC" : "", window_id.c_str(),
             api_->getMediaID() ? api_->getMediaID() : "?");
    return true;
#else
    (void)app_id, (void)window_id, (void)codec, (void)width, (void)height, (void)fps, (void)audio;
    *error = "LG media pipeline only on webOS";
    return false;
#endif
}

LgMediaPlayer::Feed LgMediaPlayer::feed(bool audio, const uint8_t *data, size_t size, int64_t pts_us) {
#if defined(WNK_SDL_WEBOS)
    if (api_ == nullptr || (audio && !has_audio_)) return Feed::Error;
    if (seek_pending_) {
        // The first unit after a load or a flush: the pipeline's timeline starts there (Chromium seeks the platform
        // pipeline to the new position, then feeds).
        seek_pending_ = false;
        if (fed_ > 0 || pts_us > 0) {
            const std::string ms = std::to_string(pts_us / 1000);
            const bool ok = api_->Seek(ms.c_str());
            WNK_LOGI(kCat, "Seek(%s ms) -> %d", ms.c_str(), ok ? 1 : 0);
        }
    }
    if (!playing_ && !paused_) {
        playing_ = true;
        const bool ok = api_->Play();
        WNK_LOGI(kCat, "Play -> %d", ok ? 1 : 0);
    }
    char json[160];
    std::snprintf(json, sizeof json, "{\"bufferAddr\":\"%p\",\"bufferSize\":%u,\"pts\":%lld,\"esData\":%d}",
                  static_cast<const void *>(data), static_cast<unsigned>(size), static_cast<long long>(pts_us) * 1000,
                  audio ? 2 : 1);
    const std::string reply = api_->Feed(json);
    const int64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                               std::chrono::steady_clock::now().time_since_epoch()).count();
    if (now_ms - stats_at_ms_ >= 5000) {
        stats_at_ms_ = now_ms;
        WNK_LOGI(kCat, "fed %lld units + %lld audio (last video pts %.3f s), %lld refused as full, %lld frames presented, "
                 "play time %.3f s",
                 static_cast<long long>(fed_), static_cast<long long>(audio_fed_), last_pts_us_ / 1e6, static_cast<long long>(full_),
                 static_cast<long long>(frames_.load()), api_->getCurrentPlaytime() / 1e9);
    }
    if (reply.find("BufferFull") != std::string::npos) {
        ++full_;
        return Feed::Full;
    }
    if (reply.find("Ok") == std::string::npos) {
        WNK_LOGW(kCat, "Feed: \"%s\"", reply.c_str());
        return Feed::Error;
    }
    if (audio) {
        if (++audio_fed_ == 1) WNK_LOGI(kCat, "first audio frame fed: pts %.3f s, %zu bytes", pts_us / 1e6, size);
        return Feed::Ok;
    }
    last_pts_us_ = pts_us;
    if (++fed_ == 1) WNK_LOGI(kCat, "first access unit fed: pts %.3f s, %zu bytes", pts_us / 1e6, size);
    return Feed::Ok;
#else
    (void)audio, (void)data, (void)size, (void)pts_us;
    return Feed::Error;
#endif
}

void LgMediaPlayer::flush() { seek_pending_ = true; }

void LgMediaPlayer::set_volume(unsigned permille, bool muted) {
#if defined(WNK_SDL_WEBOS)
    if (api_ == nullptr || !has_audio_) return;
    // LG's player takes setVolume(volume, duration, ease) underneath (libpf); the JSON keys are not documented.
    char json[96];
    std::snprintf(json, sizeof json, "{\"volume\":%u,\"duration\":0,\"easeType\":\"Linear\"}", muted ? 0u : permille / 10);
    const bool v = api_->setVolume(json);
    const bool m = api_->setAudioMute(muted ? "{\"mute\":true}" : "{\"mute\":false}");
    WNK_LOGI(kCat, "volume %u%%%s: setVolume -> %d, setAudioMute -> %d", permille / 10, muted ? " (muted)" : "", v ? 1 : 0,
             m ? 1 : 0);
#else
    (void)permille, (void)muted;
#endif
}

void LgMediaPlayer::pause(bool paused) {
#if defined(WNK_SDL_WEBOS)
    if (api_ == nullptr || paused == paused_) return;
    paused_ = paused;
    const bool ok = paused ? api_->Pause() : api_->Play();
    if (!paused) playing_ = true;
    WNK_LOGI(kCat, "%s -> %d", paused ? "Pause" : "Play", ok ? 1 : 0);
#else
    (void)paused;
#endif
}

void LgMediaPlayer::unload() {
#if defined(WNK_SDL_WEBOS)
    if (api_ == nullptr) return;
    StarfishMediaAPIs *api = api_;
    api_ = nullptr;
    const int64_t fed = fed_;
    // Unload can take long or, rarely, not return: never on the caller's thread for good.
    std::thread([api, fed] {
        const bool ok = api->Unload();
        api->~StarfishMediaAPIs();
        WNK_LOGI(kCat, "Unload -> %d after %lld access units", ok ? 1 : 0, static_cast<long long>(fed));
    }).detach();
#endif
}

void LgMediaPlayer::on_event(int type, int64_t num, const char *str) {
#if defined(WNK_SDL_WEBOS)
    switch (type) {
    case 0x31:  // frames presented in the last 200 ms (measured: 4-5 at 24 fps)
        frames_ += num;
        return;
    case PF_EVENT_TYPE_FRAMEREADY:
        if (frames_ == 0) WNK_LOGI(kCat, "first frame presented (pts %.3f s)", num / 1e9);
        return;
    case PF_EVENT_TYPE_STR_STATE_UPDATE__LOADCOMPLETED:
        load_completed_ = true;
        WNK_LOGI(kCat, "load completed");
        return;
    case PF_EVENT_TYPE_INT_ERROR:
    case PF_EVENT_TYPE_STR_ERROR:
        WNK_LOGW(kCat, "pipeline error %lld %s", static_cast<long long>(num), str ? str : "");
        return;
    case PF_EVENT_TYPE_STR_BUFFERFULL:
    case PF_EVENT_TYPE_STR_BUFFERLOW:
    case PF_EVENT_TYPE_INT_BUFFERLOW:
    case PF_EVENT_TYPE_INT_NEED_DATA:
    case PF_EVENT_TYPE_INT_ENOUGH_DATA:
    case PF_EVENT_TYPE_DROPPED_FRAME:
        return;  // frequent
    default:
        WNK_LOGI(kCat, "event 0x%x %lld %.160s", type, static_cast<long long>(num), str ? str : "");
    }
#else
    (void)type, (void)num, (void)str;
#endif
}

}  // namespace browser
