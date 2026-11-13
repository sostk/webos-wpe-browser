// SPDX-License-Identifier: GPL-3.0-or-later
// The UI side of the wpe-engine connection (common/wpe_ipc.h): spawns the engine, opens and switches tabs,
// sends navigation commands and input, receives per-tab page state and the active tab's frames.
// Main thread only; poll() is called once per loop iteration.
#pragma once

#include "wpe_ipc.h"

#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <sys/types.h>
#include <vector>

namespace browser {

using TabId = uint32_t;

struct EngineFrame {
    TabId tab = 0;
    const uint8_t *pixels = nullptr;  // B,G,R,A bytes, `stride` per row
    uint32_t width = 0, height = 0, stride = 0;
    std::vector<WpeRect> damage;      // empty = whole frame
    uint32_t slot = 0, serial = 0;
};

struct EngineCallbacks {
    std::function<void(const WpeEngineReady &)> ready;
    std::function<void(TabId, const std::string &)> uri;
    std::function<void(TabId, const std::string &)> title;
    std::function<void(TabId, float)> progress;  // 0..1
    std::function<void(TabId, WpeLoadEvent)> load;
    std::function<void(TabId, bool back, bool forward)> history;
    std::function<void(TabId, const WpeIme &)> ime;
    std::function<void(TabId, WpeErrorKind, uint32_t code, const std::string &)> error;
    std::function<void(uint32_t what, bool ok)> data_cleared;  // answer to clear_data()
    std::function<void(TabId, bool at_top)> scroll;  // the page reached / left its top (shows the toolbar)
    std::function<void(TabId)> edge_up;
    std::function<void(TabId, const std::string &url)> icon;  // the page's icon URL (WPE_ENGINE_ICON)              // Up found nothing above at the page's top (WPE_ENGINE_EDGE)
    std::function<void(TabId, bool on)> fullscreen;  // the page entered / left fullscreen
    std::function<void(TabId, const WpePresent &)> present;  // how the page reaches the screen (direct mode)
    std::function<void(TabId, TabId opener)> tab_created;      // a page opened a window (the engine's tab id)
    std::function<void(TabId)> tab_close;                       // a page called window.close()
    std::function<void(const std::string &why)> exited;  // the engine process went away
};

struct EngineLaunch {
    std::string engine_dir;   // contains wpe-engine, lib/, libexec/, etc/, share/
    std::string data_dir;     // persistent (cookies, storage)
    std::string cache_dir;
    std::string runtime_dir;  // logs
    uint32_t width = 1280, height = 720;  // page size, CSS px
    // Graphics for WPEWebProcess: false = bundled Mesa llvmpipe (software GL, engine/lib/mesa on the library path),
    // Skia paints on the CPU; true = the TV's own EGL/GLES (Mali) through the default EGL display, Skia paints with
    // GPU and CPU (hybrid). Frames reach the screen by SHM or, with surface_group, directly.
    bool gpu = false;
    // Direct presentation (needs gpu): the web processes draw into their own surfaces in this surface group
    // (DirectOwner) instead of sending frames. Empty = SHM frames.
    std::string surface_group, surface_layer, wayland_socket;
    // Hardware video (browser/media_server.hpp): the UI's decoder service, empty when there is none.
    std::string media_socket;
    // Extra engine environment for experiments (launch parameter {"wpebEnv": {"WEBKIT_...": "..."}}): applied last,
    // only names starting with WEBKIT_, JSC_, WPE_BROWSER_DEBUG or GST_DEBUG.
    std::vector<std::pair<std::string, std::string>> extra_env;
};

class EngineClient {
 public:
    explicit EngineClient(EngineCallbacks cb) : cb_(std::move(cb)) {}
    ~EngineClient();
    EngineClient(const EngineClient &) = delete;
    EngineClient &operator=(const EngineClient &) = delete;

    bool start(const EngineLaunch &launch, std::string &error);
    bool running() const { return sock_ >= 0; }
    void poll();
    // The newest frame not yet acknowledged, or nullptr.
    const EngineFrame *frame() const { return have_frame_ ? &frame_ : nullptr; }
    void frame_done();
    void stop();

    // Tabs (ids are chosen by the app, never 0).
    void tab_open(TabId tab, const std::string &uri) { send_text(WPE_APP_TAB_OPEN, tab, uri); }
    void tab_close(TabId tab) { send(WPE_APP_TAB_CLOSE, tab, nullptr, 0); }
    void tab_activate(TabId tab) { send(WPE_APP_TAB_ACTIVATE, tab, nullptr, 0); }

    // Navigation, per tab.
    void load_uri(TabId tab, const std::string &uri) { send_text(WPE_APP_LOAD_URI, tab, uri); }
    void go_back(TabId tab) { send(WPE_APP_GO_BACK, tab, nullptr, 0); }
    void go_forward(TabId tab) { send(WPE_APP_GO_FORWARD, tab, nullptr, 0); }
    void reload(TabId tab) { send(WPE_APP_RELOAD, tab, nullptr, 0); }
    void stop_loading(TabId tab) { send(WPE_APP_STOP, tab, nullptr, 0); }
    void run_js(TabId tab, const std::string &script) { send_text(WPE_APP_RUN_JS, tab, script); }
    // Leaves the page's fullscreen (Back on the remote), from the top document down.
    void exit_fullscreen(TabId tab) { run_js(tab, "document.fullscreenElement && document.exitFullscreen()"); }
    void set_layer(const std::string &layer) { send_text(WPE_APP_SET_LAYER, 0, layer); }

    // Page size and input: always the active tab.
    void resize(uint32_t w, uint32_t h);
    void set_visible(bool v);
    void set_focus(bool f);
    void key(bool down, uint32_t keyval, uint32_t keycode, uint32_t modifiers);
    void pointer(WpePointerKind kind, uint32_t button, int32_t x, int32_t y, uint32_t time_ms);
    void scroll(int32_t x, int32_t y, int32_t dx120, int32_t dy120);
    void commit_text(const std::string &utf8) { send_text(WPE_APP_TEXT_COMMIT, 0, utf8); }

    // Website data (WPE_CLEAR_* bits; seconds = 0 for all time) and page zoom for every tab.
    void clear_data(uint32_t what, uint32_t seconds);
    void set_zoom(uint32_t permille);
    // Device scale: the engine renders CSS size x permille/1000 pixels (1500 = native 1080p for a 720p page).
    void set_scale(uint32_t permille);

 private:
    bool send(uint32_t type, TabId tab, const void *payload, uint32_t size);
    void send_text(uint32_t type, TabId tab, const std::string &text);
    void handle(const uint8_t *buf, size_t n, int fd);
    void lost(const std::string &why);
    void unmap();

    EngineCallbacks cb_;
    int sock_ = -1;
    pid_t pid_ = -1;
    uint8_t *frames_ = nullptr;
    size_t frames_size_ = 0;
    uint32_t slot_bytes_ = 0;
    EngineFrame frame_;
    bool have_frame_ = false;
};

}  // namespace browser
