// SPDX-License-Identifier: GPL-3.0-or-later
// Surface-group probe: can a second process draw with the TV's GPU (Mali EGL) straight into our app window, the way
// Chromium's GLSurfaceWayland draws into its window (no readback, no copies)? See docs/webkit-like-chromium.md, part 2.
//
//   APPID=org.sostk.wpebrowser ./sg-probe owner [below|above] [seconds] [client-width client-height]
//
// owner  = stands in for the browser UI: a normal webOS app window (wl_shell + wl_webos_shell, appId from $APPID).
//          It creates surface group "wpebprobe" on its surface and layer "page" (z -1 = below the owner, 1 = above),
//          draws an opaque blue bar at the top, an opaque grey bar at the bottom and leaves the middle transparent,
//          then starts "sg-probe client" (the same binary) as a separate process.
// client = stands in for WPEWebProcess: its own Wayland connection and wl_surface, attached to the group's layer,
//          animated with Mali EGL + eglSwapBuffers. Prints frame timing every 120 frames.
// What to look at on the TV: does the client's animation (moving white bar on cycling colours) show in the middle,
// where is it placed, is the owner's top/bottom bar on top of it (z -1), and the frame times in the log.
#define _GNU_SOURCE
#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <wayland-client.h>
#include <wayland-egl.h>
#include "wayland-webos-shell-client-protocol.h"
#include "webos-surface-group-client-protocol.h"

static const char *kGroup = "wpebprobe", *kLayer = "page";
static const char *role = "?";

struct app {
    struct wl_display *display;
    struct wl_compositor *compositor;
    struct wl_shell *shell;
    struct wl_webos_shell *webos_shell;
    struct wl_webos_surface_group_compositor *groups;
    EGLDisplay egl;
    EGLConfig config;
    EGLContext context;
};

static double now(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec / 1e9;
}

#define LOG(...) do { printf("[%s %.3f] ", role, now()); printf(__VA_ARGS__); printf("\n"); fflush(stdout); } while (0)

static void global(void *data, struct wl_registry *registry, uint32_t name, const char *interface, uint32_t version)
{
    struct app *a = data;
    if (!strcmp(interface, "wl_compositor")) a->compositor = wl_registry_bind(registry, name, &wl_compositor_interface, 1);
    else if (!strcmp(interface, "wl_shell")) a->shell = wl_registry_bind(registry, name, &wl_shell_interface, 1);
    else if (!strcmp(interface, "wl_webos_shell")) a->webos_shell = wl_registry_bind(registry, name, &wl_webos_shell_interface, 1);
    else if (!strcmp(interface, "wl_webos_surface_group_compositor"))
        a->groups = wl_registry_bind(registry, name, &wl_webos_surface_group_compositor_interface, 1);
    (void)version;
}
static void global_remove(void *data, struct wl_registry *registry, uint32_t name) { (void)data; (void)registry; (void)name; }
static const struct wl_registry_listener registry_listener = { global, global_remove };

static int connect_all(struct app *a)
{
    a->display = wl_display_connect(NULL);
    if (!a->display) { LOG("wl_display_connect failed (XDG_RUNTIME_DIR=%s)", getenv("XDG_RUNTIME_DIR")); return 0; }
    struct wl_registry *registry = wl_display_get_registry(a->display);
    wl_registry_add_listener(registry, &registry_listener, a);
    wl_display_roundtrip(a->display);
    LOG("globals: compositor=%d wl_shell=%d webos_shell=%d surface_group_compositor=%d",
        !!a->compositor, !!a->shell, !!a->webos_shell, !!a->groups);
    if (!a->compositor || !a->groups) return 0;

    a->egl = eglGetDisplay((EGLNativeDisplayType)a->display);
    EGLint major = 0, minor = 0;
    if (!eglInitialize(a->egl, &major, &minor)) { LOG("eglInitialize failed 0x%x", eglGetError()); return 0; }
    const EGLint attribs[] = { EGL_SURFACE_TYPE, EGL_WINDOW_BIT, EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8,
                               EGL_ALPHA_SIZE, 8, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT, EGL_NONE };
    EGLint n = 0;
    if (!eglChooseConfig(a->egl, attribs, &a->config, 1, &n) || n < 1) { LOG("no RGBA8888 window config"); return 0; }
    const EGLint ctx[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
    a->context = eglCreateContext(a->egl, a->config, EGL_NO_CONTEXT, ctx);
    LOG("EGL %d.%d vendor=%s", major, minor, eglQueryString(a->egl, EGL_VENDOR));
    return a->context != EGL_NO_CONTEXT;
}

static EGLSurface make_window(struct app *a, struct wl_surface *surface, int w, int h, struct wl_egl_window **win)
{
    *win = wl_egl_window_create(surface, w, h);
    EGLSurface s = eglCreateWindowSurface(a->egl, a->config, (EGLNativeWindowType)*win, NULL);
    if (s == EGL_NO_SURFACE) { LOG("eglCreateWindowSurface failed 0x%x", eglGetError()); return s; }
    eglMakeCurrent(a->egl, s, s, a->context);
    eglSwapInterval(a->egl, 1);
    LOG("EGL window %dx%d, GL renderer=%s", w, h, (const char *)glGetString(GL_RENDERER));
    return s;
}

static int pump(struct app *a)
{
    if (wl_display_dispatch_pending(a->display) < 0 || wl_display_flush(a->display) < 0) {
        LOG("Wayland error %d (protocol error on interface %s?)", wl_display_get_error(a->display),
            "see WAYLAND_DEBUG=1");
        return 0;
    }
    return 1;
}

static void layer_attached(void *data, struct wl_webos_surface_group_layer *layer)
{ (void)data; (void)layer; LOG("layer '%s': a surface was attached", kLayer); }
static void layer_detached(void *data, struct wl_webos_surface_group_layer *layer)
{ (void)data; (void)layer; LOG("layer '%s': a surface was detached", kLayer); }
static const struct wl_webos_surface_group_layer_listener layer_listener = { layer_attached, layer_detached };

static void owner_destroyed(void *data, struct wl_webos_surface_group *group)
{ (void)group; *(int *)data = 1; LOG("group owner destroyed"); }
static const struct wl_webos_surface_group_listener group_listener = { owner_destroyed };

static int run_owner(int z, double seconds, const char *client_w, const char *client_h)
{
    role = "owner";
    const char *appid = getenv("APPID");
    if (!appid) { LOG("set APPID (the app id LSM shows the window for)"); return 2; }
    struct app a = {0};
    if (!connect_all(&a) || !a.shell || !a.webos_shell) { LOG("missing globals"); return 1; }

    struct wl_surface *surface = wl_compositor_create_surface(a.compositor);
    struct wl_shell_surface *shell_surface = wl_shell_get_shell_surface(a.shell, surface);
    wl_shell_surface_set_toplevel(shell_surface);
    struct wl_webos_shell_surface *webos = wl_webos_shell_get_shell_surface(a.webos_shell, surface);
    wl_webos_shell_surface_set_property(webos, "appId", appid);
    wl_webos_shell_surface_set_property(webos, "_WEBOS_ACCESS_POLICY_KEYS_BACK", "true");

    struct wl_webos_surface_group *group = wl_webos_surface_group_compositor_create_surface_group(a.groups, surface, kGroup);
    int destroyed = 0;
    wl_webos_surface_group_add_listener(group, &group_listener, &destroyed);
    struct wl_webos_surface_group_layer *layer = wl_webos_surface_group_create_layer(group, kLayer, z);
    wl_webos_surface_group_layer_add_listener(layer, &layer_listener, NULL);
    LOG("group '%s' with layer '%s' at z %d", kGroup, kLayer, z);

    struct wl_egl_window *win;
    EGLSurface s = make_window(&a, surface, 1920, 1080, &win);
    if (s == EGL_NO_SURFACE) return 1;

    pid_t child = -1;
    double end = now() + seconds;
    for (int frame = 0; now() < end; ++frame) {
        glDisable(GL_SCISSOR_TEST);
        glClearColor(0, 0, 0, 0); // transparent middle: the client's layer should show through
        glClear(GL_COLOR_BUFFER_BIT);
        glEnable(GL_SCISSOR_TEST);
        glScissor(0, 1080 - 160, 1920, 160); glClearColor(0.10f, 0.30f, 0.80f, 1); glClear(GL_COLOR_BUFFER_BIT);
        glScissor(0, 0, 1920, 100); glClearColor(0.35f, 0.35f, 0.35f, 1); glClear(GL_COLOR_BUFFER_BIT);
        eglSwapBuffers(a.egl, s);
        if (!pump(&a)) break;
        if (frame == 30 && child < 0) {
            child = fork();
            if (child == 0) {
                execl("/proc/self/exe", "sg-probe", "client", client_w, client_h, (char *)NULL);
                _exit(127);
            }
            LOG("started client pid %d", (int)child);
        }
    }
    if (child > 0) { kill(child, SIGTERM); waitpid(child, NULL, 0); }
    LOG("done");
    return 0;
}

static int run_client(int w, int h)
{
    role = "client";
    struct app a = {0};
    if (!connect_all(&a)) return 1;
    struct wl_surface *surface = wl_compositor_create_surface(a.compositor);
    // SGP_CLIENT_ROLE=shell: give the surface a window role first (wl_shell toplevel + webOS shell surface with
    // the app's appId), as Chromium's webOS windows have. A bare surface was accepted but never shown (2026-11-10).
    const char *client_role = getenv("SGP_CLIENT_ROLE");
    if (client_role && !strcmp(client_role, "shell")) {
        const char *appid = getenv("APPID");
        if (!a.shell || !a.webos_shell || !appid) { LOG("shell role needs wl_shell, wl_webos_shell and APPID"); return 1; }
        struct wl_shell_surface *shell_surface = wl_shell_get_shell_surface(a.shell, surface);
        wl_shell_surface_set_toplevel(shell_surface);
        struct wl_webos_shell_surface *webos = wl_webos_shell_get_shell_surface(a.webos_shell, surface);
        wl_webos_shell_surface_set_property(webos, "appId", appid);
        const char *extra = getenv("SGP_CLIENT_PROPS"); // "name=value,name=value"
        if (extra) {
            char buf[256];
            snprintf(buf, sizeof(buf), "%s", extra);
            for (char *save = NULL, *kv = strtok_r(buf, ",", &save); kv; kv = strtok_r(NULL, ",", &save)) {
                char *eq = strchr(kv, '=');
                if (eq) { *eq = 0; wl_webos_shell_surface_set_property(webos, kv, eq + 1); LOG("property %s=%s", kv, eq + 1); }
            }
        }
        LOG("client surface has a shell role (appId %s)", appid);
    }
    struct wl_webos_surface_group *group = wl_webos_surface_group_compositor_get_surface_group(a.groups, kGroup);
    int destroyed = 0;
    wl_webos_surface_group_add_listener(group, &group_listener, &destroyed);
    wl_webos_surface_group_attach(group, surface, kLayer);
    LOG("attached own surface to '%s'/'%s'", kGroup, kLayer);

    struct wl_egl_window *win;
    EGLSurface s = make_window(&a, surface, w, h, &win);
    if (s == EGL_NO_SURFACE) return 1;

    double last = now(), sum = 0, worst = 0, swap_sum = 0, swap_worst = 0;
    for (int frame = 1; !destroyed; ++frame) {
        const float t = (float)(frame % 240) / 240.0f;
        glDisable(GL_SCISSOR_TEST);
        glClearColor(t, 1.0f - t, 0.5f, 1);
        glClear(GL_COLOR_BUFFER_BIT);
        glEnable(GL_SCISSOR_TEST);
        glScissor((frame * 8) % (w - 40), 0, 40, h); // moving white bar: shows tearing / stutter
        glClearColor(1, 1, 1, 1);
        glClear(GL_COLOR_BUFFER_BIT);
        const double before = now();
        if (!eglSwapBuffers(a.egl, s)) { LOG("eglSwapBuffers failed 0x%x", eglGetError()); return 1; }
        const double after = now(), interval = after - last, swap = after - before;
        last = after;
        sum += interval; swap_sum += swap;
        if (interval > worst) worst = interval;
        if (swap > swap_worst) swap_worst = swap;
        if (!pump(&a)) return 1;
        if (frame % 120 == 0) {
            LOG("frames %d: interval avg %.2f ms worst %.2f ms; eglSwapBuffers avg %.2f ms worst %.2f ms",
                frame, sum / 120 * 1000, worst * 1000, swap_sum / 120 * 1000, swap_worst * 1000);
            sum = worst = swap_sum = swap_worst = 0;
        }
    }
    return 0;
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    if (argc >= 2 && !strcmp(argv[1], "owner")) {
        const int z = argc >= 3 && !strcmp(argv[2], "above") ? 1 : -1;
        const double seconds = argc >= 4 ? atof(argv[3]) : 15;
        return run_owner(z, seconds > 0 ? seconds : 15, argc >= 6 ? argv[4] : "1920", argc >= 6 ? argv[5] : "1080");
    }
    if (argc >= 2 && !strcmp(argv[1], "client"))
        return run_client(argc >= 4 ? atoi(argv[2]) : 1920, argc >= 4 ? atoi(argv[3]) : 1080);
    fprintf(stderr, "usage: APPID=<app id> %s owner [below|above] [seconds] [client-width client-height]\n", argv[0]);
    return 2;
}
