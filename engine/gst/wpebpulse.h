/* SPDX-License-Identifier: GPL-3.0-or-later
 * The TV's PulseAudio without GStreamer's pulsesink (engine/gst/wpebpulse.c). */
#pragma once

#include <gst/gst.h>

G_BEGIN_DECLS

/* The sound server's major version (PULSE_SERVER, else the system socket), asked without opening any stream;
 * -1 when it can't be reached or libpulse.so.0 is missing. */
int wpeb_pulse_server_major(void);

/* "wpebpulseplain": an audio sink that plays S16LE through one ordinary PulseAudio stream on PULSE_SINK (else the
 * default sink), as moonlight-tv's pa_simple output does. Made by name only. */
GType wpeb_pulse_plain_sink_get_type(void);

G_END_DECLS
