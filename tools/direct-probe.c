// SPDX-License-Identifier: GPL-3.0-or-later
// Direct-presentation plumbing check for a staged softfp engine, on this PC (binfmt QEMU; tools/direct-probe.sh).
// There is no webOS compositor here, so the page cannot reach a screen; what this checks is everything up to that:
//   - the engine loads libWPEBackend-webos.so and reports WPE_PRESENT_DIRECT at start,
//   - WebKit creates the tab with the libwpe backend, the web process loads the same backend (wpe_loader),
//   - the page's drawing target reports its status back through the per-view channel (expected here:
//     WPEB_WEBOS_STATUS_NO_WAYLAND, -1), and the engine forwards it as WPE_ENGINE_PRESENT for the tab,
//   - the page itself still loads and runs JavaScript (title), i.e. the web process is healthy.
//   direct-probe ENGINE_DIR
#define _GNU_SOURCE
#include "wpe_ipc.h"
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static int send_packet(int fd, unsigned type, unsigned tab, const void *data, unsigned size)
{
    unsigned char packet[WPE_IPC_MAX_PACKET];
    if (size > sizeof(packet) - sizeof(WpeMsgHeader)) return 0;
    WpeMsgHeader h = {type, size, tab};
    memcpy(packet, &h, sizeof(h));
    if (size) memcpy(packet + sizeof(h), data, size);
    return send(fd, packet, sizeof(h) + size, MSG_NOSIGNAL) == (ssize_t)(sizeof(h) + size);
}

static double seconds(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec / 1e9;
}

int main(int argc, char **argv)
{
    if (argc != 2) { fprintf(stderr, "usage: %s ENGINE_DIR\n", argv[0]); return 2; }
    setvbuf(stdout, NULL, _IOLBF, 0);
    char runtime[] = "/tmp/wpe-direct-probe-XXXXXX";
    if (!mkdtemp(runtime)) return 1;
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sv)) return 1;
    pid_t child = fork();
    if (child == 0) {
        close(sv[0]);
        if (dup2(sv[1], 3) < 0 || fcntl(3, F_SETFD, 0) < 0) _exit(125);
        char path[2048];
        snprintf(path, sizeof(path), "%s/engine.log", runtime);
        int log = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
        if (log >= 0) { dup2(log, 1); dup2(log, 2); }
        const char *d = argv[1];
        // Mesa stands in for the TV's EGL here; the backend's libwayland comes from QEMU_LD_PREFIX.
        snprintf(path, sizeof(path), "%s/lib:%s/lib/mesa:%s", d, d, getenv("DIRECT_PROBE_EXTRA_LIBS") ? getenv("DIRECT_PROBE_EXTRA_LIBS") : "");
        setenv("LD_LIBRARY_PATH", path, 1);
        snprintf(path, sizeof(path), "%s/libexec", d); setenv("WEBKIT_EXEC_PATH", path, 1);
        snprintf(path, sizeof(path), "%s/lib/gio/modules", d); setenv("GIO_MODULE_DIR", path, 1);
        snprintf(path, sizeof(path), "%s/etc/fonts/fonts.conf", d); setenv("FONTCONFIG_FILE", path, 1);
        snprintf(path, sizeof(path), "%s/lib/injected-bundle", d); setenv("WEBKIT_INJECTED_BUNDLE_PATH", path, 1);
        setenv("GIO_EXTRA_MODULES", "", 1);
        setenv("WPE_BROWSER_DATA_DIR", runtime, 1);
        setenv("WPE_BROWSER_CACHE_DIR", runtime, 1);
        setenv("XDG_DATA_HOME", runtime, 1); setenv("XDG_CACHE_HOME", runtime, 1); setenv("XDG_CONFIG_HOME", runtime, 1);
        setenv("WEBKIT_SKIA_ENABLE_CPU_RENDERING", "1", 1);
        setenv("GALLIUM_DRIVER", "llvmpipe", 1);
        setenv("WPE_BROWSER_PRESENT", "direct", 1);
        setenv("WPEB_SURFACE_GROUP", "probe-group", 1);
        setenv("WPEB_SURFACE_LAYER", "page", 1);
        setenv("WPEB_WAYLAND_SOCKET", "/nonexistent/wayland-probe", 1);
        snprintf(path, sizeof(path), "%s/wpe-engine", d);
        execl(path, path, (char *)NULL);
        _exit(126);
    }
    close(sv[1]);
    WpeAppHello hello = {WPE_IPC_VERSION, 640, 360, 0};
    send_packet(sv[0], WPE_APP_HELLO, 0, &hello, sizeof(hello));
    WpeText text = {0};
    const char *uri = "data:text/html,<body>direct<script>document.title='direct-probe:'+(6*7)</script>";
    text.length = (unsigned)strlen(uri);
    memcpy(text.text, uri, text.length);

    int opened = 0, mode_direct = -1, tab_status = 0, title_ok = 0, frames = 0;
    const double start = seconds(), deadline = start + 300;
    while (seconds() < deadline && !(tab_status && title_ok)) {
        struct pollfd pfd = {sv[0], POLLIN, 0};
        if (poll(&pfd, 1, 1000) <= 0) continue;
        unsigned char packet[WPE_IPC_MAX_PACKET];
        char control[CMSG_SPACE(sizeof(int))];
        struct iovec iov = {packet, sizeof(packet)};
        struct msghdr msg = {0};
        msg.msg_iov = &iov; msg.msg_iovlen = 1; msg.msg_control = control; msg.msg_controllen = sizeof(control);
        ssize_t count = recvmsg(sv[0], &msg, MSG_CMSG_CLOEXEC);
        if (count < (ssize_t)sizeof(WpeMsgHeader)) break;
        WpeMsgHeader h; memcpy(&h, packet, sizeof(h));
        size_t length = (size_t)count - sizeof(h);
        unsigned char *payload = packet + sizeof(h);
        for (struct cmsghdr *c = CMSG_FIRSTHDR(&msg); c; c = CMSG_NXTHDR(&msg, c))
            if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_RIGHTS) { int fd; memcpy(&fd, CMSG_DATA(c), sizeof(fd)); close(fd); }
        if (h.type == WPE_ENGINE_READY && !opened) {
            opened = 1;
            send_packet(sv[0], WPE_APP_TAB_OPEN, 1, &text, (unsigned)(offsetof(WpeText, text) + text.length + 1));
            send_packet(sv[0], WPE_APP_TAB_ACTIVATE, 1, NULL, 0);
            WpeFlag visible = {1}; send_packet(sv[0], WPE_APP_VISIBLE, 0, &visible, sizeof(visible));
            printf("engine ready after %.1f s; page opened\n", seconds() - start);
        } else if (h.type == WPE_ENGINE_PRESENT && length >= sizeof(WpePresent)) {
            WpePresent p; memcpy(&p, payload, sizeof(p));
            printf("present: tab %u mode %u status %d (%ux%u) at %.1f s\n", h.tab, p.mode, p.status, p.width, p.height,
                seconds() - start);
            if (h.tab == 0) mode_direct = p.mode == WPE_PRESENT_DIRECT;
            else if (p.status < 0 && tab_status == 0) tab_status = p.status;
        } else if (h.type == WPE_ENGINE_TITLE && length >= offsetof(WpeText, text)) {
            WpeText t = {0}; memcpy(&t, payload, length < sizeof(t) ? length : sizeof(t));
            t.text[sizeof(t.text) - 1] = 0;
            printf("title: %s\n", t.text);
            if (strcmp(t.text, "direct-probe:42") == 0) title_ok = 1;
        } else if (h.type == WPE_ENGINE_FRAME && length >= offsetof(WpeFrame, damage)) {
            WpeFrame f = {0}; memcpy(&f, payload, length < sizeof(f) ? length : sizeof(f));
            ++frames; // must not happen in direct mode
            WpeFrameDone done = {f.slot, f.serial}; send_packet(sv[0], WPE_APP_FRAME_DONE, 0, &done, sizeof(done));
        }
    }
    send_packet(sv[0], WPE_APP_QUIT, 0, NULL, 0);
    close(sv[0]);
    int status = 0;
    const double stop = seconds() + 5;
    while (waitpid(child, &status, WNOHANG) == 0 && seconds() < stop) usleep(20000);
    if (waitpid(child, &status, WNOHANG) == 0) { kill(child, SIGKILL); waitpid(child, &status, 0); }
    const int ok = mode_direct == 1 && tab_status == -1 && title_ok && frames == 0;
    printf("result: %s (direct mode=%d, tab surface status=%d (want -1: no compositor here), page JS=%d, SHM frames=%d) log=%s/engine.log\n",
        ok ? "PASS" : "FAIL", mode_direct, tab_status, title_ok, frames, runtime);
    return ok ? 0 : 1;
}
