// SPDX-License-Identifier: GPL-3.0-or-later
// Direct GPU presentation (WPE_BROWSER_PRESENT=direct): each tab's WPEWebProcess draws with the TV's EGL into its
// own Wayland surface in the UI's surface group (engine/webos-backend). Uses WebKit's libwpe API, so a process
// either runs all tabs this way or all through WPEPlatform/SHM (platform.cpp); main.cpp decides at start-up.
#pragma once

#include "../../common/wpe_ipc.h"

#include <cstdint>
#include <wpe/webkit.h>

namespace direct {

struct View;

// Tells the UI what the tab's surface is doing (wpeb_webos_status, engine/webos-backend/protocol.h).
using StatusFn = void (*)(uint32_t tab, int32_t status, uint32_t width, uint32_t height);

// Loads libWPEBackend-webos.so and makes libwpe (here and in every web process) use it. False: use SHM mode.
bool init(const char *group, const char *layer, StatusFn status);

// A backend for a new WebKitWebView ("backend" property). `view` receives the per-tab handle.
WebKitWebViewBackend *create_backend(uint32_t tab, uint32_t width, uint32_t height, View **view);

void set_size(View *view, uint32_t width, uint32_t height);
void set_scale(View *view, double scale);
// Shown = attached to the UI's surface group and visible to WebKit; hidden = detached and not rendering.
void set_shown(View *view, bool shown);
void set_focused(View *view, bool focused);
// Pages attach to `layer` from now on; a shown view is attached again there (its web process is new or reloading).
void set_layer(const char *layer);
void reattach(View *view);

// The TV keyboard: an input method context for the tab's WebKitWebView that reports editable focus (purpose is
// a WPEInputPurpose, as in SHM mode) and commits the text the UI sends back.
using ImeFn = void (*)(uint32_t tab, bool focused, uint32_t purpose, int x, int y, int width, int height);
WebKitInputMethodContext *create_ime(uint32_t tab, ImeFn fn);
void commit_text(WebKitInputMethodContext *ime, const char *text);

void key(View *view, const WpeKey &k);
void pointer(View *view, const WpePointer &p, uint32_t &buttons);
void scroll(View *view, const WpeScroll &s, uint32_t buttons);

} // namespace direct
