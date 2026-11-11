/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Hardware video for the browser on webOS (common/wpe_media_ipc.h).
 *
 * WebKit picks platform sinks through its GStreamer quirks. Its "rialto" quirks are generic: they look up two
 * elements by name, take their sink caps as decodebin's stop caps (so encoded video reaches the sink instead of a
 * software decoder), use the audio one as the audio sink and the video one as a hole-punch sink, which WebKit
 * tells where the video is through a "rectangle" property while it paints a transparent hole there. These
 * elements carry those names, so the stock WebKit 2.50 uses them with WEBKIT_GST_QUIRKS=rialto and
 * WEBKIT_GST_HOLE_PUNCH_QUIRK=rialto (set by the UI only when its media server runs).
 *
 *   rialtomsevideosink: encoded H.264/HEVC (Annex B, whole access units) or VP9, sent to the UI process at the
 *     pipeline clock's pace (a sync sink), which feeds LG's decoder (NDL). The picture is on the video plane.
 *   rialtomseaudiosink: audio stays in software for now (audioconvert ! audioresample ! pulsesink). It also
 *     plays every page sound, and LG's decoder has one session per app: a click sound must not take it.
 */
#include "wpe_media_ipc.h"

#include <gst/base/gstbasesink.h>
#include <gst/gst.h>

#include <dlfcn.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>

GST_DEBUG_CATEGORY_STATIC(wpeb_media_debug);
#define GST_CAT_DEFAULT wpeb_media_debug

/* ---- the page's video window (engine/webos-backend, loaded by libwpe in this process) ------------------------- */

static struct {
    gsize once;
    int (*open)(char *id, size_t size, unsigned timeout_ms);
    void (*place)(int sw, int sh, int x, int y, int w, int h);
    void (*close)(void);
} video_window;

static void video_window_init(void)
{
    if (!g_once_init_enter(&video_window.once))
        return;
    void *lib = dlopen("libWPEBackend-webos.so", RTLD_NOW | RTLD_NOLOAD);
    if (lib) {
        video_window.open = (int (*)(char *, size_t, unsigned))dlsym(lib, "wpeb_webos_video_open");
        video_window.place = (void (*)(int, int, int, int, int, int))dlsym(lib, "wpeb_webos_video_place");
        video_window.close = (void (*)(void))dlsym(lib, "wpeb_webos_video_close");
    }
    GST_INFO("page video window %s", video_window.open ? "available" : "unavailable (no webOS backend in this process)");
    g_once_init_leave(&video_window.once, 1);
}

/* ---- video sink ------------------------------------------------------------------------------------------- */

#define WPEB_TYPE_VIDEO_SINK (wpeb_video_sink_get_type())
G_DECLARE_FINAL_TYPE(WpebVideoSink, wpeb_video_sink, WPEB, VIDEO_SINK, GstBaseSink)

struct _WpebVideoSink {
    GstBaseSink parent;
    GMutex lock; /* fd, area */
    int fd;
    uint32_t codec;
    gboolean have_area, area_sent;
    WpebMediaArea area;
    guint64 sent;
    /* MP4's length-prefixed H.264/HEVC (avc/hvc1): LG's decoder takes Annex B, so NAL lengths become start codes
     * and the parameter sets from the caps go before every keyframe. 0 = already Annex B (or VP9). */
    guint length_size;
    int video_width, video_height; /* the coded picture, for the window's source region */
    gboolean have_window;
    GByteArray *parameter_sets; /* Annex B */
    GByteArray *scratch;
};

G_DEFINE_TYPE(WpebVideoSink, wpeb_video_sink, GST_TYPE_BASE_SINK)

enum { PROP_0, PROP_RECTANGLE };

static GstStaticPadTemplate video_sink_template = GST_STATIC_PAD_TEMPLATE("sink", GST_PAD_SINK, GST_PAD_ALWAYS,
    GST_STATIC_CAPS("video/x-h264, stream-format = (string) { byte-stream, avc, avc3 }, alignment = (string) au; "
                    "video/x-h265, stream-format = (string) { byte-stream, hvc1, hev1 }, alignment = (string) au; "
                    "video/x-vp9"));

/* Lock held. One message; false when the UI is gone (the stream then errors out). */
static gboolean send_locked(WpebVideoSink *self, uint32_t type, int64_t pts_us, const void *data, uint32_t size)
{
    if (self->fd < 0)
        return FALSE;
    WpebMediaHeader h = {type, size, pts_us};
    struct iovec iov[2] = {{&h, sizeof(h)}, {(void *)data, size}};
    struct msghdr msg = {0};
    msg.msg_iov = iov;
    msg.msg_iovlen = size ? 2 : 1;
    size_t want = sizeof(h) + size;
    while (want) {
        ssize_t n = sendmsg(self->fd, &msg, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            GST_WARNING_OBJECT(self, "media server: %s", g_strerror(errno));
            close(self->fd);
            self->fd = -1;
            return FALSE;
        }
        want -= (size_t)n;
        while (n > 0 && msg.msg_iovlen) {
            if ((size_t)n >= msg.msg_iov[0].iov_len) {
                n -= (ssize_t)msg.msg_iov[0].iov_len;
                msg.msg_iov++;
                msg.msg_iovlen--;
            } else {
                msg.msg_iov[0].iov_base = (char *)msg.msg_iov[0].iov_base + n;
                msg.msg_iov[0].iov_len -= (size_t)n;
                n = 0;
            }
        }
    }
    return TRUE;
}

static gboolean send_msg(WpebVideoSink *self, uint32_t type, int64_t pts_us, const void *data, uint32_t size)
{
    g_mutex_lock(&self->lock);
    gboolean ok = send_locked(self, type, pts_us, data, size);
    g_mutex_unlock(&self->lock);
    return ok;
}

static void send_area_locked(WpebVideoSink *self)
{
    if (self->have_area && !self->area_sent && self->fd >= 0) {
        if (send_locked(self, WPEB_MEDIA_AREA, 0, &self->area, sizeof(self->area)))
            self->area_sent = TRUE;
    }
}

static const guint8 kStartCode[4] = {0, 0, 0, 1};

/* avcC (ISO 14496-15 5.3.3.1): lengthSizeMinusOne, then SPS and PPS lists. */
static gboolean parse_avcc(const guint8 *d, gsize n, guint *length_size, GByteArray *out)
{
    if (n < 7)
        return FALSE;
    *length_size = (d[4] & 3) + 1;
    gsize at = 5;
    for (int list = 0; list < 2; ++list) {
        if (at >= n)
            return FALSE;
        guint count = list == 0 ? (d[at] & 0x1f) : d[at];
        at++;
        for (guint i = 0; i < count; ++i) {
            if (at + 2 > n)
                return FALSE;
            guint len = (guint)(d[at] << 8 | d[at + 1]);
            at += 2;
            if (at + len > n)
                return FALSE;
            g_byte_array_append(out, kStartCode, 4);
            g_byte_array_append(out, d + at, len);
            at += len;
        }
    }
    return TRUE;
}

/* hvcC (ISO 14496-15 8.3.3.1): 22 bytes, lengthSizeMinusOne in byte 21, then arrays of VPS/SPS/PPS/SEI. */
static gboolean parse_hvcc(const guint8 *d, gsize n, guint *length_size, GByteArray *out)
{
    if (n < 23)
        return FALSE;
    *length_size = (d[21] & 3) + 1;
    guint arrays = d[22];
    gsize at = 23;
    for (guint a = 0; a < arrays; ++a) {
        if (at + 3 > n)
            return FALSE;
        guint count = (guint)(d[at + 1] << 8 | d[at + 2]);
        at += 3;
        for (guint i = 0; i < count; ++i) {
            if (at + 2 > n)
                return FALSE;
            guint len = (guint)(d[at] << 8 | d[at + 1]);
            at += 2;
            if (at + len > n)
                return FALSE;
            g_byte_array_append(out, kStartCode, 4);
            g_byte_array_append(out, d + at, len);
            at += len;
        }
    }
    return TRUE;
}

/* Length-prefixed NAL units -> Annex B in self->scratch; false on a malformed access unit. */
static gboolean to_annex_b(WpebVideoSink *self, const guint8 *d, gsize n, gboolean keyframe)
{
    g_byte_array_set_size(self->scratch, 0);
    if (keyframe && self->parameter_sets->len)
        g_byte_array_append(self->scratch, self->parameter_sets->data, self->parameter_sets->len);
    gsize at = 0;
    while (at + self->length_size <= n) {
        guint32 len = 0;
        for (guint i = 0; i < self->length_size; ++i)
            len = len << 8 | d[at + i];
        at += self->length_size;
        if (len > n - at)
            return FALSE;
        g_byte_array_append(self->scratch, kStartCode, 4);
        g_byte_array_append(self->scratch, d + at, len);
        at += len;
    }
    return at == n;
}

static gboolean wpeb_video_sink_start(GstBaseSink *sink)
{
    WpebVideoSink *self = WPEB_VIDEO_SINK(sink);
    const char *path = g_getenv("WPEB_MEDIA_SOCKET");
    if (!path || !*path) {
        GST_ELEMENT_ERROR(self, RESOURCE, NOT_FOUND, ("No hardware video service"), ("WPEB_MEDIA_SOCKET is not set"));
        return FALSE;
    }
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    struct sockaddr_un addr = {0};
    addr.sun_family = AF_UNIX;
    g_strlcpy(addr.sun_path, path, sizeof(addr.sun_path));
    if (fd < 0 || connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        GST_ELEMENT_ERROR(self, RESOURCE, OPEN_WRITE, ("Hardware video service unavailable"), ("%s: %s", path, g_strerror(errno)));
        if (fd >= 0)
            close(fd);
        return FALSE;
    }
    /* A stuck UI must not hang the streaming thread forever (and with it the page's media element). */
    struct timeval tv = {2, 0};
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    g_mutex_lock(&self->lock);
    self->fd = fd;
    self->codec = 0;
    self->sent = 0;
    self->area_sent = FALSE;
    uint32_t version = WPEB_MEDIA_IPC_VERSION;
    send_locked(self, WPEB_MEDIA_HELLO, 0, &version, sizeof(version));
    send_area_locked(self);
    g_mutex_unlock(&self->lock);
    GST_INFO_OBJECT(self, "connected to %s", path);
    return TRUE;
}

static gboolean wpeb_video_sink_stop(GstBaseSink *sink)
{
    WpebVideoSink *self = WPEB_VIDEO_SINK(sink);
    g_mutex_lock(&self->lock);
    send_locked(self, WPEB_MEDIA_UNLOAD, 0, NULL, 0);
    if (self->fd >= 0)
        close(self->fd);
    self->fd = -1;
    g_mutex_unlock(&self->lock);
    if (self->have_window && video_window.close)
        video_window.close();
    self->have_window = FALSE;
    GST_INFO_OBJECT(self, "stopped after %" G_GUINT64_FORMAT " access units", self->sent);
    return TRUE;
}

static gboolean wpeb_video_sink_set_caps(GstBaseSink *sink, GstCaps *caps)
{
    WpebVideoSink *self = WPEB_VIDEO_SINK(sink);
    GstStructure *s = gst_caps_get_structure(caps, 0);
    const char *name = gst_structure_get_name(s);
    WpebMediaLoad load = {0};
    if (g_str_equal(name, "video/x-h264"))
        load.codec = WPEB_MEDIA_H264;
    else if (g_str_equal(name, "video/x-h265"))
        load.codec = WPEB_MEDIA_HEVC;
    else if (g_str_equal(name, "video/x-vp9"))
        load.codec = WPEB_MEDIA_VP9;
    else
        return FALSE;
    int width = 0, height = 0, num = 0, den = 1;
    gst_structure_get_int(s, "width", &width);
    gst_structure_get_int(s, "height", &height);
    if (gst_structure_get_fraction(s, "framerate", &num, &den) && den)
        load.fps_milli = (uint32_t)((guint64)num * 1000 / (guint64)den);
    load.width = (uint32_t)width;
    load.height = (uint32_t)height;
    GST_INFO_OBJECT(self, "caps %" GST_PTR_FORMAT, caps);
    /* New caps (also a resolution switch) bring new parameter sets. */
    const char *format = gst_structure_get_string(s, "stream-format");
    self->length_size = 0;
    g_byte_array_set_size(self->parameter_sets, 0);
    if (format && !g_str_equal(format, "byte-stream")) {
        const GValue *v = gst_structure_get_value(s, "codec_data");
        GstBuffer *cd = v && G_VALUE_HOLDS(v, GST_TYPE_BUFFER) ? gst_value_get_buffer(v) : NULL;
        GstMapInfo m;
        gboolean ok = FALSE;
        if (cd && gst_buffer_map(cd, &m, GST_MAP_READ)) {
            ok = load.codec == WPEB_MEDIA_H264 ? parse_avcc(m.data, m.size, &self->length_size, self->parameter_sets)
                                               : parse_hvcc(m.data, m.size, &self->length_size, self->parameter_sets);
            gst_buffer_unmap(cd, &m);
        }
        /* avc3/hev1 may carry the parameter sets in-band only: the lengths still come from codec_data. */
        if (!ok) {
            GST_ELEMENT_ERROR(self, STREAM, FORMAT, ("Unsupported %s stream", format), ("no usable codec_data"));
            return FALSE;
        }
    }
    /* Adaptive streams change resolution within the stream: the decoder follows the bitstream. Only a codec
     * change needs a new decoder (and a black picture). */
    self->video_width = width;
    self->video_height = height;
    if (load.codec == self->codec)
        return TRUE;
    self->codec = load.codec;
    video_window_init();
    if (video_window.open && !self->have_window && !g_getenv("WPE_BROWSER_DEBUG_NO_VIDEO_WINDOW"))
        self->have_window = video_window.open(load.window_id, sizeof(load.window_id), 1000) != 0;
    if (self->have_window && !load.window_id[0])
        self->have_window = FALSE;
    if (self->have_window) {
        GST_INFO_OBJECT(self, "video window %s on the page surface", load.window_id);
        g_mutex_lock(&self->lock);
        if (self->have_area)
            video_window.place(width, height, self->area.x, self->area.y, self->area.width, self->area.height);
        g_mutex_unlock(&self->lock);
    }
    return send_msg(self, WPEB_MEDIA_LOAD, 0, &load, sizeof(load));
}

static GstFlowReturn wpeb_video_sink_render(GstBaseSink *sink, GstBuffer *buffer)
{
    WpebVideoSink *self = WPEB_VIDEO_SINK(sink);
    GstClockTime pts = GST_BUFFER_PTS_IS_VALID(buffer) ? GST_BUFFER_PTS(buffer) : GST_BUFFER_DTS(buffer);
    /* Media time, as Chromium feeds LG's pipeline (a seek re-anchors it there: WPEB_MEDIA_FLUSH). */
    guint64 stream = gst_segment_to_stream_time(&sink->segment, GST_FORMAT_TIME, pts);
    int64_t pts_us = GST_CLOCK_TIME_IS_VALID(stream) ? (int64_t)(stream / 1000) : 0;
    GstMapInfo map;
    if (!gst_buffer_map(buffer, &map, GST_MAP_READ))
        return GST_FLOW_ERROR;
    const guint8 *data = map.data;
    gsize size = map.size;
    gboolean ok = TRUE;
    if (self->length_size) {
        ok = to_annex_b(self, map.data, map.size, !GST_BUFFER_FLAG_IS_SET(buffer, GST_BUFFER_FLAG_DELTA_UNIT));
        data = self->scratch->data;
        size = self->scratch->len;
        if (!ok)
            GST_WARNING_OBJECT(self, "malformed access unit (%" G_GSIZE_FORMAT " bytes): skipped", map.size);
    }
    if (ok)
        ok = size <= WPEB_MEDIA_MAX_PAYLOAD && send_msg(self, WPEB_MEDIA_VIDEO, pts_us, data, (uint32_t)size);
    else
        ok = TRUE; /* one bad unit is not the end of the stream */
    gst_buffer_unmap(buffer, &map);
    if (!ok) {
        GST_ELEMENT_ERROR(self, RESOURCE, WRITE, ("Hardware video service lost"), (NULL));
        return GST_FLOW_ERROR;
    }
    GST_LOG_OBJECT(self, "unit %" G_GUINT64_FORMAT ": media %" G_GINT64_FORMAT " us, %" G_GSIZE_FORMAT " bytes", self->sent + 1, pts_us, size);
    if (++self->sent == 1)
        GST_INFO_OBJECT(self, "first access unit: media time %" G_GINT64_FORMAT " us, %" G_GSIZE_FORMAT " bytes", pts_us, map.size);
    return GST_FLOW_OK;
}

static gboolean wpeb_video_sink_event(GstBaseSink *sink, GstEvent *event)
{
    WpebVideoSink *self = WPEB_VIDEO_SINK(sink);
    if (GST_EVENT_TYPE(event) == GST_EVENT_FLUSH_STOP)
        send_msg(self, WPEB_MEDIA_FLUSH, 0, NULL, 0);
    return GST_BASE_SINK_CLASS(wpeb_video_sink_parent_class)->event(sink, event);
}

static GstStateChangeReturn wpeb_video_sink_change_state(GstElement *element, GstStateChange transition)
{
    WpebVideoSink *self = WPEB_VIDEO_SINK(element);
    uint32_t paused = 1;
    switch (transition) {
    case GST_STATE_CHANGE_PAUSED_TO_PLAYING:
        paused = 0;
        send_msg(self, WPEB_MEDIA_PAUSE, 0, &paused, sizeof(paused));
        break;
    case GST_STATE_CHANGE_PLAYING_TO_PAUSED:
        send_msg(self, WPEB_MEDIA_PAUSE, 0, &paused, sizeof(paused));
        break;
    default:
        break;
    }
    return GST_ELEMENT_CLASS(wpeb_video_sink_parent_class)->change_state(element, transition);
}

static void wpeb_video_sink_set_property(GObject *object, guint id, const GValue *value, GParamSpec *pspec)
{
    WpebVideoSink *self = WPEB_VIDEO_SINK(object);
    if (id != PROP_RECTANGLE) {
        G_OBJECT_WARN_INVALID_PROPERTY_ID(object, id, pspec);
        return;
    }
    const char *text = g_value_get_string(value);
    WpebMediaArea a = {0};
    if (!text || sscanf(text, "%d,%d,%d,%d", &a.x, &a.y, &a.width, &a.height) != 4)
        return;
    g_mutex_lock(&self->lock);
    /* WebKit sets this on every composited frame: only changes go out. */
    if (!self->have_area || memcmp(&a, &self->area, sizeof(a))) {
        self->area = a;
        self->have_area = TRUE;
        self->area_sent = FALSE;
        GST_DEBUG_OBJECT(self, "rectangle %d,%d %dx%d", a.x, a.y, a.width, a.height);
        send_area_locked(self);
        if (self->have_window && video_window.place)
            video_window.place(self->video_width, self->video_height, a.x, a.y, a.width, a.height);
    }
    g_mutex_unlock(&self->lock);
}

static void wpeb_video_sink_get_property(GObject *object, guint id, GValue *value, GParamSpec *pspec)
{
    WpebVideoSink *self = WPEB_VIDEO_SINK(object);
    if (id != PROP_RECTANGLE) {
        G_OBJECT_WARN_INVALID_PROPERTY_ID(object, id, pspec);
        return;
    }
    g_mutex_lock(&self->lock);
    char *text = g_strdup_printf("%d,%d,%d,%d", self->area.x, self->area.y, self->area.width, self->area.height);
    g_mutex_unlock(&self->lock);
    g_value_take_string(value, text);
}

static void wpeb_video_sink_finalize(GObject *object)
{
    WpebVideoSink *self = WPEB_VIDEO_SINK(object);
    if (self->fd >= 0)
        close(self->fd);
    g_byte_array_unref(self->parameter_sets);
    g_byte_array_unref(self->scratch);
    g_mutex_clear(&self->lock);
    G_OBJECT_CLASS(wpeb_video_sink_parent_class)->finalize(object);
}

static void wpeb_video_sink_init(WpebVideoSink *self)
{
    g_mutex_init(&self->lock);
    self->fd = -1;
    self->parameter_sets = g_byte_array_new();
    self->scratch = g_byte_array_new();
    gst_base_sink_set_sync(GST_BASE_SINK(self), TRUE);
    /* Late access units still go to the decoder: dropping encoded data breaks the frames after it. */
    gst_base_sink_set_max_lateness(GST_BASE_SINK(self), -1);
    gst_base_sink_set_qos_enabled(GST_BASE_SINK(self), FALSE);
    /* LG's pipeline presents by its own clock and needs data ahead of it (decoder latency, its start buffer): hand
     * each unit over a second before its time. Exactly on time it ran dry after ~3.6 s (TV, 2026-11-11). */
    gst_base_sink_set_ts_offset(GST_BASE_SINK(self), -(GstClockTimeDiff)GST_SECOND);
}

static void wpeb_video_sink_class_init(WpebVideoSinkClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS(klass);
    GstElementClass *element_class = GST_ELEMENT_CLASS(klass);
    GstBaseSinkClass *sink_class = GST_BASE_SINK_CLASS(klass);
    object_class->set_property = wpeb_video_sink_set_property;
    object_class->get_property = wpeb_video_sink_get_property;
    object_class->finalize = wpeb_video_sink_finalize;
    g_object_class_install_property(object_class, PROP_RECTANGLE,
        g_param_spec_string("rectangle", "Rectangle", "Video area in page pixels: x,y,width,height", "0,0,0,0",
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
    element_class->change_state = wpeb_video_sink_change_state;
    sink_class->start = wpeb_video_sink_start;
    sink_class->stop = wpeb_video_sink_stop;
    sink_class->set_caps = wpeb_video_sink_set_caps;
    sink_class->render = wpeb_video_sink_render;
    sink_class->event = wpeb_video_sink_event;
    gst_element_class_add_static_pad_template(element_class, &video_sink_template);
    gst_element_class_set_static_metadata(element_class, "webOS hardware video sink", "Sink/Video/Hardware",
        "Sends encoded video to the browser UI, which decodes it on LG's video plane", "wpebrowser");
}

/* ---- audio sink ------------------------------------------------------------------------------------------- */

#define WPEB_TYPE_AUDIO_SINK (wpeb_audio_sink_get_type())
G_DECLARE_FINAL_TYPE(WpebAudioSink, wpeb_audio_sink, WPEB, AUDIO_SINK, GstBin)

struct _WpebAudioSink {
    GstBin parent;
};

G_DEFINE_TYPE(WpebAudioSink, wpeb_audio_sink, GST_TYPE_BIN)

static GstStaticPadTemplate audio_sink_template
    = GST_STATIC_PAD_TEMPLATE("sink", GST_PAD_SINK, GST_PAD_ALWAYS, GST_STATIC_CAPS("audio/x-raw"));

static void wpeb_audio_sink_init(WpebAudioSink *self)
{
    GstElement *convert = gst_element_factory_make("audioconvert", NULL);
    GstElement *resample = gst_element_factory_make("audioresample", NULL);
    GstElement *sink = gst_element_factory_make("pulsesink", NULL);
    if (!convert || !resample || !sink) {
        GST_ERROR_OBJECT(self, "audioconvert, audioresample or pulsesink missing");
        return;
    }
    /* The pipeline's clock must not be the sound server's: a PulseAudio stream that has not started (or is held)
     * does not advance it, and the video, fed to LG's pipeline against that clock, froze with it (TV, 2026-11-11:
     * no sink input at all while the video stalled). The system clock runs regardless; audio follows it. */
    g_object_set(sink, "provide-clock", FALSE, NULL);
    gst_bin_add_many(GST_BIN(self), convert, resample, sink, NULL);
    gst_element_link_many(convert, resample, sink, NULL);
    GstPad *target = gst_element_get_static_pad(convert, "sink");
    GstPadTemplate *templ = gst_static_pad_template_get(&audio_sink_template);
    gst_element_add_pad(GST_ELEMENT(self), gst_ghost_pad_new_from_template("sink", target, templ));
    gst_object_unref(templ);
    gst_object_unref(target);
}

static void wpeb_audio_sink_class_init(WpebAudioSinkClass *klass)
{
    GstElementClass *element_class = GST_ELEMENT_CLASS(klass);
    gst_element_class_add_static_pad_template(element_class, &audio_sink_template);
    gst_element_class_set_static_metadata(element_class, "webOS browser audio sink", "Sink/Audio",
        "Software audio output (PulseAudio) for pages using the hardware video sink", "wpebrowser");
}

/* ---- plugin ----------------------------------------------------------------------------------------------- */

static gboolean plugin_init(GstPlugin *plugin)
{
    GST_DEBUG_CATEGORY_INIT(wpeb_media_debug, "wpebmedia", 0, "webOS browser hardware media");
    /* Rank above MARGINAL: WebKit's quirk checks it. Nothing else autoplugs sinks by rank here. */
    return gst_element_register(plugin, "rialtomsevideosink", GST_RANK_SECONDARY, WPEB_TYPE_VIDEO_SINK)
        && gst_element_register(plugin, "rialtomseaudiosink", GST_RANK_SECONDARY, WPEB_TYPE_AUDIO_SINK);
}

#define PACKAGE "wpebrowser"
GST_PLUGIN_DEFINE(GST_VERSION_MAJOR, GST_VERSION_MINOR, wpebmedia, "webOS browser hardware media", plugin_init, "0.1",
    "GPL", "wpebrowser", "https://github.com/")
