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
 *   rialtomseaudiosink: a bin that decides on its first caps. AAC of a playback whose video is on the sink above
 *     (and not muted) goes to the same LG pipeline as the video, over the video sink's connection: LG's pipeline
 *     then keeps them in sync, as Chromium on webOS does (MediaPlatformAPIWebOSTV feeds both). With WPEB_HW_OPUS=1,
 *     Opus does too (experimental: LG's opusInfo is declared as Chromium's strings name it, its exact form is not
 *     verified). Everything else (page sounds, muted autoplay) is decoded here and played by PulseAudio: LG's
 *     pipeline is one per app.
 */
#include "wpe_media_ipc.h"

#include <gst/audio/streamvolume.h>
#include <gst/pbutils/codec-utils.h>
#include <gst/video/video.h>
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
    int64_t last_pts_us; /* media time of the last unit sent */
    /* MP4's length-prefixed H.264/HEVC (avc/hvc1): LG's decoder takes Annex B, so NAL lengths become start codes
     * and the parameter sets from the caps go before every keyframe. 0 = already Annex B (or VP9). */
    guint length_size;
    int video_width, video_height; /* the coded picture, for the window's source region */
    gboolean have_window;
    GByteArray *parameter_sets; /* Annex B */
    GByteArray *scratch;
    gint64 load_time;     /* monotonic µs of the last LOAD (g_caps_lock): the audio sink must decide soon after */
    gboolean tracks_sent;
    WpebMediaLoad load;   /* the last LOAD sent (its window, HDR and the envelope the UI declared from it) */
};

/* The envelope the UI declares to LG for a LOAD (browser/lg_media_player.cpp, the same rule): H.264 that fits FHD
 * as 1920x1080 (larger declarations are refused by some firmware), else the decoder's maximum; H.264 at its rate
 * class (30 or 60), other codecs at 60. */
static gboolean envelope_fhd(uint32_t codec, uint32_t width, uint32_t height)
{
    return codec == WPEB_MEDIA_H264 && width > 0 && width <= 1920 && height <= 1088;
}

static gboolean envelope_60(uint32_t codec, uint32_t fps_milli)
{
    return codec != WPEB_MEDIA_H264 || fps_milli > 30500;
}

/* Colorimetry and HDR metadata from the caps (demuxers put the container's there). */
static void hdr_from_caps(GstCaps *caps, WpebMediaHdr *hdr)
{
    GstStructure *s = gst_caps_get_structure(caps, 0);
    const char *c = gst_structure_get_string(s, "colorimetry");
    GstVideoColorimetry cm;
    memset(hdr, 0, sizeof(*hdr));
    hdr->transfer = hdr->primaries = hdr->matrix = 2; /* unspecified */
    if (!c || !gst_video_colorimetry_from_string(&cm, c))
        return;
    hdr->transfer = (uint32_t)gst_video_transfer_function_to_iso(cm.transfer);
    hdr->primaries = (uint32_t)gst_video_color_primaries_to_iso(cm.primaries);
    hdr->matrix = (uint32_t)gst_video_color_matrix_to_iso(cm.matrix);
    hdr->full_range = cm.range == GST_VIDEO_COLOR_RANGE_0_255;
    GstVideoMasteringDisplayInfo mdi;
    if (gst_video_mastering_display_info_from_caps(&mdi, caps)) {
        hdr->has_mastering = 1;
        for (int i = 0; i < 3; ++i) {
            hdr->display_primaries[i][0] = mdi.display_primaries[i].x;
            hdr->display_primaries[i][1] = mdi.display_primaries[i].y;
        }
        hdr->white_point[0] = mdi.white_point.x;
        hdr->white_point[1] = mdi.white_point.y;
        hdr->max_luminance = mdi.max_display_mastering_luminance;
        hdr->min_luminance = mdi.min_display_mastering_luminance;
    }
    GstVideoContentLightLevel cll;
    if (gst_video_content_light_level_from_caps(&cll, caps)) {
        hdr->has_light_level = 1;
        hdr->max_cll = cll.max_content_light_level;
        hdr->max_fall = cll.max_frame_average_light_level;
    }
}

/* Video caps arriving (an audio sink waiting to know whether this playback has video on the LG pipeline). */
static GMutex g_caps_lock;
static GCond g_caps_cond;

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
    memset(&self->load, 0, sizeof(self->load));
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
     * change needs a new decoder (and a black picture), or a rendition the declared envelope does not cover (an
     * ABR step from FHD H.264 to UHD, or from 30 to 60 fps): LG's pipeline was allocated for the first one. */
    self->video_width = width;
    self->video_height = height;
    if (load.codec == self->codec) {
        const WpebMediaLoad *was = &self->load;
        gboolean grow = (envelope_fhd(was->codec, was->width, was->height) && !envelope_fhd(load.codec, load.width, load.height))
            || (!envelope_60(was->codec, was->fps_milli) && envelope_60(load.codec, load.fps_milli));
        if (!grow)
            return TRUE;
        GST_INFO_OBJECT(self, "%ux%u @ %.2f is outside the envelope of %ux%u @ %.2f: reloading LG's pipeline", load.width,
                        load.height, load.fps_milli / 1000.0, was->width, was->height, was->fps_milli / 1000.0);
        memcpy(load.window_id, was->window_id, sizeof(load.window_id));
        hdr_from_caps(caps, &load.hdr);
        self->load = load;
        return send_msg(self, WPEB_MEDIA_LOAD, 0, &load, sizeof(load));
    }
    self->codec = load.codec;
    self->tracks_sent = FALSE;
    hdr_from_caps(caps, &load.hdr);
    if (load.hdr.transfer == 16 || load.hdr.transfer == 18)
        GST_INFO_OBJECT(self, "HDR (transfer %u)%s%s", load.hdr.transfer, load.hdr.has_mastering ? ", mastering display" : "",
                        load.hdr.has_light_level ? ", light level" : "");
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
    self->load = load;
    gboolean sent = send_msg(self, WPEB_MEDIA_LOAD, 0, &load, sizeof(load));
    g_mutex_lock(&g_caps_lock);
    self->load_time = g_get_monotonic_time();
    g_cond_broadcast(&g_caps_cond);
    g_mutex_unlock(&g_caps_lock);
    return sent;
}

/* Whether the stream collection (decodebin3 forwards it downstream) has an audio stream: -1 unknown. */
static int collection_has_audio(GstPad *pad)
{
    GstEvent *e = gst_pad_get_sticky_event(pad, GST_EVENT_STREAM_COLLECTION, 0);
    if (!e)
        return -1;
    GstStreamCollection *c = NULL;
    gst_event_parse_stream_collection(e, &c);
    int audio = c ? 0 : -1;
    for (guint i = 0; c && i < gst_stream_collection_get_size(c); ++i) {
        if (gst_stream_get_stream_type(gst_stream_collection_get_stream(c, i)) & GST_STREAM_TYPE_AUDIO)
            audio = 1;
    }
    if (c)
        gst_object_unref(c);
    gst_event_unref(e);
    return audio;
}

/* Media time of a buffer, in µs. gst_segment_to_stream_time() has no answer for a buffer that starts before the
 * segment, which the first AAC frame after a seek or when joining a live stream usually does (frames don't line up
 * with the seek point); sending 0 for it put the audio 30 s (or a live stream's whole position) behind the video, and
 * LG's pipeline then showed the first frame and never started its clock (TV, 2026-11-13: Angel One from 30 s,
 * NetTV live). _full() gives those times too. A buffer without a timestamp keeps the previous one. */
static int64_t media_time_us(const GstSegment *segment, GstBuffer *buffer, int64_t previous_us)
{
    GstClockTime pts = GST_BUFFER_PTS_IS_VALID(buffer) ? GST_BUFFER_PTS(buffer) : GST_BUFFER_DTS(buffer);
    guint64 stream = GST_CLOCK_TIME_NONE;
    if (!GST_CLOCK_TIME_IS_VALID(pts))
        return previous_us;
    int sign = gst_segment_to_stream_time_full(segment, GST_FORMAT_TIME, pts, &stream);
    if (sign == 0 || !GST_CLOCK_TIME_IS_VALID(stream))
        return previous_us;
    return sign > 0 ? (int64_t)(stream / 1000) : 0; /* before media time 0: not something LG takes */
}

static GstFlowReturn wpeb_video_sink_render(GstBaseSink *sink, GstBuffer *buffer)
{
    WpebVideoSink *self = WPEB_VIDEO_SINK(sink);
    if (!self->tracks_sent) {
        /* The UI holds LG's Load for the audio side's answer; a stream without audio need not wait. */
        self->tracks_sent = TRUE;
        int audio = collection_has_audio(GST_BASE_SINK_PAD(sink));
        if (audio >= 0) {
            uint32_t has_audio = (uint32_t)audio;
            send_msg(self, WPEB_MEDIA_TRACKS, 0, &has_audio, sizeof(has_audio));
        }
    }
    /* Media time, as Chromium feeds LG's pipeline (a seek re-anchors it there: WPEB_MEDIA_FLUSH). */
    int64_t pts_us = self->last_pts_us = media_time_us(&sink->segment, buffer, self->last_pts_us);
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
    if (GST_EVENT_TYPE(event) == GST_EVENT_FLUSH_STOP) {
        send_msg(self, WPEB_MEDIA_FLUSH, 0, NULL, 0);
    } else if (GST_EVENT_TYPE(event) == GST_EVENT_EOS) {
        uint32_t audio = 0;
        send_msg(self, WPEB_MEDIA_EOS, 0, &audio, sizeof(audio));
    }
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

/* ---- audio: LG's pipeline (hardware) ---------------------------------------------------------------------- */

/* The video sink of the pipeline `element` is in (a new reference), or NULL. */
static WpebVideoSink *find_video_sink(GstElement *element)
{
    GstObject *top = gst_object_ref(GST_OBJECT(element));
    for (GstObject *p; (p = gst_object_get_parent(top));) {
        gst_object_unref(top);
        top = p;
    }
    WpebVideoSink *found = NULL;
    if (GST_IS_BIN(top)) {
        GstIterator *it = gst_bin_iterate_recurse(GST_BIN(top));
        GValue v = G_VALUE_INIT;
        while (!found && gst_iterator_next(it, &v) == GST_ITERATOR_OK) {
            GstElement *e = g_value_get_object(&v);
            if (WPEB_IS_VIDEO_SINK(e))
                found = WPEB_VIDEO_SINK(gst_object_ref(e));
            g_value_reset(&v);
        }
        g_value_unset(&v);
        gst_iterator_free(it);
    }
    gst_object_unref(top);
    return found;
}

#define WPEB_TYPE_AUDIO_HW (wpeb_audio_hw_get_type())
G_DECLARE_FINAL_TYPE(WpebAudioHw, wpeb_audio_hw, WPEB, AUDIO_HW, GstBaseSink)

struct _WpebAudioHw {
    GstBaseSink parent;
    WpebVideoSink *video; /* whose connection carries the audio */
    uint32_t codec;       /* WPEB_MEDIA_AAC or WPEB_MEDIA_OPUS */
    gboolean adts;        /* the stream already has ADTS headers */
    guint8 profile, freq_index, channel_config;
    GByteArray *frame;
    guint64 sent;
    int64_t last_pts_us;
};

G_DEFINE_TYPE(WpebAudioHw, wpeb_audio_hw, GST_TYPE_BASE_SINK)

static GstStaticPadTemplate audio_hw_template = GST_STATIC_PAD_TEMPLATE("sink", GST_PAD_SINK, GST_PAD_ALWAYS,
    GST_STATIC_CAPS("audio/mpeg, mpegversion = (int) { 2, 4 }, stream-format = (string) { raw, adts }; audio/x-opus"));

/* Opus: LG's pipeline gets the OpusHead (from the caps' stream header, else rebuilt from the caps' fields). */
static gboolean audio_hw_opus_caps(WpebAudioHw *self, GstCaps *caps)
{
    GstStructure *s = gst_caps_get_structure(caps, 0);
    GstBuffer *head = NULL;
    const GValue *sh = gst_structure_get_value(s, "streamheader");
    if (sh && GST_VALUE_HOLDS_ARRAY(sh) && gst_value_array_get_size(sh) > 0) {
        const GValue *first = gst_value_array_get_value(sh, 0);
        if (G_VALUE_HOLDS(first, GST_TYPE_BUFFER))
            head = gst_buffer_ref(gst_value_get_buffer(first));
    }
    guint32 rate = 0;
    guint8 channels = 0, family = 0, streams = 0, coupled = 0, mapping[256];
    if (!gst_codec_utils_opus_parse_caps(caps, &rate, &channels, &family, &streams, &coupled, mapping)) {
        if (head)
            gst_buffer_unref(head);
        GST_ELEMENT_ERROR(self, STREAM, FORMAT, ("Opus caps without channels"), (NULL));
        return FALSE;
    }
    if (!head)
        head = gst_codec_utils_opus_create_header(rate, channels, family, streams, coupled, mapping, 0, 0);
    GstMapInfo m;
    if (!head || !gst_buffer_map(head, &m, GST_MAP_READ)) {
        if (head)
            gst_buffer_unref(head);
        GST_ELEMENT_ERROR(self, STREAM, FORMAT, ("No OpusHead"), (NULL));
        return FALSE;
    }
    gsize size = sizeof(WpebMediaAudio) + m.size;
    guint8 *msg = g_malloc(size);
    /* Opus decodes at 48 kHz whatever the input rate was (RFC 7845 5.1): that is the rate LG's decoder outputs. */
    WpebMediaAudio a = {WPEB_MEDIA_OPUS, 48000, channels, (uint32_t)m.size};
    memcpy(msg, &a, sizeof(a));
    memcpy(msg + sizeof(a), m.data, m.size);
    gst_buffer_unmap(head, &m);
    gst_buffer_unref(head);
    self->codec = WPEB_MEDIA_OPUS;
    GST_INFO_OBJECT(self, "caps %" GST_PTR_FORMAT, caps);
    gboolean ok = self->video && send_msg(self->video, WPEB_MEDIA_AUDIO_FORMAT, 0, msg, (uint32_t)size);
    g_free(msg);
    return ok;
}

static gboolean wpeb_audio_hw_set_caps(GstBaseSink *sink, GstCaps *caps)
{
    WpebAudioHw *self = WPEB_AUDIO_HW(sink);
    GstStructure *s = gst_caps_get_structure(caps, 0);
    if (g_str_equal(gst_structure_get_name(s), "audio/x-opus"))
        return audio_hw_opus_caps(self, caps);
    self->codec = WPEB_MEDIA_AAC;
    const char *format = gst_structure_get_string(s, "stream-format");
    int rate = 0, channels = 0;
    gst_structure_get_int(s, "rate", &rate);
    gst_structure_get_int(s, "channels", &channels);
    self->adts = format && g_str_equal(format, "adts");
    if (!self->adts) {
        /* MP4's raw AAC: an ADTS header per frame, from the AudioSpecificConfig (ISO 14496-3 1.6.2.1), as Chromium's
         * FFmpegAACBitstreamConverter does for LG's pipeline. HE-AAC plays as its AAC-LC core (SBR is implicit). */
        const GValue *v = gst_structure_get_value(s, "codec_data");
        GstBuffer *cd = v && G_VALUE_HOLDS(v, GST_TYPE_BUFFER) ? gst_value_get_buffer(v) : NULL;
        GstMapInfo m;
        if (!cd || !gst_buffer_map(cd, &m, GST_MAP_READ) || m.size < 2) {
            GST_ELEMENT_ERROR(self, STREAM, FORMAT, ("AAC without codec_data"), (NULL));
            return FALSE;
        }
        guint object = m.data[0] >> 3;
        self->freq_index = (guint8)(((m.data[0] & 7) << 1) | (m.data[1] >> 7));
        self->channel_config = (guint8)((m.data[1] >> 3) & 0xf);
        gst_buffer_unmap(cd, &m);
        self->profile = (guint8)(object >= 1 && object <= 4 ? object - 1 : 1);
        if (self->freq_index > 12) {
            GST_ELEMENT_ERROR(self, STREAM, FORMAT, ("Unsupported AAC sampling rate"), (NULL));
            return FALSE;
        }
    }
    WpebMediaAudio a = {WPEB_MEDIA_AAC, (uint32_t)rate, (uint32_t)channels, 0};
    GST_INFO_OBJECT(self, "caps %" GST_PTR_FORMAT, caps);
    return self->video && send_msg(self->video, WPEB_MEDIA_AUDIO_FORMAT, 0, &a, sizeof(a));
}

static GstFlowReturn wpeb_audio_hw_render(GstBaseSink *sink, GstBuffer *buffer)
{
    WpebAudioHw *self = WPEB_AUDIO_HW(sink);
    int64_t pts_us = self->last_pts_us = media_time_us(&sink->segment, buffer, self->last_pts_us);
    GstMapInfo map;
    if (!gst_buffer_map(buffer, &map, GST_MAP_READ))
        return GST_FLOW_ERROR;
    const guint8 *data = map.data;
    gsize size = map.size;
    if (self->codec == WPEB_MEDIA_AAC && !self->adts) {
        gsize len = map.size + 7;
        guint8 h[7] = {
            0xff, 0xf1, /* sync, MPEG-4, layer 0, no CRC */
            (guint8)((self->profile << 6) | (self->freq_index << 2) | (self->channel_config >> 2)),
            (guint8)(((self->channel_config & 3) << 6) | ((len >> 11) & 3)),
            (guint8)((len >> 3) & 0xff),
            (guint8)(((len & 7) << 5) | 0x1f),
            0xfc,
        };
        g_byte_array_set_size(self->frame, 0);
        g_byte_array_append(self->frame, h, 7);
        g_byte_array_append(self->frame, map.data, (guint)map.size);
        data = self->frame->data;
        size = self->frame->len;
    }
    /* A failed send means the video sink has stopped (or the UI is gone, which the video sink reports): the
     * audio has nowhere to go and is dropped quietly. */
    if (!self->video || !send_msg(self->video, WPEB_MEDIA_AUDIO, pts_us, data, (uint32_t)size))
        GST_DEBUG_OBJECT(self, "audio frame dropped: no media connection");
    gst_buffer_unmap(buffer, &map);
    if (++self->sent == 1)
        GST_INFO_OBJECT(self, "first audio frame: media time %" G_GINT64_FORMAT " us", pts_us);
    return GST_FLOW_OK;
}

static gboolean wpeb_audio_hw_event(GstBaseSink *sink, GstEvent *event)
{
    WpebAudioHw *self = WPEB_AUDIO_HW(sink);
    if (GST_EVENT_TYPE(event) == GST_EVENT_EOS && self->video) {
        uint32_t audio = 1;
        send_msg(self->video, WPEB_MEDIA_EOS, 0, &audio, sizeof(audio));
    }
    return GST_BASE_SINK_CLASS(wpeb_audio_hw_parent_class)->event(sink, event);
}

static void wpeb_audio_hw_finalize(GObject *object)
{
    WpebAudioHw *self = WPEB_AUDIO_HW(object);
    if (self->video)
        gst_object_unref(self->video);
    g_byte_array_unref(self->frame);
    G_OBJECT_CLASS(wpeb_audio_hw_parent_class)->finalize(object);
}

static void wpeb_audio_hw_init(WpebAudioHw *self)
{
    self->frame = g_byte_array_new();
    /* Paced like the video (same clock, same second ahead): LG's pipeline syncs them by their timestamps. */
    gst_base_sink_set_sync(GST_BASE_SINK(self), TRUE);
    gst_base_sink_set_max_lateness(GST_BASE_SINK(self), -1);
    gst_base_sink_set_qos_enabled(GST_BASE_SINK(self), FALSE);
    gst_base_sink_set_ts_offset(GST_BASE_SINK(self), -(GstClockTimeDiff)GST_SECOND);
    /* Added to a running pipeline (see wpeb_audio_sink_build): a sink that prerolls would put it back to PAUSED
     * until someone sets it playing again (18 s on dash.js, TV 2026-11-11). The video sink prerolls for both. */
    gst_base_sink_set_async_enabled(GST_BASE_SINK(self), FALSE);
}

static void wpeb_audio_hw_class_init(WpebAudioHwClass *klass)
{
    G_OBJECT_CLASS(klass)->finalize = wpeb_audio_hw_finalize;
    GST_BASE_SINK_CLASS(klass)->set_caps = wpeb_audio_hw_set_caps;
    GST_BASE_SINK_CLASS(klass)->render = wpeb_audio_hw_render;
    GST_BASE_SINK_CLASS(klass)->event = wpeb_audio_hw_event;
    gst_element_class_add_static_pad_template(GST_ELEMENT_CLASS(klass), &audio_hw_template);
    gst_element_class_set_static_metadata(GST_ELEMENT_CLASS(klass), "webOS browser hardware audio", "Sink/Audio/Hardware",
        "AAC (or Opus) to LG's media pipeline, with the playback's hardware video", "wpebrowser");
}

/* ---- audio sink: the bin WebKit uses ---------------------------------------------------------------------- */

#define WPEB_TYPE_AUDIO_SINK (wpeb_audio_sink_get_type())
G_DECLARE_FINAL_TYPE(WpebAudioSink, wpeb_audio_sink, WPEB, AUDIO_SINK, GstBin)

struct _WpebAudioSink {
    GstBin parent;
    GstPad *ghost;
    GstElement *pulse; /* software path's sink (volume and mute go there) */
    GstElement *hw;    /* hardware path */
    gboolean built;
    double volume;
    gboolean mute;
};

static void wpeb_audio_sink_stream_volume_init(GstStreamVolumeInterface *iface) { (void)iface; }

G_DEFINE_TYPE_WITH_CODE(WpebAudioSink, wpeb_audio_sink, GST_TYPE_BIN,
    G_IMPLEMENT_INTERFACE(GST_TYPE_STREAM_VOLUME, wpeb_audio_sink_stream_volume_init))

enum { AUDIO_PROP_0, AUDIO_PROP_VOLUME, AUDIO_PROP_MUTE };

/* What decodebin3 may hand over undecoded (WebKit takes these caps as its stop caps): AAC (for the hardware path,
 * or decoded here), with WPEB_HW_OPUS=1 Opus too, and raw audio. */
#define AUDIO_SINK_CAPS "audio/x-raw; audio/mpeg, mpegversion = (int) { 2, 4 }, stream-format = (string) { raw, adts }"
static GstStaticPadTemplate audio_sink_template = GST_STATIC_PAD_TEMPLATE("sink", GST_PAD_SINK, GST_PAD_ALWAYS,
    GST_STATIC_CAPS(AUDIO_SINK_CAPS));
static GstStaticPadTemplate audio_sink_template_opus = GST_STATIC_PAD_TEMPLATE("sink", GST_PAD_SINK, GST_PAD_ALWAYS,
    GST_STATIC_CAPS(AUDIO_SINK_CAPS "; audio/x-opus"));

static gboolean hw_opus_enabled(void)
{
    const char *v = g_getenv("WPEB_HW_OPUS");
    return v && g_str_equal(v, "1");
}

/* The codec LG's pipeline could take as it is (WPEB_MEDIA_AAC / WPEB_MEDIA_OPUS), 0 for raw audio. */
static uint32_t hw_codec(GstCaps *caps)
{
    GstStructure *s = gst_caps_get_structure(caps, 0);
    const char *name = gst_structure_get_name(s);
    int version = 0;
    if (g_str_equal(name, "audio/mpeg") && gst_structure_get_int(s, "mpegversion", &version) && (version == 2 || version == 4))
        return WPEB_MEDIA_AAC;
    if (g_str_equal(name, "audio/x-opus"))
        return WPEB_MEDIA_OPUS;
    return 0;
}

/* The video sink of this playback, if its video is on LG's pipeline and its Load was moments ago (the UI holds the
 * Load for this answer for a second). Waits a little for video caps that have not come yet. */
static WpebVideoSink *hardware_partner(WpebAudioSink *self)
{
    WpebVideoSink *video = find_video_sink(GST_ELEMENT(self));
    if (!video)
        return NULL;
    gint64 deadline = g_get_monotonic_time() + 400 * G_TIME_SPAN_MILLISECOND;
    gboolean ok = FALSE;
    g_mutex_lock(&g_caps_lock);
    for (;;) {
        g_mutex_lock(&video->lock);
        gboolean ready = video->codec != 0 && video->fd >= 0 && video->have_window;
        g_mutex_unlock(&video->lock);
        if (ready) {
            ok = g_get_monotonic_time() - video->load_time < 800 * G_TIME_SPAN_MILLISECOND;
            break;
        }
        if (!g_cond_wait_until(&g_caps_cond, &g_caps_lock, deadline))
            break;
    }
    g_mutex_unlock(&g_caps_lock);
    if (!ok) {
        gst_object_unref(video);
        return NULL;
    }
    return video;
}

static void decodebin_pad_added(GstElement *decodebin, GstPad *pad, gpointer next)
{
    (void)decodebin;
    GstPad *sinkpad = gst_element_get_static_pad(GST_ELEMENT(next), "sink");
    if (!gst_pad_is_linked(sinkpad))
        gst_pad_link(pad, sinkpad);
    gst_object_unref(sinkpad);
}

static GstElement *make_software_chain(WpebAudioSink *self, gboolean decode)
{
    GstElement *first = NULL, *prev = NULL;
    const char *names[] = {decode ? "decodebin" : NULL, "audioconvert", "audioresample", "pulsesink"};
    for (guint i = 0; i < G_N_ELEMENTS(names); ++i) {
        if (!names[i])
            continue;
        GstElement *e = gst_element_factory_make(names[i], NULL);
        if (!e) {
            GST_ERROR_OBJECT(self, "%s missing", names[i]);
            return NULL;
        }
        gst_bin_add(GST_BIN(self), e);
        if (!first)
            first = e;
        if (prev && decode && prev == first) {
            /* decodebin's source pad appears with the stream */
            g_signal_connect_object(prev, "pad-added", G_CALLBACK(decodebin_pad_added), e, 0);
        } else if (prev) {
            gst_element_link(prev, e);
        }
        prev = e;
    }
    self->pulse = prev;
    /* The pipeline's clock must not be the sound server's: a PulseAudio stream that has not started (or is held)
     * does not advance it, and the video, fed to LG's pipeline against that clock, froze with it (TV, 2026-11-11). */
    g_object_set(self->pulse, "provide-clock", FALSE, "volume", self->volume, "mute", self->mute, NULL);
    /* Added to a running pipeline: no preroll of its own (see wpeb_audio_hw_init). */
    g_object_set(self->pulse, "async", FALSE, NULL);
    return first;
}

/* The first caps decide the path; later caps go to what was built. */
static gboolean wpeb_audio_sink_build(WpebAudioSink *self, GstCaps *caps)
{
    const uint32_t codec = hw_codec(caps);
    const gboolean encoded = codec != 0;
    const char *label = codec == WPEB_MEDIA_OPUS ? "Opus" : codec == WPEB_MEDIA_AAC ? "AAC" : "raw audio";
    WpebVideoSink *video = NULL;
    if (encoded && !self->mute && self->volume > 0.001 && !g_getenv("WPE_BROWSER_DEBUG_NO_HW_AUDIO"))
        video = hardware_partner(self);
    GstElement *first;
    if (video) {
        self->hw = g_object_new(WPEB_TYPE_AUDIO_HW, NULL);
        WPEB_AUDIO_HW(self->hw)->video = video;
        gst_bin_add(GST_BIN(self), self->hw);
        first = self->hw;
        GST_INFO_OBJECT(self, "%s with hardware video: LG's media pipeline plays it", label);
    } else {
        if (encoded) {
            /* The UI may be holding LG's Load for this playback's audio: tell it to go ahead without. */
            WpebVideoSink *v = find_video_sink(GST_ELEMENT(self));
            if (v) {
                send_msg(v, WPEB_MEDIA_AUDIO_NONE, 0, NULL, 0);
                gst_object_unref(v);
            }
        }
        first = make_software_chain(self, encoded);
        if (!first)
            return FALSE;
        GST_INFO_OBJECT(self, "%s: software audio (PulseAudio)", label);
    }
    GstPad *target = gst_element_get_static_pad(first, "sink");
    gst_ghost_pad_set_target(GST_GHOST_PAD(self->ghost), target);
    gst_object_unref(target);
    /* Children follow the bin's current state (we are in its streaming thread, it is at least PAUSED). */
    GstIterator *it = gst_bin_iterate_elements(GST_BIN(self));
    GValue v = G_VALUE_INIT;
    while (gst_iterator_next(it, &v) == GST_ITERATOR_OK) {
        gst_element_sync_state_with_parent(GST_ELEMENT(g_value_get_object(&v)));
        g_value_reset(&v);
    }
    g_value_unset(&v);
    gst_iterator_free(it);
    self->built = TRUE;
    return TRUE;
}

static gboolean wpeb_audio_sink_pad_event(GstPad *pad, GstObject *parent, GstEvent *event)
{
    WpebAudioSink *self = WPEB_AUDIO_SINK(parent);
    if (GST_EVENT_TYPE(event) == GST_EVENT_CAPS && !self->built) {
        GstCaps *caps;
        gst_event_parse_caps(event, &caps);
        if (!wpeb_audio_sink_build(self, caps)) {
            gst_event_unref(event);
            return FALSE;
        }
    }
    return gst_pad_event_default(pad, parent, event);
}

/* Until the first caps build the chain, the ghost pad has no target to ask: answer from the template. */
static gboolean wpeb_audio_sink_pad_query(GstPad *pad, GstObject *parent, GstQuery *query)
{
    WpebAudioSink *self = WPEB_AUDIO_SINK(parent);
    if (!self->built) {
        GstCaps *templ = gst_pad_get_pad_template_caps(pad);
        gboolean handled = TRUE;
        if (GST_QUERY_TYPE(query) == GST_QUERY_CAPS) {
            GstCaps *filter, *result;
            gst_query_parse_caps(query, &filter);
            result = filter ? gst_caps_intersect_full(filter, templ, GST_CAPS_INTERSECT_FIRST) : gst_caps_ref(templ);
            gst_query_set_caps_result(query, result);
            gst_caps_unref(result);
        } else if (GST_QUERY_TYPE(query) == GST_QUERY_ACCEPT_CAPS) {
            GstCaps *caps;
            gst_query_parse_accept_caps(query, &caps);
            gst_query_set_accept_caps_result(query, gst_caps_can_intersect(caps, templ));
        } else {
            handled = FALSE;
        }
        gst_caps_unref(templ);
        if (handled)
            return TRUE;
    }
    return gst_pad_query_default(pad, parent, query);
}

static void send_volume(WpebAudioSink *self)
{
    if (self->pulse)
        g_object_set(self->pulse, "volume", self->volume, "mute", self->mute, NULL);
    if (self->hw && WPEB_AUDIO_HW(self->hw)->video) {
        WpebMediaVolume v = {(uint32_t)(CLAMP(self->volume, 0.0, 1.0) * 1000 + 0.5), self->mute ? 1u : 0u};
        send_msg(WPEB_AUDIO_HW(self->hw)->video, WPEB_MEDIA_VOLUME, 0, &v, sizeof(v));
    }
}

static void wpeb_audio_sink_set_property(GObject *object, guint id, const GValue *value, GParamSpec *pspec)
{
    WpebAudioSink *self = WPEB_AUDIO_SINK(object);
    switch (id) {
    case AUDIO_PROP_VOLUME: self->volume = g_value_get_double(value); break;
    case AUDIO_PROP_MUTE: self->mute = g_value_get_boolean(value); break;
    default: G_OBJECT_WARN_INVALID_PROPERTY_ID(object, id, pspec); return;
    }
    send_volume(self);
}

static void wpeb_audio_sink_get_property(GObject *object, guint id, GValue *value, GParamSpec *pspec)
{
    WpebAudioSink *self = WPEB_AUDIO_SINK(object);
    switch (id) {
    case AUDIO_PROP_VOLUME: g_value_set_double(value, self->volume); break;
    case AUDIO_PROP_MUTE: g_value_set_boolean(value, self->mute); break;
    default: G_OBJECT_WARN_INVALID_PROPERTY_ID(object, id, pspec); break;
    }
}

static void wpeb_audio_sink_init(WpebAudioSink *self)
{
    self->volume = 1.0;
    GstPadTemplate *templ = gst_static_pad_template_get(hw_opus_enabled() ? &audio_sink_template_opus : &audio_sink_template);
    self->ghost = gst_ghost_pad_new_no_target_from_template("sink", templ);
    gst_object_unref(templ);
    gst_pad_set_event_function(self->ghost, wpeb_audio_sink_pad_event);
    gst_pad_set_query_function(self->ghost, wpeb_audio_sink_pad_query);
    gst_element_add_pad(GST_ELEMENT(self), self->ghost);
    /* A sink for the pipeline's bookkeeping (async state changes, EOS) before its children exist. */
    GST_OBJECT_FLAG_SET(self, GST_ELEMENT_FLAG_SINK);
}

static void wpeb_audio_sink_class_init(WpebAudioSinkClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS(klass);
    GstElementClass *element_class = GST_ELEMENT_CLASS(klass);
    object_class->set_property = wpeb_audio_sink_set_property;
    object_class->get_property = wpeb_audio_sink_get_property;
    g_object_class_override_property(object_class, AUDIO_PROP_VOLUME, "volume");
    g_object_class_override_property(object_class, AUDIO_PROP_MUTE, "mute");
    gst_element_class_add_static_pad_template(element_class, hw_opus_enabled() ? &audio_sink_template_opus : &audio_sink_template);
    gst_element_class_set_static_metadata(element_class, "webOS browser audio sink", "Sink/Audio",
        "AAC (and with WPEB_HW_OPUS=1 Opus) with hardware video to LG's media pipeline; other audio decoded and played by "
        "PulseAudio", "wpebrowser");
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
