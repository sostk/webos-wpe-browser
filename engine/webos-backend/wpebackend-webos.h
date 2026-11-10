// SPDX-License-Identifier: GPL-3.0-or-later
// libWPEBackend-webos: direct GPU presentation for WPE WebKit on webOS (docs/webkit-like-chromium.md, part 2).
//
// WPEWebProcess draws each page with the TV's Mali EGL into its own wl_surface (a wl_egl_window), attached to a
// webOS surface group that the browser UI created on its window. LSM composes it with the UI: no glReadPixels,
// no shared-memory copies, no texture upload in the UI.
//
// The engine uses the legacy libwpe API for this (WebKit picks it per process: a WebKitWebView created with a
// WebKitWebViewBackend instead of a WPEDisplay). Call wpe_loader_init("libWPEBackend-webos.so") first, so the web
// process loads this library too. The engine dlopen()s it and looks these functions up by name (engine/src/direct.cpp).
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct wpe_view_backend;
struct wpeb_webos_view;

// `status` is a wpeb_webos_status (protocol.h), reported by the page's drawing target in the web process.
typedef void (*wpeb_webos_status_fn)(void *user, int32_t status, uint32_t width, uint32_t height);

// A view backend for one tab, `width` x `height` CSS pixels. Owned by the returned wpe_view_backend:
// wpe_view_backend_destroy() (WebKitWebViewBackend's destroy notify) frees it.
struct wpeb_webos_view *wpeb_webos_view_new(uint32_t width, uint32_t height);
struct wpe_view_backend *wpeb_webos_view_get_backend(struct wpeb_webos_view *view);
void wpeb_webos_view_set_status_callback(struct wpeb_webos_view *view, wpeb_webos_status_fn fn, void *user);
// Show the page: attach its surface to `layer` of the UI's surface group `group`. Kept and re-sent when the
// web process is restarted.
void wpeb_webos_view_attach(struct wpeb_webos_view *view, const char *group, const char *layer);
// Hide it.
void wpeb_webos_view_detach(struct wpeb_webos_view *view);

#ifdef __cplusplus
}
#endif
