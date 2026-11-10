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

static void view_send_state(struct wpeb_webos_view *v)
{
    struct wpeb_webos_msg msg = { .type = v->attached ? WPEB_WEBOS_ATTACH : WPEB_WEBOS_DETACH };
    copy_name(msg.group, v->group);
    copy_name(msg.layer, v->layer);
    send_msg(v->fd, &msg);
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
    view_close_channel(v);
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
    struct wl_callback *frame;
    uint32_t width, height;
    bool attached, waiting_first_frame, failed, swap_interval_set;
    char group_name[WPEB_WEBOS_NAME_MAX], layer_name[WPEB_WEBOS_NAME_MAX];
};

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
    wl_webos_surface_group_destroy(t->group);
    t->group = NULL;
    t->attached = false;
    target_status(t, WPEB_WEBOS_STATUS_OWNER_GONE);
}
static const struct wl_webos_surface_group_listener group_listener = { group_owner_destroyed };

static void target_detach(struct target *t)
{
    if (t->group && t->attached)
        wl_webos_surface_group_detach(t->group, t->surface);
    t->attached = false;
}

static void target_attach(struct target *t, const char *group, const char *layer)
{
    if (!t->surface || t->failed)
        return;
    if (t->group && strcmp(t->group_name, group) != 0) {
        target_detach(t);
        wl_webos_surface_group_destroy(t->group);
        t->group = NULL;
    }
    copy_name(t->group_name, group);
    copy_name(t->layer_name, layer);
    if (!t->group) {
        t->group = wl_webos_surface_group_compositor_get_surface_group(t->renderer->groups, t->group_name);
        wl_proxy_set_queue((struct wl_proxy *)t->group, t->queue);
        wl_webos_surface_group_add_listener(t->group, &group_listener, t);
    }
    wl_webos_surface_group_attach(t->group, t->surface, t->layer_name);
    t->attached = true;
    t->waiting_first_frame = true;
    LOG("page surface %ux%u attached to '%s'/'%s'", t->width, t->height, t->group_name, t->layer_name);
    target_status(t, WPEB_WEBOS_STATUS_ATTACHED);
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
                target_status(t, WPEB_WEBOS_STATUS_DETACHED);
            }
        }
        if (t->renderer->display)
            wl_display_flush(t->renderer->display);
        if (n == 0)
            condition |= G_IO_HUP;
    }
    if (condition & (G_IO_HUP | G_IO_ERR)) {
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
    if (t->group)
        wl_webos_surface_group_destroy(t->group);
    t->group = NULL;
    if (t->window)
        wl_egl_window_destroy(t->window);
    t->window = NULL;
    if (t->webos_surface)
        wl_webos_shell_surface_destroy(t->webos_surface);
    t->webos_surface = NULL;
    if (t->shell_surface)
        wl_shell_surface_destroy(t->shell_surface);
    t->shell_surface = NULL;
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
    t->surface = wl_compositor_create_surface(r->compositor);
    wl_proxy_set_queue((struct wl_proxy *)t->surface, t->queue);
    // LSM shows a group member only if it has a window role: a toplevel with the app's appId (TV, 2026-11-10:
    // a bare surface was accepted by attach but never drawn). LSM scales it to the owner window's size.
    t->shell_surface = wl_shell_get_shell_surface(r->shell, t->surface);
    wl_proxy_set_queue((struct wl_proxy *)t->shell_surface, t->queue);
    wl_shell_surface_set_toplevel(t->shell_surface);
    t->webos_surface = wl_webos_shell_get_shell_surface(r->webos_shell, t->surface);
    wl_proxy_set_queue((struct wl_proxy *)t->webos_surface, t->queue);
    const char *app_id = getenv("WPEB_APP_ID");
    wl_webos_shell_surface_set_property(t->webos_surface, "appId", app_id && *app_id ? app_id : "org.sostk.wpebrowser");
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
