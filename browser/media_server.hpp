// SPDX-License-Identifier: GPL-3.0-or-later
// Hardware video for pages (common/wpe_media_ipc.h): the web process's video sink sends the encoded stream here,
// and this process feeds LG's decoder through the kit's NdlBackend. LG's media stack registers bus names under
// the app's luna role, which is bound to this executable, so the decoder cannot live in WPEWebProcess.
#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "lg_media_player.hpp"
#include "wnk/geometry.hpp"

namespace wnk::media { class MediaBackend; }

namespace browser {

struct WpebAreaPx {
    int32_t x = 0, y = 0, width = 0, height = 0;
};

class MediaServer {
public:
    MediaServer();
    ~MediaServer();
    MediaServer(const MediaServer &) = delete;
    MediaServer &operator=(const MediaServer &) = delete;

    // Listens on `path` if LG's decoder library is there; false with *why otherwise.
    // video_window: the compositor offers video windows, without which LG's media pipeline cannot show video.
    // acb: without video windows, play through LG's pipeline placed by ACB (libAcbAPI, webOS 3.x and 4.x) instead
    // of giving up (experimental, {"wpebVideo":"acb"}).
    bool start(const std::string &path, bool video_window, bool acb, std::string *why);
    void stop();
    bool running() const { return listen_fd_ >= 0; }
    const std::string &path() const { return path_; }
    // A page's video is on LG's decoder right now (any thread).
    bool video_active() const { return owner_.load() != 0; }
    // App lifecycle: LG's decoder must know when the app leaves the foreground.
    static void set_foreground(bool foreground);
    // The ACB path (no video windows) places the video on the screen itself: where the page is on the screen
    // (UI pixels, 1920x1080) and the page's size in page pixels, which the web process's areas are in.
    void set_page_geometry(const wnk::Rect &screen, uint32_t page_width, uint32_t page_height);
    // The video plays through ACB (the page cannot show video windows); any thread.
    bool acb() const { return acb_; }

private:
    void accept_loop();
    void serve(int fd, uint64_t id);
    void place_acb_locked();  // mutex_

    std::string path_;
    int listen_fd_ = -1;
    int wake_[2] = {-1, -1};
    std::thread acceptor_;
    std::mutex mutex_;  // the backends
    // As Chromium on webOS: a stream with a video window on the page surface plays through LG's media pipeline
    // bound to it (LgMediaPlayer); without one, through NDL (whose video sits below the UI window). On webOS 3.x
    // and 4.x (no video windows, an NDL without NDL_DirectMediaLoad), backend_ is the kit's StarfishBackend on
    // its ACB plane instead (acb_), and every stream goes there, its AAC audio with it.
    std::unique_ptr<wnk::media::MediaBackend> backend_;
    std::atomic<bool> acb_{false};
    WpebAreaPx area_{};        // mutex_; the owner's last area, page pixels
    wnk::Rect page_screen_{};  // mutex_
    uint32_t page_w_ = 0, page_h_ = 0;
    LgMediaPlayer lg_;
    bool use_lg_ = false;  // the owner's stream is on lg_ (else on NDL)
    bool ndl_loaded_ = false;     // backend_ has the owner's stream (NDL, or Starfish on ACB)
    bool backend_audio_ = false;  // mutex_; backend_ plays the owner's audio too (ACB)
    std::atomic<uint64_t> owner_{0};  // the connection whose stream is loaded (latest LOAD wins)
    std::atomic<uint64_t> next_id_{1};
    std::atomic<int> connections_{0};
};

}  // namespace browser
