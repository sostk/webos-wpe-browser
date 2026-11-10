#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Host-only ARM runtime test. Uses QEMU user-mode, no TV connection.
set -euo pipefail
src=$(cd "$(dirname "$0")/../.." && pwd)
br=${BR:-$HOME/webos-browser-build/br-out}
engine=${ENGINE_STAGE:-$src/build/engine-stage/engine}
sysroot="$br/host/arm-buildroot-linux-gnueabi/sysroot"
registry=$(mktemp /tmp/wpe-qemu-registry-XXXXXX)
trap 'rm -f "$registry"' EXIT
for pair in 'mp4 avc-aac.mp4' 'webm vp9-opus.webm'; do
    read -r format filename <<< "$pair"
    timeout 90 qemu-arm-static -L "$sysroot" \
        -E "LD_LIBRARY_PATH=$engine/lib" \
        -E "GST_PLUGIN_SYSTEM_PATH_1_0=$engine/lib/gstreamer-1.0" \
        -E "GST_PLUGIN_SYSTEM_PATH=$engine/lib/gstreamer-1.0" \
        -E GST_PLUGIN_PATH_1_0= -E GST_PLUGIN_PATH= \
        -E "GST_REGISTRY=$registry" -E "GST_REGISTRY_1_0=$registry" -E GST_REGISTRY_FORK=no \
        -E "GIO_MODULE_DIR=$engine/lib/gio/modules" -E GIO_EXTRA_MODULES= \
        "$src/build/media-tests/decode-probe" "$format" "$src/tools/media-tests/generated/$filename"
done
