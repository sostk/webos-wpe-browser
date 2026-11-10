// SPDX-License-Identifier: GPL-3.0-or-later
#include "platform.h"

// ---------------------------------------------------------------------------------------------------------
// WebOSIme: forwards editable focus to the UI (which opens the TV keyboard) and commits the text it sends back.

struct _WebOSIme {
    WPEInputMethodContext parent_instance;
};

G_DEFINE_FINAL_TYPE(WebOSIme, webos_ime, WPE_TYPE_INPUT_METHOD_CONTEXT)

struct _WebOSDisplay {
    WPEDisplay parent_instance;
    WebOSSink sink;
    WebOSIme *focused_ime; // weak: cleared in focus_out and dispose
    int cursor_x, cursor_y, cursor_w, cursor_h;
};

G_DEFINE_FINAL_TYPE(WebOSDisplay, webos_display, WPE_TYPE_DISPLAY)

static WebOSDisplay *ime_display(WPEInputMethodContext *context)
{
    WPEDisplay *display = wpe_input_method_context_get_display(context);
    return WEBOS_IS_DISPLAY(display) ? WEBOS_DISPLAY(display) : nullptr;
}

static void webos_ime_focus_in(WPEInputMethodContext *context)
{
    WebOSDisplay *display = ime_display(context);
    if (!display)
        return;
    display->focused_ime = WEBOS_IME(context);
    if (display->sink.ime_focus)
        display->sink.ime_focus(display->sink.user, wpe_input_method_context_get_view(context), TRUE,
            wpe_input_method_context_get_input_purpose(context),
            display->cursor_x, display->cursor_y, display->cursor_w, display->cursor_h);
}

static void webos_ime_focus_out(WPEInputMethodContext *context)
{
    WebOSDisplay *display = ime_display(context);
    if (!display || display->focused_ime != WEBOS_IME(context))
        return;
    display->focused_ime = nullptr;
    if (display->sink.ime_focus)
        display->sink.ime_focus(display->sink.user, wpe_input_method_context_get_view(context), FALSE,
            WPE_INPUT_PURPOSE_FREE_FORM, 0, 0, 0, 0);
}

static void webos_ime_set_cursor_area(WPEInputMethodContext *context, int x, int y, int width, int height)
{
    if (WebOSDisplay *display = ime_display(context)) {
        display->cursor_x = x;
        display->cursor_y = y;
        display->cursor_w = width;
        display->cursor_h = height;
    }
}

static gboolean webos_ime_filter_key_event(WPEInputMethodContext *, WPEEvent *)
{
    // The TV keyboard delivers whole strings (see webos_display_commit_text), not key events to compose.
    return FALSE;
}

static void webos_ime_get_preedit_string(WPEInputMethodContext *, gchar **text, GList **underlines, guint *cursor_offset)
{
    if (text)
        *text = g_strdup("");
    if (underlines)
        *underlines = nullptr;
    if (cursor_offset)
        *cursor_offset = 0;
}

static void webos_ime_dispose(GObject *object)
{
    if (WebOSDisplay *display = ime_display(WPE_INPUT_METHOD_CONTEXT(object))) {
        if (display->focused_ime == WEBOS_IME(object))
            display->focused_ime = nullptr;
    }
    G_OBJECT_CLASS(webos_ime_parent_class)->dispose(object);
}

static void webos_ime_class_init(WebOSImeClass *klass)
{
    G_OBJECT_CLASS(klass)->dispose = webos_ime_dispose;
    WPEInputMethodContextClass *ime = WPE_INPUT_METHOD_CONTEXT_CLASS(klass);
    ime->focus_in = webos_ime_focus_in;
    ime->focus_out = webos_ime_focus_out;
    ime->set_cursor_area = webos_ime_set_cursor_area;
    ime->filter_key_event = webos_ime_filter_key_event;
    ime->get_preedit_string = webos_ime_get_preedit_string;
}

static void webos_ime_init(WebOSIme *)
{
}

// ---------------------------------------------------------------------------------------------------------
// WebOSToplevel: one fixed "window", always active; its size is the page size the UI asks for.

struct _WebOSToplevel {
    WPEToplevel parent_instance;
};

G_DEFINE_FINAL_TYPE(WebOSToplevel, webos_toplevel, WPE_TYPE_TOPLEVEL)

static gboolean webos_toplevel_resize(WPEToplevel *toplevel, int width, int height)
{
    wpe_toplevel_resized(toplevel, width, height);
    wpe_toplevel_foreach_view(toplevel, [](WPEToplevel *toplevel, WPEView *view, gpointer) -> gboolean {
        int width = 0, height = 0;
        wpe_toplevel_get_size(toplevel, &width, &height);
        wpe_view_resized(view, width, height);
        return FALSE;
    }, nullptr);
    return TRUE;
}

static gboolean webos_toplevel_set_fullscreen(WPEToplevel *toplevel, gboolean fullscreen)
{
    // The browser is always full screen on the TV; just report the state WebKit asked for.
    auto state = wpe_toplevel_get_state(toplevel);
    state = static_cast<WPEToplevelState>(fullscreen ? (state | WPE_TOPLEVEL_STATE_FULLSCREEN) : (state & ~WPE_TOPLEVEL_STATE_FULLSCREEN));
    wpe_toplevel_state_changed(toplevel, state);
    return TRUE;
}

static void webos_toplevel_constructed(GObject *object)
{
    G_OBJECT_CLASS(webos_toplevel_parent_class)->constructed(object);
    wpe_toplevel_state_changed(WPE_TOPLEVEL(object), WPE_TOPLEVEL_STATE_ACTIVE);
}

static void webos_toplevel_class_init(WebOSToplevelClass *klass)
{
    G_OBJECT_CLASS(klass)->constructed = webos_toplevel_constructed;
    WPEToplevelClass *toplevel = WPE_TOPLEVEL_CLASS(klass);
    toplevel->resize = webos_toplevel_resize;
    toplevel->set_fullscreen = webos_toplevel_set_fullscreen;
}

static void webos_toplevel_init(WebOSToplevel *)
{
}

// ---------------------------------------------------------------------------------------------------------
// WebOSView: hands rendered SHM buffers to the sink; WebKit waits for webos_view_frame_shown before the next.

struct _WebOSView {
    WPEView parent_instance;
    WPEBuffer *shown;    // the frame on screen now (ref), released when the next one is shown
};

G_DEFINE_FINAL_TYPE(WebOSView, webos_view, WPE_TYPE_VIEW)

static gboolean webos_view_render_buffer(WPEView *view, WPEBuffer *buffer, const WPERectangle *damage, guint n_damage, GError **error)
{
    if (!WPE_IS_BUFFER_SHM(buffer)) {
        g_set_error_literal(error, WPE_VIEW_ERROR, WPE_VIEW_ERROR_RENDER_FAILED, "only shared-memory buffers are supported");
        return FALSE;
    }
    auto *display = WEBOS_DISPLAY(wpe_view_get_display(view));
    if (!display->sink.frame) {
        g_set_error_literal(error, WPE_VIEW_ERROR, WPE_VIEW_ERROR_RENDER_FAILED, "no frame sink");
        return FALSE;
    }
    display->sink.frame(display->sink.user, view, buffer, damage, n_damage);
    return TRUE;
}

static gboolean webos_view_can_be_mapped(WPEView *)
{
    return TRUE;
}

// Map as soon as there is a toplevel with a size (same as WPE's headless view).
static void webos_view_toplevel_changed(WPEView *view, GParamSpec *, gpointer)
{
    WPEToplevel *toplevel = wpe_view_get_toplevel(view);
    if (!toplevel) {
        wpe_view_unmap(view);
        return;
    }
    int width = 0;
    int height = 0;
    wpe_toplevel_get_size(toplevel, &width, &height);
    if (width && height)
        wpe_view_resized(view, width, height);
    wpe_view_map(view);
}

static void webos_view_constructed(GObject *object)
{
    G_OBJECT_CLASS(webos_view_parent_class)->constructed(object);
    g_signal_connect(object, "notify::toplevel", G_CALLBACK(webos_view_toplevel_changed), nullptr);
}

static void webos_view_dispose(GObject *object)
{
    g_clear_object(&WEBOS_VIEW(object)->shown);
    G_OBJECT_CLASS(webos_view_parent_class)->dispose(object);
}

static void webos_view_class_init(WebOSViewClass *klass)
{
    GObjectClass *object = G_OBJECT_CLASS(klass);
    object->constructed = webos_view_constructed;
    object->dispose = webos_view_dispose;
    WPEViewClass *view = WPE_VIEW_CLASS(klass);
    view->render_buffer = webos_view_render_buffer;
    view->can_be_mapped = webos_view_can_be_mapped;
}

static void webos_view_init(WebOSView *)
{
}

void webos_view_frame_shown(WebOSView *view, WPEBuffer *buffer)
{
    g_return_if_fail(WEBOS_IS_VIEW(view));
    // Same order as WPE's headless platform: release the previous frame, then report this one rendered.
    if (view->shown && view->shown != buffer)
        wpe_view_buffer_released(WPE_VIEW(view), view->shown);
    g_set_object(&view->shown, buffer);
    wpe_view_buffer_rendered(WPE_VIEW(view), buffer);
}

// ---------------------------------------------------------------------------------------------------------
// WebOSDisplay

static gboolean webos_display_connect(WPEDisplay *, GError **)
{
    return TRUE;
}

static WPEView *webos_display_create_view(WPEDisplay *display)
{
    auto *view = WPE_VIEW(g_object_new(WEBOS_TYPE_VIEW, "display", display, nullptr));
    if (wpe_settings_get_boolean(wpe_display_get_settings(display), WPE_SETTING_CREATE_VIEWS_WITH_A_TOPLEVEL, nullptr)) {
        auto *toplevel = WPE_TOPLEVEL(g_object_new(WEBOS_TYPE_TOPLEVEL, "display", display, nullptr));
        wpe_view_set_toplevel(view, toplevel);
        g_object_unref(toplevel);
    }
    return view;
}

static gpointer webos_display_get_egl_display(WPEDisplay *, GError **error)
{
    // Deliberately none: the UI process owns the TV's GPU. Without an EGL display here, WebKit's web
    // process composites with its own (bundled Mesa, surfaceless) EGL and sends shared-memory buffers.
    g_set_error_literal(error, WPE_EGL_ERROR, WPE_EGL_ERROR_NOT_AVAILABLE, "the webOS engine process has no EGL display");
    return nullptr;
}

static WPEInputMethodContext *webos_display_create_input_method_context(WPEDisplay *, WPEView *view)
{
    // The context finds its display through the view (WPEInputMethodContext has no "display" property).
    return WPE_INPUT_METHOD_CONTEXT(g_object_new(WEBOS_TYPE_IME, "view", view, nullptr));
}

static void webos_display_class_init(WebOSDisplayClass *klass)
{
    WPEDisplayClass *display = WPE_DISPLAY_CLASS(klass);
    display->connect = webos_display_connect;
    display->create_view = webos_display_create_view;
    display->get_egl_display = webos_display_get_egl_display;
    display->create_input_method_context = webos_display_create_input_method_context;
}

static void webos_display_init(WebOSDisplay *)
{
}

WPEDisplay *webos_display_new(const WebOSSink *sink)
{
    auto *display = WEBOS_DISPLAY(g_object_new(WEBOS_TYPE_DISPLAY, nullptr));
    display->sink = *sink;
    return WPE_DISPLAY(display);
}

void webos_display_commit_text(WebOSDisplay *display, const char *utf8)
{
    g_return_if_fail(WEBOS_IS_DISPLAY(display));
    if (!display->focused_ime || !utf8 || !*utf8)
        return;
    g_signal_emit_by_name(display->focused_ime, "committed", utf8);
}
