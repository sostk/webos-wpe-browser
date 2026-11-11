// SPDX-License-Identifier: GPL-3.0-or-later
// Direct GPU presentation, the UI's part (docs/webkit-like-chromium.md, part 2): a webOS surface group on this
// app's window with a "page" layer below it. The engine's web processes attach their own EGL surfaces to that
// layer; the UI draws its chrome as usual and leaves the page area transparent (PageView).
// The Wayland work lives in engine/lib/libwpeb-owner.so (dlopen()ed: this executable does not link libwayland,
// and a host build simply finds no library and stays in SHM mode).
#pragma once

#include <string>

struct wpeb_owner;

namespace browser {

class DirectOwner {
 public:
    DirectOwner() = default;
    ~DirectOwner();
    DirectOwner(const DirectOwner &) = delete;
    DirectOwner &operator=(const DirectOwner &) = delete;

    // Creates the group on the current SDL GL window. False (with why) where that is not possible.
    bool open(const std::string &engine_dir, std::string &why, int page_z = -1);
    bool active() const { return owner_ != nullptr; }
    const std::string &group() const { return group_; }
    const std::string &layer() const { return layer_; }
    const std::string &socket() const { return socket_; }  // compositor socket for WPEB_WAYLAND_SOCKET
    void dispatch();  // once per UI frame
    // Moves the page layer: below the UI window (-1, the default) or above it (1). LG's video is drawn as a
    // punch-through directly below the app's main window and cuts out everything under it, the page included:
    // while hardware video plays the page goes above, or the page's own controls over the video vanish.
    void set_page_z(int z);
    // A new page layer (see wpeb_owner_renew_layer); layer() is its name. False if the library cannot.
    bool renew_layer();
    int page_z() const { return page_z_; }
    void close();

 private:
    void *lib_ = nullptr;
    wpeb_owner *owner_ = nullptr;
    std::string group_, layer_ = "page", socket_;
    void (*dispatch_)(wpeb_owner *) = nullptr;
    void (*destroy_)(wpeb_owner *) = nullptr;
    void (*set_z_)(wpeb_owner *, int) = nullptr;
    int (*renew_layer_)(wpeb_owner *, const char *) = nullptr;
    int layer_generation_ = 0;
    int page_z_ = -1;
};

}  // namespace browser
