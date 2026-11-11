// SPDX-License-Identifier: GPL-3.0-or-later
// libwpeb-owner: the browser UI's half of direct presentation. It creates the webOS surface group on the UI's
// own window (the SDL window's wl_surface) and the layer the web processes attach their page surfaces to.
// The UI dlopen()s it (browser/direct_page.cpp), so the UI executable itself does not link libwayland; this
// library needs only the TV's libwayland-client. Built by tools/build-webos-backend.sh.
#define _GNU_SOURCE
#include "owner.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <wayland-client.h>

#include "webos-surface-group-client-protocol.h"

struct wpeb_owner {
    struct wl_display *display;
    struct wl_event_queue *queue;
    struct wl_webos_surface_group_compositor *groups;
    struct wl_webos_surface_group *group;
    struct wl_webos_surface_group_layer *layer;
    int attached; // page surfaces currently attached to the layer
};

static void registry_global(void *data, struct wl_registry *registry, uint32_t name, const char *interface, uint32_t version)
{
    struct wpeb_owner *o = data;
    (void)version;
    if (!strcmp(interface, "wl_webos_surface_group_compositor") && !o->groups)
        o->groups = wl_registry_bind(registry, name, &wl_webos_surface_group_compositor_interface, 1);
}
static void registry_global_remove(void *data, struct wl_registry *registry, uint32_t name) { (void)data; (void)registry; (void)name; }
static const struct wl_registry_listener registry_listener = { registry_global, registry_global_remove };

static void layer_attached(void *data, struct wl_webos_surface_group_layer *layer)
{
    (void)layer;
    struct wpeb_owner *o = data;
    ++o->attached;
    fprintf(stderr, "[wpeb-owner] page surface attached (%d)\n", o->attached);
}
static void layer_detached(void *data, struct wl_webos_surface_group_layer *layer)
{
    (void)layer;
    struct wpeb_owner *o = data;
    if (o->attached > 0)
        --o->attached;
    fprintf(stderr, "[wpeb-owner] page surface detached (%d)\n", o->attached);
}
static const struct wl_webos_surface_group_layer_listener layer_listener = { layer_attached, layer_detached };

WPEB_OWNER_EXPORT struct wpeb_owner *wpeb_owner_create(struct wl_display *display, struct wl_surface *window,
    const char *group, const char *layer, int z_index, char *socket_path, size_t socket_path_size)
{
    if (!display || !window || !group || !*group || !layer || !*layer)
        return NULL;
    struct wpeb_owner *o = calloc(1, sizeof(*o));
    if (!o)
        return NULL;
    o->display = display;
    // Our own queue: SDL dispatches the default one, and must never see these objects' events.
    o->queue = wl_display_create_queue(display);
    struct wl_registry *registry = wl_display_get_registry(display);
    wl_proxy_set_queue((struct wl_proxy *)registry, o->queue);
    wl_registry_add_listener(registry, &registry_listener, o);
    if (wl_display_roundtrip_queue(display, o->queue) < 0 || !o->groups) {
        fprintf(stderr, "[wpeb-owner] no wl_webos_surface_group_compositor\n");
        wl_registry_destroy(registry);
        wpeb_owner_destroy(o);
        return NULL;
    }
    wl_registry_destroy(registry);

    o->group = wl_webos_surface_group_compositor_create_surface_group(o->groups, window, group);
    o->layer = wl_webos_surface_group_create_layer(o->group, layer, z_index);
    wl_webos_surface_group_layer_add_listener(o->layer, &layer_listener, o);
    // A protocol error here (e.g. the name is taken) is fatal for the whole connection, SDL's included, so check
    // it now while nothing else depends on the group.
    if (wl_display_roundtrip_queue(display, o->queue) < 0) {
        fprintf(stderr, "[wpeb-owner] creating surface group '%s' failed (Wayland error %d)\n", group,
            wl_display_get_error(display));
        o->layer = NULL;
        o->group = NULL;
        o->groups = NULL;
        wpeb_owner_destroy(o);
        return NULL;
    }

    // The web processes connect to the same compositor socket: its path is what this connection is bound to.
    if (socket_path && socket_path_size) {
        socket_path[0] = '\0';
        struct sockaddr_un addr;
        socklen_t len = sizeof(addr);
        memset(&addr, 0, sizeof(addr));
        if (getpeername(wl_display_get_fd(display), (struct sockaddr *)&addr, &len) == 0 && addr.sun_family == AF_UNIX
            && len > offsetof(struct sockaddr_un, sun_path) && addr.sun_path[0])
            snprintf(socket_path, socket_path_size, "%s", addr.sun_path);
    }
    fprintf(stderr, "[wpeb-owner] surface group '%s', layer '%s' (z %d), socket '%s'\n", group, layer, z_index,
        socket_path ? socket_path : "");
    return o;
}

WPEB_OWNER_EXPORT void wpeb_owner_dispatch(struct wpeb_owner *o)
{
    if (o)
        wl_display_dispatch_queue_pending(o->display, o->queue);
}

WPEB_OWNER_EXPORT void wpeb_owner_set_z(struct wpeb_owner *o, int z_index)
{
    if (!o || !o->layer)
        return;
    wl_webos_surface_group_layer_set_z_index(o->layer, z_index);
    wl_display_flush(o->display);
    fprintf(stderr, "[wpeb-owner] layer z %d\n", z_index);
}

WPEB_OWNER_EXPORT int wpeb_owner_attached(struct wpeb_owner *o)
{
    return o ? o->attached : 0;
}

WPEB_OWNER_EXPORT void wpeb_owner_destroy(struct wpeb_owner *o)
{
    if (!o)
        return;
    if (o->layer)
        wl_webos_surface_group_layer_destroy(o->layer);
    if (o->group)
        wl_webos_surface_group_destroy(o->group);
    if (o->groups)
        wl_proxy_destroy((struct wl_proxy *)o->groups); // the interface has no destructor request
    if (o->queue) {
        wl_display_flush(o->display);
        wl_event_queue_destroy(o->queue);
    }
    free(o);
}
