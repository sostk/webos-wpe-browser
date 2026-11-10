// SPDX-License-Identifier: GPL-3.0-or-later
// Decode generated fixtures using the actual target plugins, with no display or sound server.
#include <gst/gst.h>
#include <gst/app/gstappsink.h>
#include <stdio.h>
#include <string.h>

static GstFlowReturn sample_ready(GstAppSink *sink, gpointer data)
{
    GstSample *sample = gst_app_sink_pull_sample(sink);
    if (!sample) return GST_FLOW_ERROR;
    GstBuffer *buffer = gst_sample_get_buffer(sample);
    if (buffer && gst_buffer_get_size(buffer)) g_atomic_int_inc((gint *)data);
    gst_sample_unref(sample);
    return GST_FLOW_OK;
}

static int wait_eos(GstElement *pipeline)
{
    GstBus *bus = gst_element_get_bus(pipeline);
    GstMessage *msg = gst_bus_timed_pop_filtered(bus, 60 * GST_SECOND, GST_MESSAGE_EOS | GST_MESSAGE_ERROR);
    int ok = msg && GST_MESSAGE_TYPE(msg) == GST_MESSAGE_EOS;
    if (msg && !ok) {
        GError *error = NULL;
        gchar *debug = NULL;
        gst_message_parse_error(msg, &error, &debug);
        fprintf(stderr, "%s (%s)\n", error->message, debug ? debug : "no details");
        g_clear_error(&error);
        g_free(debug);
    } else if (!msg) fprintf(stderr, "timeout waiting for decoded EOS\n");
    if (msg) gst_message_unref(msg);
    gst_object_unref(bus);
    return ok;
}

int main(int argc, char **argv)
{
    if (argc != 3 || (strcmp(argv[1], "mp4") && strcmp(argv[1], "webm") && strcmp(argv[1], "url"))) {
        fprintf(stderr, "usage: %s mp4|webm|url FILE_OR_URI\n", argv[0]);
        return 2;
    }
    gst_init(NULL, NULL);
    int is_url = !strcmp(argv[1], "url");
    const char *pipeline_description = is_url ?
        "playbin3 name=source video-sink=\"appsink name=video emit-signals=true sync=false\" "
        "audio-sink=\"appsink name=audio emit-signals=true sync=false\"" : !strcmp(argv[1], "mp4") ?
        "filesrc name=source ! qtdemux name=d "
        "d.video_0 ! queue ! h264parse ! avdec_h264 max-threads=2 ! appsink name=video emit-signals=true sync=false "
        "d.audio_0 ! queue ! aacparse ! avdec_aac ! appsink name=audio emit-signals=true sync=false" :
        "filesrc name=source ! matroskademux name=d "
        "d.video_0 ! queue ! vp9dec threads=2 ! appsink name=video emit-signals=true sync=false "
        "d.audio_0 ! queue ! opusdec ! appsink name=audio emit-signals=true sync=false";
    GError *error = NULL;
    GstElement *pipeline = gst_parse_launch(pipeline_description, &error);
    if (error || !pipeline) {
        fprintf(stderr, "pipeline: %s\n", error ? error->message : "unavailable");
        g_clear_error(&error);
        if (pipeline) gst_object_unref(pipeline);
        return 1;
    }
    GstElement *source = is_url ? gst_object_ref(pipeline) : gst_bin_get_by_name(GST_BIN(pipeline), "source");
    g_object_set(source, is_url ? "uri" : "location", argv[2], NULL);
    gint video_samples = 0, audio_samples = 0;
    GstElement *video = NULL, *audio = NULL;
    if (is_url)
        g_object_get(source, "video-sink", &video, "audio-sink", &audio, NULL);
    else {
        video = gst_bin_get_by_name(GST_BIN(pipeline), "video");
        audio = gst_bin_get_by_name(GST_BIN(pipeline), "audio");
    }
    gst_object_unref(source);
    g_signal_connect(video, "new-sample", G_CALLBACK(sample_ready), &video_samples);
    g_signal_connect(audio, "new-sample", G_CALLBACK(sample_ready), &audio_samples);
    int ok = gst_element_set_state(pipeline, GST_STATE_PLAYING) != GST_STATE_CHANGE_FAILURE;
    if (ok) ok = wait_eos(pipeline);
    printf("%s: video=%d audio=%d\n", argv[1], g_atomic_int_get(&video_samples), g_atomic_int_get(&audio_samples));
    ok = ok && g_atomic_int_get(&video_samples) > 0 && g_atomic_int_get(&audio_samples) > 0;
    if (ok) {
        gint64 duration = GST_CLOCK_TIME_NONE;
        gst_element_query_duration(pipeline, GST_FORMAT_TIME, &duration);
        printf("duration=%.3fs\n", duration / (double)GST_SECOND);
        // playbin maintains EOS state in its internal bins; return to PAUSED
        // before flushing the finished stream and resuming playback.
        gst_element_set_state(pipeline, GST_STATE_PAUSED);
        gst_element_get_state(pipeline, NULL, NULL, 10 * GST_SECOND);
        g_atomic_int_set(&video_samples, 0);
        g_atomic_int_set(&audio_samples, 0);
        ok = gst_element_seek_simple(pipeline, GST_FORMAT_TIME, GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_KEY_UNIT, 4 * GST_SECOND);
        printf("seek accepted: %s\n", ok ? "yes" : "no");
        if (ok) ok = gst_element_set_state(pipeline, GST_STATE_PLAYING) != GST_STATE_CHANGE_FAILURE;
        if (ok) ok = wait_eos(pipeline);
        printf("seek 4s: video=%d audio=%d\n", g_atomic_int_get(&video_samples), g_atomic_int_get(&audio_samples));
        ok = ok && g_atomic_int_get(&video_samples) > 0 && g_atomic_int_get(&audio_samples) > 0;
    }
    gst_element_set_state(pipeline, GST_STATE_NULL);
    gst_object_unref(video);
    gst_object_unref(audio);
    gst_object_unref(pipeline);
    puts(ok ? "PASS: decode + seek" : "FAIL: decode or seek");
    return ok ? 0 : 1;
}
