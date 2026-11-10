#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Inside the build container; this never runs a device command.
set -eu
BR=${BR:-/build/br-out}
SRC=${SRC:-/src}
mkdir -p "$SRC/build/media-tests"
# pkg-config prints compiler flags, deliberately split into arguments.
"$BR/host/bin/arm-webos-linux-gnueabi-gcc" -std=c11 -O2 -Wall -Wextra -Werror \
    "$SRC/tools/media-tests/decode-probe.c" -o "$SRC/build/media-tests/decode-probe" \
    $("$BR/host/bin/pkg-config" --cflags --libs gstreamer-app-1.0)
