#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Run tools/direct-probe.c on this PC: a copy of the staged softfp engine with a fresh wpe-engine and the direct
# presentation backend (build/webos-backend), run through binfmt QEMU. No TV connection.
#   bash tools/direct-probe.sh [ENGINE_DIR] [WPE_ENGINE_BINARY]
#   defaults: build/engine-stage/engine, ~/webos-browser-build/engine-build-direct/wpe-engine
set -euo pipefail
src=$(cd "$(dirname "$0")/.." && pwd)
base=$(cd "${1:-$src/build/engine-stage/engine}" && pwd)
binary=${2:-$HOME/webos-browser-build/engine-build-direct/wpe-engine}
br=${BR_OUT:-$HOME/webos-browser-build/br-out}
out=$src/build/direct-probe
rm -rf "$out/engine"
mkdir -p "$out"
cp -al "$base" "$out/engine"           # hard links: no extra disk space
rm "$out/engine/wpe-engine"
cp "$binary" "$out/engine/wpe-engine"
cp "$src/build/webos-backend/libWPEBackend-webos.so" "$out/engine/lib/"
# The TV provides libwayland-client.so.0 and an unversioned libwayland-egl.so; here Buildroot's stand in.
mkdir -p "$out/extra"
sysroot=$(ls -d "$br"/host/arm-*-linux-gnueabi/sysroot | head -n 1)
ln -sfn "$(readlink -f "$sysroot/usr/lib/libwayland-egl.so.1")" "$out/extra/libwayland-egl.so"
cc -O1 -Wall -I"$src/common" -o "$out/direct-probe" "$src/tools/direct-probe.c"
export QEMU_LD_PREFIX=$sysroot DIRECT_PROBE_EXTRA_LIBS=$out/extra
exec "$out/direct-probe" "$out/engine"
