#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Run tools/js-probe.c against a staged engine on this PC (binfmt QEMU runs the ARM engine). No TV connection.
#   bash tools/js-probe.sh [ENGINE_DIR] [SCALE_PERMILLE]     default: build/engine-stage-hf/engine
set -euo pipefail
src=$(cd "$(dirname "$0")/.." && pwd)
engine=$(cd "${1:-$src/build/engine-stage-hf/engine}" && pwd)
out=$src/build/js-probe
mkdir -p "$out"
cc -O1 -Wall -I"$src/common" -o "$out/js-probe" "$src/tools/js-probe.c"
# A hard-float engine names its loader in the app's runtime folder (browser/private_loader.cpp); do the same here.
if [ -f "$engine/lib/ld-linux-armhf.so.3" ]; then
    link=/tmp/org.sostk.wpebrowser/ld-linux-armhf.so.3
    mkdir -p "$(dirname "$link")"
    ln -sfn "$engine/lib/ld-linux-armhf.so.3" "$link"
fi
exec "$out/js-probe" "$engine" ${2:-}
