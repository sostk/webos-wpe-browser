#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Screenshot one URL with a staged softfp engine on this PC (tools/page-shot.c, binfmt QEMU). No TV connection.
#   bash tools/page-shot.sh ENGINE_DIR URL OUT.png [WIDTH HEIGHT [SETTLE_SECONDS]]
set -euo pipefail
src=$(cd "$(dirname "$0")/.." && pwd)
engine=$(cd "$1" && pwd); url=$2; out=$3; shift 3
br=${BR_OUT:-$HOME/webos-browser-build/br-out}
mkdir -p "$src/build/page-shot"
cc -O1 -Wall -I"$src/common" -o "$src/build/page-shot/page-shot" "$src/tools/page-shot.c"
export QEMU_LD_PREFIX=$(ls -d "$br"/host/arm-*-linux-gnueabi/sysroot | head -n 1)
"$src/build/page-shot/page-shot" "$engine" "$url" "${out%.png}.ppm" "$@"
python3 -c 'import sys; from PIL import Image; Image.open(sys.argv[1]).save(sys.argv[2])' "${out%.png}.ppm" "$out" 2>/dev/null \
    || convert "${out%.png}.ppm" "$out"
echo "$out"
