// SPDX-License-Identifier: GPL-3.0-or-later
// Sampling profiler for one thread on the TV (ARM32), for finding where WebKit's main thread spends a page load.
// Every PERIOD_MS: PTRACE_INTERRUPT the thread, record pc, lr, sp and the top STACK_BYTES of its stack, continue it.
// Output: OUT/maps (copy of /proc/PID/maps) and OUT/samples.bin (records below); tools/tv-sampler/report.py
// resolves them on the PC against unstripped libraries. Read-only towards the target except the brief stops.
//   sampler TID SECONDS OUT_DIR [PERIOD_MS]      (TID = PID for the main thread)
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ptrace.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define STACK_BYTES 8192

// The NDK's glibc headers predate these (Linux 3.4 ABI values; the TV runs 4.4).
#ifndef PTRACE_SEIZE
#define PTRACE_SEIZE 0x4206
#endif
#ifndef PTRACE_INTERRUPT
#define PTRACE_INTERRUPT 0x4207
#endif
#ifndef __NR_process_vm_readv
#define __NR_process_vm_readv 376 // ARM EABI
#endif
static ssize_t read_remote(pid_t pid, const struct iovec *local, const struct iovec *remote)
{
    return syscall(__NR_process_vm_readv, pid, local, 1UL, remote, 1UL, 0UL);
}
#ifndef PTRACE_EVENT_STOP
#define PTRACE_EVENT_STOP 128
#endif

struct record {
    uint32_t magic;   // 0x53414d50 "SAMP"
    uint32_t t_ms;
    uint32_t pc, lr, sp, cpsr;
    uint32_t stack_len;
    // followed by stack_len bytes from sp upwards
};

static double now_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1e3 + t.tv_nsec / 1e6;
}

int main(int argc, char **argv)
{
    if (argc < 4) {
        fprintf(stderr, "usage: %s TID SECONDS OUT_DIR [PERIOD_MS]\n", argv[0]);
        return 2;
    }
    pid_t tid = (pid_t)atoi(argv[1]);
    double seconds = atof(argv[2]);
    const char *out = argv[3];
    int period = argc > 4 ? atoi(argv[4]) : 10;
    char path[512];

    snprintf(path, sizeof(path), "/proc/%d/maps", tid);
    FILE *in = fopen(path, "r");
    snprintf(path, sizeof(path), "%s/maps", out);
    FILE *maps = fopen(path, "w");
    if (!in || !maps) { perror("maps"); return 1; }
    char line[1024];
    while (fgets(line, sizeof(line), in)) fputs(line, maps);
    fclose(in);
    fclose(maps);

    snprintf(path, sizeof(path), "%s/samples.bin", out);
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) { perror("samples"); return 1; }

    if (ptrace(PTRACE_SEIZE, tid, 0, 0) != 0) { perror("PTRACE_SEIZE"); return 1; }
    const double start = now_ms(), end = start + seconds * 1000;
    unsigned taken = 0, failed = 0;
    static uint8_t stack[STACK_BYTES];
    while (now_ms() < end) {
        if (ptrace(PTRACE_INTERRUPT, tid, 0, 0) != 0) { if (errno == ESRCH) break; ++failed; usleep(period * 1000); continue; }
        int status = 0;
        if (waitpid(tid, &status, __WALL) != tid) break;
        if (WIFEXITED(status) || WIFSIGNALED(status)) break;
        int sig = WIFSTOPPED(status) ? WSTOPSIG(status) : 0;
        int is_interrupt = WIFSTOPPED(status) && (status >> 16) == PTRACE_EVENT_STOP;
        uint32_t regs[18];
        if (is_interrupt && ptrace(PTRACE_GETREGS, tid, 0, regs) == 0) {
            struct record r = { 0x53414d50, (uint32_t)(now_ms() - start), regs[15], regs[14], regs[13], regs[16], 0 };
            struct iovec local = { stack, STACK_BYTES }, remote = { (void *)(uintptr_t)r.sp, STACK_BYTES };
            ssize_t got = read_remote(tid, &local, &remote);
            if (got < 0) {
                // The stack may end within STACK_BYTES: retry with less.
                for (size_t len = STACK_BYTES / 2; len >= 512 && got < 0; len /= 2) {
                    local.iov_len = remote.iov_len = len;
                    got = read_remote(tid, &local, &remote);
                }
            }
            r.stack_len = got > 0 ? (uint32_t)got : 0;
            if (write(fd, &r, sizeof(r)) != sizeof(r) || (r.stack_len && write(fd, stack, r.stack_len) != (ssize_t)r.stack_len)) break;
            ++taken;
        }
        // Resume; a real signal that arrived meanwhile is passed on.
        ptrace(PTRACE_CONT, tid, 0, (void *)(uintptr_t)(is_interrupt ? 0 : sig));
        usleep(period * 1000);
    }
    // Detaching needs a stopped tracee.
    if (ptrace(PTRACE_INTERRUPT, tid, 0, 0) == 0) {
        int status = 0;
        waitpid(tid, &status, __WALL);
    }
    ptrace(PTRACE_DETACH, tid, 0, 0);
    close(fd);
    fprintf(stderr, "sampler: %u samples (%u failed) in %.1f s\n", taken, failed, (now_ms() - start) / 1000);
    return 0;
}
