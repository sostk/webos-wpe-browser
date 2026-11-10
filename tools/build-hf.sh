#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Hard-float engine pipeline (JavaScript JIT + WebAssembly). Local only: no device commands.
#   bash tools/build-hf.sh start    detached: toolchain (if missing) -> Buildroot (br-out-hf) -> stage engine
#   bash tools/build-hf.sh status
# The engine tree gets its own glibc and loader (see stage-engine.sh, ABI=hf); the UI app stays softfp.
set -euo pipefail
project=$(cd "$(dirname "$0")/.." && pwd)
build_root=${WPE_BUILD_ROOT:-$HOME/webos-browser-build}
container=webos-wpe-hf-build
tuple=arm-webos-linux-gnueabihf
case "${1:-status}" in
  start)
    if docker ps --format '{{.Names}}' | grep -qx "$container"; then echo "already running" >&2; exit 1; fi
    printf 'starting\n' > "$build_root/br-hf.status"
    docker run --detach --rm --name "$container" --user "$(id -u):$(id -g)" -e HOME=/build \
      -v "$project:/src" -v "$build_root:/build" \
      -v /usr/bin/qemu-arm-static:/usr/local/bin/qemu-arm-static:ro \
      -w /build webos-browser-build:1 bash /src/tools/build-hf.sh inside ;;
  inside)
    exec >> /build/br-hf.log 2>&1
    trap 'result=$?; printf "exit=%d\n" "$result" > /build/br-hf.status' EXIT
    echo "== $(date) start"
    if [ ! -x "/build/x-tools/$tuple/bin/$tuple-gcc" ]; then
      printf 'toolchain\n' > /build/br-hf.status
      mkdir -p /build/ctng-hf
      cp /src/toolchain/ct-ng-hf.defconfig /build/ctng-hf/defconfig
      (cd /build/ctng-hf && ct-ng defconfig && ct-ng build.4)
    fi
    # linux/dma-buf.h appeared in Linux 4.6; WebKit's GBM code includes it (see NOTES.md). The sysroot is read-only.
    inc="/build/x-tools/$tuple/$tuple/sysroot/usr/include/linux"
    if [ ! -f "$inc/dma-buf.h" ]; then
      chmod u+w "$inc"
      cp /src/toolchain/extra-headers/linux/dma-buf.h "$inc/"
      chmod u-w "$inc"
    fi
    printf 'buildroot\n' > /build/br-hf.status
    make -C /build/buildroot O=/build/br-out-hf BR2_EXTERNAL=/src/buildroot-external webos_browser_hf_defconfig
    make -C /build/buildroot O=/build/br-out-hf BR2_JLEVEL=4
    printf 'stage\n' > /build/br-hf.status
    BR=/build/br-out-hf OUT=/src/build/engine-stage-hf/engine ENGINE_BUILD=/build/engine-build-hf sh /src/tools/stage-engine.sh
    echo "== $(date) PASS: hard-float engine staged in build/engine-stage-hf/engine"
    ;;
  status)
    cat "$build_root/br-hf.status" 2>/dev/null || true
    tail -n 4 "$build_root/br-hf.log" 2>/dev/null | cut -c1-200 || true ;;
  *) echo 'usage: bash tools/build-hf.sh start|status' >&2; exit 2 ;;
esac
