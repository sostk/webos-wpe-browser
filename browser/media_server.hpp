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

namespace wnk::media { class NdlBackend; }

namespace browser {

class MediaServer {
public:
    MediaServer();
    ~MediaServer();
    MediaServer(const MediaServer &) = delete;
    MediaServer &operator=(const MediaServer &) = delete;

    // Listens on `path` if LG's decoder library is there; false with *why otherwise.
    bool start(const std::string &path, std::string *why);
    void stop();
    bool running() const { return listen_fd_ >= 0; }
    const std::string &path() const { return path_; }
    // A page's video is on LG's decoder right now (any thread).
    bool video_active() const { return owner_.load() != 0; }
    // App lifecycle: LG's decoder must know when the app leaves the foreground.
    static void set_foreground(bool foreground);

private:
    void accept_loop();
    void serve(int fd, uint64_t id);

    std::string path_;
    int listen_fd_ = -1;
    int wake_[2] = {-1, -1};
    std::thread acceptor_;
    std::mutex mutex_;  // the backends
    // As Chromium on webOS: a stream with a video window on the page surface plays through LG's media pipeline
    // bound to it (LgMediaPlayer); without one, through NDL (whose video sits below the UI window).
    std::unique_ptr<wnk::media::NdlBackend> backend_;
    LgMediaPlayer lg_;
    bool use_lg_ = false;  // the owner's stream is on lg_ (else on NDL)
    bool ndl_loaded_ = false;
    std::atomic<uint64_t> owner_{0};  // the connection whose stream is loaded (latest LOAD wins)
    std::atomic<uint64_t> next_id_{1};
    std::atomic<int> connections_{0};
};

}  // namespace browser
