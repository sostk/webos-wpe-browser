// SPDX-License-Identifier: GPL-3.0-or-later
// Load one URL in a staged engine on this PC (binfmt QEMU; tools/page-shot.sh) and save the page's last frame as a
// PPM, to look at rendering bugs without the TV. Software rendering (Skia CPU), like the TV's default.
//   page-shot ENGINE_DIR URL OUT.ppm [WIDTH HEIGHT [SETTLE_SECONDS]]
#define _GNU_SOURCE
#include "wpe_ipc.h"
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
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
    if (argc != 4 && argc != 6 && argc != 7) {
        fprintf(stderr, "usage: %s ENGINE_DIR URL OUT.ppm [WIDTH HEIGHT [SETTLE_SECONDS]]\n", argv[0]);
        return 2;
    }
    setvbuf(stdout, NULL, _IOLBF, 0);
    const unsigned width = argc >= 6 ? (unsigned)atoi(argv[4]) : 960, height = argc >= 6 ? (unsigned)atoi(argv[5]) : 540;
    const double settle = argc == 7 ? atof(argv[6]) : 3;
    char runtime[] = "/tmp/wpe-page-shot-XXXXXX";
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
        snprintf(path, sizeof(path), "%s/etc/fonts/fonts.conf", d); setenv("FONTCONFIG_FILE", path, 1);
        snprintf(path, sizeof(path), "%s/lib/injected-bundle", d); setenv("WEBKIT_INJECTED_BUNDLE_PATH", path, 1);
        setenv("GIO_EXTRA_MODULES", "", 1);
        setenv("WPE_BROWSER_DATA_DIR", runtime, 1); setenv("WPE_BROWSER_CACHE_DIR", runtime, 1);
        setenv("XDG_DATA_HOME", runtime, 1); setenv("XDG_CACHE_HOME", runtime, 1); setenv("XDG_CONFIG_HOME", runtime, 1);
        setenv("WEBKIT_SKIA_ENABLE_CPU_RENDERING", "1", 1);
        setenv("GALLIUM_DRIVER", "llvmpipe", 1);
        snprintf(path, sizeof(path), "%s/wpe-engine", d);
        execl(path, path, (char *)NULL);
        _exit(126);
    }
    close(sv[1]);
    WpeAppHello hello = {WPE_IPC_VERSION, width, height, 0};
    send_packet(sv[0], WPE_APP_HELLO, 0, &hello, sizeof(hello));
    WpeText text = {0};
    text.length = (unsigned)strlen(argv[2]);
    if (text.length >= sizeof(text.text)) return 2;
    memcpy(text.text, argv[2], text.length);

    unsigned char *mapping = NULL;
    size_t map_size = 0, slot_bytes = 0;
    int opened = 0, frames = 0, saved = 0;
    double finished_at = 0;
    const double deadline = seconds() + 300;
    while (seconds() < deadline && !(finished_at && seconds() > finished_at + settle)) {
        struct pollfd pfd = {sv[0], POLLIN, 0};
        if (poll(&pfd, 1, 200) <= 0) continue;
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
        int fd = -1;
        for (struct cmsghdr *c = CMSG_FIRSTHDR(&msg); c; c = CMSG_NXTHDR(&msg, c))
            if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_RIGHTS) memcpy(&fd, CMSG_DATA(c), sizeof(int));
        if (h.type == WPE_ENGINE_READY && !opened) {
            opened = 1;
            send_packet(sv[0], WPE_APP_TAB_OPEN, 1, &text, (unsigned)(offsetof(WpeText, text) + text.length + 1));
            send_packet(sv[0], WPE_APP_TAB_ACTIVATE, 1, NULL, 0);
            WpeFlag visible = {1}; send_packet(sv[0], WPE_APP_VISIBLE, 0, &visible, sizeof(visible));
        } else if (h.type == WPE_ENGINE_FRAMES && fd >= 0 && length >= sizeof(WpeFrames)) {
            WpeFrames f; memcpy(&f, payload, sizeof(f));
            if (mapping) munmap(mapping, map_size);
            slot_bytes = f.slot_bytes; map_size = slot_bytes * WPE_IPC_FRAME_SLOTS;
            mapping = mmap(NULL, map_size, PROT_READ, MAP_SHARED, fd, 0);
            if (mapping == MAP_FAILED) mapping = NULL;
        } else if (h.type == WPE_ENGINE_LOAD && length >= sizeof(WpeLoad)) {
            WpeLoad l; memcpy(&l, payload, sizeof(l));
            if ((l.event == WPE_LOAD_FINISHED || l.event == WPE_LOAD_FAILED) && !finished_at) {
                finished_at = seconds();
                printf("load %s\n", l.event == WPE_LOAD_FINISHED ? "finished" : "FAILED");
            }
        } else if (h.type == WPE_ENGINE_FRAME && length >= offsetof(WpeFrame, damage)) {
            WpeFrame f = {0}; memcpy(&f, payload, length < sizeof(f) ? length : sizeof(f));
            ++frames;
            if (mapping && f.slot < WPE_IPC_FRAME_SLOTS && (size_t)f.stride * f.height <= slot_bytes) {
                FILE *out = fopen(argv[3], "wb");
                if (out) {
                    fprintf(out, "P6\n%u %u\n255\n", f.width, f.height);
                    for (unsigned y = 0; y < f.height; ++y) for (unsigned x = 0; x < f.width; ++x) {
                        const unsigned char *px = mapping + f.slot * slot_bytes + (size_t)y * f.stride + x * 4;
                        unsigned char rgb[] = {px[2], px[1], px[0]};
                        fwrite(rgb, 1, 3, out);
                    }
                    fclose(out);
                    saved = 1;
                }
            }
            WpeFrameDone done = {f.slot, f.serial}; send_packet(sv[0], WPE_APP_FRAME_DONE, 0, &done, sizeof(done));
        }
        if (fd >= 0) close(fd);
    }
    send_packet(sv[0], WPE_APP_QUIT, 0, NULL, 0);
    close(sv[0]);
    int status = 0;
    const double stop = seconds() + 5;
    while (waitpid(child, &status, WNOHANG) == 0 && seconds() < stop) usleep(20000);
    if (waitpid(child, &status, WNOHANG) == 0) { kill(child, SIGKILL); waitpid(child, &status, 0); }
    printf("frames %d, %s %s; log %s/engine.log\n", frames, saved ? "saved" : "NOT saved", argv[3], runtime);
    return saved ? 0 : 1;
}
