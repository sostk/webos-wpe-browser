#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Local-only multimedia build and package pipeline. Contains no device commands.
set -euo pipefail
project=$(cd "$(dirname "$0")/.." && pwd)
build_root=${WPE_BUILD_ROOT:-$HOME/webos-browser-build}
container=webos-wpe-media-build
case "${1:-status}" in
  start)
    # Detached Docker work survives the chat's foreground shell being interrupted.
    test -f "$project/browser/build/webos-release/ipk/wpebrowser/stage/appinfo.json"
    if [[ "$project/browser/engine_client.cpp" -nt "$project/browser/build/webos-release/wpebrowser" ]]; then
      echo 'Build the UI first: bash browser/build.sh webos' >&2
      exit 1
    fi
    printf 'starting\n' > "$build_root/br-media-current.status"
    docker run --detach --rm --name "$container" --user "$(id -u):$(id -g)" \
      -v "$project:/src" -v "$build_root:/build" \
      -v /usr/bin/qemu-arm-static:/usr/local/bin/qemu-arm-static:ro \
      -w /build webos-browser-build:1 bash /src/tools/build-media.sh inside ;;
  inside)
    exec > /build/br-media-current.log 2>&1
    trap 'result=$?; printf "exit=%d\n" "$result" > /build/br-media-current.status' EXIT
    printf 'building\n' > /build/br-media-current.status
    make -C /build/buildroot O=/build/br-out BR2_EXTERNAL=/src/buildroot-external webos_browser_defconfig
    make -C /build/buildroot O=/build/br-out BR2_JLEVEL=3 wpewebkit-reconfigure
    sh /src/tools/stage-engine.sh
    sh /src/tools/media-tests/build-probe.sh
    BR=/build/br-out bash /src/tools/media-tests/qemu.sh
    BR=/build/br-out python3 /src/tools/media-tests/url-tests.py
    # Snapshot the already-built UI package tree, then replace only its generated engine tree.
    stage=$(mktemp -d /src/build/media-package-XXXXXX)
    cp -a /src/browser/build/webos-release/ipk/wpebrowser/stage/. "$stage/"
    cmake -E rm -rf "$stage/engine"
    cp -a /src/build/engine-stage/engine "$stage/engine"
    python3 /src/third_party/webos-native-app-kit/tools/mkipk.py \
      --app-dir "$stage" --out-dir /src/build/media-dist --arch arm
    python3 /src/third_party/webos-native-app-kit/tools/check-package.py \
      /src/build/media-dist/org.sostk.wpebrowser_0.2.0_arm.ipk \
      --expected-needed /src/browser/expected-needed.txt --glibc-runtime engine --arch arm
    echo 'PASS: multimedia engine, staged ARM decoder/URL checks and package checks'
    ;;
  status)
    if [[ -f "$build_root/br-media-current.status" ]]; then cat "$build_root/br-media-current.status"; fi
    if [[ -f "$build_root/br-media-current.log" ]]; then tail -6 "$build_root/br-media-current.log"; fi
    ;;
  *) echo 'usage: bash tools/build-media.sh start|status' >&2; exit 2 ;;
esac
