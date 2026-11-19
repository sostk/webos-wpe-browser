/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The TV's PulseAudio without GStreamer's pulsesink.
 *
 * On webOS 3.9 (an LG B7, PulseAudio 2.1) the sound server quits as soon as a client opens a stream the way pulsesink
 * checks its formats (pa_stream_new_extended with a format list): tools/tv-diag/pulse-probe showed "Connection
 * terminated" on that stream and a new server process after it (2026-11-19). pulsesink does that whenever its caps
 * are asked, so on such a server it can't be used at all; Wren's old check alone restarted the TV's sound once per run,
 * and in alpha 5 once per video, which likely froze that TV.
 *
 * This sink opens one ordinary stream (pa_stream_new with a sample spec), as moonlight-tv's pa_simple output does on
 * those TVs. libpulse.so.0 is the TV's own and is loaded at run time, so the plugin (and hardware video) loads where
 * there is none. Only calls that PulseAudio 2.x has are used.
 */
#include "wpebpulse.h"

#include <gst/audio/gstaudiosink.h>
#include <pulse/pulseaudio.h>

#include <dlfcn.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

GST_DEBUG_CATEGORY_EXTERN(wpeb_media_debug);
#define GST_CAT_DEFAULT wpeb_media_debug

/* ---- libpulse.so.0, loaded once ---------------------------------------------------------------------------- */

#define PA_FUNCS(X)                                                                                                    \
    X(pa_mainloop_new) X(pa_mainloop_free) X(pa_mainloop_get_api) X(pa_mainloop_iterate)                             \
    X(pa_threaded_mainloop_new) X(pa_threaded_mainloop_free) X(pa_threaded_mainloop_start)                            \
    X(pa_threaded_mainloop_stop) X(pa_threaded_mainloop_lock) X(pa_threaded_mainloop_unlock)                          \
    X(pa_threaded_mainloop_wait) X(pa_threaded_mainloop_signal) X(pa_threaded_mainloop_get_api)                       \
    X(pa_context_new) X(pa_context_connect) X(pa_context_disconnect) X(pa_context_unref) X(pa_context_get_state)      \
    X(pa_context_set_state_callback) X(pa_context_errno) X(pa_context_get_server_info) X(pa_operation_unref)          \
    X(pa_stream_new) X(pa_stream_unref) X(pa_stream_connect_playback) X(pa_stream_disconnect)                         \
    X(pa_stream_get_state) X(pa_stream_set_state_callback) X(pa_stream_set_write_callback)                            \
    X(pa_stream_writable_size) X(pa_stream_write) X(pa_stream_get_latency) X(pa_stream_flush) X(pa_strerror)

static struct {
#define X(name) __typeof__(name) *name;
    PA_FUNCS(X)
#undef X
} pa;

static gboolean pa_load(void)
{
    static gsize once = 0;
    static gboolean ok = FALSE;
    if (g_once_init_enter(&once)) {
        void *h = dlopen("libpulse.so.0", RTLD_NOW | RTLD_GLOBAL);
        ok = h != NULL;
#define X(name)                                                                                                        \
    if (ok && !(pa.name = (__typeof__(name) *)dlsym(h, #name)))                                                        \
        ok = FALSE;
        PA_FUNCS(X)
#undef X
        if (!ok)
            GST_WARNING("libpulse.so.0 missing or incomplete: %s", h ? "a function is missing" : dlerror());
        g_once_init_leave(&once, 1);
    }
    return ok;
}

static const char *pulse_server(void)
{
    const char *s = g_getenv("PULSE_SERVER");
    return s && *s ? s : NULL;
}

/* ---- server version, without a stream ---------------------------------------------------------------------- */

static void probe_state(pa_context *c, void *data)
{
    pa_context_state_t s = pa.pa_context_get_state(c);
    if (s == PA_CONTEXT_READY || s == PA_CONTEXT_FAILED || s == PA_CONTEXT_TERMINATED)
        *(int *)data = 1;
}

static void probe_info(pa_context *c, const pa_server_info *i, void *data)
{
    int *major = data;
    *major = i && i->server_version ? atoi(i->server_version) : -1;
    if (*major == 0)
        *major = -1;
    GST_INFO("PulseAudio server %s %s", i && i->server_name ? i->server_name : "?",
             i && i->server_version ? i->server_version : "?");
}

int wpeb_pulse_server_major(void)
{
    if (!pa_load())
        return -1;
    pa_mainloop *ml = pa.pa_mainloop_new();
    pa_context *ctx = pa.pa_context_new(pa.pa_mainloop_get_api(ml), "wpebrowser");
    int major = -2, done = 0;
    pa.pa_context_set_state_callback(ctx, probe_state, &done);
    if (pa.pa_context_connect(ctx, pulse_server(), PA_CONTEXT_NOAUTOSPAWN, NULL) >= 0) {
        for (int ms = 0; !done && ms < 3000; ms += 10) {
            pa.pa_mainloop_iterate(ml, 0, NULL);
            usleep(10000);
        }
        if (pa.pa_context_get_state(ctx) == PA_CONTEXT_READY) {
            pa_operation *op = pa.pa_context_get_server_info(ctx, probe_info, &major);
            for (int ms = 0; major == -2 && ms < 3000; ms += 10) {
                pa.pa_mainloop_iterate(ml, 0, NULL);
                usleep(10000);
            }
            if (op)
                pa.pa_operation_unref(op);
        }
    }
    if (major == -2)
        GST_WARNING("PulseAudio not reachable: %s", pa.pa_strerror(pa.pa_context_errno(ctx)));
    pa.pa_context_set_state_callback(ctx, NULL, NULL);
    pa.pa_context_disconnect(ctx);
    pa.pa_context_unref(ctx);
    pa.pa_mainloop_free(ml);
    return major < 0 ? -1 : major;
}

/* ---- the sink ---------------------------------------------------------------------------------------------- */

typedef struct {
    GstAudioSink parent;
    pa_threaded_mainloop *ml;
    pa_context *ctx;
    pa_stream *stream;
    guint rate;
    gboolean resetting;
} WpebPulsePlainSink;

typedef struct {
    GstAudioSinkClass parent_class;
} WpebPulsePlainSinkClass;

G_DEFINE_TYPE(WpebPulsePlainSink, wpeb_pulse_plain_sink, GST_TYPE_AUDIO_SINK)

static GstStaticPadTemplate plain_template = GST_STATIC_PAD_TEMPLATE("sink", GST_PAD_SINK, GST_PAD_ALWAYS,
    GST_STATIC_CAPS("audio/x-raw, format=(string)S16LE, layout=(string)interleaved, rate=(int)[ 8000, 96000 ], "
                    "channels=(int)[ 1, 2 ]"));

static void plain_context_state(pa_context *c, void *data)
{
    pa.pa_threaded_mainloop_signal(((WpebPulsePlainSink *)data)->ml, 0);
}

static void plain_stream_signal(pa_stream *s, void *data)
{
    pa.pa_threaded_mainloop_signal(((WpebPulsePlainSink *)data)->ml, 0);
}

static void plain_stream_request(pa_stream *s, size_t n, void *data)
{
    pa.pa_threaded_mainloop_signal(((WpebPulsePlainSink *)data)->ml, 0);
}

static gboolean plain_open(GstAudioSink *sink)
{
    WpebPulsePlainSink *self = (WpebPulsePlainSink *)sink;
    if (!pa_load())
        return FALSE;
    self->ml = pa.pa_threaded_mainloop_new();
    if (!self->ml || pa.pa_threaded_mainloop_start(self->ml) < 0)
        return FALSE;
    pa.pa_threaded_mainloop_lock(self->ml);
    self->ctx = pa.pa_context_new(pa.pa_threaded_mainloop_get_api(self->ml), "wpebrowser");
    pa.pa_context_set_state_callback(self->ctx, plain_context_state, self);
    gboolean ok = pa.pa_context_connect(self->ctx, pulse_server(), PA_CONTEXT_NOAUTOSPAWN, NULL) >= 0;
    while (ok) {
        pa_context_state_t s = pa.pa_context_get_state(self->ctx);
        if (s == PA_CONTEXT_READY)
            break;
        if (!PA_CONTEXT_IS_GOOD(s))
            ok = FALSE;
        else
            pa.pa_threaded_mainloop_wait(self->ml);
    }
    if (!ok)
        GST_ELEMENT_ERROR(self, RESOURCE, OPEN_WRITE, ("PulseAudio connection failed"),
                          ("%s", pa.pa_strerror(pa.pa_context_errno(self->ctx))));
    pa.pa_threaded_mainloop_unlock(self->ml);
    return ok;
}

/* One ordinary stream on `device` (NULL: the server's default sink). Called locked. */
static gboolean plain_connect(WpebPulsePlainSink *self, const pa_sample_spec *ss, const pa_buffer_attr *attr,
                              const char *device)
{
    self->stream = pa.pa_stream_new(self->ctx, "Wren", ss, NULL);
    if (!self->stream)
        return FALSE;
    pa.pa_stream_set_state_callback(self->stream, plain_stream_signal, self);
    pa.pa_stream_set_write_callback(self->stream, plain_stream_request, self);
    const pa_stream_flags_t flags = PA_STREAM_INTERPOLATE_TIMING | PA_STREAM_AUTO_TIMING_UPDATE | PA_STREAM_ADJUST_LATENCY;
    gboolean ok = pa.pa_stream_connect_playback(self->stream, device, attr, flags, NULL, NULL) >= 0;
    while (ok) {
        pa_stream_state_t s = pa.pa_stream_get_state(self->stream);
        if (s == PA_STREAM_READY)
            break;
        if (!PA_STREAM_IS_GOOD(s))
            ok = FALSE;
        else
            pa.pa_threaded_mainloop_wait(self->ml);
    }
    if (!ok) {
        GST_WARNING_OBJECT(self, "stream on %s failed: %s", device ? device : "the default sink",
                           pa.pa_strerror(pa.pa_context_errno(self->ctx)));
        pa.pa_stream_set_state_callback(self->stream, NULL, NULL);
        pa.pa_stream_set_write_callback(self->stream, NULL, NULL);
        pa.pa_stream_disconnect(self->stream);
        pa.pa_stream_unref(self->stream);
        self->stream = NULL;
    }
    return ok;
}

static gboolean plain_prepare(GstAudioSink *sink, GstAudioRingBufferSpec *spec)
{
    WpebPulsePlainSink *self = (WpebPulsePlainSink *)sink;
    pa_sample_spec ss = {PA_SAMPLE_S16LE, GST_AUDIO_INFO_RATE(&spec->info), GST_AUDIO_INFO_CHANNELS(&spec->info)};
    pa_buffer_attr attr = {
        .maxlength = (uint32_t)-1,
        .tlength = (uint32_t)(spec->segsize * spec->segtotal),
        .prebuf = (uint32_t)-1,
        .minreq = (uint32_t)spec->segsize,
        .fragsize = (uint32_t)-1,
    };
    self->rate = ss.rate;
    pa.pa_threaded_mainloop_lock(self->ml);
    const char *device = g_getenv("PULSE_SINK");
    gboolean ok = plain_connect(self, &ss, &attr, device && *device ? device : NULL);
    if (!ok && device && *device && PA_CONTEXT_IS_GOOD(pa.pa_context_get_state(self->ctx)))
        ok = plain_connect(self, &ss, &attr, NULL);
    pa.pa_threaded_mainloop_unlock(self->ml);
    if (ok)
        GST_INFO_OBJECT(self, "PulseAudio stream: %u Hz, %u channel(s), plain", ss.rate, ss.channels);
    else
        GST_ELEMENT_ERROR(self, RESOURCE, OPEN_WRITE, ("PulseAudio took no stream"), (NULL));
    return ok;
}

static gboolean plain_unprepare(GstAudioSink *sink)
{
    WpebPulsePlainSink *self = (WpebPulsePlainSink *)sink;
    pa.pa_threaded_mainloop_lock(self->ml);
    if (self->stream) {
        pa.pa_stream_set_state_callback(self->stream, NULL, NULL);
        pa.pa_stream_set_write_callback(self->stream, NULL, NULL);
        pa.pa_stream_disconnect(self->stream);
        pa.pa_stream_unref(self->stream);
        self->stream = NULL;
    }
    pa.pa_threaded_mainloop_unlock(self->ml);
    return TRUE;
}

static gboolean plain_close(GstAudioSink *sink)
{
    WpebPulsePlainSink *self = (WpebPulsePlainSink *)sink;
    if (!self->ml)
        return TRUE;
    pa.pa_threaded_mainloop_lock(self->ml);
    if (self->ctx) {
        pa.pa_context_set_state_callback(self->ctx, NULL, NULL);
        pa.pa_context_disconnect(self->ctx);
        pa.pa_context_unref(self->ctx);
        self->ctx = NULL;
    }
    pa.pa_threaded_mainloop_unlock(self->ml);
    pa.pa_threaded_mainloop_stop(self->ml);
    pa.pa_threaded_mainloop_free(self->ml);
    self->ml = NULL;
    return TRUE;
}

static gint plain_write(GstAudioSink *sink, gpointer data, guint length)
{
    WpebPulsePlainSink *self = (WpebPulsePlainSink *)sink;
    guint done = 0;
    pa.pa_threaded_mainloop_lock(self->ml);
    self->resetting = FALSE;
    while (done < length && !self->resetting) {
        if (!self->stream || pa.pa_stream_get_state(self->stream) != PA_STREAM_READY) {
            pa.pa_threaded_mainloop_unlock(self->ml);
            GST_ELEMENT_ERROR(self, RESOURCE, WRITE, ("PulseAudio stream lost"), (NULL));
            return -1;
        }
        size_t n = pa.pa_stream_writable_size(self->stream);
        if (n == 0 || n == (size_t)-1) {
            pa.pa_threaded_mainloop_wait(self->ml);
            continue;
        }
        if (n > length - done)
            n = length - done;
        if (pa.pa_stream_write(self->stream, (const guint8 *)data + done, n, NULL, 0, PA_SEEK_RELATIVE) < 0)
            break;
        done += n;
    }
    pa.pa_threaded_mainloop_unlock(self->ml);
    return (gint)(self->resetting ? length : done);
}

static guint plain_delay(GstAudioSink *sink)
{
    WpebPulsePlainSink *self = (WpebPulsePlainSink *)sink;
    pa_usec_t usec = 0;
    int negative = 0;
    pa.pa_threaded_mainloop_lock(self->ml);
    if (!self->stream || pa.pa_stream_get_latency(self->stream, &usec, &negative) < 0 || negative)
        usec = 0;
    pa.pa_threaded_mainloop_unlock(self->ml);
    return (guint)(usec * self->rate / G_USEC_PER_SEC);
}

static void plain_reset(GstAudioSink *sink)
{
    WpebPulsePlainSink *self = (WpebPulsePlainSink *)sink;
    pa.pa_threaded_mainloop_lock(self->ml);
    self->resetting = TRUE;
    if (self->stream) {
        pa_operation *op = pa.pa_stream_flush(self->stream, NULL, NULL);
        if (op)
            pa.pa_operation_unref(op);
    }
    pa.pa_threaded_mainloop_signal(self->ml, 0);
    pa.pa_threaded_mainloop_unlock(self->ml);
}

static void wpeb_pulse_plain_sink_init(WpebPulsePlainSink *self)
{
}

static void wpeb_pulse_plain_sink_class_init(WpebPulsePlainSinkClass *klass)
{
    GstElementClass *element_class = GST_ELEMENT_CLASS(klass);
    GstAudioSinkClass *sink_class = GST_AUDIO_SINK_CLASS(klass);
    gst_element_class_add_static_pad_template(element_class, &plain_template);
    gst_element_class_set_static_metadata(element_class, "webOS browser PulseAudio sink", "Sink/Audio",
        "Plays through one ordinary PulseAudio stream (old sound servers)", "wpebrowser");
    sink_class->open = plain_open;
    sink_class->prepare = plain_prepare;
    sink_class->unprepare = plain_unprepare;
    sink_class->close = plain_close;
    sink_class->write = plain_write;
    sink_class->delay = plain_delay;
    sink_class->reset = plain_reset;
}
