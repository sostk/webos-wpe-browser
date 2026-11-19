/* SPDX-License-Identifier: GPL-3.0-or-later
 * Wren: can this TV's PulseAudio play an app's sound, and on which sink?
 *
 * Uses the TV's own libpulse (as Wren's engine does) to connect to the sound server, print its version and sinks,
 * then open playback streams the way Wren's GStreamer pulsesink does (a format probe stream, then a plain one) on
 * the sink Wren uses (pmedia), the default sink, and optionally every sink, playing a short quiet tone on each.
 *
 *   pulse-probe [--plain] [--silent] [--all-sinks] [--sink NAME] [--as UID:GID:G1,G2,...]
 *
 *   --plain       only plain streams (PulseAudio 2.1 on webOS 3.9 quits on the format stream)
 *   --silent      open the streams but write no sound
 *   --all-sinks   also try every sink the server lists
 *   --sink NAME   try this sink too
 *   --as ...      run as that user and groups (from root), e.g. Wren's: --as 5601:5000:29,44,505,509,777
 *
 * The server comes from PULSE_SERVER, else unix:/var/run/pulse/native (webOS runs PulseAudio in system mode).
 * If the server drops the connection (it quit), the probe waits for it to come back and goes on. */
#define _GNU_SOURCE
#include <grp.h>
#include <math.h>
#include <pulse/pulseaudio.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static pa_mainloop *ml;
static pa_context *ctx;
static int silent;
static const char *server;

/* Runs the loop until *done is set or about `ms` pass. */
static void run_until(int *done, int ms)
{
    for (int waited = 0; !*done && waited < ms; waited += 10) {
        pa_mainloop_iterate(ml, 0, NULL);
        usleep(10000);
    }
}

static void context_state(pa_context *c, void *data)
{
    int *done = data;
    pa_context_state_t s = pa_context_get_state(c);
    if (s == PA_CONTEXT_READY || s == PA_CONTEXT_FAILED || s == PA_CONTEXT_TERMINATED)
        *done = 1;
}

static void server_info(pa_context *c, const pa_server_info *i, void *data)
{
    (void)c;
    if (i)
        printf("server: %s %s, default sink %s, default sample spec %s %u Hz %u ch\n", i->server_name,
               i->server_version, i->default_sink_name ? i->default_sink_name : "(none)",
               pa_sample_format_to_string(i->sample_spec.format), i->sample_spec.rate, i->sample_spec.channels);
    *(int *)data = 1;
}

#define MAX_SINKS 32
static char *sinks[MAX_SINKS];
static int nsinks;

static void sink_info(pa_context *c, const pa_sink_info *i, int eol, void *data)
{
    (void)c;
    if (eol) {
        *(int *)data = 1;
        return;
    }
    printf("  sink %u %-24s %s, %s %u Hz %u ch, %s\n", i->index, i->name, i->description,
           pa_sample_format_to_string(i->sample_spec.format), i->sample_spec.rate, i->sample_spec.channels,
           i->state == PA_SINK_RUNNING ? "running" : i->state == PA_SINK_IDLE ? "idle" : i->state == PA_SINK_SUSPENDED ? "suspended" : "other");
    if (nsinks < MAX_SINKS)
        sinks[nsinks++] = strdup(i->name);
}

static void stream_state(pa_stream *s, void *data)
{
    int *done = data;
    pa_stream_state_t st = pa_stream_get_state(s);
    if (st == PA_STREAM_READY || st == PA_STREAM_FAILED || st == PA_STREAM_TERMINATED)
        *done = 1;
}

static void drained(pa_stream *s, int ok, void *data)
{
    (void)s;
    (void)ok;
    *(int *)data = 1;
}

/* One stream on `sink` (NULL: the server's default). extended: as pulsesink's format probe does. */
static int try_stream(const char *sink, int extended)
{
    const char *label = sink ? sink : "(default sink)";
    pa_sample_spec ss = {PA_SAMPLE_S16LE, 48000, 2};
    pa_stream *s;
    if (extended) {
        pa_format_info *f = pa_format_info_new();
        f->encoding = PA_ENCODING_PCM;
        pa_format_info_set_sample_format(f, PA_SAMPLE_S16LE);
        pa_format_info_set_rate(f, 48000);
        pa_format_info_set_channels(f, 2);
        s = pa_stream_new_extended(ctx, "wren probe (format)", &f, 1, NULL);
        pa_format_info_free(f);
    } else {
        s = pa_stream_new(ctx, "wren probe", &ss, NULL);
    }
    if (!s) {
        printf("  %-24s %-9s new failed: %s\n", label, extended ? "format" : "plain", pa_strerror(pa_context_errno(ctx)));
        return 0;
    }
    int done = 0;
    pa_stream_set_state_callback(s, stream_state, &done);
    if (pa_stream_connect_playback(s, sink, NULL, 0, NULL, NULL) < 0) {
        printf("  %-24s %-9s connect failed: %s\n", label, extended ? "format" : "plain", pa_strerror(pa_context_errno(ctx)));
        pa_stream_set_state_callback(s, NULL, NULL);
        pa_stream_unref(s);
        return 0;
    }
    run_until(&done, 3000);
    const int ok = pa_stream_get_state(s) == PA_STREAM_READY;
    printf("  %-24s %-9s %s", label, extended ? "format" : "plain",
           ok ? "READY" : done ? "FAILED" : "no answer in 3 s");
    if (!ok)
        printf(": %s", pa_strerror(pa_context_errno(ctx)));
    if (ok)
        printf(" (on sink %s)", pa_stream_get_device_name(s) ? pa_stream_get_device_name(s) : "?");
    printf("\n");
    if (ok && !silent && !extended) {
        /* 0.6 s of a 440 Hz tone at about -20 dB. */
        const int frames = 48000 * 6 / 10;
        short *buf = malloc(frames * 4);
        for (int i = 0; i < frames; ++i)
            buf[2 * i] = buf[2 * i + 1] = (short)(3000 * sin(2 * M_PI * 440 * i / 48000.0));
        pa_stream_write(s, buf, frames * 4, free, 0, PA_SEEK_RELATIVE);
        int d = 0;
        pa_operation *op = pa_stream_drain(s, drained, &d);
        run_until(&d, 3000);
        if (op)
            pa_operation_unref(op);
        printf("  %-24s tone played (did you hear it?)\n", "");
        usleep(300000);
    }
    pa_stream_set_state_callback(s, NULL, NULL);  /* its TERMINATED must not end the next stream's wait */
    pa_stream_disconnect(s);
    pa_stream_unref(s);
    return ok;
}

/* (Re)connects the context. After the server quit, retries for up to 15 s while it restarts. */
static int connect_server(int first)
{
    for (int attempt = 0; attempt < (first ? 1 : 15); ++attempt) {
        if (ctx) {
            pa_context_set_state_callback(ctx, NULL, NULL);
            pa_context_disconnect(ctx);
            pa_context_unref(ctx);
        }
        ctx = pa_context_new(pa_mainloop_get_api(ml), "wren pulse-probe");
        int done = 0;
        pa_context_set_state_callback(ctx, context_state, &done);
        if (pa_context_connect(ctx, server, PA_CONTEXT_NOAUTOSPAWN, NULL) >= 0) {
            run_until(&done, 5000);
            if (pa_context_get_state(ctx) == PA_CONTEXT_READY)
                return 1;
        }
        if (first)
            printf("connect to %s: %s\n", server, pa_strerror(pa_context_errno(ctx)));
        else
            sleep(1);
    }
    if (!first)
        printf("  the server did not come back within 15 s\n");
    return 0;
}

static int drop_to(const char *spec)
{
    unsigned uid, gid;
    char groups[256] = "";
    if (sscanf(spec, "%u:%u:%255s", &uid, &gid, groups) < 2)
        return -1;
    gid_t list[32];
    int n = 0;
    for (char *t = strtok(groups, ","); t && n < 32; t = strtok(NULL, ","))
        list[n++] = (gid_t)atoi(t);
    if (setgroups(n, list) || setgid(gid) || setuid(uid))
        return -1;
    return 0;
}

int main(int argc, char **argv)
{
    int all = 0, plain = 0;
    const char *extra = NULL, *as = NULL;
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--silent"))
            silent = 1;
        else if (!strcmp(argv[i], "--plain"))
            plain = 1;
        else if (!strcmp(argv[i], "--all-sinks"))
            all = 1;
        else if (!strcmp(argv[i], "--sink") && i + 1 < argc)
            extra = argv[++i];
        else if (!strcmp(argv[i], "--as") && i + 1 < argc)
            as = argv[++i];
        else {
            fprintf(stderr, "usage: pulse-probe [--plain] [--silent] [--all-sinks] [--sink NAME] [--as UID:GID:G1,G2,...]\n");
            return 2;
        }
    }
    if (as && drop_to(as)) {
        perror("--as (run as root)");
        return 2;
    }
    printf("pulse-probe 2: libpulse %s, uid %u gid %u groups", pa_get_library_version(), getuid(), getgid());
    gid_t g[64];
    int ng = getgroups(64, g);
    for (int i = 0; i < ng; ++i)
        printf(" %u", g[i]);
    printf("\n");
    server = getenv("PULSE_SERVER") ? getenv("PULSE_SERVER") : "unix:/var/run/pulse/native";
    const char *path = strncmp(server, "unix:", 5) == 0 ? server + 5 : NULL;
    if (path)
        printf("socket %s: %s\n", path, access(path, W_OK) == 0 ? "writable" : "NOT writable by this user");

    ml = pa_mainloop_new();
    if (!connect_server(1))
        return 1;
    int done;
    printf("connected to %s (protocol %u, server protocol %u)\n", server, pa_context_get_protocol_version(ctx),
           pa_context_get_server_protocol_version(ctx));

    done = 0;
    pa_operation *op = pa_context_get_server_info(ctx, server_info, &done);
    run_until(&done, 3000);
    if (op)
        pa_operation_unref(op);
    printf("sinks:\n");
    done = 0;
    op = pa_context_get_sink_info_list(ctx, sink_info, &done);
    run_until(&done, 3000);
    if (op)
        pa_operation_unref(op);

    printf("streams (format = the probe GStreamer's pulsesink opens first, plain = an ordinary stream):\n");
    const char *targets[MAX_SINKS + 3];
    int nt = 0;
    targets[nt++] = "pmedia";  /* Wren's PULSE_SINK */
    targets[nt++] = NULL;      /* the default sink */
    if (extra)
        targets[nt++] = extra;
    if (all)
        for (int i = 0; i < nsinks; ++i)
            if (strcmp(sinks[i], "pmedia") && (!extra || strcmp(sinks[i], extra)))
                targets[nt++] = sinks[i];
    for (int i = 0; i < nt; ++i) {
        for (int extended = plain ? 0 : 1; extended >= 0; --extended) {
            if (pa_context_get_state(ctx) != PA_CONTEXT_READY) {
                printf("  the server dropped the connection (it quit?): reconnecting\n");
                if (!connect_server(0))
                    goto out;
                printf("  reconnected\n");
            }
            try_stream(targets[i], extended);
        }
    }
out:
    pa_context_disconnect(ctx);
    pa_context_unref(ctx);
    pa_mainloop_free(ml);
    return 0;
}
