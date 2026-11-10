// SPDX-License-Identifier: GPL-3.0-or-later
// JavaScript/WebAssembly and device-scale probe for a staged engine. Host-only: build natively, the ARM engine
// runs through binfmt QEMU (tools/js-probe.sh). Measures a fixed JS workload and checks WebAssembly; with
// a scale it also checks that frames come at CSS size x scale (the "Sharp text" setting).
//   js-probe ENGINE_DIR [SCALE_PERMILLE]
#define _GNU_SOURCE
#include "wpe_ipc.h"
#include <poll.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <signal.h>
#include <fcntl.h>
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
    if (argc != 2 && argc != 3) { fprintf(stderr, "usage: %s ENGINE_DIR [SCALE_PERMILLE]\n", argv[0]); return 2; }
    const unsigned scale = argc == 3 ? (unsigned)strtoul(argv[2], NULL, 10) : 0;
    setvbuf(stdout, NULL, _IOLBF, 0);
    char runtime[] = "/tmp/wpe-js-probe-XXXXXX";
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
        snprintf(path, sizeof(path), "%s/lib:%s/lib/mesa", d, d); setenv("LD_LIBRARY_PATH", path, 1);
        snprintf(path, sizeof(path), "%s/libexec", d); setenv("WEBKIT_EXEC_PATH", path, 1);
        snprintf(path, sizeof(path), "%s/lib/gio/modules", d); setenv("GIO_MODULE_DIR", path, 1);
        snprintf(path, sizeof(path), "%s/lib/gconv", d); setenv("GCONV_PATH", path, 1);
        snprintf(path, sizeof(path), "%s/etc/fonts/fonts.conf", d); setenv("FONTCONFIG_FILE", path, 1);
        snprintf(path, sizeof(path), "%s/lib/injected-bundle", d); setenv("WEBKIT_INJECTED_BUNDLE_PATH", path, 1);
        setenv("GIO_EXTRA_MODULES", "", 1);
        setenv("WPE_BROWSER_DATA_DIR", runtime, 1);
        setenv("WPE_BROWSER_CACHE_DIR", runtime, 1);
        setenv("XDG_DATA_HOME", runtime, 1); setenv("XDG_CACHE_HOME", runtime, 1); setenv("XDG_CONFIG_HOME", runtime, 1);
        setenv("WEBKIT_SKIA_ENABLE_CPU_RENDERING", "1", 1);
        setenv("GALLIUM_DRIVER", "llvmpipe", 1);
        snprintf(path, sizeof(path), "%s/wpe-engine", d);
        execl(path, path, (char *)NULL);
        _exit(126);
    }
    close(sv[1]);
    WpeAppHello hello = {WPE_IPC_VERSION, 640, 360, 0};
    send_packet(sv[0], WPE_APP_HELLO, 0, &hello, sizeof(hello));
    // fib(27) + sort of 200k numbers: a few seconds in an interpreter, much less with the JIT. A tiny
    // WebAssembly module (export add(i32,i32)) must compile and run.
    const char *html =
        "<body style='margin:0;background:#fff;font:20px sans-serif'>probe<script>"
        "function fib(n){return n<2?n:fib(n-1)+fib(n-2)}"
        "var t0=performance.now();var f=fib(27);var a=[];for(var i=0;i<200000;i++)a.push((i*7919)%100003);a.sort(function(x,y){return x-y});"
        "var ms=Math.round(performance.now()-t0);var w='none';"
        "try{var m=new WebAssembly.Instance(new WebAssembly.Module(new Uint8Array([0,97,115,109,1,0,0,0,1,7,1,96,2,127,127,1,127,3,2,1,0,7,7,1,3,97,100,100,0,0,10,9,1,7,0,32,0,32,1,106,11])));w=m.exports.add(2,3)==5?'ok':'wrong'}catch(e){w='error:'+e}"
        "document.title='js-probe:'+ms+':fib='+f+':wasm='+w</script>";
    WpeText text = {0};
    strcpy(text.text, "data:text/html,");
    size_t used = strlen(text.text);
    for (const unsigned char *p = (const unsigned char *)html; *p; ++p) {
        if (used + 4 >= sizeof(text.text)) { fputs("page too long\n", stderr); return 1; }
        used += (size_t)sprintf(text.text + used, "%%%02X", *p);
    }
    text.length = (unsigned)used;

    int opened = 0, js_ok = 0, wasm_ok = 0, scale_ok = scale == 0, frames = 0;
    unsigned want_w = scale ? 640 * scale / 1000 : 640, last_w = 0;
    double deadline = seconds() + 300;
    while (seconds() < deadline && !(js_ok && scale_ok)) {
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
            if (scale) { WpeZoom z = {scale}; send_packet(sv[0], WPE_APP_SET_SCALE, 0, &z, sizeof(z)); }
            send_packet(sv[0], WPE_APP_TAB_OPEN, 1, &text, (unsigned)(offsetof(WpeText, text) + used + 1));
            send_packet(sv[0], WPE_APP_TAB_ACTIVATE, 1, NULL, 0);
            WpeFlag visible = {1}; send_packet(sv[0], WPE_APP_VISIBLE, 0, &visible, sizeof(visible));
            printf("engine ready after %.1f s; page opened\n", 300 - (deadline - seconds()));
        } else if (h.type == WPE_ENGINE_TITLE && length >= offsetof(WpeText, text)) {
            WpeText t = {0}; memcpy(&t, payload, length < sizeof(t) ? length : sizeof(t));
            t.text[sizeof(t.text) - 1] = 0;
            if (strncmp(t.text, "js-probe:", 9) == 0) {
                printf("title: %s\n", t.text);
                js_ok = strstr(t.text, ":fib=196418:") != NULL;
                wasm_ok = strstr(t.text, ":wasm=ok") != NULL;
            }
        } else if (h.type == WPE_ENGINE_FRAME && length >= offsetof(WpeFrame, damage)) {
            WpeFrame f = {0}; memcpy(&f, payload, length < sizeof(f) ? length : sizeof(f));
            ++frames;
            last_w = f.width;
            if (scale && f.width == want_w) scale_ok = 1;
            WpeFrameDone done = {f.slot, f.serial}; send_packet(sv[0], WPE_APP_FRAME_DONE, 0, &done, sizeof(done));
        }
    }
    send_packet(sv[0], WPE_APP_QUIT, 0, NULL, 0);
    close(sv[0]);
    int status = 0;
    double stop = seconds() + 5;
    while (waitpid(child, &status, WNOHANG) == 0 && seconds() < stop) usleep(20000);
    if (waitpid(child, &status, WNOHANG) == 0) { kill(child, SIGKILL); waitpid(child, &status, 0); }
    printf("result: js=%d wasm=%d scale=%d (frame width %u, want %u) frames=%d log=%s/engine.log\n",
        js_ok, wasm_ok, scale_ok, last_w, want_w, frames, runtime);
    return js_ok && scale_ok ? 0 : 1;
}
