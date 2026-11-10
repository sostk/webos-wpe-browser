#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Run inside docker/run.sh; never deploys or contacts the TV.
set -eu
BR=${BR:-/build/br-out}
SRC=${SRC:-/src}
OUT=${OUT:-$SRC/build/egl-probe}
mkdir -p "$(dirname "$OUT")"
"$BR/host/bin/arm-webos-linux-gnueabi-gcc" -std=c11 -O2 -Wall -Wextra -Werror \
    "$SRC/tools/egl-probe/egl-probe.c" -o "$OUT" -ldl
"$BR/host/bin/arm-webos-linux-gnueabi-readelf" -d "$OUT"
