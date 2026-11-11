// SPDX-License-Identifier: GPL-3.0-or-later
// LG's buffer-feed media pipeline (StarfishMediaAPIs in libplayerAPIs, the NDK's starfish-media-pipeline header),
// driven the way Chromium-based browsers on webOS drive it: a BUFFERSTREAM Load whose option.windowId is the
// exported video window made on the surface that draws the page (engine/webos-backend), elementary-stream access
// units fed with their media timestamps, and the pipeline's own Play, Pause and Seek. The page's hole and the video
// window are placed by the web process; nothing here touches the screen.
#pragma once

#include <atomic>
#include <cstdint>
#include <string>

class StarfishMediaAPIs;

namespace browser {

class LgMediaPlayer {
public:
    enum class Codec { H264, Hevc, Vp9, Av1 };
    enum class Feed { Ok, Full, Error };

    LgMediaPlayer() = default;
    ~LgMediaPlayer();  // unloads
    LgMediaPlayer(const LgMediaPlayer &) = delete;
    LgMediaPlayer &operator=(const LgMediaPlayer &) = delete;

    static bool available();  // false in host builds

    // Video only for now (audio stays on PulseAudio in the web process).
    bool load(const std::string &app_id, const std::string &window_id, Codec codec, int width, int height, double fps,
              std::string *error);
    // One access unit (Annex B for H.264/HEVC), presentation time in microseconds of media time.
    Feed feed_video(const uint8_t *data, size_t size, int64_t pts_us);
    // A seek: the next access unit fed starts the new position (LG's Seek at its time).
    void flush();
    void pause(bool paused);
    void unload();
    bool loaded() const { return api_ != nullptr; }
    int64_t fed() const { return fed_; }

    // The pipeline's event callback (any thread).
    void on_event(int type, int64_t num, const char *str);

private:
    StarfishMediaAPIs *api_ = nullptr;
    std::atomic<bool> load_completed_{false};
    bool playing_ = false, paused_ = false, seek_pending_ = true;
    int64_t fed_ = 0, full_ = 0, last_pts_us_ = 0;
    std::atomic<int64_t> frames_{0};  // from the pipeline's thread
    int64_t stats_at_ms_ = 0;
};

}  // namespace browser
