#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Builds libWPEBackend-webos.so (engine/webos-backend) on the host with the webOS NDK: it must use the TV's own
# libwayland-client 1.11 / libwayland-egl (Mali) and their headers, not Buildroot's 1.24 (newer generated code
# calls wl_proxy_marshal_flags, which the TV's libwayland lacks). libwpe headers and the link stub come from the
# softfp Buildroot tree; at run time libwpe/glib are the engine's bundled ones. Host only: no device commands.
# tools/stage-engine.sh copies the result into engine/lib when it exists.
set -euo pipefail
project=$(cd "$(dirname "$0")/.." && pwd)
ndk=${WEBOS_NDK:-$HOME/webos-ndk/arm-webos-linux-gnueabi_sdk-buildroot}
sysroot=$ndk/arm-webos-linux-gnueabi/sysroot
br=${BR_OUT:-$HOME/webos-browser-build/br-out}
br_sysroot=$(ls -d "$br"/host/arm-*-linux-gnueabi/sysroot | head -n 1)
src=$project/engine/webos-backend
out=$project/build/webos-backend
mkdir -p "$out/wpe-link"

"$ndk/bin/wayland-scanner" client-header "$sysroot/usr/share/wayland-webos/webos-surface-group.xml" \
    "$out/webos-surface-group-client-protocol.h"
"$ndk/bin/wayland-scanner" private-code "$sysroot/usr/share/wayland-webos/webos-surface-group.xml" \
    "$out/webos-surface-group-protocol.c"
shell_xml=$(ls "$sysroot"/usr/share/wayland-webos/webos-shell.xml 2>/dev/null || ls "$project"/browser/build/webos-release/third_party/sdl-webos-build/src/wnk_sdl_webos_ext/wayland-protocols/webos-shell.xml)
"$ndk/bin/wayland-scanner" private-code "$shell_xml" "$out/webos-shell-protocol.c"
# Only libwpe from the Buildroot tree, so nothing else of it can leak into the link.
ln -sf "$(readlink -f "$br_sysroot/usr/lib/libwpe-1.0.so")" "$out/wpe-link/libwpe-1.0.so"

"$ndk/bin/arm-webos-linux-gnueabi-gcc" -shared -fPIC -O2 -g0 -Wall -Wextra -Werror -std=gnu11 \
    -fvisibility=hidden -Wl,-soname,libWPEBackend-webos.so -Wl,--no-undefined -Wl,--as-needed \
    -I"$src" -I"$out" -I"$br_sysroot/usr/include/wpe-1.0" \
    $(PKG_CONFIG_SYSROOT_DIR=$sysroot PKG_CONFIG_LIBDIR=$sysroot/usr/lib/pkgconfig pkg-config --cflags glib-2.0) \
    -o "$out/libWPEBackend-webos.so" "$src/wpebackend-webos.c" "$out/webos-surface-group-protocol.c" "$out/webos-shell-protocol.c" \
    -L"$out/wpe-link" -lwpe-1.0 -lwayland-client -lwayland-egl -lglib-2.0 -ldl
# The UI's half (window owner): only the TV's libwayland-client, dlopen()ed by the browser UI.
"$ndk/bin/arm-webos-linux-gnueabi-gcc" -shared -fPIC -O2 -g0 -Wall -Wextra -Werror -std=gnu11 \
    -fvisibility=hidden -Wl,-soname,libwpeb-owner.so -Wl,--no-undefined -Wl,--as-needed \
    -I"$src" -I"$out" -o "$out/libwpeb-owner.so" "$src/owner.c" "$out/webos-surface-group-protocol.c" -lwayland-client
"$ndk/bin/arm-webos-linux-gnueabi-strip" --strip-unneeded "$out/libWPEBackend-webos.so" "$out/libwpeb-owner.so"
cp "$src/wpebackend-webos.h" "$src/protocol.h" "$src/owner.h" "$out/"

# Must not need anything newer than the TV's glibc 2.28 or its libwayland.
for lib in libWPEBackend-webos.so libwpeb-owner.so; do
    echo "$lib:"
    "$ndk/bin/arm-webos-linux-gnueabi-readelf" -d "$out/$lib" | grep NEEDED
    if "$ndk/bin/arm-webos-linux-gnueabi-objdump" -T "$out/$lib" | grep -E 'wl_proxy_marshal_flags|GLIBC_2\.(29|3[0-9])'; then
        echo "$lib needs symbols the TV doesn't have" >&2
        exit 1
    fi
done
ls -l "$out"/*.so
