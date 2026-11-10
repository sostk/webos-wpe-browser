// SPDX-License-Identifier: GPL-3.0-or-later
// Headless IPC smoke test. Writes test data only beneath a fresh /tmp directory.
#define _GNU_SOURCE
#include "wpe_ipc.h"
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <poll.h>
#include <signal.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
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
static void show_maps(pid_t parent)
{
    DIR *dir = opendir("/proc");
    if (!dir) return;
    struct dirent *entry;
    while ((entry = readdir(dir))) {
        if (!isdigit((unsigned char)entry->d_name[0])) continue;
        char path[512], line[1024];
        snprintf(path, sizeof(path), "/proc/%s/status", entry->d_name);
        FILE *file = fopen(path, "r");
        if (!file) continue;
        int ppid = -1;
        while (fgets(line, sizeof(line), file)) if (sscanf(line, "PPid: %d", &ppid) == 1) break;
        fclose(file);
        if (ppid != parent) continue;
        snprintf(path, sizeof(path), "/proc/%s/maps", entry->d_name);
        file = fopen(path, "r");
        if (!file) continue;
        while (fgets(line, sizeof(line), file)) {
            if (strstr(line, "libmali") || strstr(line, "libgallium") || strstr(line, "/mesa/libEGL"))
                printf("child %s: %s", entry->d_name, line);
        }
        fclose(file);
    }
    closedir(dir);
}
int main(int argc, char **argv)
{
    if ((argc != 3 && argc != 5) || (strcmp(argv[2], "gpu") && strcmp(argv[2], "gpu-cpu") && strcmp(argv[2], "software"))) {
        fprintf(stderr, "usage: %s ENGINE_DIR gpu|gpu-cpu|software [UID GID]\n", argv[0]); return 2;
    }
    if (argc == 5) {
        char *end_uid, *end_gid;
        unsigned long uid = strtoul(argv[3], &end_uid, 10), gid = strtoul(argv[4], &end_gid, 10);
        if (*end_uid || *end_gid || !uid || uid > 65535 || !gid || gid > 65535) return 2;
        if (setgroups(0, NULL) || setgid((gid_t)gid) || setuid((uid_t)uid)) { perror("drop test identity"); return 1; }
    }
    setvbuf(stdout, NULL, _IOLBF, 0);
    char runtime[] = "/tmp/wpe-engine-probe-XXXXXX";
    if (!mkdtemp(runtime)) return 1;
    printf("runtime: %s; renderer: %s; uid: %u\n", runtime, argv[2], (unsigned)getuid());
    int sockets[2];
    if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sockets)) return 1;
    pid_t child = fork();
    if (child == 0) {
        close(sockets[0]);
        if (dup2(sockets[1], 3) < 0 || fcntl(3, F_SETFD, 0) < 0) _exit(125);
        if (sockets[1] != 3) close(sockets[1]);
        char path[2048];
        snprintf(path, sizeof(path), "%s/engine.log", runtime);
        int log = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
        if (log >= 0) { dup2(log, 1); dup2(log, 2); if (log > 3) close(log); }
        snprintf(path, sizeof(path), "%s/lib", argv[1]);
        if (!strcmp(argv[2], "software")) snprintf(path, sizeof(path), "%s/lib:%s/lib/mesa", argv[1], argv[1]);
        setenv("LD_LIBRARY_PATH", path, 1);
        snprintf(path, sizeof(path), "%s/libexec", argv[1]); setenv("WEBKIT_EXEC_PATH", path, 1);
        snprintf(path, sizeof(path), "%s/lib/gio/modules", argv[1]); setenv("GIO_MODULE_DIR", path, 1);
        setenv("GIO_EXTRA_MODULES", "", 1);
        snprintf(path, sizeof(path), "%s/etc/fonts/fonts.conf", argv[1]); setenv("FONTCONFIG_FILE", path, 1);
        setenv("WPE_BROWSER_DATA_DIR", runtime, 1);
        setenv("WPE_BROWSER_CACHE_DIR", runtime, 1);
        setenv("XDG_DATA_HOME", runtime, 1); setenv("XDG_CACHE_HOME", runtime, 1);
        setenv("XDG_CONFIG_HOME", runtime, 1);
        setenv("WEBKIT_DISABLE_DEFAULT_EGL_DISPLAY", "0", 1);
        setenv("WEBKIT_SKIA_ENABLE_CPU_RENDERING", !strcmp(argv[2], "gpu") ? "0" : "1", 1);
        setenv("GALLIUM_DRIVER", "llvmpipe", 1);
        snprintf(path, sizeof(path), "%s/wpe-engine", argv[1]);
        execl(path, path, (char *)NULL);
        _exit(126);
    }
    if (child < 0) return 1;
    close(sockets[1]);
    WpeAppHello hello = {WPE_IPC_VERSION, 640, 360, 0};
    send_packet(sockets[0], WPE_APP_HELLO, 0, &hello, sizeof(hello));
    const char *html = "<style>html,body{margin:0;background:rgb(32,80,160)}canvas{display:block}</style><canvas id=c width=64 height=64></canvas><script>var g=c.getContext('webgl');document.title='gpu-probe:'+!!g+':'+(g?g.getParameter(g.RENDERER):'none');if(g){g.clearColor(1,0,0,1);g.clear(g.COLOR_BUFFER_BIT)}</script>";
    WpeText text = {0};
    strcpy(text.text, "data:text/html,");
    size_t used = strlen(text.text);
    for (const unsigned char *p = (const unsigned char *)html; *p; ++p) {
        if (used + 4 >= sizeof(text.text)) return 1;
        used += (size_t)sprintf(text.text + used, "%%%02X", *p);
    }
    text.length = (unsigned)used;
    unsigned char *mapping = NULL;
    size_t map_size = 0, slot_bytes = 0;
    int opened = 0, background_ok = 0, webgl_ok = 0, canvas_ok = 0, frames = 0;
    double deadline = seconds() + 40;
    while (seconds() < deadline && !(background_ok && webgl_ok && canvas_ok)) {
        struct pollfd pollfd = {sockets[0], POLLIN, 0};
        if (poll(&pollfd, 1, 1000) <= 0) continue;
        unsigned char packet[WPE_IPC_MAX_PACKET];
        char control[CMSG_SPACE(sizeof(int))];
        struct iovec iov = {packet, sizeof(packet)};
        struct msghdr msg = {0};
        msg.msg_iov = &iov; msg.msg_iovlen = 1; msg.msg_control = control; msg.msg_controllen = sizeof(control);
        ssize_t count = recvmsg(sockets[0], &msg, MSG_CMSG_CLOEXEC);
        if (count < (ssize_t)sizeof(WpeMsgHeader)) break;
        WpeMsgHeader header; memcpy(&header, packet, sizeof(header));
        size_t length = (size_t)count - sizeof(header);
        unsigned char *payload = packet + sizeof(header);
        int received_fd = -1;
        for (struct cmsghdr *c = CMSG_FIRSTHDR(&msg); c; c = CMSG_NXTHDR(&msg, c))
            if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_RIGHTS) memcpy(&received_fd, CMSG_DATA(c), sizeof(int));
        if (header.type == WPE_ENGINE_READY && !opened) {
            opened = 1;
            send_packet(sockets[0], WPE_APP_TAB_OPEN, 1, &text, (unsigned)(offsetof(WpeText, text) + used + 1));
            send_packet(sockets[0], WPE_APP_TAB_ACTIVATE, 1, NULL, 0);
            WpeFlag visible = {1}; send_packet(sockets[0], WPE_APP_VISIBLE, 0, &visible, sizeof(visible));
            puts("engine ready; test page opened");
        } else if (header.type == WPE_ENGINE_FRAMES && received_fd >= 0 && length >= sizeof(WpeFrames)) {
            WpeFrames f; memcpy(&f, payload, sizeof(f));
            if (mapping) munmap(mapping, map_size);
            slot_bytes = f.slot_bytes; map_size = slot_bytes * WPE_IPC_FRAME_SLOTS;
            mapping = mmap(NULL, map_size, PROT_READ, MAP_SHARED, received_fd, 0);
            if (mapping == MAP_FAILED) mapping = NULL;
        } else if (header.type == WPE_ENGINE_TITLE && length >= offsetof(WpeText, text)) {
            WpeText title = {0}; memcpy(&title, payload, length < sizeof(title) ? length : sizeof(title));
            title.text[sizeof(title.text) - 1] = 0;
            printf("title: %s\n", title.text);
            if (strstr(title.text, "gpu-probe:true:")) webgl_ok = 1;
        } else if (header.type == WPE_ENGINE_FRAME && length >= offsetof(WpeFrame, damage)) {
            WpeFrame f = {0}; memcpy(&f, payload, length < sizeof(f) ? length : sizeof(f));
            ++frames;
            if (mapping && f.slot < WPE_IPC_FRAME_SLOTS && f.width > 100 && f.height > 100 && (size_t)f.stride * f.height <= slot_bytes) {
                const unsigned char *blue = mapping + f.slot * slot_bytes + 100 * f.stride + 100 * 4;
                const unsigned char *red = mapping + f.slot * slot_bytes + 10 * f.stride + 10 * 4;
                background_ok |= blue[0] == 160 && blue[1] == 80 && blue[2] == 32;
                canvas_ok |= red[0] < 5 && red[1] < 5 && red[2] > 250;
                char shot_path[256];
                snprintf(shot_path, sizeof(shot_path), "%s/frame.ppm", runtime);
                FILE *shot = fopen(shot_path, "wb");
                if (shot) {
                    fprintf(shot, "P6\n%u %u\n255\n", f.width, f.height);
                    for (unsigned y = 0; y < f.height; ++y) for (unsigned x = 0; x < f.width; ++x) {
                        const unsigned char *px = mapping + f.slot * slot_bytes + y * f.stride + x * 4;
                        unsigned char rgb[] = {px[2], px[1], px[0]};
                        fwrite(rgb, 1, 3, shot);
                    }
                    fclose(shot);
                }
                printf("frame %d %ux%u: background BGRA=%u,%u,%u,%u canvas=%u,%u,%u,%u\n", frames, f.width, f.height, blue[0], blue[1], blue[2], blue[3], red[0], red[1], red[2], red[3]);
            }
            WpeFrameDone done = {f.slot, f.serial}; send_packet(sockets[0], WPE_APP_FRAME_DONE, 0, &done, sizeof(done));
        } else if (header.type == WPE_ENGINE_ERROR) puts("engine reported an error; inspect engine.log");
        if (received_fd >= 0) close(received_fd);
    }
    show_maps(child);
    send_packet(sockets[0], WPE_APP_QUIT, 0, NULL, 0);
    close(sockets[0]);
    if (mapping) munmap(mapping, map_size);
    int status = 0;
    double stop_deadline = seconds() + 3;
    while (waitpid(child, &status, WNOHANG) == 0 && seconds() < stop_deadline) usleep(10000);
    if (waitpid(child, &status, WNOHANG) == 0) { kill(child, SIGKILL); waitpid(child, &status, 0); }
    printf("result: background=%d WebGL=%d canvas=%d frames=%d log=%s/engine.log\n", background_ok, webgl_ok, canvas_ok, frames, runtime);
    return background_ok && webgl_ok && canvas_ok ? 0 : 1;
}
