#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# WPE Browser: build, test, package and install with the webOS native app kit.
#   ./build.sh            host build + smoke test (build/host-debug)
#   ./build.sh asan       the same with AddressSanitizer (build/host-asan)
#   ./build.sh run        run on the desktop (arrow keys, Enter, Escape)
#   ./build.sh webos      cross-build and package: build/webos-release/dist/*.ipk, package-checked
#   ./build.sh install    install the newest ipk on the TV (ARES_DEVICE, else ares' default device)
#   ./build.sh launch     start it on the TV
# The kit: $WNK_KIT_DIR, else ../third_party/webos-native-app-kit (relative to this folder).
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")"
kit="${WNK_KIT_DIR:-../third_party/webos-native-app-kit}"
[[ -f "$kit/tools/common.sh" ]] || { echo "error: the kit is not at $kit (set WNK_KIT_DIR)" >&2; exit 1; }
source "$kit/tools/common.sh"
export WNK_KIT_DIR="$KIT_ROOT"
host() {
    cmake --preset "$1"
    cmake --build --preset "$1" -j"$(nproc)"
    ctest --preset "$1"
}
case "${1:-host}" in
    host) host host-debug ;;
    asan) host host-asan ;;
    run) cmake --build --preset host-debug -j"$(nproc)" && exec build/host-debug/wpebrowser ;;
    webos)
        resolve_ndk
        cmake --preset webos-release
        cmake --build --preset webos-release -j"$(nproc)"
        cmake --build --preset webos-release --target wnk-ipks
        ls -l build/webos-release/dist/ ;;
    install)
        ipk="$(ls -t build/webos-release/dist/org.sostk.wpebrowser_*_arm.ipk 2>/dev/null | head -n 1)"
        [[ -n "$ipk" ]] || die "no package yet: ./build.sh webos"
        exec "$KIT_ROOT/tools/install.sh" "$ipk" ;;
    launch) exec "$KIT_ROOT/tools/launch.sh" org.sostk.wpebrowser ;;
    *) die "usage: ./build.sh [host|asan|run|webos|install|launch]" ;;
esac
