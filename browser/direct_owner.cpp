// SPDX-License-Identifier: GPL-3.0-or-later
#include "direct_owner.hpp"

#include "wnk/log.hpp"

#include <SDL.h>
#include <SDL_syswm.h>

#include <cstddef>
#include <dlfcn.h>
#include <unistd.h>

namespace browser {
namespace {
constexpr const char *kCat = "direct";
}  // namespace

DirectOwner::~DirectOwner() { close(); }

bool DirectOwner::open(const std::string &engine_dir, std::string &why, int page_z) {
    close();
#if defined(SDL_VIDEO_DRIVER_WAYLAND)
    SDL_Window *window = SDL_GL_GetCurrentWindow();
    SDL_SysWMinfo info;
    SDL_VERSION(&info.version);
    if (window == nullptr || !SDL_GetWindowWMInfo(window, &info)) {
        why = std::string("no window information: ") + SDL_GetError();
        return false;
    }
    if (info.subsystem != SDL_SYSWM_WAYLAND || info.info.wl.display == nullptr || info.info.wl.surface == nullptr) {
        why = "the window is not a Wayland window";
        return false;
    }
    const std::string path = engine_dir + "/lib/libwpeb-owner.so";
    lib_ = ::dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (lib_ == nullptr) {
        why = ::dlerror();
        return false;
    }
    using Create = wpeb_owner *(*)(wl_display *, wl_surface *, const char *, const char *, int, char *, size_t);
    auto create = reinterpret_cast<Create>(::dlsym(lib_, "wpeb_owner_create"));
    dispatch_ = reinterpret_cast<void (*)(wpeb_owner *)>(::dlsym(lib_, "wpeb_owner_dispatch"));
    destroy_ = reinterpret_cast<void (*)(wpeb_owner *)>(::dlsym(lib_, "wpeb_owner_destroy"));
    set_z_ = reinterpret_cast<void (*)(wpeb_owner *, int)>(::dlsym(lib_, "wpeb_owner_set_z"));  // optional
    page_z_ = page_z;
    if (create == nullptr || dispatch_ == nullptr || destroy_ == nullptr) {
        why = "libwpeb-owner.so is incomplete";
        close();
        return false;
    }
    // Surface group names are global in the compositor: one per app instance.
    group_ = "wpeb-" + std::to_string(::getpid());
    char socket[108] = {};
    owner_ = create(info.info.wl.display, info.info.wl.surface, group_.c_str(), layer_.c_str(), page_z, socket,
                    sizeof(socket));
    if (owner_ == nullptr) {
        why = "the compositor did not create the surface group (see the log)";
        close();
        return false;
    }
    socket_ = socket;
    WNK_LOGI(kCat, "surface group %s, layer %s, compositor socket %s", group_.c_str(), layer_.c_str(),
             socket_.empty() ? "(default)" : socket_.c_str());
    return true;
#else
    (void)engine_dir;
    why = "SDL without Wayland";
    return false;
#endif
}

void DirectOwner::dispatch() {
    if (owner_ != nullptr) dispatch_(owner_);
}

void DirectOwner::set_page_z(int z) {
    if (owner_ == nullptr || set_z_ == nullptr || z == page_z_) return;
    set_z_(owner_, z);
    page_z_ = z;
    WNK_LOGI(kCat, "page layer %s the UI window", z > 0 ? "above" : "below");
}

void DirectOwner::close() {
    if (owner_ != nullptr) destroy_(owner_);
    owner_ = nullptr;
    if (lib_ != nullptr) ::dlclose(lib_);
    lib_ = nullptr;
    dispatch_ = nullptr;
    destroy_ = nullptr;
    set_z_ = nullptr;
}

}  // namespace browser
