#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Build the surface-group probe with the webOS NDK (softfp, the TV's own libwayland/EGL/GLES). Host only.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
ndk=${WEBOS_NDK:-$HOME/webos-ndk/arm-webos-linux-gnueabi_sdk-buildroot}
sysroot=$ndk/arm-webos-linux-gnueabi/sysroot
out=$here/../../build/sg-probe
mkdir -p "$out"
"$ndk/bin/wayland-scanner" client-header "$sysroot/usr/share/wayland-webos/webos-surface-group.xml" "$out/webos-surface-group-client-protocol.h"
"$ndk/bin/wayland-scanner" private-code "$sysroot/usr/share/wayland-webos/webos-surface-group.xml" "$out/webos-surface-group-protocol.c"
shell_xml=$(ls "$sysroot"/usr/share/wayland-webos/webos-shell.xml 2>/dev/null || ls "$here"/../../browser/build/webos-release/third_party/sdl-webos-build/src/wnk_sdl_webos_ext/wayland-protocols/webos-shell.xml)
"$ndk/bin/wayland-scanner" private-code "$shell_xml" "$out/webos-shell-protocol.c"
"$ndk/bin/arm-webos-linux-gnueabi-gcc" -O2 -Wall -Wextra -I"$out" -o "$out/sg-probe" \
    "$here/sg-probe.c" "$out/webos-surface-group-protocol.c" "$out/webos-shell-protocol.c" \
    -lwayland-client -lwayland-egl -lEGL -lGLESv2
ls -l "$out/sg-probe"
