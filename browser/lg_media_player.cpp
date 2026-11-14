// SPDX-License-Identifier: GPL-3.0-or-later
#include "lg_media_player.hpp"

#include "wnk/log.hpp"
#include "wpe_media_ipc.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <new>
#include <thread>

#if defined(WNK_SDL_WEBOS)
#include <StarfishMediaAPIs.h>
#include <dlfcn.h>
#endif

namespace browser {
namespace {
constexpr const char *kCat = "lgmedia";

#if defined(WNK_SDL_WEBOS)
// libplayerAPIs functions that not every firmware has, looked up at run time so the app still starts without them
// (tools/fw-check.py): setHdrInfo and setAudioMute are missing before webOS 5, setVolume before webOS 3, and
// smp::util::getMaxVideoResolution takes its codec by value up to webOS 25 but by const reference in webOS 26.
// Member functions are called as functions taking `this` first (ARM EABI, non-virtual).
struct OptionalPlayerApis {
    using Message = bool (*)(StarfishMediaAPIs *, const char *);
    using MaxResByValue = bool (*)(std::string, int32_t *, int32_t *, int32_t *);
    using MaxResByRef = bool (*)(const std::string &, int32_t *, int32_t *, int32_t *);
    Message set_hdr_info = nullptr, set_audio_mute = nullptr, set_volume = nullptr;
    MaxResByValue max_res_by_value = nullptr;
    MaxResByRef max_res_by_ref = nullptr;

    OptionalPlayerApis() {
        set_hdr_info = find<Message>("_ZN17StarfishMediaAPIs10setHdrInfoEPKc");
        set_audio_mute = find<Message>("_ZN17StarfishMediaAPIs12setAudioMuteEPKc");
        set_volume = find<Message>("_ZN17StarfishMediaAPIs9setVolumeEPKc");
        max_res_by_value = find<MaxResByValue>(
            "_ZN3smp4util21getMaxVideoResolutionENSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEEPiS7_S7_");
        max_res_by_ref = find<MaxResByRef>(
            "_ZN3smp4util21getMaxVideoResolutionERKNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEEPiS9_S9_");
        WNK_LOGI(kCat, "player APIs: setHdrInfo %s, setAudioMute %s, setVolume %s, getMaxVideoResolution %s",
                 set_hdr_info ? "yes" : "no", set_audio_mute ? "yes" : "no", set_volume ? "yes" : "no",
                 max_res_by_ref ? "yes (const ref)" : max_res_by_value ? "yes" : "no");
    }

    bool max_video_resolution(const std::string &codec, int32_t *w, int32_t *h, int32_t *fps) const {
        if (max_res_by_ref) return max_res_by_ref(codec, w, h, fps);
        if (max_res_by_value) return max_res_by_value(codec, w, h, fps);
        return false;
    }

 private:
    template <class F>
    static F find(const char *name) {
        return reinterpret_cast<F>(dlsym(RTLD_DEFAULT, name));
    }
};

const OptionalPlayerApis &player_apis() {
    static const OptionalPlayerApis apis;
    return apis;
}

const char *codec_name(LgMediaPlayer::Codec c) {
    switch (c) {
    case LgMediaPlayer::Codec::H264: return "H264";
    case LgMediaPlayer::Codec::Hevc: return "H265";
    case LgMediaPlayer::Codec::Vp9: return "VP9";
    case LgMediaPlayer::Codec::Av1: return "AV1";
    }
    return "H264";
}

std::string hex(const std::vector<uint8_t> &bytes) {
    static const char kDigits[] = "0123456789abcdef";
    std::string out;
    out.reserve(bytes.size() * 2);
    for (uint8_t b : bytes) out += kDigits[b >> 4], out += kDigits[b & 15];
    return out;
}

std::string base64(const std::vector<uint8_t> &bytes) {
    static const char kAlphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    for (size_t i = 0; i < bytes.size(); i += 3) {
        const uint32_t n = static_cast<uint32_t>(bytes[i]) << 16 | (i + 1 < bytes.size() ? bytes[i + 1] << 8 : 0) |
                           (i + 2 < bytes.size() ? bytes[i + 2] : 0);
        out += kAlphabet[n >> 18 & 63];
        out += kAlphabet[n >> 12 & 63];
        out += i + 1 < bytes.size() ? kAlphabet[n >> 6 & 63] : '=';
        out += i + 2 < bytes.size() ? kAlphabet[n & 63] : '=';
    }
    return out;
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
    if (player_apis().max_video_resolution(codec_name(codec), &dw, &dh, &dfps) && dw > 0 && dh > 0) max_w = dw, max_h = dh;
    // H.264 that fits FHD is declared as FHD (larger declarations are refused by some firmware); H.264 at its rate
    // class, other codecs at 60 (the kit's verified values). The web process sends a new Load when a later
    // rendition outside this envelope comes (engine/gst/wpebmediasink.c, the same rule).
    if (codec == Codec::H264 && width > 0 && width <= 1920 && height <= 1088) max_w = 1920, max_h = 1080;
    const int max_fps = codec != Codec::H264 || fps > 30.5 ? 60 : 30;
    // With audio, as the kit's StarfishBackend loads it (verified on this TV): the pipeline syncs the video to the
    // audio (audioSync), with a larger audio buffer and without low-delay mode.
    const bool with_audio = audio != nullptr;
    const bool opus = with_audio && audio->codec == Audio::Codec::Opus;
    // Opus as Chromium on webOS declares it (opusInfo: channels, sampleRate, streamHeader); the encoding of the
    // header is not known (WPEB_HW_OPUS_HEADER=base64 tries the other one).
    std::string audio_info;
    if (opus) {
        const char *encoding = std::getenv("WPEB_HW_OPUS_HEADER");
        const bool b64 = encoding != nullptr && std::strcmp(encoding, "base64") == 0;
        audio_info = ",\"opusInfo\":{\"channels\":" + std::to_string(audio->channels) +
                     ",\"sampleRate\":" + std::to_string(audio->rate) + ",\"streamHeader\":\"" +
                     (b64 ? base64(audio->header) : hex(audio->header)) + "\"}";
    }
    char payload[4096];
    std::snprintf(payload, sizeof payload,
                  "{\"args\":[{\"mediaTransportType\":\"BUFFERSTREAM\",\"option\":{\"appId\":\"%s\",\"windowId\":\"%s\","
                  "\"externalStreamingInfo\":{\"contents\":{\"codec\":{\"video\":\"%s\"%s},\"esInfo\":{\"pauseAtDecodeTime\":false,"
                  "\"ptsToDecode\":0,\"seperatedPTS\":true},\"format\":\"RAW\",\"provider\":\"wpebrowser\"%s},"
                  "\"streamQualityInfo\":true,\"streamQualityInfoNonFlushable\":true,\"audioSync\":%s,\"restartStreaming\":false,"
                  "\"bufferingCtrInfo\":{\"bufferMaxLevel\":0,\"bufferMinLevel\":0,\"preBufferByte\":0,\"qBufferLevelAudio\":0,"
                  "\"qBufferLevelVideo\":0,\"srcBufferLevelAudio\":{\"minimum\":1,\"maximum\":%d},"
                  "\"srcBufferLevelVideo\":{\"minimum\":1,\"maximum\":8388608}}},\"needAudio\":%s,\"queryPosition\":false,"
                  "\"lowDelayMode\":%s,\"transmission\":{\"contentsType\":\"LIVE\"},\"adaptiveStreaming\":{\"audioOnly\":false,"
                  "\"adaptiveResolution\":true,\"maxWidth\":%d,\"maxHeight\":%d,\"maxFrameRate\":%d}}}]}",
                  app_id.c_str(), window_id.c_str(), codec_name(codec),
                  !with_audio ? "" : opus ? ",\"audio\":\"OPUS\"" : ",\"audio\":\"AAC\"", audio_info.c_str(),
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
             fps, max_w, max_h, max_fps, !with_audio ? "" : opus ? " + Opus" : " + AAC", window_id.c_str(),
             api_->getMediaID() ? api_->getMediaID() : "?");
    if (opus) WNK_LOGI(kCat, "opusInfo%s", audio_info.c_str() + std::strlen(",\"opusInfo\""));
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

void LgMediaPlayer::flush() {
#if defined(WNK_SDL_WEBOS)
    if (api_ != nullptr && (fed_ > 0 || audio_fed_ > 0)) {
        const bool ok = api_->flush();
        WNK_LOGI(kCat, "flush -> %d", ok ? 1 : 0);
    }
#endif
    seek_pending_ = true;
}

void LgMediaPlayer::end_of_stream() {
#if defined(WNK_SDL_WEBOS)
    if (api_ == nullptr) return;
    const bool ok = api_->pushEOS();
    WNK_LOGI(kCat, "pushEOS after %lld units + %lld audio (last video pts %.3f s) -> %d", static_cast<long long>(fed_),
             static_cast<long long>(audio_fed_), last_pts_us_ / 1e6, ok ? 1 : 0);
#endif
}

void LgMediaPlayer::set_hdr(const WpebMediaHdr &c) {
#if defined(WNK_SDL_WEBOS)
    // The kit's StarfishBackend::build_hdr_payload (verified on this TV): SEI units, primaries in G, B, R order.
    const char *type = c.transfer == 16 ? "HDR10" : c.transfer == 18 ? "HLG" : nullptr;
    if (api_ == nullptr || type == nullptr) return;
    std::string sei;
    const auto add = [&sei](const std::string &key, long long v) {
        sei += (sei.empty() ? "\"" : ",\"") + key + "\":" + std::to_string(v);
    };
    if (c.has_mastering) {
        static constexpr int kOrder[3] = {1, 2, 0};  // G, B, R from R, G, B
        for (int i = 0; i < 3; ++i) {
            add("displayPrimariesX" + std::to_string(i), c.display_primaries[kOrder[i]][0]);
            add("displayPrimariesY" + std::to_string(i), c.display_primaries[kOrder[i]][1]);
        }
        add("whitePointX", c.white_point[0]);
        add("whitePointY", c.white_point[1]);
        add("minDisplayMasteringLuminance", c.min_luminance);
        add("maxDisplayMasteringLuminance", c.max_luminance);
    }
    if (c.has_light_level) {
        add("maxContentLightLevel", c.max_cll);
        add("maxPicAverageLightLevel", c.max_fall);
    }
    // Without any of it the call is skipped: some TVs crash on an HDR message with no SEI (Kodi), and the decoder
    // still sees whatever the bitstream carries.
    if (sei.empty()) {
        WNK_LOGI(kCat, "%s without mastering or light-level values: no setHdrInfo", type);
        return;
    }
    const std::string payload = std::string("{\"hdrType\":\"") + type + "\",\"sei\":{" + sei +
                                "},\"vui\":{\"transferCharacteristics\":" + std::to_string(c.transfer) +
                                ",\"colorPrimaries\":" + std::to_string(c.primaries) + ",\"matrixCoeffs\":" +
                                std::to_string(c.matrix) + ",\"videoFullRangeFlag\":" + (c.full_range ? "true" : "false") +
                                "}}";
    const auto set_hdr_info = player_apis().set_hdr_info;
    if (set_hdr_info == nullptr) {
        WNK_LOGI(kCat, "%s: this firmware has no setHdrInfo", type);
        return;
    }
    const bool ok = set_hdr_info(api_, payload.c_str());
    WNK_LOGI(kCat, "setHdrInfo %s -> %d", payload.c_str(), ok ? 1 : 0);
#else
    (void)c;
#endif
}

void LgMediaPlayer::set_volume(unsigned permille, bool muted) {
#if defined(WNK_SDL_WEBOS)
    if (api_ == nullptr || !has_audio_) return;
    // LG's player takes setVolume(volume, duration, ease) underneath (libpf); the JSON keys are not documented.
    char json[96];
    std::snprintf(json, sizeof json, "{\"volume\":%u,\"duration\":0,\"easeType\":\"Linear\"}", muted ? 0u : permille / 10);
    const auto &apis = player_apis();
    const bool v = apis.set_volume != nullptr && apis.set_volume(api_, json);
    const bool m = apis.set_audio_mute != nullptr && apis.set_audio_mute(api_, muted ? "{\"mute\":true}" : "{\"mute\":false}");
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
    case PF_EVENT_TYPE_STR_STATE_UPDATE__ENDOFSTREAM:
        WNK_LOGI(kCat, "end of stream reached (%lld frames presented)", static_cast<long long>(frames_.load()));
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
