// SPDX-License-Identifier: GPL-3.0-or-later
// Messages between the engine (one wpeb_webos_view per tab) and the page's drawing target in WPEWebProcess.
// One SOCK_SEQPACKET pair per view: the engine keeps one end, WebKit hands the other to the web process
// (wpe_view_backend get_renderer_host_fd -> wpe_renderer_backend_egl_target create).
#pragma once

#include <stdint.h>

#define WPEB_WEBOS_NAME_MAX 64

enum wpeb_webos_msg_type {
    // engine -> target
    WPEB_WEBOS_ATTACH = 1, // show: attach the page surface to surface group `group`, layer `layer`
    WPEB_WEBOS_DETACH = 2, // hide: detach it from the group
    // target -> engine
    WPEB_WEBOS_STATUS = 3, // `code` is a wpeb_webos_status
};

enum wpeb_webos_status {
    WPEB_WEBOS_STATUS_READY = 1,       // Wayland surface + wl_egl_window created
    WPEB_WEBOS_STATUS_ATTACHED = 2,    // attach request sent
    WPEB_WEBOS_STATUS_FIRST_FRAME = 3, // the compositor showed the first frame after an attach
    WPEB_WEBOS_STATUS_DETACHED = 4,
    WPEB_WEBOS_STATUS_NO_WAYLAND = -1,        // could not connect to the compositor
    WPEB_WEBOS_STATUS_NO_SURFACE_GROUPS = -2, // no wl_webos_surface_group_compositor global
    WPEB_WEBOS_STATUS_OWNER_GONE = -3,        // the group's owner (the UI window) destroyed it
    WPEB_WEBOS_STATUS_WAYLAND_ERROR = -4,     // the connection failed (protocol error, e.g. attach refused)
};

struct wpeb_webos_msg {
    uint32_t type;
    int32_t code;
    uint32_t width, height; // STATUS: current surface size in pixels
    char group[WPEB_WEBOS_NAME_MAX];
    char layer[WPEB_WEBOS_NAME_MAX];
};
