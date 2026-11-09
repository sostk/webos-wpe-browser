// SPDX-License-Identifier: GPL-3.0-or-later
// A WPEPlatform implementation for running WebKit inside the webOS browser's engine process.
//
// There is no window system here: the "screen" is the browser UI app at the other end of the IPC socket.
// The display offers no EGL, so WebKit renders into shared-memory buffers (WPEBufferSHM), which the view
// hands to its FrameSink. Input arrives as WPEEvents built by the engine from IPC messages.
#pragma once

#include <wpe/wpe-platform.h>

G_BEGIN_DECLS

// Receives each frame WebKit renders, and the editable-focus changes of the input method.
typedef struct {
    // A new frame. The sink copies the pixels and later calls webos_view_frame_shown() for it.
    void (*frame)(void *user, WPEView *view, WPEBuffer *buffer, const WPERectangle *damage, guint n_damage);
    // An editable gained (focused = TRUE) or lost focus; purpose is a WPEInputPurpose.
    void (*ime_focus)(void *user, WPEView *view, gboolean focused, WPEInputPurpose purpose, int x, int y, int width, int height);
    void *user;
} WebOSSink;

#define WEBOS_TYPE_DISPLAY (webos_display_get_type())
G_DECLARE_FINAL_TYPE(WebOSDisplay, webos_display, WEBOS, DISPLAY, WPEDisplay)

#define WEBOS_TYPE_VIEW (webos_view_get_type())
G_DECLARE_FINAL_TYPE(WebOSView, webos_view, WEBOS, VIEW, WPEView)

#define WEBOS_TYPE_TOPLEVEL (webos_toplevel_get_type())
G_DECLARE_FINAL_TYPE(WebOSToplevel, webos_toplevel, WEBOS, TOPLEVEL, WPEToplevel)

#define WEBOS_TYPE_IME (webos_ime_get_type())
G_DECLARE_FINAL_TYPE(WebOSIme, webos_ime, WEBOS, IME, WPEInputMethodContext)

WPEDisplay *webos_display_new(const WebOSSink *sink);

// The frame passed to WebOSSink.frame has been shown by the UI: WebKit may render the next one.
void webos_view_frame_shown(WebOSView *view, WPEBuffer *buffer);

// Text typed on the TV keyboard, for the editable that has focus (if any).
void webos_display_commit_text(WebOSDisplay *display, const char *utf8);

G_END_DECLS
