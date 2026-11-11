// SPDX-License-Identifier: GPL-3.0-or-later
// libWPEBackend-webos: see wpebackend-webos.h. Two halves in one library, loaded by libwpe in both processes:
//   engine (WebKit UI process): renderer host + view backend (one per tab, wpeb_webos_view_*)
//   WPEWebProcess:              renderer backend (the Wayland connection) + EGL target (one per page)
// Built with the webOS NDK against the TV's own libwayland 1.11 and webOS protocols (tools/build-webos-backend.sh).
#define _GNU_SOURCE
#include "protocol.h"
#include "wpebackend-webos.h"

#include <dlfcn.h>
#include <errno.h>
#include <glib-unix.h>
#include <glib.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <wayland-client.h>
#include <wayland-egl.h>
#include <wpe/wpe-egl.h>
#include <wpe/wpe.h>

#include "wayland-webos-shell-client-protocol.h"
#include "webos-surface-group-client-protocol.h"
#include "webos-foreign-client-protocol.h"
#include <pthread.h>
#include <time.h>

#define LOG(...) do { fprintf(stderr, "[wpebackend-webos %d] ", (int)getpid()); fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } while (0)

// No frame callback within this time (surface detached or hidden by the compositor): let WebKit go on anyway,
// so its compositor thread never stalls for good.
#define FRAME_TIMEOUT_MS 250

static void copy_name(char *dst, const char *src)
{
    g_strlcpy(dst, src ? src : "", WPEB_WEBOS_NAME_MAX);
}

static bool send_msg(int fd, const struct wpeb_webos_msg *msg)
{
    if (fd < 0)
        return false;
    ssize_t n;
    do
        n = send(fd, msg, sizeof(*msg), MSG_NOSIGNAL | MSG_DONTWAIT);
    while (n < 0 && errno == EINTR);
    return n == (ssize_t)sizeof(*msg);
}

static bool make_pair(int fds[2])
{
    return socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, fds) == 0;
}

// ===== engine side ============================================================================================

// libwpe asks for a renderer host before it creates a web process; the web process half ignores it (it talks to
// the compositor directly), so this is only a closed socket end.
static void *host_create(void) { return GINT_TO_POINTER(1); }
static void host_destroy(void *data) { (void)data; }
static int host_create_client(void *data)
{
    (void)data;
    int fds[2];
    if (!make_pair(fds))
        return -1;
    close(fds[0]);
    return fds[1];
}

static struct wpe_renderer_host_interface host_interface = {
    .create = host_create,
    .destroy = host_destroy,
    .create_client = host_create_client,
};

struct wpeb_webos_view {
    struct wpe_view_backend *backend;
    uint32_t width, height;
    int fd; // our end of the channel to the current web process' target
    GSource *source;
    bool attached;
    // Handoff: WebKit moved the view to another web process (cross-site navigation) while the previous one still
    // holds the layer; the compositor refuses a second surface on it ("Layer already attached", a protocol error).
    // The old target is told to detach, and the new one attaches once it confirms (or after kHandoffMs).
    int old_fd;
    GSource *old_source;
    guint handoff_timer;
    char group[WPEB_WEBOS_NAME_MAX], layer[WPEB_WEBOS_NAME_MAX];
    wpeb_webos_status_fn status_fn;
    void *status_user;
};

static void view_close_channel(struct wpeb_webos_view *v)
{
    if (v->source) {
        g_source_destroy(v->source);
        g_source_unref(v->source);
        v->source = NULL;
    }
    if (v->fd >= 0)
        close(v->fd);
    v->fd = -1;
}

static void send_state_to(const struct wpeb_webos_view *v, int fd, bool attach)
{
    struct wpeb_webos_msg msg = { .type = attach ? WPEB_WEBOS_ATTACH : WPEB_WEBOS_DETACH };
    copy_name(msg.group, v->group);
    copy_name(msg.layer, v->layer);
    send_msg(fd, &msg);
}

static void view_send_state(struct wpeb_webos_view *v)
{
    // During a handoff the new target waits (detached) until the old one has let go of the layer.
    send_state_to(v, v->fd, v->attached && v->old_fd < 0);
}

enum { kHandoffMs = 500 };

static void view_finish_handoff(struct wpeb_webos_view *v, const char *why)
{
    if (v->handoff_timer) {
        g_source_remove(v->handoff_timer);
        v->handoff_timer = 0;
    }
    if (v->old_source) {
        g_source_destroy(v->old_source);
        g_source_unref(v->old_source);
        v->old_source = NULL;
    }
    if (v->old_fd >= 0) {
        close(v->old_fd);
        v->old_fd = -1;
        LOG("layer handed to the new web process (%s)", why);
    }
    view_send_state(v);
}

static gboolean view_on_old_channel(gint fd, GIOCondition condition, gpointer data)
{
    struct wpeb_webos_view *v = data;
    bool done = (condition & (G_IO_HUP | G_IO_ERR)) != 0;
    if (condition & G_IO_IN) {
        struct wpeb_webos_msg msg;
        ssize_t n;
        while ((n = recv(fd, &msg, sizeof(msg), MSG_DONTWAIT)) == (ssize_t)sizeof(msg)) {
            if (msg.type == WPEB_WEBOS_STATUS && (msg.code == WPEB_WEBOS_STATUS_DETACHED || msg.code < 0))
                done = true; // detached (or it failed: then it holds nothing either)
        }
        if (n == 0 || (n < 0 && errno != EAGAIN && errno != EINTR))
            done = true;
    }
    if (!done)
        return G_SOURCE_CONTINUE;
    // view_finish_handoff destroys this source; keep it alive until we return.
    g_source_ref(v->old_source);
    GSource *self = v->old_source;
    view_finish_handoff(v, "old one detached");
    g_source_unref(self);
    return G_SOURCE_REMOVE;
}

static gboolean view_on_handoff_timeout(gpointer data)
{
    struct wpeb_webos_view *v = data;
    v->handoff_timer = 0;
    view_finish_handoff(v, "timeout");
    return G_SOURCE_REMOVE;
}

static gboolean view_on_channel(gint fd, GIOCondition condition, gpointer data)
{
    struct wpeb_webos_view *v = data;
    if (condition & G_IO_IN) {
        struct wpeb_webos_msg msg;
        ssize_t n;
        while ((n = recv(fd, &msg, sizeof(msg), MSG_DONTWAIT)) == (ssize_t)sizeof(msg)) {
            if (msg.type == WPEB_WEBOS_STATUS && v->status_fn)
                v->status_fn(v->status_user, msg.code, msg.width, msg.height);
        }
        if (n == 0 || (n < 0 && errno != EAGAIN && errno != EINTR))
            condition |= G_IO_HUP;
    }
    if (condition & (G_IO_HUP | G_IO_ERR)) {
        // The web process went away; WebKit asks for a new fd when it starts another one.
        g_source_unref(v->source);
        v->source = NULL;
        close(v->fd);
        v->fd = -1;
        return G_SOURCE_REMOVE;
    }
    return G_SOURCE_CONTINUE;
}

static void *view_create(void *params, struct wpe_view_backend *backend)
{
    struct wpeb_webos_view *v = params;
    v->backend = backend;
    return v;
}

static void view_destroy(void *data)
{
    struct wpeb_webos_view *v = data;
    if (v->handoff_timer)
        g_source_remove(v->handoff_timer);
    if (v->old_source) {
        g_source_destroy(v->old_source);
        g_source_unref(v->old_source);
    }
    if (v->old_fd >= 0)
        close(v->old_fd);
    view_close_channel(v);
    g_free(v);
}

static void view_initialize(void *data)
{
    struct wpeb_webos_view *v = data;
    wpe_view_backend_dispatch_set_size(v->backend, v->width, v->height);
}

static int view_get_renderer_host_fd(void *data)
{
    struct wpeb_webos_view *v = data;
    int fds[2];
    if (!make_pair(fds)) {
        LOG("socketpair failed: %s", strerror(errno));
        return -1;
    }
    if (v->fd >= 0 && v->attached && v->old_fd < 0) {
        // The current target holds the layer: keep its channel to tell it to detach, and wait for it.
        v->old_fd = v->fd;
        if (v->source) {
            g_source_destroy(v->source);
            g_source_unref(v->source);
            v->source = NULL;
        }
        v->fd = -1;
        v->old_source = g_unix_fd_source_new(v->old_fd, G_IO_IN | G_IO_HUP | G_IO_ERR);
        g_source_set_callback(v->old_source, (GSourceFunc)(void (*)(void))view_on_old_channel, v, NULL);
        g_source_attach(v->old_source, NULL);
        send_state_to(v, v->old_fd, false);
        v->handoff_timer = g_timeout_add(kHandoffMs, view_on_handoff_timeout, v);
        LOG("new web process for this view: asking the previous one to detach");
    } else {
        view_close_channel(v);
    }
    v->fd = fds[0];
    v->source = g_unix_fd_source_new(v->fd, G_IO_IN | G_IO_HUP | G_IO_ERR);
    g_source_set_callback(v->source, (GSourceFunc)(void (*)(void))view_on_channel, v, NULL);
    g_source_attach(v->source, NULL);
    view_send_state(v); // queued until the target reads it
    // WebKit sends this end to the web process and closes its own copy.
    return fds[1];
}

static struct wpe_view_backend_interface view_interface = {
    .create = view_create,
    .destroy = view_destroy,
    .initialize = view_initialize,
    .get_renderer_host_fd = view_get_renderer_host_fd,
};

__attribute__((visibility("default"))) struct wpeb_webos_view *wpeb_webos_view_new(uint32_t width, uint32_t height)
{
    struct wpeb_webos_view *v = g_new0(struct wpeb_webos_view, 1);
    v->fd = -1;
    v->old_fd = -1;
    v->width = width ? width : 1;
    v->height = height ? height : 1;
    if (!wpe_view_backend_create_with_backend_interface(&view_interface, v)) {
        g_free(v);
        return NULL;
    }
    return v;
}

__attribute__((visibility("default"))) struct wpe_view_backend *wpeb_webos_view_get_backend(struct wpeb_webos_view *v)
{
    return v->backend;
}

__attribute__((visibility("default"))) void wpeb_webos_view_set_status_callback(struct wpeb_webos_view *v,
    wpeb_webos_status_fn fn, void *user)
{
    v->status_fn = fn;
    v->status_user = user;
}

__attribute__((visibility("default"))) void wpeb_webos_view_attach(struct wpeb_webos_view *v, const char *group,
    const char *layer)
{
    v->attached = true;
    copy_name(v->group, group);
    copy_name(v->layer, layer);
    view_send_state(v);
}

__attribute__((visibility("default"))) void wpeb_webos_view_detach(struct wpeb_webos_view *v)
{
    if (!v->attached)
        return;
    v->attached = false;
    view_send_state(v);
}

// ===== web process side =======================================================================================

struct renderer {
    struct wl_display *display;
    struct wl_event_queue *queue; // registry + globals; never dispatched after create()
    struct wl_compositor *compositor;
    struct wl_shell *shell;
    struct wl_webos_shell *webos_shell;
    struct wl_webos_surface_group_compositor *groups;
    struct wl_seat *seat;
    struct wl_webos_foreign *foreign;
    // Page surfaces of closed pages, unmapped, with their window role (see target_pool_surface).
    pthread_mutex_t pool_lock;
    GPtrArray *pool; // struct pooled_surface *
};

struct pooled_surface {
    struct wl_surface *surface;
    struct wl_shell_surface *shell_surface;
    struct wl_webos_shell_surface *webos_surface;
};

static void registry_global(void *data, struct wl_registry *registry, uint32_t name, const char *interface, uint32_t version)
{
    struct renderer *r = data;
    (void)version;
    if (!strcmp(interface, "wl_compositor"))
        r->compositor = wl_registry_bind(registry, name, &wl_compositor_interface, 1);
    else if (!strcmp(interface, "wl_shell"))
        r->shell = wl_registry_bind(registry, name, &wl_shell_interface, 1);
    else if (!strcmp(interface, "wl_webos_shell"))
        r->webos_shell = wl_registry_bind(registry, name, &wl_webos_shell_interface, 1);
    else if (!strcmp(interface, "wl_webos_surface_group_compositor"))
        r->groups = wl_registry_bind(registry, name, &wl_webos_surface_group_compositor_interface, 1);
    else if (!strcmp(interface, "wl_webos_foreign") && !r->foreign)
        r->foreign = wl_registry_bind(registry, name, &wl_webos_foreign_interface, 1);
    else if (!strcmp(interface, "wl_seat") && !r->seat)
        r->seat = wl_registry_bind(registry, name, &wl_seat_interface, 1);
}
static void registry_global_remove(void *data, struct wl_registry *registry, uint32_t name) { (void)data; (void)registry; (void)name; }
static const struct wl_registry_listener registry_listener = { registry_global, registry_global_remove };

// WPEB_WAYLAND_SOCKET: absolute path of the UI's compositor socket. The engine's XDG_RUNTIME_DIR is a private
// folder, so wl_display_connect(NULL) would look in the wrong place.
static struct wl_display *connect_display(void)
{
    const char *path = getenv("WPEB_WAYLAND_SOCKET");
    if (!path || !*path)
        return wl_display_connect(NULL);
    struct sockaddr_un addr = { .sun_family = AF_UNIX };
    if (strlen(path) >= sizeof(addr.sun_path)) {
        LOG("WPEB_WAYLAND_SOCKET too long: %s", path);
        return NULL;
    }
    strcpy(addr.sun_path, path);
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0)
        return NULL;
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        LOG("connect %s: %s", path, strerror(errno));
        close(fd);
        return NULL;
    }
    struct wl_display *display = wl_display_connect_to_fd(fd); // owns fd from now on
    if (!display)
        close(fd);
    return display;
}

static void *renderer_create(int host_fd)
{
    if (host_fd >= 0)
        close(host_fd);
    struct renderer *r = g_new0(struct renderer, 1);
    pthread_mutex_init(&r->pool_lock, NULL);
    r->pool = g_ptr_array_new();
    r->display = connect_display();
    if (!r->display) {
        LOG("cannot connect to the compositor (WPEB_WAYLAND_SOCKET=%s)", getenv("WPEB_WAYLAND_SOCKET"));
        return r; // targets report WPEB_WEBOS_STATUS_NO_WAYLAND
    }
    r->queue = wl_display_create_queue(r->display);
    struct wl_registry *registry = wl_display_get_registry(r->display);
    wl_proxy_set_queue((struct wl_proxy *)registry, r->queue);
    wl_registry_add_listener(registry, &registry_listener, r);
    wl_display_roundtrip_queue(r->display, r->queue);
    wl_registry_destroy(registry);
    LOG("connected: compositor=%d shell=%d webos shell=%d surface groups=%d", !!r->compositor, !!r->shell,
        !!r->webos_shell, !!r->groups);
    return r;
}

static void renderer_destroy(void *data)
{
    struct renderer *r = data;
    if (r->seat)
        wl_seat_destroy(r->seat);
    if (r->foreign)
        wl_webos_foreign_destroy(r->foreign);
    if (r->groups)
        wl_proxy_destroy((struct wl_proxy *)r->groups);
    if (r->webos_shell)
        wl_proxy_destroy((struct wl_proxy *)r->webos_shell);
    if (r->shell)
        wl_shell_destroy(r->shell);
    if (r->compositor)
        wl_compositor_destroy(r->compositor);
    if (r->queue)
        wl_event_queue_destroy(r->queue);
    if (r->display)
        wl_display_disconnect(r->display);
    g_free(r);
}

static EGLNativeDisplayType renderer_get_native_display(void *data)
{
    return (EGLNativeDisplayType)((struct renderer *)data)->display;
}

// 0: WebKit uses eglGetDisplay(wl_display), which Mali recognises; Mali on webOS 6 has no EGL_EXT_platform_wayland.
static uint32_t renderer_get_platform(void *data) { (void)data; return 0; }

static struct wpe_renderer_backend_egl_interface renderer_interface = {
    .create = renderer_create,
    .destroy = renderer_destroy,
    .get_native_display = renderer_get_native_display,
    .get_platform = renderer_get_platform,
};

// Dispatches one wl_event_queue from a GMainContext (the target's WebKit compositing thread), using the
// prepare_read / read_events protocol so that Mali's own reads on other threads are not disturbed.
typedef struct {
    GSource base;
    struct wl_display *display;
    struct wl_event_queue *queue;
    gpointer tag;
    bool reading;
    void (*on_error)(void *);
    void *user;
} WaylandSource;

static gboolean wl_source_prepare(GSource *base, gint *timeout)
{
    WaylandSource *s = (WaylandSource *)base;
    *timeout = -1;
    if (s->reading)
        return FALSE;
    if (wl_display_prepare_read_queue(s->display, s->queue) != 0)
        return TRUE; // events already queued
    s->reading = true;
    wl_display_flush(s->display);
    return FALSE;
}

static gboolean wl_source_check(GSource *base)
{
    WaylandSource *s = (WaylandSource *)base;
    if (!s->reading)
        return TRUE;
    s->reading = false;
    if (g_source_query_unix_fd(base, s->tag) & (G_IO_IN | G_IO_ERR | G_IO_HUP)) {
        if (wl_display_read_events(s->display) < 0)
            return TRUE; // dispatch reports the error
        return TRUE;
    }
    wl_display_cancel_read(s->display);
    return FALSE;
}

static gboolean wl_source_dispatch(GSource *base, GSourceFunc callback, gpointer data)
{
    (void)callback;
    (void)data;
    WaylandSource *s = (WaylandSource *)base;
    if (wl_display_dispatch_queue_pending(s->display, s->queue) < 0 || wl_display_get_error(s->display)) {
        if (s->on_error)
            s->on_error(s->user);
        return G_SOURCE_REMOVE;
    }
    return G_SOURCE_CONTINUE;
}

static void wl_source_finalize(GSource *base)
{
    WaylandSource *s = (WaylandSource *)base;
    if (s->reading)
        wl_display_cancel_read(s->display);
    s->reading = false;
}

static GSourceFuncs wl_source_funcs = { wl_source_prepare, wl_source_check, wl_source_dispatch, wl_source_finalize, NULL, NULL };

struct target {
    struct wpe_renderer_backend_egl_target *wpe;
    struct renderer *renderer;
    int fd; // channel to the engine's wpeb_webos_view
    GMainContext *context;
    GSource *channel_source, *wayland_source, *timeout_source;
    struct wl_event_queue *queue;
    struct wl_surface *surface;
    struct wl_shell_surface *shell_surface;
    struct wl_webos_shell_surface *webos_surface;
    struct wl_egl_window *window;
    struct wl_webos_surface_group *group;
    struct wl_keyboard *keyboard;
    struct wl_webos_exported *video; // the page's video window (wpeb_webos_video_open), NULL without one
    struct wl_callback *frame;
    uint32_t width, height;
    bool attached, waiting_first_frame, failed, swap_interval_set;
    char group_name[WPEB_WEBOS_NAME_MAX], layer_name[WPEB_WEBOS_NAME_MAX];
};

// Keyboard focus. The UI window takes the remote's keys and forwards them to the engine. While hardware video plays,
// the UI raises the page layer above its window (LG's video punch-through cuts out everything below the app's main
// window), and the compositor then gives keyboard focus to this surface: hand it straight back to the group owner.
// (The owner's own focus_owner, or a key index, made LG's compositor drop the group; TV 2026-11-11.)
static void keyboard_keymap(void *data, struct wl_keyboard *k, uint32_t format, int32_t fd, uint32_t size)
{
    (void)data; (void)k; (void)format; (void)size;
    close(fd);
}
static void keyboard_enter(void *data, struct wl_keyboard *k, uint32_t serial, struct wl_surface *surface, struct wl_array *keys)
{
    struct target *t = data;
    (void)k; (void)serial; (void)keys;
    if (surface != t->surface || !t->group)
        return;
    // focus_owner here is a protocol error on LG's compositor (Wayland error 22, TV 2026-11-11): only log.
    LOG("keyboard focus came to the page surface");
}
static void keyboard_leave(void *data, struct wl_keyboard *k, uint32_t serial, struct wl_surface *surface) { (void)data; (void)k; (void)serial; (void)surface; }
static void keyboard_key(void *data, struct wl_keyboard *k, uint32_t serial, uint32_t time, uint32_t key, uint32_t state) { (void)data; (void)k; (void)serial; (void)time; (void)key; (void)state; }
static void keyboard_modifiers(void *data, struct wl_keyboard *k, uint32_t serial, uint32_t d, uint32_t l, uint32_t lk, uint32_t g) { (void)data; (void)k; (void)serial; (void)d; (void)l; (void)lk; (void)g; }
static const struct wl_keyboard_listener keyboard_listener = { keyboard_keymap, keyboard_enter, keyboard_leave, keyboard_key, keyboard_modifiers, NULL };

// A page's handle on the UI's surface group is only freed here, never destroyed with the protocol's destroy request:
// LG's compositor takes that request as the group itself going away, and webOS then minimises the whole app (TV,
// 2026-11-11: closing a tab or a window.open popup sent the browser to the background; likely also the black screen
// after a web-process handoff). The group belongs to the UI window, which destroys it.
static void release_group_handle(struct wl_webos_surface_group *group)
{
    if (group)
        wl_proxy_destroy((struct wl_proxy *)group);
}

static void target_status(struct target *t, int32_t code)
{
    struct wpeb_webos_msg msg = { .type = WPEB_WEBOS_STATUS, .code = code, .width = t->width, .height = t->height };
    copy_name(msg.group, t->group_name);
    copy_name(msg.layer, t->layer_name);
    send_msg(t->fd, &msg);
}

static void target_fail(struct target *t, int32_t code)
{
    if (t->failed)
        return;
    t->failed = true;
    LOG("target failed (%d), Wayland error %d", code, t->renderer->display ? wl_display_get_error(t->renderer->display) : 0);
    target_status(t, code);
}

static void target_frame_complete(struct target *t)
{
    if (t->timeout_source) {
        g_source_destroy(t->timeout_source);
        g_source_unref(t->timeout_source);
        t->timeout_source = NULL;
    }
    if (t->frame) {
        wl_callback_destroy(t->frame);
        t->frame = NULL;
    }
    wpe_renderer_backend_egl_target_dispatch_frame_complete(t->wpe);
}

static void frame_done(void *data, struct wl_callback *callback, uint32_t time)
{
    (void)callback;
    (void)time;
    struct target *t = data;
    if (t->waiting_first_frame) {
        t->waiting_first_frame = false;
        target_status(t, WPEB_WEBOS_STATUS_FIRST_FRAME);
    }
    target_frame_complete(t);
}
static const struct wl_callback_listener frame_listener = { frame_done };

static gboolean frame_timeout(gpointer data)
{
    struct target *t = data;
    g_source_unref(t->timeout_source);
    t->timeout_source = NULL;
    target_frame_complete(t);
    return G_SOURCE_REMOVE;
}

static void group_owner_destroyed(void *data, struct wl_webos_surface_group *group)
{
    (void)group;
    struct target *t = data;
    LOG("surface group '%s' destroyed by its owner", t->group_name);
    release_group_handle(t->group);
    t->group = NULL;
    t->attached = false;
    target_status(t, WPEB_WEBOS_STATUS_OWNER_GONE);
}
static const struct wl_webos_surface_group_listener group_listener = { group_owner_destroyed };

// LSM shows a group member only if it has a window role: a toplevel with the app's appId (TV, 2026-11-10: a bare
// surface was accepted by attach but never drawn). LSM scales it to the owner window's size. The role exists only
// while the surface is in the group: a detached toplevel of this app would be shown as a window of its own, on top
// of the browser (TV, 2026-11-11: black screen after a web-process handoff).
static void target_add_role(struct target *t)
{
    if (t->shell_surface)
        return;
    struct renderer *r = t->renderer;
    t->shell_surface = wl_shell_get_shell_surface(r->shell, t->surface);
    wl_proxy_set_queue((struct wl_proxy *)t->shell_surface, t->queue);
    wl_shell_surface_set_toplevel(t->shell_surface);
    t->webos_surface = wl_webos_shell_get_shell_surface(r->webos_shell, t->surface);
    wl_proxy_set_queue((struct wl_proxy *)t->webos_surface, t->queue);
    // Not the app's own id: webOS counts a surface with the app's id as one of its windows and minimises the whole
    // app when one goes away, which a crashed web process does with all its surfaces at once (black screen until
    // the app was closed; TV, 2026-11-11). Any other non-empty id shows it in the group the same way.
    const char *page_id = getenv("WPE_BROWSER_DEBUG_PAGE_APPID");
    char derived[160];
    if (!page_id || !*page_id) {
        const char *app_id = getenv("WPEB_APP_ID");
        snprintf(derived, sizeof(derived), "%s.page", app_id && *app_id ? app_id : "org.sostk.wpebrowser");
        page_id = derived;
    }
    wl_webos_shell_surface_set_property(t->webos_surface, "appId", page_id);
}

static void target_remove_role(struct target *t)
{
    if (t->webos_surface)
        wl_webos_shell_surface_destroy(t->webos_surface);
    t->webos_surface = NULL;
    if (t->shell_surface)
        wl_shell_surface_destroy(t->shell_surface);
    t->shell_surface = NULL;
}

// Hardware video window (end of file).
static pthread_mutex_t g_video_lock = PTHREAD_MUTEX_INITIALIZER;
static struct target *g_shown_target; // the last attached page target (g_video_lock)
static void target_close_video(struct target *t); // g_video_lock held

// Attaching and detaching pages of this process run on each page's own WebKit thread; one lock serialises them,
// so taking the layer from another page (target_attach) never interleaves with that page's own detach.
static pthread_mutex_t g_layer_lock = PTHREAD_MUTEX_INITIALIZER;

static void target_detach_locked(struct target *t)
{
    pthread_mutex_lock(&g_video_lock);
    if (g_shown_target == t)
        g_shown_target = NULL;
    pthread_mutex_unlock(&g_video_lock);
    const bool was_attached = t->group && t->attached;
    if (was_attached)
        wl_webos_surface_group_detach(t->group, t->surface);
    t->attached = false;
    // The window role stays for the page's whole life: webOS counts each page surface as one of the app's windows,
    // and removing a role (or destroying a surface that has one) looked to it like the app's window closing; it then
    // minimised the browser on tab switches and when a window.open popup closed (TV, 2026-11-11). Hidden, the surface
    // is unmapped instead, so a detached page is not drawn on its own. target_destroy removes the role.
    if (was_attached && t->surface) {
        wl_surface_attach(t->surface, NULL, 0, 0);
        wl_surface_commit(t->surface);
    }
}

static void target_detach(struct target *t)
{
    pthread_mutex_lock(&g_layer_lock);
    target_detach_locked(t);
    pthread_mutex_unlock(&g_layer_lock);
}

static void target_attach_locked(struct target *t, const char *group, const char *layer);

static void target_attach(struct target *t, const char *group, const char *layer)
{
    pthread_mutex_lock(&g_layer_lock);
    target_attach_locked(t, group, layer);
    pthread_mutex_unlock(&g_layer_lock);
}

static void target_attach_locked(struct target *t, const char *group, const char *layer)
{
    if (!t->surface || t->failed)
        return;
    if (t->group && strcmp(t->group_name, group) != 0) {
        target_detach_locked(t);
        release_group_handle(t->group);
        t->group = NULL;
    }
    // One surface per layer: another page of this web process may still hold it (tab switches send its detach and
    // our attach through separate threads; a window.open popup attached before its opener let go: "Layer already
    // attached", TV 2026-11-11). Every page of the process shares this Wayland connection, so detaching it here
    // first reaches the compositor before our attach.
    pthread_mutex_lock(&g_video_lock);
    struct target *other = g_shown_target != t ? g_shown_target : NULL;
    pthread_mutex_unlock(&g_video_lock);
    if (other && other->attached && other->renderer == t->renderer && !strcmp(other->layer_name, layer)) {
        target_detach_locked(other);
        wl_display_flush(t->renderer->display);
        target_status(other, WPEB_WEBOS_STATUS_DETACHED);
        LOG("detached the page that held the layer before attaching this one");
    }
    copy_name(t->group_name, group);
    copy_name(t->layer_name, layer);
    if (!t->group) {
        t->group = wl_webos_surface_group_compositor_get_surface_group(t->renderer->groups, t->group_name);
        wl_proxy_set_queue((struct wl_proxy *)t->group, t->queue);
        wl_webos_surface_group_add_listener(t->group, &group_listener, t);
    }
    target_add_role(t);
    wl_webos_surface_group_attach(t->group, t->surface, t->layer_name);
    // Show what was last drawn: WebKit may have finished drawing before the attach and draw nothing new.
    wl_surface_commit(t->surface);
    t->attached = true;
    t->waiting_first_frame = true;
    LOG("page surface %ux%u attached to '%s'/'%s'", t->width, t->height, t->group_name, t->layer_name);
    target_status(t, WPEB_WEBOS_STATUS_ATTACHED);
    pthread_mutex_lock(&g_video_lock);
    g_shown_target = t;
    pthread_mutex_unlock(&g_video_lock);
}

static gboolean target_on_channel(gint fd, GIOCondition condition, gpointer data)
{
    struct target *t = data;
    if (condition & G_IO_IN) {
        struct wpeb_webos_msg msg;
        ssize_t n;
        while ((n = recv(fd, &msg, sizeof(msg), MSG_DONTWAIT)) == (ssize_t)sizeof(msg)) {
            msg.group[WPEB_WEBOS_NAME_MAX - 1] = msg.layer[WPEB_WEBOS_NAME_MAX - 1] = '\0';
            if (msg.type == WPEB_WEBOS_ATTACH)
                target_attach(t, msg.group, msg.layer);
            else if (msg.type == WPEB_WEBOS_DETACH && t->attached) {
                target_detach(t);
                // The detach must reach the compositor before the view lets another process attach.
                if (t->renderer->display)
                    wl_display_flush(t->renderer->display);
                target_status(t, WPEB_WEBOS_STATUS_DETACHED);
            }
        }
        if (t->renderer->display)
            wl_display_flush(t->renderer->display);
        if (n == 0)
            condition |= G_IO_HUP;
    }
    if (condition & (G_IO_HUP | G_IO_ERR)) {
        // The view has moved on (another web process, or it is gone): this surface must not keep the layer.
        if (t->attached) {
            target_detach(t);
            if (t->renderer->display)
                wl_display_flush(t->renderer->display);
        }
        g_source_unref(t->channel_source);
        t->channel_source = NULL;
        return G_SOURCE_REMOVE;
    }
    return G_SOURCE_CONTINUE;
}

static void target_on_wayland_error(void *data)
{
    struct target *t = data;
    g_source_unref(t->wayland_source);
    t->wayland_source = NULL;
    target_fail(t, WPEB_WEBOS_STATUS_WAYLAND_ERROR);
}

static void *target_create(struct wpe_renderer_backend_egl_target *wpe, int fd)
{
    struct target *t = g_new0(struct target, 1);
    t->wpe = wpe;
    t->fd = fd;
    return t;
}

static void target_deinitialize(void *data)
{
    struct target *t = data;
    GSource **sources[] = { &t->channel_source, &t->wayland_source, &t->timeout_source };
    for (size_t i = 0; i < G_N_ELEMENTS(sources); ++i) {
        if (*sources[i]) {
            g_source_destroy(*sources[i]);
            g_source_unref(*sources[i]);
            *sources[i] = NULL;
        }
    }
    if (t->frame)
        wl_callback_destroy(t->frame);
    t->frame = NULL;
    target_detach(t);
    pthread_mutex_lock(&g_video_lock);
    target_close_video(t);
    pthread_mutex_unlock(&g_video_lock);
    if (t->keyboard)
        wl_keyboard_destroy(t->keyboard);
    t->keyboard = NULL;
    if (t->group)
        release_group_handle(t->group);
    t->group = NULL;
    if (t->window)
        wl_egl_window_destroy(t->window);
    t->window = NULL;
    if (t->surface && t->shell_surface && t->renderer && t->renderer->pool) {
        // Destroying a surface that has a window role made webOS minimise the whole app (TV, 2026-11-11, even
        // unmapped): webOS counts it as one of the app's windows closing. It is kept, unmapped, for the next page.
        wl_surface_attach(t->surface, NULL, 0, 0);
        wl_surface_commit(t->surface);
        struct pooled_surface *p = g_new0(struct pooled_surface, 1);
        p->surface = t->surface;
        p->shell_surface = t->shell_surface;
        p->webos_surface = t->webos_surface;
        // Off this page's queue, which is destroyed below; their events are never dispatched (none are needed).
        wl_proxy_set_queue((struct wl_proxy *)p->surface, t->renderer->queue);
        wl_proxy_set_queue((struct wl_proxy *)p->shell_surface, t->renderer->queue);
        if (p->webos_surface)
            wl_proxy_set_queue((struct wl_proxy *)p->webos_surface, t->renderer->queue);
        pthread_mutex_lock(&t->renderer->pool_lock);
        g_ptr_array_add(t->renderer->pool, p);
        pthread_mutex_unlock(&t->renderer->pool_lock);
        t->surface = NULL;
        t->shell_surface = NULL;
        t->webos_surface = NULL;
        LOG("page closed: its surface is kept for the next page");
    }
    target_remove_role(t);
    if (t->surface)
        wl_surface_destroy(t->surface);
    t->surface = NULL;
    if (t->renderer && t->renderer->display)
        wl_display_flush(t->renderer->display);
    if (t->queue)
        wl_event_queue_destroy(t->queue);
    t->queue = NULL;
    if (t->context)
        g_main_context_unref(t->context);
    t->context = NULL;
}

static void target_destroy(void *data)
{
    struct target *t = data;
    target_deinitialize(t);
    if (t->fd >= 0)
        close(t->fd);
    g_free(t);
}

// Called on WebKit's compositing thread, whose GMainContext is the thread default there.
static void target_initialize(void *data, void *backend_data, uint32_t width, uint32_t height)
{
    struct target *t = data;
    t->renderer = backend_data;
    t->width = width;
    t->height = height;
    t->context = g_main_context_ref_thread_default();

    t->channel_source = g_unix_fd_source_new(t->fd, G_IO_IN | G_IO_HUP | G_IO_ERR);
    g_source_set_callback(t->channel_source, (GSourceFunc)(void (*)(void))target_on_channel, t, NULL);
    g_source_attach(t->channel_source, t->context);

    struct renderer *r = t->renderer;
    if (!r->display) {
        target_fail(t, WPEB_WEBOS_STATUS_NO_WAYLAND);
        return;
    }
    if (!r->compositor || !r->groups || !r->shell || !r->webos_shell) {
        target_fail(t, WPEB_WEBOS_STATUS_NO_SURFACE_GROUPS);
        return;
    }
    t->queue = wl_display_create_queue(r->display);
    struct pooled_surface *reuse = NULL;
    pthread_mutex_lock(&r->pool_lock);
    if (r->pool->len)
        reuse = g_ptr_array_steal_index(r->pool, r->pool->len - 1);
    pthread_mutex_unlock(&r->pool_lock);
    if (reuse) {
        // A closed page's surface, still with its window role (and empty input region): see target_pool_surface.
        t->surface = reuse->surface;
        t->shell_surface = reuse->shell_surface;
        t->webos_surface = reuse->webos_surface;
        g_free(reuse);
        if (t->shell_surface)
            wl_proxy_set_queue((struct wl_proxy *)t->shell_surface, t->queue);
        if (t->webos_surface)
            wl_proxy_set_queue((struct wl_proxy *)t->webos_surface, t->queue);
        LOG("reusing a closed page's surface");
    } else {
        t->surface = wl_compositor_create_surface(r->compositor);
    }
    wl_proxy_set_queue((struct wl_proxy *)t->surface, t->queue);
    // Input always goes to the UI window, which forwards it to the engine: an empty input region lets pointer
    // events fall through even when the page layer is above the UI (while hardware video plays: the compositor
    // cuts everything below the app's main window out where the video is).
    struct wl_region *no_input = wl_compositor_create_region(r->compositor);
    wl_surface_set_input_region(t->surface, no_input);
    wl_region_destroy(no_input);
    if (r->seat) {
        t->keyboard = wl_seat_get_keyboard(r->seat);
        wl_proxy_set_queue((struct wl_proxy *)t->keyboard, t->queue);
        wl_keyboard_add_listener(t->keyboard, &keyboard_listener, t);
    }
    // No window role yet: target_attach adds it (see target_add_role).
    t->window = wl_egl_window_create(t->surface, (int)width, (int)height);

    WaylandSource *s = (WaylandSource *)g_source_new(&wl_source_funcs, sizeof(WaylandSource));
    s->display = r->display;
    s->queue = t->queue;
    s->on_error = target_on_wayland_error;
    s->user = t;
    s->tag = g_source_add_unix_fd(&s->base, wl_display_get_fd(r->display), G_IO_IN | G_IO_ERR | G_IO_HUP);
    g_source_set_priority(&s->base, G_PRIORITY_HIGH);
    g_source_attach(&s->base, t->context);
    t->wayland_source = &s->base;

    LOG("page target %ux%u ready", width, height);
    target_status(t, WPEB_WEBOS_STATUS_READY);
}

static EGLNativeWindowType target_get_native_window(void *data)
{
    return (EGLNativeWindowType)((struct target *)data)->window;
}

static void target_resize(void *data, uint32_t width, uint32_t height)
{
    struct target *t = data;
    t->width = width;
    t->height = height;
    if (t->window)
        wl_egl_window_resize(t->window, (int)width, (int)height, 0, 0);
}

static void target_frame_will_render(void *data)
{
    struct target *t = data;
    if (!t->swap_interval_set) {
        // WebKit's context is current here. Swap interval 0: eglSwapBuffers must never wait for a frame callback
        // (a detached surface gets none, and this thread also serves our channel). Pacing comes from frame_done.
        t->swap_interval_set = true;
        typedef unsigned (*SwapInterval)(void *, int);
        typedef void *(*GetCurrentDisplay)(void);
        SwapInterval swap_interval = (SwapInterval)dlsym(RTLD_DEFAULT, "eglSwapInterval");
        GetCurrentDisplay current = (GetCurrentDisplay)dlsym(RTLD_DEFAULT, "eglGetCurrentDisplay");
        if (swap_interval && current && !swap_interval(current(), 0))
            LOG("eglSwapInterval(0) failed");
    }
    if (t->surface && t->attached && !t->failed && !t->frame) {
        // Requested before WebKit's eglSwapBuffers, so it belongs to the commit the swap makes.
        t->frame = wl_surface_frame(t->surface);
        wl_callback_add_listener(t->frame, &frame_listener, t);
    }
}

static void target_frame_rendered(void *data)
{
    struct target *t = data;
    if (t->renderer && t->renderer->display)
        wl_display_flush(t->renderer->display);
    if (!t->timeout_source) {
        // Paces a detached page too; cancelled by frame_done when the compositor answers first.
        t->timeout_source = g_timeout_source_new(t->frame ? FRAME_TIMEOUT_MS : 16);
        g_source_set_callback(t->timeout_source, frame_timeout, t, NULL);
        g_source_attach(t->timeout_source, t->context);
    }
}

static struct wpe_renderer_backend_egl_target_interface target_interface = {
    .create = target_create,
    .destroy = target_destroy,
    .initialize = target_initialize,
    .get_native_window = target_get_native_window,
    .resize = target_resize,
    .frame_will_render = target_frame_will_render,
    .frame_rendered = target_frame_rendered,
    .deinitialize = target_deinitialize,
};

// Offscreen GL contexts (WebGL, canvas): no native window, so WebKit falls back to a pbuffer context.
static void *offscreen_create(void) { return GINT_TO_POINTER(1); }
static void offscreen_destroy(void *data) { (void)data; }
static void offscreen_initialize(void *data, void *backend) { (void)data; (void)backend; }
static EGLNativeWindowType offscreen_get_native_window(void *data) { (void)data; return (EGLNativeWindowType)0; }

static struct wpe_renderer_backend_egl_offscreen_target_interface offscreen_interface = {
    .create = offscreen_create,
    .destroy = offscreen_destroy,
    .initialize = offscreen_initialize,
    .get_native_window = offscreen_get_native_window,
};

// ===== libwpe loader ==========================================================================================

static void *load_object(const char *name)
{
    if (!strcmp(name, "_wpe_renderer_host_interface"))
        return &host_interface;
    if (!strcmp(name, "_wpe_renderer_backend_egl_interface"))
        return &renderer_interface;
    if (!strcmp(name, "_wpe_renderer_backend_egl_target_interface"))
        return &target_interface;
    if (!strcmp(name, "_wpe_renderer_backend_egl_offscreen_target_interface"))
        return &offscreen_interface;
    // No "_wpe_view_backend_interface": views are made only by wpeb_webos_view_new().
    return NULL;
}

__attribute__((visibility("default"))) struct wpe_loader_interface _wpe_loader_interface = {
    .load_object = load_object,
};

// ===== hardware video window (web process) =====================================================================
// As Chromium does on webOS (ui/ozone/platform/wayland/extensions/webos/host/webos_foreign_video_window.cc): LG's
// video plane is bound to an exported window made on the surface that draws the page, so the compositor punches
// the video through below *this* surface and everything the page draws above its hole stays on top. (NDL's video
// is bound to the app's main window instead, which cut our page layer out under the UI.) The media sink
// (engine/gst/wpebmediasink.c, same process) asks for it; the window id goes to the UI's Starfish pipeline
// (option.windowId). One video window per page target; the newest attached target is the one shown.

static pthread_cond_t g_video_cond = PTHREAD_COND_INITIALIZER;
static char g_video_id[128]; // window_id_assigned (g_video_lock)

static void video_window_id_assigned(void *data, struct wl_webos_exported *exported, const char *id, uint32_t type)
{
    (void)data; (void)exported; (void)type;
    pthread_mutex_lock(&g_video_lock);
    snprintf(g_video_id, sizeof(g_video_id), "%s", id ? id : "");
    pthread_cond_broadcast(&g_video_cond);
    pthread_mutex_unlock(&g_video_lock);
    LOG("video window id %s", id ? id : "(none)");
}
static const struct wl_webos_exported_listener video_listener = { video_window_id_assigned };

static void target_close_video(struct target *t)
{
    if (t->video) {
        wl_webos_exported_destroy(t->video);
        t->video = NULL;
        if (t->renderer && t->renderer->display)
            wl_display_flush(t->renderer->display);
        LOG("video window closed");
    }
}

// Creates the video window on the shown page surface and waits up to timeout_ms for its id. 1 with id, 0 if not.
__attribute__((visibility("default"))) int wpeb_webos_video_open(char *id, size_t size, unsigned timeout_ms)
{
    pthread_mutex_lock(&g_video_lock);
    struct target *t = g_shown_target;
    if (!t || !t->surface || !t->renderer->foreign) {
        pthread_mutex_unlock(&g_video_lock);
        LOG("video window: %s", !t ? "no page surface shown" : "no wl_webos_foreign");
        return 0;
    }
    target_close_video(t);
    g_video_id[0] = '\0';
    t->video = wl_webos_foreign_export_element(t->renderer->foreign, t->surface, WL_WEBOS_FOREIGN_WEBOS_EXPORTED_TYPE_VIDEO_OBJECT);
    // The event arrives on the target's queue, which WebKit's compositing thread dispatches.
    wl_proxy_set_queue((struct wl_proxy *)t->video, t->queue);
    wl_webos_exported_add_listener(t->video, &video_listener, NULL);
    wl_display_flush(t->renderer->display);
    struct timespec until;
    clock_gettime(CLOCK_REALTIME, &until);
    until.tv_sec += timeout_ms / 1000;
    until.tv_nsec += (long)(timeout_ms % 1000) * 1000000L;
    if (until.tv_nsec >= 1000000000L) { until.tv_sec++; until.tv_nsec -= 1000000000L; }
    while (!g_video_id[0] && pthread_cond_timedwait(&g_video_cond, &g_video_lock, &until) == 0) { }
    int ok = g_video_id[0] != '\0';
    if (ok)
        snprintf(id, size, "%s", g_video_id);
    else
        LOG("video window: no id within %u ms", timeout_ms);
    pthread_mutex_unlock(&g_video_lock);
    return ok;
}

// Source: the coded picture; destination: page-surface pixels (the page surface covers the screen).
__attribute__((visibility("default"))) void wpeb_webos_video_place(int sw, int sh, int x, int y, int w, int h)
{
    pthread_mutex_lock(&g_video_lock);
    struct target *t = g_shown_target;
    if (t && t->video && w > 0 && h > 0) {
        struct wl_region *src = wl_compositor_create_region(t->renderer->compositor);
        wl_region_add(src, 0, 0, sw > 0 ? sw : w, sh > 0 ? sh : h);
        struct wl_region *dst = wl_compositor_create_region(t->renderer->compositor);
        wl_region_add(dst, x, y, w, h);
        wl_webos_exported_set_exported_window(t->video, src, dst);
        wl_region_destroy(src);
        wl_region_destroy(dst);
        wl_display_flush(t->renderer->display);
    }
    pthread_mutex_unlock(&g_video_lock);
}

__attribute__((visibility("default"))) void wpeb_webos_video_close(void)
{
    pthread_mutex_lock(&g_video_lock);
    if (g_shown_target)
        target_close_video(g_shown_target);
    pthread_mutex_unlock(&g_video_lock);
}
