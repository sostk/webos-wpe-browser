// SPDX-License-Identifier: GPL-3.0-or-later
#include "direct.h"

#include "../webos-backend/protocol.h"

#include <wpe/wpe.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <dlfcn.h>
#include <string>

void log_msg(const char *fmt, ...) G_GNUC_PRINTF(1, 2);

namespace direct {

namespace {

constexpr const char *kLibrary = "libWPEBackend-webos.so";

struct wpeb_webos_view;
using StatusCallback = void (*)(void *, int32_t, uint32_t, uint32_t);

struct Api {
    wpeb_webos_view *(*view_new)(uint32_t, uint32_t);
    wpe_view_backend *(*view_get_backend)(wpeb_webos_view *);
    void (*view_set_status_callback)(wpeb_webos_view *, StatusCallback, void *);
    void (*view_attach)(wpeb_webos_view *, const char *, const char *);
    void (*view_detach)(wpeb_webos_view *);
};

Api api;
std::string group_name, layer_name;
StatusFn status_fn;

} // namespace

struct View {
    uint32_t tab = 0;
    wpeb_webos_view *backend_view = nullptr;
    wpe_view_backend *backend = nullptr;
    double scale = 1.0;
    bool shown = false;
    bool fullscreen = false;
};

namespace {

void on_status(void *user, int32_t status, uint32_t width, uint32_t height)
{
    auto *view = static_cast<View *>(user);
    if (status < 0 || status == WPEB_WEBOS_STATUS_FIRST_FRAME)
        log_msg("tab %u direct surface: status %d (%ux%u)", view->tab, status, width, height);
    if (status_fn)
        status_fn(view->tab, status, width, height);
}

// The page asked for fullscreen (or to leave it): the page surface already covers the screen, so this only
// confirms. Without a handler libwpe refuses, and WebKit rejects every requestFullscreen() (TV, 2026-11-11).
// libwpe wants the confirmation after the handler has returned.
gboolean confirm_fullscreen(gpointer data)
{
    auto *view = static_cast<View *>(data);
    if (view->fullscreen)
        wpe_view_backend_dispatch_did_enter_fullscreen(view->backend);
    else
        wpe_view_backend_dispatch_did_exit_fullscreen(view->backend);
    return G_SOURCE_REMOVE;
}

bool on_fullscreen(void *data, bool fullscreen)
{
    auto *view = static_cast<View *>(data);
    view->fullscreen = fullscreen;
    g_idle_add(confirm_fullscreen, view);
    return true;
}

void destroy_view(gpointer data)
{
    auto *view = static_cast<View *>(data);
    // Frees the wpeb_webos_view too (it is the view backend's implementation).
    wpe_view_backend_destroy(view->backend);
    delete view;
}

template<typename T> bool lookup(void *lib, const char *name, T &out)
{
    out = reinterpret_cast<T>(dlsym(lib, name));
    if (!out)
        log_msg("direct: %s has no %s", kLibrary, name);
    return out != nullptr;
}

uint32_t to_wpe_modifiers(uint32_t m)
{
    // WPEModifiers keyboard bits (0..3) are the same in libwpe; pointer buttons move from bit 8 to bit 20.
    uint32_t out = m & 0xF;
    for (int i = 0; i < 5; ++i) {
        if (m & (1u << (8 + i)))
            out |= 1u << (20 + i);
    }
    return out;
}

} // namespace

bool init(const char *group, const char *layer, StatusFn status)
{
    void *lib = dlopen(kLibrary, RTLD_NOW | RTLD_GLOBAL);
    if (!lib) {
        log_msg("direct: %s", dlerror());
        return false;
    }
    if (!lookup(lib, "wpeb_webos_view_new", api.view_new) || !lookup(lib, "wpeb_webos_view_get_backend", api.view_get_backend)
        || !lookup(lib, "wpeb_webos_view_set_status_callback", api.view_set_status_callback)
        || !lookup(lib, "wpeb_webos_view_attach", api.view_attach) || !lookup(lib, "wpeb_webos_view_detach", api.view_detach))
        return false;
    // The web processes get the library name from WebKit (WebProcessCreationParameters.implementationLibraryName).
    if (!wpe_loader_init(kLibrary)) {
        log_msg("direct: wpe_loader_init(%s) failed (another libwpe backend already loaded?)", kLibrary);
        return false;
    }
    group_name = group;
    layer_name = layer;
    status_fn = status;
    log_msg("direct presentation: surface group '%s', layer '%s'", group, layer);
    return true;
}

WebKitWebViewBackend *create_backend(uint32_t tab, uint32_t width, uint32_t height, View **out)
{
    auto *view = new View;
    view->tab = tab;
    view->backend_view = api.view_new(width, height);
    if (!view->backend_view) {
        delete view;
        return nullptr;
    }
    view->backend = api.view_get_backend(view->backend_view);
    api.view_set_status_callback(view->backend_view, on_status, view);
    wpe_view_backend_set_fullscreen_handler(view->backend, on_fullscreen, view);
    // Hidden until the tab is shown: not visible, so WebKit does not render it.
    wpe_view_backend_add_activity_state(view->backend, wpe_view_activity_state_in_window);
    *out = view;
    return webkit_web_view_backend_new(view->backend, destroy_view, view);
}

void set_size(View *view, uint32_t width, uint32_t height)
{
    wpe_view_backend_dispatch_set_size(view->backend, width, height);
}

void set_scale(View *view, double scale)
{
    if (view->scale == scale)
        return;
    view->scale = scale;
    wpe_view_backend_dispatch_set_device_scale_factor(view->backend, static_cast<float>(scale));
}

void set_shown(View *view, bool shown)
{
    if (view->shown == shown)
        return;
    view->shown = shown;
    if (shown) {
        // Attach first: the frame WebKit renders on becoming visible then goes to the attached surface.
        api.view_attach(view->backend_view, group_name.c_str(), layer_name.c_str());
        wpe_view_backend_add_activity_state(view->backend, wpe_view_activity_state_visible);
    } else {
        wpe_view_backend_remove_activity_state(view->backend, wpe_view_activity_state_visible);
        api.view_detach(view->backend_view);
    }
}

void set_layer(const char *layer)
{
    layer_name = layer;
    log_msg("direct presentation: pages attach to layer '%s' now", layer);
}

void reattach(View *view)
{
    if (view && view->shown)
        api.view_attach(view->backend_view, group_name.c_str(), layer_name.c_str());
}

void set_focused(View *view, bool focused)
{
    if (focused)
        wpe_view_backend_add_activity_state(view->backend, wpe_view_activity_state_focused);
    else
        wpe_view_backend_remove_activity_state(view->backend, wpe_view_activity_state_focused);
}

void key(View *view, const WpeKey &k)
{
    wpe_input_keyboard_event event {};
    event.time = static_cast<uint32_t>(g_get_monotonic_time() / 1000);
    event.key_code = k.keyval;
    event.hardware_key_code = k.keycode;
    event.pressed = k.down != 0;
    event.modifiers = to_wpe_modifiers(k.modifiers);
    wpe_view_backend_dispatch_keyboard_event(view->backend, &event);
}

// libwpe positions are in device pixels (WebKit divides by the scale); ours are CSS pixels.
void pointer(View *view, const WpePointer &p, uint32_t &buttons)
{
    wpe_input_pointer_event event {};
    event.time = p.time_ms ? p.time_ms : static_cast<uint32_t>(g_get_monotonic_time() / 1000);
    event.x = static_cast<int>(std::lround(p.x * view->scale));
    event.y = static_cast<int>(std::lround(p.y * view->scale));
    switch (p.kind) {
    case WPE_POINTER_MOVE:
        event.type = wpe_input_pointer_event_type_motion;
        break;
    case WPE_POINTER_DOWN:
    case WPE_POINTER_UP: {
        const bool down = p.kind == WPE_POINTER_DOWN;
        const uint32_t bit = p.button >= 1 && p.button <= 5 ? 1u << (8 + p.button - 1) : 0;
        buttons = down ? (buttons | bit) : (buttons & ~bit);
        event.type = wpe_input_pointer_event_type_button;
        event.button = p.button;
        event.state = down ? 1 : 0;
        break;
    }
    case WPE_POINTER_LEAVE:
    default:
        return; // libwpe has no leave event
    }
    event.modifiers = to_wpe_modifiers(buttons);
    wpe_view_backend_dispatch_pointer_event(view->backend, &event);
}

void scroll(View *view, const WpeScroll &s, uint32_t buttons)
{
    // libwpe's discrete wheel event is one notch (WebKit uses only the sign); positive value = content moves
    // down, the opposite of our delta. axis 0 vertical, 1 horizontal.
    const struct { uint32_t axis; int32_t delta; } axes[] = { { 0, s.delta_y }, { 1, s.delta_x } };
    for (const auto &a : axes) {
        const int notches = std::clamp(std::abs(a.delta) / 120, a.delta ? 1 : 0, 20);
        for (int i = 0; i < notches; ++i) {
            wpe_input_axis_event event {};
            event.type = wpe_input_axis_event_type_motion;
            event.time = static_cast<uint32_t>(g_get_monotonic_time() / 1000);
            event.x = static_cast<int>(std::lround(s.x * view->scale));
            event.y = static_cast<int>(std::lround(s.y * view->scale));
            event.axis = a.axis;
            event.value = a.delta > 0 ? -1 : 1;
            event.modifiers = to_wpe_modifiers(buttons);
            wpe_view_backend_dispatch_axis_event(view->backend, &event);
        }
    }
}

// ---- input method ---------------------------------------------------------------------------------------------

} // namespace direct

struct DirectIme {
    WebKitInputMethodContext parent;
    uint32_t tab;
    direct::ImeFn fn;
    int x, y, width, height;
};
struct DirectImeClass {
    WebKitInputMethodContextClass parent_class;
};

G_DEFINE_TYPE(DirectIme, direct_ime, WEBKIT_TYPE_INPUT_METHOD_CONTEXT)

namespace {

uint32_t wpe_purpose(WebKitInputPurpose purpose)
{
    // WPEInputPurpose has ALPHA and NAME in between; same order otherwise.
    switch (purpose) {
    case WEBKIT_INPUT_PURPOSE_DIGITS: return 2;
    case WEBKIT_INPUT_PURPOSE_NUMBER: return 3;
    case WEBKIT_INPUT_PURPOSE_PHONE: return 4;
    case WEBKIT_INPUT_PURPOSE_URL: return 5;
    case WEBKIT_INPUT_PURPOSE_EMAIL: return 6;
    case WEBKIT_INPUT_PURPOSE_PASSWORD: return 8;
    case WEBKIT_INPUT_PURPOSE_FREE_FORM:
    default: return 0;
    }
}

void ime_notify(DirectIme *ime, bool focused)
{
    if (ime->fn)
        ime->fn(ime->tab, focused, wpe_purpose(webkit_input_method_context_get_input_purpose(&ime->parent)),
            ime->x, ime->y, ime->width, ime->height);
}

} // namespace

static void direct_ime_init(DirectIme *) { }

static void direct_ime_class_init(DirectImeClass *klass)
{
    auto *ime = WEBKIT_INPUT_METHOD_CONTEXT_CLASS(klass);
    ime->notify_focus_in = [](WebKitInputMethodContext *context) {
        ime_notify(G_TYPE_CHECK_INSTANCE_CAST(context, direct_ime_get_type(), DirectIme), true);
    };
    ime->notify_focus_out = [](WebKitInputMethodContext *context) {
        ime_notify(G_TYPE_CHECK_INSTANCE_CAST(context, direct_ime_get_type(), DirectIme), false);
    };
    ime->notify_cursor_area = [](WebKitInputMethodContext *context, int x, int y, int width, int height) {
        auto *self = G_TYPE_CHECK_INSTANCE_CAST(context, direct_ime_get_type(), DirectIme);
        self->x = x;
        self->y = y;
        self->width = width;
        self->height = height;
    };
    ime->filter_key_event = [](WebKitInputMethodContext *, gpointer) -> gboolean { return FALSE; };
    ime->get_preedit = [](WebKitInputMethodContext *, gchar **text, GList **underlines, guint *cursor_offset) {
        if (text)
            *text = g_strdup("");
        if (underlines)
            *underlines = nullptr;
        if (cursor_offset)
            *cursor_offset = 0;
    };
}

namespace direct {

WebKitInputMethodContext *create_ime(uint32_t tab, ImeFn fn)
{
    auto *ime = static_cast<DirectIme *>(g_object_new(direct_ime_get_type(), nullptr));
    ime->tab = tab;
    ime->fn = fn;
    return &ime->parent;
}

void commit_text(WebKitInputMethodContext *ime, const char *text)
{
    if (ime && text && *text)
        g_signal_emit_by_name(ime, "committed", text);
}

} // namespace direct
