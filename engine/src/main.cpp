// SPDX-License-Identifier: GPL-3.0-or-later
// wpe-engine: runs WPE WebKit for the webOS browser UI app, which spawns it with a SOCK_SEQPACKET socket on
// fd 3 (common/wpe_ipc.h). Each browser tab is one WebKitWebView; all views share one web process (they are
// created as related views) to keep memory down on a TV. Only the active tab is visible and renders. Two ways
// to the screen, chosen at start-up:
//   SHM (default): pages render into shared-memory buffers (platform.cpp), and each frame is copied into a
//                  memfd slot the UI maps and draws with the TV's GPU.
//   direct:        the web process draws with the TV's EGL into its own surface in the UI's webOS surface
//                  group (direct.cpp, engine/webos-backend); no frames pass through here.
//
// Environment (set by the UI app):
//   WPE_BROWSER_DATA_DIR   cookies, local storage (persistent)
//   WPE_BROWSER_CACHE_DIR  HTTP cache
//   WEBKIT_EXEC_PATH       where WPEWebProcess / WPENetworkProcess are (patched to work in release builds)
//   LD_LIBRARY_PATH, GIO_MODULE_DIR, FONTCONFIG_FILE, ... for the bundled libraries
//   WPE_BROWSER_PRESENT    "direct" for direct presentation, with WPEB_SURFACE_GROUP (the UI's surface group),
//                          WPEB_SURFACE_LAYER (default "page") and WPEB_WAYLAND_SOCKET (the compositor's socket)
#include "../../common/wpe_ipc.h"
#include "direct.h"
#include "platform.h"

#include <wpe/webkit.h>

#include <glib-unix.h>

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <map>
#include <memory>
#include <string>

// Also used by direct.cpp.
void log_msg(const char *fmt, ...) G_GNUC_PRINTF(1, 2);
void log_msg(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "wpe-engine: ");
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
}

namespace {

struct Tab {
    uint32_t id = 0;
    WebKitWebView *web_view = nullptr;
    WPEView *view = nullptr;                  // SHM mode
    direct::View *direct = nullptr;           // direct mode (owned by the web view's backend)
    WebKitInputMethodContext *ime = nullptr;  // direct mode (owned by the web view)
};

struct Engine {
    int sock = WPE_IPC_ENGINE_FD;
    GMainLoop *loop = nullptr;
    WPEDisplay *display = nullptr;  // SHM mode only
    bool direct = false;            // WPE_BROWSER_PRESENT=direct and the backend loaded (direct.cpp)
    WebKitNetworkSession *session = nullptr;
    WebKitSettings *settings = nullptr;
    std::map<uint32_t, Tab> tabs;
    uint32_t active = 0;
    uint32_t width = 1280, height = 720;
    double zoom = 1.0;
    double scale = 1.0;      // device scale: buffers are CSS size x scale (sharp text on a 1080p screen)

    // Frame slots in a memfd shared with the UI.
    int frames_fd = -1;
    uint8_t *frames = nullptr;
    uint32_t slot_bytes = 0;
    uint32_t next_slot = 0;
    uint32_t serial = 0;
    WPEBuffer *in_flight[WPE_IPC_FRAME_SLOTS] = {};
    WPEView *in_flight_view[WPE_IPC_FRAME_SLOTS] = {};

    // Pointer state for modifiers and press counts.
    uint32_t buttons = 0;
    double pointer_x = 0, pointer_y = 0;
};

Engine g;


guint32 now_ms()
{
    return static_cast<guint32>(g_get_monotonic_time() / 1000);
}

Tab *tab_by_id(uint32_t id)
{
    auto it = g.tabs.find(id);
    return it == g.tabs.end() ? nullptr : &it->second;
}

uint32_t tab_of_web_view(WebKitWebView *web_view)
{
    for (auto &[id, tab] : g.tabs) {
        if (tab.web_view == web_view)
            return id;
    }
    return 0;
}

uint32_t tab_of_view(WPEView *view)
{
    for (auto &[id, tab] : g.tabs) {
        if (tab.view == view)
            return id;
    }
    return 0;
}

Tab *active_tab()
{
    return tab_by_id(g.active);
}

// ---- sending ---------------------------------------------------------------------------------------------

bool send_msg(uint32_t type, uint32_t tab, const void *payload, uint32_t size, int fd = -1)
{
    uint8_t packet[WPE_IPC_MAX_PACKET];
    if (sizeof(WpeMsgHeader) + size > sizeof(packet))
        return false;
    WpeMsgHeader header { type, size, tab };
    memcpy(packet, &header, sizeof(header));
    if (size)
        memcpy(packet + sizeof(header), payload, size);

    iovec iov { packet, sizeof(header) + size };
    msghdr msg {};
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    alignas(cmsghdr) char control[CMSG_SPACE(sizeof(int))];
    if (fd >= 0) {
        msg.msg_control = control;
        msg.msg_controllen = sizeof(control);
        cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
        cmsg->cmsg_level = SOL_SOCKET;
        cmsg->cmsg_type = SCM_RIGHTS;
        cmsg->cmsg_len = CMSG_LEN(sizeof(int));
        memcpy(CMSG_DATA(cmsg), &fd, sizeof(int));
    }
    for (;;) {
        ssize_t n = sendmsg(g.sock, &msg, MSG_NOSIGNAL);
        if (n >= 0)
            return true;
        if (errno == EINTR)
            continue;
        log_msg("send failed: %s", strerror(errno));
        return false;
    }
}

void send_text(uint32_t type, uint32_t tab, const char *text)
{
    WpeText t {};
    const size_t len = text ? std::min<size_t>(strlen(text), WPE_IPC_MAX_TEXT - 1) : 0;
    t.length = static_cast<uint32_t>(len);
    if (len)
        memcpy(t.text, text, len);
    t.text[len] = '\0';
    send_msg(type, tab, &t, static_cast<uint32_t>(offsetof(WpeText, text) + len + 1));
}

void send_error(uint32_t tab, uint32_t kind, uint32_t code, const char *message)
{
    WpeError e {};
    e.kind = kind;
    e.code = code;
    const size_t len = message ? std::min<size_t>(strlen(message), WPE_IPC_MAX_TEXT - 1) : 0;
    e.message.length = static_cast<uint32_t>(len);
    if (len)
        memcpy(e.message.text, message, len);
    send_msg(WPE_ENGINE_ERROR, tab, &e, static_cast<uint32_t>(offsetof(WpeError, message) + offsetof(WpeText, text) + len + 1));
}

void send_history(uint32_t tab, WebKitWebView *web_view)
{
    WpeHistory h { webkit_web_view_can_go_back(web_view) ? 1u : 0u, webkit_web_view_can_go_forward(web_view) ? 1u : 0u };
    send_msg(WPE_ENGINE_HISTORY, tab, &h, sizeof(h));
}

// ---- frames ----------------------------------------------------------------------------------------------

bool ensure_frames(uint32_t needed)
{
    if (g.frames && needed <= g.slot_bytes)
        return true;
    for (auto *b : g.in_flight) {
        if (b)
            return false; // cannot remap under a frame the UI is still reading; the caller drops this one
    }
    const uint32_t slot = (needed + 0xFFFFFu) & ~0xFFFFFu;
    if (g.frames) {
        munmap(g.frames, static_cast<size_t>(g.slot_bytes) * WPE_IPC_FRAME_SLOTS);
        g.frames = nullptr;
    }
    if (g.frames_fd >= 0)
        close(g.frames_fd);
    g.frames_fd = memfd_create("wpe-browser-frames", MFD_CLOEXEC);
    if (g.frames_fd < 0) {
        log_msg("memfd_create: %s", strerror(errno));
        return false;
    }
    const size_t total = static_cast<size_t>(slot) * WPE_IPC_FRAME_SLOTS;
    if (ftruncate(g.frames_fd, static_cast<off_t>(total)) < 0) {
        log_msg("ftruncate: %s", strerror(errno));
        return false;
    }
    void *map = mmap(nullptr, total, PROT_READ | PROT_WRITE, MAP_SHARED, g.frames_fd, 0);
    if (map == MAP_FAILED) {
        log_msg("mmap: %s", strerror(errno));
        return false;
    }
    g.frames = static_cast<uint8_t *>(map);
    g.slot_bytes = slot;
    g.next_slot = 0;
    WpeFrames f { slot, WPE_IPC_FRAME_SLOTS };
    return send_msg(WPE_ENGINE_FRAMES, 0, &f, sizeof(f), g.frames_fd);
}

void on_frame(void *, WPEView *view, WPEBuffer *buffer, const WPERectangle *damage, guint n_damage)
{
    const uint32_t tab = tab_of_view(view);
    WPEBufferSHM *shm = WPE_BUFFER_SHM(buffer);
    GBytes *bytes = wpe_buffer_shm_get_data(shm);
    gsize size = 0;
    const auto *pixels = static_cast<const uint8_t *>(g_bytes_get_data(bytes, &size));
    const uint32_t width = static_cast<uint32_t>(wpe_buffer_get_width(buffer));
    const uint32_t height = static_cast<uint32_t>(wpe_buffer_get_height(buffer));
    const uint32_t stride = wpe_buffer_shm_get_stride(shm);
    const uint32_t needed = stride * height;
    // A hidden tab's frame (e.g. rendered just before a switch) is not shown: hand it straight back.
    if (tab != g.active || !pixels || size < needed || !ensure_frames(needed)) {
        webos_view_frame_shown(WEBOS_VIEW(view), buffer);
        return;
    }

    uint32_t slot = g.next_slot;
    for (uint32_t i = 0; i < WPE_IPC_FRAME_SLOTS && g.in_flight[slot]; ++i)
        slot = (slot + 1) % WPE_IPC_FRAME_SLOTS;
    if (g.in_flight[slot]) {
        webos_view_frame_shown(WEBOS_VIEW(view), buffer);
        return;
    }
    g.next_slot = (slot + 1) % WPE_IPC_FRAME_SLOTS;
    memcpy(g.frames + static_cast<size_t>(slot) * g.slot_bytes, pixels, needed);

    WpeFrame f {};
    f.slot = slot;
    f.serial = ++g.serial;
    f.width = width;
    f.height = height;
    f.stride = stride;
    if (n_damage > 0 && n_damage <= WPE_IPC_MAX_DAMAGE) {
        f.n_damage = n_damage;
        for (guint i = 0; i < n_damage; ++i)
            f.damage[i] = WpeRect { damage[i].x, damage[i].y, damage[i].width, damage[i].height };
    } else if (n_damage > WPE_IPC_MAX_DAMAGE) {
        int x0 = damage[0].x, y0 = damage[0].y, x1 = x0 + damage[0].width, y1 = y0 + damage[0].height;
        for (guint i = 1; i < n_damage; ++i) {
            x0 = std::min(x0, damage[i].x);
            y0 = std::min(y0, damage[i].y);
            x1 = std::max(x1, damage[i].x + damage[i].width);
            y1 = std::max(y1, damage[i].y + damage[i].height);
        }
        f.n_damage = 1;
        f.damage[0] = WpeRect { x0, y0, x1 - x0, y1 - y0 };
    }
    g.in_flight[slot] = WPE_BUFFER(g_object_ref(buffer));
    g.in_flight_view[slot] = WPE_VIEW(g_object_ref(view));
    if (!send_msg(WPE_ENGINE_FRAME, tab, &f, sizeof(f))) {
        g_clear_object(&g.in_flight[slot]);
        g_clear_object(&g.in_flight_view[slot]);
        webos_view_frame_shown(WEBOS_VIEW(view), buffer);
    }
}

void frame_done(const WpeFrameDone &d)
{
    if (d.slot >= WPE_IPC_FRAME_SLOTS || !g.in_flight[d.slot])
        return;
    WPEBuffer *buffer = g.in_flight[d.slot];
    WPEView *view = g.in_flight_view[d.slot];
    g.in_flight[d.slot] = nullptr;
    g.in_flight_view[d.slot] = nullptr;
    webos_view_frame_shown(WEBOS_VIEW(view), buffer);
    g_object_unref(buffer);
    g_object_unref(view);
}

void on_ime_focus(void *, WPEView *view, gboolean focused, WPEInputPurpose purpose, int x, int y, int width, int height)
{
    WpeIme m { focused ? 1u : 0u, static_cast<uint32_t>(purpose), x, y, width, height };
    send_msg(WPE_ENGINE_IME, tab_of_view(view), &m, sizeof(m));
}

// ---- input (always to the active tab) --------------------------------------------------------------------

void dispatch(WPEEvent *event)
{
    if (event)
        wpe_view_event(wpe_event_get_view(event), event);
    if (event)
        wpe_event_unref(event);
}

WPEModifiers button_modifier(uint32_t button)
{
    switch (button) {
    case 1: return WPE_MODIFIER_POINTER_BUTTON1;
    case 2: return WPE_MODIFIER_POINTER_BUTTON2;
    case 3: return WPE_MODIFIER_POINTER_BUTTON3;
    default: return static_cast<WPEModifiers>(0);
    }
}

void handle_key(WPEView *view, const WpeKey &k)
{
    dispatch(wpe_event_keyboard_new(k.down ? WPE_EVENT_KEYBOARD_KEY_DOWN : WPE_EVENT_KEYBOARD_KEY_UP, view,
        WPE_INPUT_SOURCE_KEYBOARD, now_ms(), static_cast<WPEModifiers>(k.modifiers), k.keycode, k.keyval));
}

void handle_pointer(WPEView *view, const WpePointer &p)
{
    const double x = p.x, y = p.y;
    const guint32 t = p.time_ms ? p.time_ms : now_ms();
    switch (p.kind) {
    case WPE_POINTER_MOVE: {
        const double dx = x - g.pointer_x, dy = y - g.pointer_y;
        g.pointer_x = x;
        g.pointer_y = y;
        dispatch(wpe_event_pointer_move_new(WPE_EVENT_POINTER_MOVE, view, WPE_INPUT_SOURCE_MOUSE, t,
            static_cast<WPEModifiers>(g.buttons), x, y, dx, dy));
        break;
    }
    case WPE_POINTER_DOWN:
    case WPE_POINTER_UP: {
        const bool down = p.kind == WPE_POINTER_DOWN;
        g.pointer_x = x;
        g.pointer_y = y;
        const WPEModifiers bit = button_modifier(p.button);
        if (down)
            g.buttons |= bit;
        else
            g.buttons &= ~static_cast<uint32_t>(bit);
        const guint count = down ? wpe_view_compute_press_count(view, x, y, p.button, t) : 0;
        dispatch(wpe_event_pointer_button_new(down ? WPE_EVENT_POINTER_DOWN : WPE_EVENT_POINTER_UP, view,
            WPE_INPUT_SOURCE_MOUSE, t, static_cast<WPEModifiers>(g.buttons), p.button, x, y, count));
        break;
    }
    case WPE_POINTER_LEAVE:
        dispatch(wpe_event_pointer_move_new(WPE_EVENT_POINTER_LEAVE, view, WPE_INPUT_SOURCE_MOUSE, t,
            static_cast<WPEModifiers>(g.buttons), g.pointer_x, g.pointer_y, 0, 0));
        break;
    }
}

void handle_scroll(WPEView *view, const WpeScroll &s)
{
    // Wheel notches, not pixels (precise_deltas = FALSE). WPE's sign is the opposite of ours: like its
    // Wayland platform, negative means "content moves up" (scroll down).
    dispatch(wpe_event_scroll_new(view, WPE_INPUT_SOURCE_MOUSE, now_ms(), static_cast<WPEModifiers>(g.buttons),
        -s.delta_x / 120.0, -s.delta_y / 120.0, FALSE, FALSE, s.x, s.y));
}

void resize_view(Tab &tab)
{
    if (tab.direct) {
        direct::set_scale(tab.direct, g.scale);
        direct::set_size(tab.direct, g.width, g.height);
        return;
    }
    WPEView *view = tab.view;
    if (WPEToplevel *toplevel = wpe_view_get_toplevel(view)) {
        wpe_toplevel_resize(toplevel, static_cast<int>(g.width), static_cast<int>(g.height));
        if (wpe_toplevel_get_scale(toplevel) != g.scale)
            wpe_toplevel_scale_changed(toplevel, g.scale);
    } else
        wpe_view_resized(view, static_cast<int>(g.width), static_cast<int>(g.height));
}

void resize(uint32_t width, uint32_t height)
{
    g.width = std::clamp<uint32_t>(width, 64, 3840);
    g.height = std::clamp<uint32_t>(height, 64, 2160);
    for (auto &[id, tab] : g.tabs)
        resize_view(tab);
}

// ---- WebKit signals (user_data = nothing; the tab is looked up from the view) ----------------------------

void on_uri(WebKitWebView *view, GParamSpec *, gpointer) { send_text(WPE_ENGINE_URI, tab_of_web_view(view), webkit_web_view_get_uri(view)); }
void on_title(WebKitWebView *view, GParamSpec *, gpointer) { send_text(WPE_ENGINE_TITLE, tab_of_web_view(view), webkit_web_view_get_title(view)); }

void on_progress(WebKitWebView *view, GParamSpec *, gpointer)
{
    WpeProgress p { static_cast<uint32_t>(std::clamp(webkit_web_view_get_estimated_load_progress(view), 0.0, 1.0) * 1000) };
    send_msg(WPE_ENGINE_PROGRESS, tab_of_web_view(view), &p, sizeof(p));
}

void on_load_changed(WebKitWebView *view, WebKitLoadEvent event, gpointer)
{
    WpeLoad l {};
    switch (event) {
    case WEBKIT_LOAD_STARTED: l.event = WPE_LOAD_STARTED; break;
    case WEBKIT_LOAD_COMMITTED: l.event = WPE_LOAD_COMMITTED; break;
    case WEBKIT_LOAD_FINISHED: l.event = WPE_LOAD_FINISHED; break;
    default: return;
    }
    const uint32_t tab = tab_of_web_view(view);
    send_msg(WPE_ENGINE_LOAD, tab, &l, sizeof(l));
    send_history(tab, view);

    // Page timings for the log (TV diagnostics: which site, how long to first commit / full load).
    static guint32 started = 0;
    const char *uri = webkit_web_view_get_uri(view);
    if (event == WEBKIT_LOAD_STARTED) {
        started = now_ms();
        log_msg("tab %u load started: %.200s", tab, uri ? uri : "");
    } else {
        log_msg("tab %u load %s after %u ms: %.200s", tab, event == WEBKIT_LOAD_COMMITTED ? "committed" : "finished",
            now_ms() - started, uri ? uri : "");
    }
}

gboolean on_load_failed(WebKitWebView *view, WebKitLoadEvent, gchar *uri, GError *error, gpointer)
{
    const uint32_t tab = tab_of_web_view(view);
    WpeLoad l { WPE_LOAD_FAILED };
    send_msg(WPE_ENGINE_LOAD, tab, &l, sizeof(l));
    if (!g_error_matches(error, WEBKIT_NETWORK_ERROR, WEBKIT_NETWORK_ERROR_CANCELLED)) {
        log_msg("load failed for %s: %s", uri, error->message);
        send_error(tab, WPE_ERROR_LOAD, static_cast<uint32_t>(error->code), error->message);
    }
    return FALSE; // let WebKit show its error page
}

gboolean on_tls_error(WebKitWebView *view, gchar *uri, GTlsCertificate *, GTlsCertificateFlags flags, gpointer)
{
    log_msg("TLS error for %s (flags 0x%x)", uri, flags);
    send_error(tab_of_web_view(view), WPE_ERROR_TLS, flags, uri);
    return FALSE;
}

void on_web_process_terminated(WebKitWebView *view, WebKitWebProcessTerminationReason reason, gpointer)
{
    const char *why = reason == WEBKIT_WEB_PROCESS_CRASHED ? "crashed"
        : reason == WEBKIT_WEB_PROCESS_EXCEEDED_MEMORY_LIMIT ? "used too much memory" : "was terminated";
    // With the time and page, so a crash can be matched to what was on screen (no core dumps: see main()).
    const char *uri = webkit_web_view_get_uri(view);
    GDateTime *now = g_date_time_new_now_utc();
    gchar *when = g_date_time_format(now, "%F %T UTC");
    log_msg("web process %s at %s on %s", why, when, uri ? uri : "(no page)");
    g_free(when);
    g_date_time_unref(now);
    send_error(tab_of_web_view(view), WPE_ERROR_WEB_PROCESS_CRASHED, static_cast<uint32_t>(reason), why);
}

// Whether the page is scrolled to its top, for the UI's toolbar (shown at the top, hidden once the page scrolls).
// The script runs in its own world, so pages can neither see nor fake it. Pages that scroll an element instead of
// the document (web apps) count too: scroll events are caught on the way down (capture) from any large scroller.
constexpr const char *kScrollWorld = "wpeb";
constexpr const char *kScrollScript = R"JS((function () {
    var last = null;
    function report(top) {
        if (top === last) return;
        last = top;
        window.webkit.messageHandlers.wpebScroll.postMessage(top);
    }
    function pageTop() {
        var e = document.scrollingElement || document.documentElement;
        return (window.scrollY || (e ? e.scrollTop : 0)) <= 2;
    }
    addEventListener('scroll', function (ev) {
        var t = ev.target;
        if (t === document || t === window || t === document.scrollingElement) { report(pageTop()); return; }
        if (t && t.clientHeight > innerHeight * 0.5) report(t.scrollTop <= 2 && pageTop());
    }, {capture: true, passive: true});
    // Back/forward puts a page back at its old position, after this script ran and without a scroll event. Look
    // again shortly after the page shows (always sent: the UI may have seen other pages meanwhile).
    function recheck() {
        last = null;
        report(pageTop());
        [150, 600, 1500].forEach(function (ms) { setTimeout(function () { last = null; report(pageTop()); }, ms); });
    }
    addEventListener('pageshow', recheck);
    recheck();
})();)JS";

// Page-load timing (Navigation Timing, after the load event), one log line per page: where the time goes (network:
// DNS, connect/TLS, server, transfer; then parsing, scripts and layout up to DOMContentLoaded / load). Milliseconds
// from the start of the navigation.
constexpr const char *kPerfScript = R"JS((function () {
    addEventListener('load', function () {
        setTimeout(function () {
            var n = performance.getEntriesByType('navigation')[0];
            if (!n) return;
            var res = performance.getEntriesByType('resource'), kb = 0;
            for (var i = 0; i < res.length; i++) kb += (res[i].transferSize || 0) / 1024;
            var fcp = 0, paints = performance.getEntriesByType('paint');
            for (var j = 0; j < paints.length; j++) if (paints[j].name === 'first-contentful-paint') fcp = paints[j].startTime;
            var r = function (x) { return Math.round(x); };
            window.webkit.messageHandlers.wpebPerf.postMessage(
                'dns ' + r(n.domainLookupEnd - n.domainLookupStart) +
                ' connect ' + r(n.connectEnd - n.connectStart) +
                ' (tls ' + r(n.secureConnectionStart > 0 ? n.connectEnd - n.secureConnectionStart : 0) + ')' +
                ' ttfb ' + r(n.responseStart - n.requestStart) +
                ' html ' + r(n.responseEnd - n.responseStart) +
                ' | interactive ' + r(n.domInteractive) + ' dcl ' + r(n.domContentLoadedEventEnd) +
                ' fcp ' + r(fcp) + ' load ' + r(n.loadEventStart) +
                ' | ' + res.length + ' resources ' + r(kb) + ' KB | ' + location.host);
        }, 0);
    });
})();)JS";

void on_perf_message(WebKitUserContentManager *, JSCValue *value, gpointer data)
{
    gchar *text = jsc_value_to_string(value);
    log_msg("tab %u timing: %s", GPOINTER_TO_UINT(data), text);
    g_free(text);
}

gboolean on_fullscreen_changed(WebKitWebView *view, gpointer entered)
{
    const uint32_t id = tab_of_web_view(view);
    WpeFlag flag = {GPOINTER_TO_UINT(entered)};
    log_msg("tab %u %s fullscreen", id, flag.value ? "entered" : "left");
    send_msg(WPE_ENGINE_FULLSCREEN, id, &flag, sizeof(flag));
    return FALSE; // let it happen
}

void on_scroll_message(WebKitUserContentManager *, JSCValue *value, gpointer data)
{
    const uint32_t id = GPOINTER_TO_UINT(data);
    WpeFlag flag = {jsc_value_to_boolean(value) ? 1u : 0u};
    if (getenv("WPE_BROWSER_DEBUG_SCROLL"))
        log_msg("tab %u scroll: %s", id, flag.value ? "top" : "scrolled");
    send_msg(WPE_ENGINE_SCROLL, id, &flag, sizeof(flag));
}

// Media limits. Video is decoded on the CPU (GStreamer libav/libvpx); the TV's 4 A55 cores keep up with H.264 at 720p30.
// Offered more, sites stream what they think the screen deserves: YouTube sent VP9 1080p60 for the 1920x1080 page,
// the decoder dropped most frames and the web process grew to ~800 MB until webOS (sam) killed it (TV, 2026-11-11).
// Sites ask MediaSource.isTypeSupported('...; width=; height=; framerate='), canPlayType and mediaCapabilities, so
// answering "no" beyond the limit makes them pick a stream we can play. Page world, every frame (embedded players).
// WPE_BROWSER_DEBUG_MEDIA="off" or "PIXELS@FPS[+vp9]" (e.g. "2073600@60+vp9") changes it.
constexpr const char *kMediaLimitScript = R"JS((function () {
  const cfg = __WPEB_MEDIA__;
  const num = (t, k) => { const m = t.match(new RegExp('[;\\s]' + k + '\\s*=\\s*"?([0-9.]+)')); return m ? parseFloat(m[1]) : 0; };
  const tooBig = (w, h, fps) => (w || 0) * (h || 0) > cfg.pixels || (fps || 0) > cfg.fps + 0.5;
  // Dolby (AC-3/E-AC-3): YouTube then sends 5.1 E-AC-3, decoded in software to 6 channels for a stereo
  // PulseAudio stream; the audio branch stalled and held the video 3 s ahead of it (TV, 2026-11-11). AAC/Opus play.
  const codecOk = (t) => !/av01|av1/.test(t) && !/ec-3|ac-3/.test(t) && (cfg.vp9 || !/^video\/.*(vp9|vp09)/.test(t));
  const ok = (type) => {
    if (typeof type !== 'string') return true;
    const t = type.toLowerCase();
    return codecOk(t) && !tooBig(num(t, 'width'), num(t, 'height'), num(t, 'framerate'));
  };
  for (const name of ['MediaSource', 'ManagedMediaSource', 'WebKitMediaSource']) {
    const MS = window[name];
    if (!MS || typeof MS.isTypeSupported !== 'function') continue;
    const original = MS.isTypeSupported.bind(MS);
    Object.defineProperty(MS, 'isTypeSupported', { value: function isTypeSupported(t) { return ok(t) && original(t); }, configurable: true, writable: true });
  }
  if (window.HTMLMediaElement) {
    const original = HTMLMediaElement.prototype.canPlayType;
    Object.defineProperty(HTMLMediaElement.prototype, 'canPlayType', { value: function canPlayType(t) { return ok(t) ? original.call(this, t) : ''; }, configurable: true, writable: true });
  }
  const mc = navigator.mediaCapabilities;
  if (mc && typeof mc.decodingInfo === 'function') {
    const original = mc.decodingInfo.bind(mc);
    mc.decodingInfo = function decodingInfo(config) {
      const v = config && config.video;
      if (v && (!ok(String(v.contentType || '')) || tooBig(v.width, v.height, v.framerate)))
        return Promise.resolve({ supported: false, smooth: false, powerEfficient: false, configuration: config });
      return original(config);
    };
  }
})();)JS";

std::string media_limit_script()
{
    unsigned long pixels = 1280 * 720, fps = 30;
    bool vp9 = false;
    // LG's decoder (hardware video, WPEB_MEDIA_SOCKET) plays 4K60 H.264/HEVC/VP9.
    if (getenv("WPEB_MEDIA_SOCKET")) {
        pixels = 3840 * 2160;
        fps = 60;
        vp9 = true;
    }
    if (const char *v = getenv("WPE_BROWSER_DEBUG_MEDIA")) {
        if (!strcmp(v, "off"))
            return {};
        char *end = nullptr;
        if (unsigned long p = strtoul(v, &end, 10)) pixels = p;
        if (end && *end == '@') fps = strtoul(end + 1, &end, 10);
        vp9 = strstr(v, "+vp9") != nullptr;
        log_msg("media limit: %lu pixels, %lu fps%s", pixels, fps, vp9 ? ", VP9 allowed" : "");
    }
    char cfg[128];
    snprintf(cfg, sizeof(cfg), "{pixels: %lu, fps: %lu, vp9: %s}", pixels, fps, vp9 ? "true" : "false");
    std::string script = kMediaLimitScript;
    const std::string marker = "__WPEB_MEDIA__";
    script.replace(script.find(marker), marker.size(), cfg);
    return script;
}

void add_media_limits(WebKitWebView *web_view)
{
    static const std::string source = media_limit_script();
    WebKitUserContentManager *ucm = webkit_web_view_get_user_content_manager(web_view);
    if (!ucm || source.empty())
        return;
    WebKitUserScript *script = webkit_user_script_new(source.c_str(), WEBKIT_USER_CONTENT_INJECT_ALL_FRAMES,
        WEBKIT_USER_SCRIPT_INJECT_AT_DOCUMENT_START, nullptr, nullptr);
    webkit_user_content_manager_add_script(ucm, script);
    webkit_user_script_unref(script);
}

void add_scroll_reporter(WebKitWebView *web_view, uint32_t id)
{
    add_media_limits(web_view);
    WebKitUserContentManager *ucm = webkit_web_view_get_user_content_manager(web_view);
    if (!ucm || !webkit_user_content_manager_register_script_message_handler(ucm, "wpebScroll", kScrollWorld))
        return;
    g_signal_connect(ucm, "script-message-received::wpebScroll", G_CALLBACK(on_scroll_message), GUINT_TO_POINTER(id));
    WebKitUserScript *script = webkit_user_script_new_for_world(kScrollScript, WEBKIT_USER_CONTENT_INJECT_TOP_FRAME,
        WEBKIT_USER_SCRIPT_INJECT_AT_DOCUMENT_END, kScrollWorld, nullptr, nullptr);
    webkit_user_content_manager_add_script(ucm, script);
    webkit_user_script_unref(script);
    if (webkit_user_content_manager_register_script_message_handler(ucm, "wpebPerf", kScrollWorld)) {
        g_signal_connect(ucm, "script-message-received::wpebPerf", G_CALLBACK(on_perf_message), GUINT_TO_POINTER(id));
        script = webkit_user_script_new_for_world(kPerfScript, WEBKIT_USER_CONTENT_INJECT_TOP_FRAME,
            WEBKIT_USER_SCRIPT_INJECT_AT_DOCUMENT_START, kScrollWorld, nullptr, nullptr);
        webkit_user_content_manager_add_script(ucm, script);
        webkit_user_script_unref(script);
    }
}

void on_history_changed(WebKitBackForwardList *, WebKitBackForwardListItem *, gpointer, gpointer web_view)
{
    auto *view = WEBKIT_WEB_VIEW(web_view);
    send_history(tab_of_web_view(view), view);
}

void tab_open(uint32_t id, const std::string &uri, WebKitWebView *opener = nullptr);

// window.close() from a page: the UI closes the tab (and shows its opener).
void on_close(WebKitWebView *view, gpointer)
{
    if (const uint32_t id = tab_of_web_view(view))
        send_msg(WPE_ENGINE_TAB_CLOSE, id, nullptr, 0);
}

// window.open and target=_blank: a real new window, as a new tab. Sign-in popups (Google, Apple, ...) need it: they
// talk to window.opener and close themselves. WebKit loads the request into the view returned here.
WebKitWebView *on_create(WebKitWebView *view, WebKitNavigationAction *, gpointer)
{
    static uint32_t next = WPE_ENGINE_TAB_ID_BASE;
    const uint32_t opener = tab_of_web_view(view);
    const uint32_t id = next++;
    tab_open(id, {}, view);
    Tab *tab = tab_by_id(id);
    if (!tab)
        return nullptr;
    WpeTabCreated c { opener };
    send_msg(WPE_ENGINE_TAB_CREATED, id, &c, sizeof(c));
    log_msg("tab %u opened by tab %u (window.open)", id, opener);
    return tab->web_view;  // still ours (WebKit only takes its page)
}

// ---- direct presentation (direct.cpp) -------------------------------------------------------------------

void send_present(uint32_t tab, int32_t status, uint32_t width, uint32_t height)
{
    WpePresent p { g.direct ? WPE_PRESENT_DIRECT : WPE_PRESENT_SHM, status, width, height };
    send_msg(WPE_ENGINE_PRESENT, tab, &p, sizeof(p));
}

void on_direct_status(uint32_t tab, int32_t status, uint32_t width, uint32_t height)
{
    send_present(tab, status, width, height);
}

void on_direct_ime(uint32_t tab, bool focused, uint32_t purpose, int x, int y, int width, int height)
{
    WpeIme m { focused ? 1u : 0u, purpose, x, y, width, height };
    send_msg(WPE_ENGINE_IME, tab, &m, sizeof(m));
}

void set_visible(Tab &tab, bool visible)
{
    if (tab.direct)
        direct::set_shown(tab.direct, visible);
    else
        wpe_view_set_visible(tab.view, visible);
}

void set_focused(Tab &tab, bool focused)
{
    if (tab.direct)
        direct::set_focused(tab.direct, focused);
    else if (focused)
        wpe_view_focus_in(tab.view);
    else
        wpe_view_focus_out(tab.view);
}

// ---- tabs ------------------------------------------------------------------------------------------------

// `opener`: a popup's opener (window.open): WebKit requires the new view to be related to it, and the page gets its
// window.opener. Otherwise related to the first tab.
void tab_open(uint32_t id, const std::string &uri, WebKitWebView *opener)
{
    if (!id || tab_by_id(id))
        return;
    // Related views share the first view's web process: one process for every tab saves a lot of memory
    // on a TV (a page crash then takes all tabs down, and the UI reloads them).
    WebKitWebView *related = opener;
    for (auto &[other, tab] : g.tabs) {
        if (related)
            break;
        related = tab.web_view;
    }
    Tab tab { id };
    WebKitWebView *web_view = nullptr;
    // Its own content manager: a related view would otherwise share the first tab's (scroll reports per tab).
    WebKitUserContentManager *ucm = webkit_user_content_manager_new();
    if (g.direct) {
        WebKitWebViewBackend *backend = direct::create_backend(id, g.width, g.height, &tab.direct);
        if (backend) {
            web_view = related
                ? WEBKIT_WEB_VIEW(g_object_new(WEBKIT_TYPE_WEB_VIEW, "backend", backend, "related-view", related,
                    "settings", g.settings, "user-content-manager", ucm, nullptr))
                : WEBKIT_WEB_VIEW(g_object_new(WEBKIT_TYPE_WEB_VIEW, "backend", backend, "network-session", g.session,
                    "settings", g.settings, "user-content-manager", ucm, nullptr));
            tab.ime = direct::create_ime(id, on_direct_ime);
            webkit_web_view_set_input_method_context(web_view, tab.ime);
            g_object_unref(tab.ime); // the web view keeps it
        }
    } else {
        web_view = related
            // The display must be passed to related views too: without it WebKit would use
            // wpe_display_get_default(), which creates a display of another platform instead of ours.
            ? WEBKIT_WEB_VIEW(g_object_new(WEBKIT_TYPE_WEB_VIEW, "related-view", related, "display", g.display,
                "settings", g.settings, "user-content-manager", ucm, nullptr))
            : WEBKIT_WEB_VIEW(g_object_new(WEBKIT_TYPE_WEB_VIEW, "display", g.display, "network-session", g.session,
                "settings", g.settings, "user-content-manager", ucm, nullptr));
        tab.view = webkit_web_view_get_wpe_view(web_view);
    }
    g_object_unref(ucm); // the view keeps its own reference
    if (!web_view || (!tab.view && !tab.direct)) {
        log_msg("WebKit did not create a view for tab %u", id);
        if (web_view)
            g_object_unref(web_view);
        send_error(id, WPE_ERROR_FATAL, 0, "could not create a view");
        return;
    }
    tab.web_view = web_view;
    g.tabs[id] = tab;

    g_signal_connect(web_view, "notify::uri", G_CALLBACK(on_uri), nullptr);
    g_signal_connect(web_view, "notify::title", G_CALLBACK(on_title), nullptr);
    g_signal_connect(web_view, "notify::estimated-load-progress", G_CALLBACK(on_progress), nullptr);
    g_signal_connect(web_view, "load-changed", G_CALLBACK(on_load_changed), nullptr);
    g_signal_connect(web_view, "load-failed", G_CALLBACK(on_load_failed), nullptr);
    g_signal_connect(web_view, "load-failed-with-tls-errors", G_CALLBACK(on_tls_error), nullptr);
    g_signal_connect(web_view, "web-process-terminated", G_CALLBACK(on_web_process_terminated), nullptr);
    g_signal_connect(web_view, "create", G_CALLBACK(on_create), nullptr);
    g_signal_connect(web_view, "close", G_CALLBACK(on_close), nullptr);
    g_signal_connect(web_view, "enter-fullscreen", G_CALLBACK(on_fullscreen_changed), GUINT_TO_POINTER(1));
    g_signal_connect(web_view, "leave-fullscreen", G_CALLBACK(on_fullscreen_changed), GUINT_TO_POINTER(0));
    g_signal_connect(webkit_web_view_get_back_forward_list(web_view), "changed", G_CALLBACK(on_history_changed), web_view);
    add_scroll_reporter(web_view, id);

    Tab &t = g.tabs[id];
    resize_view(t);
    webkit_web_view_set_zoom_level(web_view, g.zoom);
    const bool active = id == g.active;
    set_visible(t, active);
    if (active)
        set_focused(t, true);
    if (!uri.empty())
        webkit_web_view_load_uri(web_view, uri.c_str());
    log_msg("tab %u opened (%zu open)", id, g.tabs.size());
}

void tab_close(uint32_t id)
{
    auto it = g.tabs.find(id);
    if (it == g.tabs.end())
        return;
    // Frames of this view still in flight keep their own references (released on FRAME_DONE).
    g_signal_handlers_disconnect_by_data(webkit_web_view_get_back_forward_list(it->second.web_view), it->second.web_view);
    webkit_web_view_try_close(it->second.web_view);
    g_object_unref(it->second.web_view);
    g.tabs.erase(it);
    if (g.active == id)
        g.active = 0;
    log_msg("tab %u closed (%zu open)", id, g.tabs.size());
}

void tab_activate(uint32_t id)
{
    if (g.active == id)
        return;
    if (Tab *old = active_tab()) {
        set_focused(*old, false);
        set_visible(*old, false);
    }
    g.active = id;
    if (Tab *tab = active_tab()) {
        set_visible(*tab, true);
        set_focused(*tab, true);
        send_history(id, tab->web_view);
    }
}

// ---- website data and zoom ---------------------------------------------------------------------------------

void clear_data(const WpeClearData &c)
{
    unsigned types = 0;
    if (c.what & WPE_CLEAR_COOKIES_AND_SITE_DATA)
        types |= WEBKIT_WEBSITE_DATA_COOKIES | WEBKIT_WEBSITE_DATA_SESSION_STORAGE | WEBKIT_WEBSITE_DATA_LOCAL_STORAGE
            | WEBKIT_WEBSITE_DATA_INDEXEDDB_DATABASES | WEBKIT_WEBSITE_DATA_SERVICE_WORKER_REGISTRATIONS
            | WEBKIT_WEBSITE_DATA_DOM_CACHE | WEBKIT_WEBSITE_DATA_HSTS_CACHE | WEBKIT_WEBSITE_DATA_OFFLINE_APPLICATION_CACHE;
    if (c.what & WPE_CLEAR_CACHE)
        types |= WEBKIT_WEBSITE_DATA_MEMORY_CACHE | WEBKIT_WEBSITE_DATA_DISK_CACHE;
    if (!types) {
        WpeDataCleared done { c.what, 1 };
        send_msg(WPE_ENGINE_DATA_CLEARED, 0, &done, sizeof(done));
        return;
    }
    auto *what = new uint32_t(c.what);
    // timespan: GTimeSpan in microseconds; 0 = everything.
    webkit_website_data_manager_clear(webkit_network_session_get_website_data_manager(g.session),
        static_cast<WebKitWebsiteDataTypes>(types), static_cast<GTimeSpan>(c.seconds) * G_TIME_SPAN_SECOND, nullptr,
        [](GObject *source, GAsyncResult *result, gpointer data) {
            std::unique_ptr<uint32_t> what(static_cast<uint32_t *>(data));
            GError *error = nullptr;
            const gboolean ok = webkit_website_data_manager_clear_finish(WEBKIT_WEBSITE_DATA_MANAGER(source), result, &error);
            if (!ok) {
                log_msg("clearing website data failed: %s", error ? error->message : "?");
                g_clear_error(&error);
            }
            WpeDataCleared done { *what, ok ? 1u : 0u };
            send_msg(WPE_ENGINE_DATA_CLEARED, 0, &done, sizeof(done));
        },
        what);
}

void set_zoom(uint32_t permille)
{
    g.zoom = std::clamp(permille, 250u, 5000u) / 1000.0;
    for (auto &[id, tab] : g.tabs)
        webkit_web_view_set_zoom_level(tab.web_view, g.zoom);
}

void set_scale(uint32_t permille)
{
    g.scale = std::clamp(permille, 1000u, 3000u) / 1000.0;
    for (auto &[id, tab] : g.tabs)
        resize_view(tab);
    log_msg("device scale %.2f", g.scale);
}

// ---- receiving -------------------------------------------------------------------------------------------

template<typename T> const T *payload(const uint8_t *buf, ssize_t n, const WpeMsgHeader &h, size_t min = sizeof(T))
{
    if (h.size < min || static_cast<size_t>(n) < sizeof(WpeMsgHeader) + min)
        return nullptr;
    return reinterpret_cast<const T *>(buf + sizeof(WpeMsgHeader));
}

std::string text_of(const uint8_t *buf, ssize_t n, const WpeMsgHeader &h)
{
    if (h.size < offsetof(WpeText, text) || static_cast<size_t>(n) < sizeof(WpeMsgHeader) + offsetof(WpeText, text))
        return {};
    const auto *t = reinterpret_cast<const WpeText *>(buf + sizeof(WpeMsgHeader));
    const size_t avail = static_cast<size_t>(n) - sizeof(WpeMsgHeader) - offsetof(WpeText, text);
    return std::string(t->text, std::min<size_t>({ t->length, avail, WPE_IPC_MAX_TEXT - 1 }));
}

void handle_message(const uint8_t *buf, ssize_t n)
{
    WpeMsgHeader h;
    memcpy(&h, buf, sizeof(h));
    Tab *tab = tab_by_id(h.tab);      // navigation target
    Tab *active = active_tab();       // input target
    switch (h.type) {
    case WPE_APP_TAB_OPEN: tab_open(h.tab, text_of(buf, n, h)); break;
    case WPE_APP_TAB_CLOSE: tab_close(h.tab); break;
    case WPE_APP_TAB_ACTIVATE: tab_activate(h.tab); break;
    case WPE_APP_LOAD_URI: {
        const std::string uri = text_of(buf, n, h);
        if (tab && !uri.empty())
            webkit_web_view_load_uri(tab->web_view, uri.c_str());
        break;
    }
    case WPE_APP_GO_BACK: if (tab) webkit_web_view_go_back(tab->web_view); break;
    case WPE_APP_GO_FORWARD: if (tab) webkit_web_view_go_forward(tab->web_view); break;
    case WPE_APP_RELOAD: if (tab) webkit_web_view_reload(tab->web_view); break;
    case WPE_APP_STOP: if (tab) webkit_web_view_stop_loading(tab->web_view); break;
    case WPE_APP_RUN_JS: {
        const std::string script = text_of(buf, n, h);
        if (tab)
            webkit_web_view_evaluate_javascript(tab->web_view, script.c_str(), static_cast<gssize>(script.size()),
                nullptr, nullptr, nullptr, nullptr, nullptr);
        break;
    }
    case WPE_APP_RESIZE:
        if (auto *s = payload<WpeSize>(buf, n, h))
            resize(s->width, s->height);
        break;
    case WPE_APP_VISIBLE:
        if (auto *f = payload<WpeFlag>(buf, n, h); f && active)
            set_visible(*active, f->value != 0);
        break;
    case WPE_APP_FOCUS:
        if (auto *f = payload<WpeFlag>(buf, n, h); f && active)
            set_focused(*active, f->value != 0);
        break;
    case WPE_APP_KEY:
        if (auto *k = payload<WpeKey>(buf, n, h); k && active) {
            if (active->direct)
                direct::key(active->direct, *k);
            else
                handle_key(active->view, *k);
        }
        break;
    case WPE_APP_POINTER:
        if (auto *p = payload<WpePointer>(buf, n, h); p && active) {
            if (active->direct)
                direct::pointer(active->direct, *p, g.buttons);
            else
                handle_pointer(active->view, *p);
        }
        break;
    case WPE_APP_SCROLL:
        if (auto *s = payload<WpeScroll>(buf, n, h); s && active) {
            if (active->direct)
                direct::scroll(active->direct, *s, g.buttons);
            else
                handle_scroll(active->view, *s);
        }
        break;
    case WPE_APP_TEXT_COMMIT: {
        const std::string text = text_of(buf, n, h);
        if (g.direct) {
            if (active)
                direct::commit_text(active->ime, text.c_str());
        } else
            webos_display_commit_text(WEBOS_DISPLAY(g.display), text.c_str());
        break;
    }
    case WPE_APP_FRAME_DONE:
        if (auto *d = payload<WpeFrameDone>(buf, n, h))
            frame_done(*d);
        break;
    case WPE_APP_CLEAR_DATA:
        if (auto *c = payload<WpeClearData>(buf, n, h))
            clear_data(*c);
        break;
    case WPE_APP_SET_ZOOM:
        if (auto *z = payload<WpeZoom>(buf, n, h))
            set_zoom(z->permille);
        break;
    case WPE_APP_SET_SCALE:
        if (auto *z = payload<WpeZoom>(buf, n, h))
            set_scale(z->permille);
        break;
    case WPE_APP_SET_LAYER: {
        const std::string layer = text_of(buf, n, h);
        if (g.direct && !layer.empty()) {
            direct::set_layer(layer.c_str());
            for (auto &[id, t] : g.tabs)
                direct::reattach(t.direct);
        }
        break;
    }
    case WPE_APP_QUIT:
        g_main_loop_quit(g.loop);
        break;
    default:
        log_msg("ignoring message type %u", h.type);
    }
}

gboolean on_socket(gint fd, GIOCondition condition, gpointer)
{
    if (condition & (G_IO_HUP | G_IO_ERR)) {
        log_msg("UI closed the connection");
        g_main_loop_quit(g.loop);
        return G_SOURCE_REMOVE;
    }
    alignas(WpeMsgHeader) uint8_t buf[WPE_IPC_MAX_PACKET];
    for (;;) {
        ssize_t n = recv(fd, buf, sizeof(buf), MSG_DONTWAIT);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                return G_SOURCE_CONTINUE;
            log_msg("recv: %s", strerror(errno));
            g_main_loop_quit(g.loop);
            return G_SOURCE_REMOVE;
        }
        if (n == 0) {
            g_main_loop_quit(g.loop);
            return G_SOURCE_REMOVE;
        }
        if (static_cast<size_t>(n) >= sizeof(WpeMsgHeader))
            handle_message(buf, n);
    }
}

std::string env_or(const char *name, const char *fallback)
{
    const char *v = g_getenv(name);
    return v && *v ? v : fallback;
}

} // namespace

int main(int, char **)
{
    signal(SIGPIPE, SIG_IGN);
    // No core dumps from the engine or its WebKit processes (inherited): the kernel writes them into the app's own
    // folder on the TV's small app partition (8 dumps, 1.4 GB, filled it on 2026-11-10). WPE_BROWSER_CORE_DUMPS=1
    // keeps them for debugging.
    if (!getenv("WPE_BROWSER_CORE_DUMPS")) {
        const struct rlimit none = {0, 0};
        setrlimit(RLIMIT_CORE, &none);
    }
    if (fcntl(g.sock, F_GETFD) < 0) {
        fprintf(stderr, "wpe-engine: fd %d is not open; this program is started by the browser app\n", g.sock);
        return 2;
    }

    // The UI speaks first: protocol version and the page size.
    alignas(WpeMsgHeader) uint8_t buf[WPE_IPC_MAX_PACKET];
    ssize_t n;
    do {
        n = recv(g.sock, buf, sizeof(buf), 0);
    } while (n < 0 && errno == EINTR);
    WpeMsgHeader h {};
    if (n >= static_cast<ssize_t>(sizeof(h)))
        memcpy(&h, buf, sizeof(h));
    const auto *hello = h.type == WPE_APP_HELLO ? payload<WpeAppHello>(buf, n, h) : nullptr;
    if (!hello || hello->version != WPE_IPC_VERSION) {
        log_msg("bad or missing hello (type %u)", h.type);
        return 3;
    }
    g.width = hello->width;
    g.height = hello->height;

    g.loop = g_main_loop_new(nullptr, FALSE);

    // Direct presentation (direct.cpp) or SHM frames (platform.cpp). WebKit allows one per process: once a
    // WPEDisplay exists, views with a libwpe backend are refused, so this is decided before anything else.
    if (env_or("WPE_BROWSER_PRESENT", "shm") == "direct") {
        g.direct = !env_or("WPEB_SURFACE_GROUP", "").empty() && direct::init(env_or("WPEB_SURFACE_GROUP", "").c_str(), env_or("WPEB_SURFACE_LAYER", "page").c_str(),
            on_direct_status);
        if (!g.direct)
            log_msg("direct presentation unavailable; using SHM frames");
        // A view keeps its web process across sites (WebKit patch 0006): a new process would need the view's layer,
        // and the webOS compositor hides the app when it moves from one process's surface to another's (TV,
        // 2026-11-11, YouTube's COOP swap). All tabs already share one web process here.
        if (g.direct)
            g_setenv("WPE_BROWSER_NO_PROCESS_SWAP", "1", TRUE);
    }

    GError *error = nullptr;
    if (!g.direct) {
        WebOSSink sink { on_frame, on_ime_focus, nullptr };
        g.display = webos_display_new(&sink);
        if (!wpe_display_connect(g.display, &error)) {
            log_msg("display: %s", error->message);
            return 4;
        }
        wpe_display_set_primary(g.display);
    }

    // Grayscale text antialiasing: LCD subpixel AA (WPE's default, RGB) shows as coloured, soft glyph edges
    // on a TV panel, more so once the page is scaled to the screen. 0xFF is not a WPE layout; WebKit
    // patch 0003 reads it as "none". (Direct mode: fontconfig's rgba setting in the bundled fonts.conf.)
    if (WPESettings *settings = g.display ? wpe_display_get_settings(g.display) : nullptr) {
        if (!wpe_settings_set_byte(settings, WPE_SETTING_FONT_SUBPIXEL_LAYOUT, 0xFF, WPE_SETTINGS_SOURCE_PLATFORM, &error)) {
            log_msg("font settings: %s", error->message);
            g_clear_error(&error);
        }
    }

    const std::string data_dir = env_or("WPE_BROWSER_DATA_DIR", "/tmp/wpe-browser/data");
    const std::string cache_dir = env_or("WPE_BROWSER_CACHE_DIR", "/tmp/wpe-browser/cache");
    g_mkdir_with_parents(data_dir.c_str(), 0700);
    g_mkdir_with_parents(cache_dir.c_str(), 0700);
    g.session = webkit_network_session_new(data_dir.c_str(), cache_dir.c_str());
    webkit_cookie_manager_set_persistent_storage(webkit_network_session_get_cookie_manager(g.session),
        (data_dir + "/cookies.sqlite").c_str(), WEBKIT_COOKIE_PERSISTENT_STORAGE_SQLITE);

    g.settings = webkit_settings_new();
    webkit_settings_set_enable_spatial_navigation(g.settings, TRUE);
    // No "webOS" in the user agent: sites' mobile checks list it (Palm's phones), e.g. webtv.nettv.com.np's
    // /iPhone|...|webOS|.../i showed its mobile page (2026-11-11). The rest is WPE's desktop Linux agent.
    webkit_settings_set_user_agent_with_application_details(g.settings, "WPEBrowser", "0.2");
    webkit_settings_set_enable_developer_extras(g.settings, FALSE);
    // EME (navigator.requestMediaKeySystemAccess) is off by default on WPE. Widevine comes from libocdm, which
    // drives the TV's own CDM (drm/ocdm-widevine); ClearKey is WebKit's own.
    webkit_settings_set_enable_encrypted_media(g.settings, TRUE);
    // Experiments: WPE_BROWSER_DEBUG_SETTINGS="enable-javascript=false;enable-page-cache=0" sets WebKitSettings properties
    // (booleans, integers, strings), e.g. from the launch parameter {"wpebEnv": {"WPE_BROWSER_DEBUG_SETTINGS": "..."}}.
    if (const char *debug = getenv("WPE_BROWSER_DEBUG_SETTINGS")) {
        gchar **pairs = g_strsplit(debug, ";", -1);
        for (gchar **p = pairs; *p; ++p) {
            gchar **kv = g_strsplit(*p, "=", 2);
            GParamSpec *spec = kv[0] && kv[1] ? g_object_class_find_property(G_OBJECT_GET_CLASS(g.settings), kv[0]) : nullptr;
            if (!spec) {
                if (**p) log_msg("debug setting %s: unknown", *p);
            } else if (spec->value_type == G_TYPE_BOOLEAN) {
                g_object_set(g.settings, kv[0], (gboolean)(g_strcmp0(kv[1], "true") == 0 || g_strcmp0(kv[1], "1") == 0), nullptr);
                log_msg("debug setting %s=%s", kv[0], kv[1]);
            } else if (spec->value_type == G_TYPE_UINT || spec->value_type == G_TYPE_INT) {
                GValue v = G_VALUE_INIT;
                g_value_init(&v, spec->value_type);
                if (spec->value_type == G_TYPE_UINT) g_value_set_uint(&v, (guint)atoi(kv[1])); else g_value_set_int(&v, atoi(kv[1]));
                g_object_set_property(G_OBJECT(g.settings), kv[0], &v);
                g_value_unset(&v);
                log_msg("debug setting %s=%s", kv[0], kv[1]);
            } else if (spec->value_type == G_TYPE_STRING) {
                g_object_set(g.settings, kv[0], kv[1], nullptr);
                log_msg("debug setting %s=%s", kv[0], kv[1]);
            }
            g_strfreev(kv);
        }
        g_strfreev(pairs);
    }
    if (g.direct) {
        // COOP (YouTube sends it) moves a page into a new web process by its own path, which ignores
        // WPE_BROWSER_NO_PROCESS_SWAP; see there for why direct presentation can't follow a process switch.
        WebKitFeatureList *features = webkit_settings_get_all_features();
        for (gsize i = 0; i < webkit_feature_list_get_length(features); ++i) {
            WebKitFeature *f = webkit_feature_list_get(features, i);
            if (g_strcmp0(webkit_feature_get_identifier(f), "CrossOriginOpenerPolicy") == 0) {
                webkit_settings_set_feature_enabled(g.settings, f, FALSE);
                log_msg("COOP off (direct presentation keeps pages in their web process)");
            }
        }
        webkit_feature_list_unref(features);
    }

    g_unix_fd_add(g.sock, static_cast<GIOCondition>(G_IO_IN | G_IO_HUP | G_IO_ERR), on_socket, nullptr);

    WpeEngineReady ready { WPE_IPC_VERSION, webkit_get_major_version(), webkit_get_minor_version(), webkit_get_micro_version() };
    send_msg(WPE_ENGINE_READY, 0, &ready, sizeof(ready));
    send_present(0, 0, 0, 0);
    log_msg("ready: WPE WebKit %u.%u.%u, page %ux%u", ready.webkit_major, ready.webkit_minor, ready.webkit_micro,
        g.width, g.height);

    g_main_loop_run(g.loop);

    for (uint32_t i = 0; i < WPE_IPC_FRAME_SLOTS; ++i) {
        g_clear_object(&g.in_flight[i]);
        g_clear_object(&g.in_flight_view[i]);
    }
    while (!g.tabs.empty())
        tab_close(g.tabs.begin()->first);
    g_object_unref(g.settings);
    g_object_unref(g.session);
    g_clear_object(&g.display);
    g_main_loop_unref(g.loop);
    return 0;
}
