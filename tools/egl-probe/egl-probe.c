// SPDX-License-Identifier: GPL-3.0-or-later
// egl-probe: what can WPEWebProcess expect from the TV's own EGL (Mali) when it has no window?
//
// dlopens libEGL.so.1 / libGLESv2.so.2 from the default search path (no DT_NEEDED, so the same
// binary probes the TV's libraries or, with LD_LIBRARY_PATH, a bundled Mesa) and reports:
//   1. client extensions (eglQueryString(EGL_NO_DISPLAY)): platform_base, surfaceless, device...
//   2. eglGetDisplay(EGL_DEFAULT_DISPLAY) + eglInitialize: vendor, version, display extensions
//      (surfaceless_context, image_base, image_dma_buf_import/export, fence_sync, native_fence...)
//   3. a GLES2 context without any surface (EGL_KHR_surfaceless_context), else a 1x1 pbuffer:
//      GL vendor/renderer/version, an FBO render + glReadPixels check (the pixel must come back)
//   4. the cost of what WebKit does per frame in shared-memory mode: render 1280x720 into an FBO
//      and glReadPixels it, 30 times, average ms
//   5. the same with eglGetPlatformDisplayEXT for the platforms the client advertises (optional)
// Run: egl-probe [--platform surfaceless|gbm|wayland|default] [--no-bench]
// Exit code 0 when a context was created and the pixel check passed, 1 otherwise.
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>

#define LOAD(lib, name) \
    do { p_##name = dlsym(lib, #name); if (!p_##name) { printf("missing symbol %s\n", #name); return 1; } } while (0)

static EGLDisplay (*p_eglGetDisplay)(EGLNativeDisplayType);
static EGLBoolean (*p_eglInitialize)(EGLDisplay, EGLint *, EGLint *);
static EGLBoolean (*p_eglTerminate)(EGLDisplay);
static const char *(*p_eglQueryString)(EGLDisplay, EGLint);
static EGLBoolean (*p_eglBindAPI)(EGLenum);
static EGLBoolean (*p_eglChooseConfig)(EGLDisplay, const EGLint *, EGLConfig *, EGLint, EGLint *);
static EGLContext (*p_eglCreateContext)(EGLDisplay, EGLConfig, EGLContext, const EGLint *);
static EGLSurface (*p_eglCreatePbufferSurface)(EGLDisplay, EGLConfig, const EGLint *);
static EGLBoolean (*p_eglMakeCurrent)(EGLDisplay, EGLSurface, EGLSurface, EGLContext);
static EGLBoolean (*p_eglDestroyContext)(EGLDisplay, EGLContext);
static EGLBoolean (*p_eglDestroySurface)(EGLDisplay, EGLSurface);
static EGLint (*p_eglGetError)(void);
static void *(*p_eglGetProcAddress)(const char *);

static const GLubyte *(*p_glGetString)(GLenum);
static GLenum (*p_glGetError)(void);
static void (*p_glGenFramebuffers)(GLsizei, GLuint *);
static void (*p_glBindFramebuffer)(GLenum, GLuint);
static void (*p_glGenRenderbuffers)(GLsizei, GLuint *);
static void (*p_glBindRenderbuffer)(GLenum, GLuint);
static void (*p_glRenderbufferStorage)(GLenum, GLenum, GLsizei, GLsizei);
static void (*p_glFramebufferRenderbuffer)(GLenum, GLenum, GLenum, GLuint);
static GLenum (*p_glCheckFramebufferStatus)(GLenum);
static void (*p_glViewport)(GLint, GLint, GLsizei, GLsizei);
static void (*p_glClearColor)(GLfloat, GLfloat, GLfloat, GLfloat);
static void (*p_glClear)(GLbitfield);
static void (*p_glReadPixels)(GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void *);
static void (*p_glFinish)(void);
static void (*p_glGetIntegerv)(GLenum, GLint *);

static int has(const char *list, const char *ext)
{
    if (!list) return 0;
    size_t n = strlen(ext);
    for (const char *p = list; (p = strstr(p, ext)); p += n)
        if ((p == list || p[-1] == ' ') && (p[n] == ' ' || p[n] == '\0')) return 1;
    return 0;
}

static double now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

static void print_list(const char *title, const char *list)
{
    printf("%s:\n", title);
    if (!list) { printf("  (null)\n"); return; }
    char *copy = strdup(list);
    for (char *tok = strtok(copy, " "); tok; tok = strtok(NULL, " ")) printf("  %s\n", tok);
    free(copy);
}

static void report(const char *list, const char *const *names)
{
    for (; *names; ++names) printf("  %-44s %s\n", *names, has(list, *names) ? "yes" : "no");
}

static int fbo_setup(int w, int h, GLuint *fbo, GLuint *rbo)
{
    p_glGenFramebuffers(1, fbo);
    p_glBindFramebuffer(GL_FRAMEBUFFER, *fbo);
    p_glGenRenderbuffers(1, rbo);
    p_glBindRenderbuffer(GL_RENDERBUFFER, *rbo);
    p_glRenderbufferStorage(GL_RENDERBUFFER, 0x8058, w, h);  // GL_RGBA8_OES; matches 32-bit SHM
    p_glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, *rbo);
    GLenum allocation_error = p_glGetError();
    GLenum st = p_glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (allocation_error != GL_NO_ERROR || st != GL_FRAMEBUFFER_COMPLETE) {
        printf("  RGBA8 unavailable; using RGBA4 (timing is not representative of WebKit)\n");
        p_glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA4, w, h);
        st = p_glCheckFramebufferStatus(GL_FRAMEBUFFER);
    }
    printf("  FBO %dx%d: %s (0x%x)\n", w, h, st == GL_FRAMEBUFFER_COMPLETE ? "complete" : "INCOMPLETE", st);
    return st == GL_FRAMEBUFFER_COMPLETE;
}

int main(int argc, char **argv)
{
    const char *platform = "default";
    int bench = 1, force_pbuffer = 0;
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--platform") && i + 1 < argc) platform = argv[++i];
        else if (!strcmp(argv[i], "--no-bench")) bench = 0;
        else if (!strcmp(argv[i], "--pbuffer")) force_pbuffer = 1;
        else { fprintf(stderr, "usage: %s [--platform default|surfaceless|gbm|wayland] [--pbuffer] [--no-bench]\n", argv[0]); return 2; }
    }

    if (strcmp(platform, "default") && strcmp(platform, "surfaceless") && strcmp(platform, "gbm") && strcmp(platform, "wayland")) {
        fprintf(stderr, "unknown platform: %s\n", platform);
        return 2;
    }
    void *egl = dlopen("libEGL.so.1", RTLD_NOW | RTLD_GLOBAL);
    if (!egl) { printf("dlopen libEGL.so.1: %s\n", dlerror()); return 1; }
    void *gles = dlopen("libGLESv2.so.2", RTLD_NOW | RTLD_GLOBAL);
    if (!gles) { printf("dlopen libGLESv2.so.2: %s\n", dlerror()); return 1; }
    Dl_info info;
    if (dladdr(dlsym(egl, "eglInitialize"), &info)) printf("libEGL: %s\n", info.dli_fname);
    if (dladdr(dlsym(gles, "glGetString"), &info)) printf("libGLESv2: %s\n", info.dli_fname);

    LOAD(egl, eglGetDisplay); LOAD(egl, eglInitialize); LOAD(egl, eglTerminate); LOAD(egl, eglQueryString);
    LOAD(egl, eglBindAPI); LOAD(egl, eglChooseConfig); LOAD(egl, eglCreateContext); LOAD(egl, eglCreatePbufferSurface);
    LOAD(egl, eglMakeCurrent); LOAD(egl, eglDestroyContext); LOAD(egl, eglDestroySurface); LOAD(egl, eglGetError);
    LOAD(egl, eglGetProcAddress);
    LOAD(gles, glGetString); LOAD(gles, glGetError); LOAD(gles, glGenFramebuffers); LOAD(gles, glBindFramebuffer);
    LOAD(gles, glGenRenderbuffers); LOAD(gles, glBindRenderbuffer); LOAD(gles, glRenderbufferStorage);
    LOAD(gles, glFramebufferRenderbuffer); LOAD(gles, glCheckFramebufferStatus); LOAD(gles, glViewport);
    LOAD(gles, glClearColor); LOAD(gles, glClear); LOAD(gles, glReadPixels); LOAD(gles, glFinish); LOAD(gles, glGetIntegerv);

    // 1. client extensions
    const char *client = p_eglQueryString(EGL_NO_DISPLAY, EGL_EXTENSIONS);
    printf("EGL client extensions (eglGetError 0x%x):\n", p_eglGetError());
    static const char *const client_names[] = {
        "EGL_EXT_platform_base", "EGL_KHR_platform_base", "EGL_MESA_platform_surfaceless",
        "EGL_EXT_platform_device", "EGL_EXT_device_base", "EGL_EXT_device_enumeration",
        "EGL_KHR_platform_gbm", "EGL_MESA_platform_gbm", "EGL_KHR_platform_wayland", "EGL_EXT_platform_wayland",
        "EGL_EXT_client_extensions", "EGL_KHR_client_get_all_proc_addresses", NULL };
    report(client, client_names);
    print_list("  (full list)", client);

    // 2. display
    EGLDisplay dpy = EGL_NO_DISPLAY;
    if (!strcmp(platform, "default")) {
        dpy = p_eglGetDisplay(EGL_DEFAULT_DISPLAY);
        printf("eglGetDisplay(EGL_DEFAULT_DISPLAY) = %p (error 0x%x)\n", dpy, p_eglGetError());
    } else {
        EGLDisplay (*getPlatformDisplayEXT)(EGLenum, void *, const EGLint *) =
            (void *)p_eglGetProcAddress("eglGetPlatformDisplayEXT");
        EGLenum id = !strcmp(platform, "surfaceless") ? EGL_PLATFORM_SURFACELESS_MESA
                   : !strcmp(platform, "gbm") ? EGL_PLATFORM_GBM_KHR
                   : !strcmp(platform, "wayland") ? EGL_PLATFORM_WAYLAND_KHR : 0;
        if (!getPlatformDisplayEXT || !id) { printf("platform %s: not available\n", platform); return 1; }
        dpy = getPlatformDisplayEXT(id, EGL_DEFAULT_DISPLAY, NULL);
        printf("eglGetPlatformDisplayEXT(%s) = %p (error 0x%x)\n", platform, dpy, p_eglGetError());
    }
    if (dpy == EGL_NO_DISPLAY) { printf("no display\n"); return 1; }
    EGLint major = 0, minor = 0;
    if (!p_eglInitialize(dpy, &major, &minor)) { printf("eglInitialize failed: 0x%x\n", p_eglGetError()); return 1; }
    printf("EGL %d.%d\n  vendor:  %s\n  version: %s\n  apis:    %s\n", major, minor,
        p_eglQueryString(dpy, EGL_VENDOR), p_eglQueryString(dpy, EGL_VERSION), p_eglQueryString(dpy, EGL_CLIENT_APIS));
    const char *dext = p_eglQueryString(dpy, EGL_EXTENSIONS);
    static const char *const display_names[] = {
        "EGL_KHR_surfaceless_context", "EGL_KHR_no_config_context", "EGL_KHR_create_context",
        "EGL_KHR_image_base", "EGL_KHR_image", "EGL_KHR_gl_texture_2D_image", "EGL_KHR_gl_renderbuffer_image",
        "EGL_EXT_image_dma_buf_import", "EGL_EXT_image_dma_buf_import_modifiers", "EGL_MESA_image_dma_buf_export",
        "EGL_KHR_fence_sync", "EGL_KHR_wait_sync", "EGL_ANDROID_native_fence_sync", "EGL_KHR_reusable_sync",
        "EGL_KHR_lock_surface", "EGL_KHR_lock_surface3", "EGL_WL_bind_wayland_display",
        "EGL_EXT_buffer_age", "EGL_KHR_partial_update", "EGL_EXT_swap_buffers_with_damage", "EGL_KHR_pixmap",
        "EGL_ARM_pixmap_multisample_discard", "EGL_ARM_implicit_external_sync", NULL };
    printf("EGL display extensions:\n");
    report(dext, display_names);
    print_list("  (full list)", dext);

    // 3. context
    if (!p_eglBindAPI(EGL_OPENGL_ES_API)) { printf("eglBindAPI failed: 0x%x\n", p_eglGetError()); return 1; }
    static const EGLint cfg_attrs_pbuffer[] = { EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8, EGL_STENCIL_SIZE, 8, EGL_DEPTH_SIZE, 0, EGL_NONE };
    EGLConfig cfg;
    EGLint n = 0;
    if (!p_eglChooseConfig(dpy, cfg_attrs_pbuffer, &cfg, 1, &n) || n < 1) {
        printf("no RGBA8 + stencil8 pbuffer config matching WebKit (0x%x)\n", p_eglGetError());
        return 1;
    }
    static const EGLint ctx_attrs[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
    EGLContext ctx = p_eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, ctx_attrs);
    if (ctx == EGL_NO_CONTEXT) { printf("eglCreateContext failed: 0x%x\n", p_eglGetError()); return 1; }
    EGLSurface surf = EGL_NO_SURFACE;
    const char *mode = "surfaceless";
    if (force_pbuffer || !has(dext, "EGL_KHR_surfaceless_context") || !p_eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx)) {
        printf("surfaceless make-current: %s (0x%x); trying a 1x1 pbuffer\n",
            force_pbuffer ? "forced pbuffer" : has(dext, "EGL_KHR_surfaceless_context") ? "failed" : "extension absent", p_eglGetError());
        static const EGLint pb[] = { EGL_WIDTH, 1, EGL_HEIGHT, 1, EGL_NONE };
        surf = p_eglCreatePbufferSurface(dpy, cfg, pb);
        if (surf == EGL_NO_SURFACE) { printf("eglCreatePbufferSurface failed: 0x%x\n", p_eglGetError()); return 1; }
        if (!p_eglMakeCurrent(dpy, surf, surf, ctx)) { printf("pbuffer make-current failed: 0x%x\n", p_eglGetError()); return 1; }
        mode = "pbuffer";
    }
    printf("context current (%s)\n  GL vendor:   %s\n  GL renderer: %s\n  GL version:  %s\n", mode,
        p_glGetString(GL_VENDOR), p_glGetString(GL_RENDERER), p_glGetString(GL_VERSION));
    const char *gext = (const char *)p_glGetString(GL_EXTENSIONS);
    static const char *const gl_names[] = { "GL_OES_EGL_image", "GL_OES_EGL_image_external", "GL_OES_rgb8_rgba8",
        "GL_EXT_texture_format_BGRA8888", "GL_EXT_read_format_bgra", "GL_OES_mapbuffer", "GL_EXT_map_buffer_range",
        "GL_OES_texture_npot", "GL_EXT_texture_storage", "GL_KHR_debug", "GL_EXT_robustness", NULL };
    printf("GL extensions:\n");
    report(gext, gl_names);
    GLint max_tex = 0, max_rb = 0;
    p_glGetIntegerv(GL_MAX_TEXTURE_SIZE, &max_tex);
    p_glGetIntegerv(GL_MAX_RENDERBUFFER_SIZE, &max_rb);
    printf("  max texture %d, max renderbuffer %d\n", max_tex, max_rb);

    GLuint fbo, rbo;
    if (!fbo_setup(64, 64, &fbo, &rbo)) return 1;
    p_glViewport(0, 0, 64, 64);
    p_glClearColor(1.0f, 0.5f, 0.0f, 1.0f);
    p_glClear(GL_COLOR_BUFFER_BIT);
    uint8_t px[4] = { 0 };
    p_glReadPixels(10, 10, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
    GLenum pixel_error = p_glGetError();
    int ok = pixel_error == GL_NO_ERROR && px[0] > 240 && px[1] > 100 && px[1] < 160 && px[2] < 16 && px[3] > 240;
    printf("  pixel check: %u %u %u %u -> %s (glGetError 0x%x)\n", px[0], px[1], px[2], px[3], ok ? "OK" : "WRONG", pixel_error);

    // 4. readback cost
    if (bench && ok) {
        GLuint fbo2, rbo2;
        if (fbo_setup(1280, 720, &fbo2, &rbo2)) {
            p_glViewport(0, 0, 1280, 720);
            uint8_t *buf = malloc(1280 * 720 * 4);
            if (!buf) { fprintf(stderr, "readback allocation failed\n"); return 1; }
            double total = 0, best = 1e9;
            for (int i = 0; i < 30; ++i) {
                double t0 = now_ms();
                p_glClearColor(i / 30.0f, 0.2f, 0.4f, 1.0f);
                p_glClear(GL_COLOR_BUFFER_BIT);
                p_glReadPixels(0, 0, 1280, 720, GL_RGBA, GL_UNSIGNED_BYTE, buf);
                double dt = now_ms() - t0;
                total += dt;
                if (dt < best) best = dt;
            }
            GLenum bench_error = p_glGetError();
            if (bench_error != GL_NO_ERROR) ok = 0;
            printf("  1280x720 clear + glReadPixels: avg %.1f ms, best %.1f ms (30 runs, glGetError 0x%x)\n",
                total / 30, best, bench_error);
            free(buf);
        } else ok = 0;
    }

    p_eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    if (surf != EGL_NO_SURFACE) p_eglDestroySurface(dpy, surf);
    p_eglDestroyContext(dpy, ctx);
    p_eglTerminate(dpy);
    printf("result: %s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
