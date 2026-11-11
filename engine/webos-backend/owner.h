// SPDX-License-Identifier: GPL-3.0-or-later
// libwpeb-owner: see owner.c. The browser UI looks these up with dlsym (browser/direct_page.cpp).
#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define WPEB_OWNER_EXPORT __attribute__((visibility("default")))

struct wl_display;
struct wl_surface;
struct wpeb_owner;

// Creates surface group `group` on `window` with one layer `layer` at `z_index` (negative = below the window, so
// the UI draws its chrome over the page and leaves the page area transparent). socket_path receives the
// compositor socket for WPEB_WAYLAND_SOCKET ("" if unknown). NULL if webOS surface groups are not available.
struct wpeb_owner *wpeb_owner_create(struct wl_display *display, struct wl_surface *window, const char *group,
    const char *layer, int z_index, char *socket_path, size_t socket_path_size);
// Handles the layer's events (call once per UI frame; SDL reads the connection).
void wpeb_owner_dispatch(struct wpeb_owner *owner);
// Moves the page layer (negative = below the UI window, positive = above it).
void wpeb_owner_set_z(struct wpeb_owner *owner, int z_index);
// Page surfaces attached to the layer right now.
int wpeb_owner_attached(struct wpeb_owner *owner);
void wpeb_owner_destroy(struct wpeb_owner *owner);

#ifdef __cplusplus
}
#endif
