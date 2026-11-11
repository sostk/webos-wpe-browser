/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Hardware video: the web process's GStreamer video sink (engine/gst/wpebmediasink.c) sends the encoded stream to
 * the UI process, which feeds LG's decoder through the kit's NdlBackend (browser/media_server.cpp). The decoder
 * has to run in the UI process: it registers luna bus names under the app's role, and that role is bound to the
 * app's executable (exeName), not to WPEWebProcess. The picture goes to the TV's video plane under all windows;
 * WebKit punches a transparent hole in the page where the video element is.
 *
 * A stream socket (AF_UNIX, path in WPEB_MEDIA_SOCKET), one connection per video sink. Every message is a
 * WpebMediaHeader followed by `size` payload bytes. Little-endian, both ends are the same TV.
 */
#ifndef WPE_MEDIA_IPC_H
#define WPE_MEDIA_IPC_H

#include <stdint.h>

#define WPEB_MEDIA_IPC_VERSION 2u
#define WPEB_MEDIA_MAX_PAYLOAD (16u * 1024u * 1024u)

enum WpebMediaType {
    WPEB_MEDIA_HELLO = 1,   /* payload: uint32_t version */
    WPEB_MEDIA_LOAD = 2,    /* payload: WpebMediaLoad */
    WPEB_MEDIA_VIDEO = 3,   /* pts_us: media (stream) time; payload: one access unit (Annex B for H.264/HEVC) */
    WPEB_MEDIA_FLUSH = 4,   /* a seek: what follows continues after what was fed */
    WPEB_MEDIA_AREA = 5,    /* payload: WpebMediaArea, page-surface pixels */
    WPEB_MEDIA_UNLOAD = 6,  /* the stream stopped; the decoder is released */
    WPEB_MEDIA_PAUSE = 7,   /* payload: uint32_t paused */
};

enum WpebMediaCodec {
    WPEB_MEDIA_H264 = 1,
    WPEB_MEDIA_HEVC = 2,
    WPEB_MEDIA_VP9 = 3,
    WPEB_MEDIA_AV1 = 4,
};

typedef struct WpebMediaHeader {
    uint32_t type;
    uint32_t size;
    int64_t pts_us;
} WpebMediaHeader;

typedef struct WpebMediaLoad {
    uint32_t codec;
    uint32_t width;
    uint32_t height;
    uint32_t fps_milli;
    /* LG exported video window made on the page surface (wl_webos_foreign, engine/webos-backend), as Chromium does:
     * the video plays through Starfish bound to it (option.windowId) and the page stays above its own video.
     * Empty: no window (the UI falls back to NDL, whose video sits below the UI window). */
    char window_id[128];
} WpebMediaLoad;

typedef struct WpebMediaArea {
    int32_t x, y, width, height;
} WpebMediaArea;

#endif
