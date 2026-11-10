#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# softfp engine with the JavaScript JIT (patch 0004, docs/webkit-like-chromium.md). Local only: no device commands.
#   bash tools/build-softfp-jit.sh start     detached: WebKit dirclean + rebuild (Thumb-2, JIT) -> jsc shell -> stage
#   bash tools/build-softfp-jit.sh status
# Output: build/engine-stage-jit/engine (the interpreter build in build/engine-stage stays as the fallback),
# jsc shell at ~/webos-browser-build/br-out/build/wpewebkit-*/bin/jsc. Then on the host:
#   bash tools/jsc-softfp/run.sh <jsc> <lib dir>   and   bash tools/js-probe.sh build/engine-stage-jit/engine
set -euo pipefail
project=$(cd "$(dirname "$0")/.." && pwd)
build_root=${WPE_BUILD_ROOT:-$HOME/webos-browser-build}
container=webos-wpe-softfp-jit-build
case "${1:-status}" in
  start)
    if docker ps --format '{{.Names}}' | grep -qx "$container"; then echo "already running" >&2; exit 1; fi
    test -f "$project/buildroot-external/patches/wpewebkit/0004-jsc-arm-softfp-jit.patch"
    printf 'starting\n' > "$build_root/br-jit.status"
    docker run --detach --rm --name "$container" --user "$(id -u):$(id -g)" -e HOME=/build \
      -v "$project:/src" -v "$build_root:/build" -w /build webos-browser-build:1 \
      bash /src/tools/build-softfp-jit.sh inside ;;
  inside)
    exec >> /build/br-jit.log 2>&1
    trap 'result=$?; printf "exit=%d\n" "$result" > /build/br-jit.status' EXIT
    echo "== $(date) start"
    mk() { make -C /build/buildroot O=/build/br-out BR2_EXTERNAL=/src/buildroot-external WPEB_SOFTFP_JIT=y BR2_JLEVEL=4 "$@"; }
    printf 'webkit\n' > /build/br-jit.status
    mk webos_browser_defconfig
    # Fresh WebKit tree: patches 0001-0004 applied by Buildroot, CMake reconfigured with the JIT and -mthumb.
    mk wpewebkit-dirclean
    mk wpewebkit
    wk=$(ls -d /build/br-out/build/wpewebkit-*/ | head -n 1)
    grep -E '^ENABLE_JIT:|^ENABLE_DFG_JIT:|^ENABLE_C_LOOP:|^ENABLE_WEBASSEMBLY:' "$wk/CMakeCache.txt"
    grep -q '^ENABLE_JIT:BOOL=ON' "$wk/CMakeCache.txt" || { echo "JIT is not enabled in the WebKit configuration" >&2; exit 1; }
    printf 'jsc\n' > /build/br-jit.status
    # The jsc shell is declared by WebKit but not installed: build it for the differential tests.
    PATH=/build/br-out/host/bin:$PATH ninja -C "$wk" jsc
    # The rest of the target tree (e.g. GStreamer) is unchanged; make sure everything else is installed.
    printf 'buildroot\n' > /build/br-jit.status
    mk
    printf 'stage\n' > /build/br-jit.status
    OUT=/src/build/engine-stage-jit/engine ENGINE_BUILD=/build/engine-build sh /src/tools/stage-engine.sh
    echo "== $(date) PASS: softfp JIT engine staged in build/engine-stage-jit/engine"
    ;;
  status)
    cat "$build_root/br-jit.status" 2>/dev/null || true
    tr '\r' '\n' < "$build_root/br-jit.log" 2>/dev/null | grep -a -E '^>>>|^\[[0-9]+/[0-9]+\]|PASS|rror' | tail -n 3 | cut -c1-200 || true ;;
  *) echo 'usage: bash tools/build-softfp-jit.sh start|status' >&2; exit 2 ;;
esac
