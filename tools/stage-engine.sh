#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Builds wpe-engine against the Buildroot output and stages it, with everything it needs at run time, into
# build/engine-stage/engine/ (packaged by browser/CMakeLists.txt). Runs inside the build container:
#   docker/run.sh 'sh /src/tools/stage-engine.sh'
#
# Layout of the staged folder (paths the UI app sets up in engine_client.cpp):
#   wpe-engine                         the engine (RUNPATH $ORIGIN/lib)
#   libexec/WPEWebProcess, WPENetworkProcess
#   lib/*.so*                          every library the above need, except the TV's glibc, plus Mesa
#                                      (libEGL/libGLESv2/libgallium: software GL for WPEWebProcess only)
#   lib/gio/modules/libgiognutls.so    TLS for libsoup
#   etc/fonts/fonts.conf, share/fonts  bundled fonts (+ the TV's own font folders)
#   etc/ssl/certs/ca-certificates.crt  copied by the app to /tmp/wpe-browser/ before start
set -eu

BR=${BR:-/build/br-out}
SRC=${SRC:-/src}
OUT=${OUT:-/src/build/engine-stage/engine}
TARGET=$BR/target
STAGING=$BR/staging
READELF=$BR/host/bin/arm-webos-linux-gnueabi-readelf
[ -x "$READELF" ] || READELF=$(ls "$BR"/host/bin/*-readelf | head -n 1)

# The TV provides these (glibc 2.28); bundling them would break the loader.
SYSTEM_LIBS=' ld-linux.so.3 libc.so.6 libm.so.6 libdl.so.2 libpthread.so.0 librt.so.1 libresolv.so.2 libutil.so.1 libanl.so.1 libnsl.so.1 libcrypt.so.1 '

echo "== build wpe-engine"
cmake -S "$SRC/engine" -B /build/engine-build -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="$BR/host/share/buildroot/toolchainfile.cmake" -DCMAKE_BUILD_TYPE=Release >/dev/null
cmake --build /build/engine-build

rm -rf "$OUT"
mkdir -p "$OUT/lib/gio/modules" "$OUT/libexec" "$OUT/etc/fonts" "$OUT/etc/ssl/certs" "$OUT/share"
cp /build/engine-build/wpe-engine "$OUT/"
"$BR"/host/bin/*-strip "$OUT/wpe-engine" 2>/dev/null || true

LIBEXEC=$(ls -d "$TARGET"/usr/libexec/wpe-webkit-* | head -n 1)
cp "$LIBEXEC"/WPEWebProcess "$LIBEXEC"/WPENetworkProcess "$OUT/libexec/"
cp "$TARGET"/usr/lib/gio/modules/libgiognutls.so "$OUT/lib/gio/modules/"

# Libraries that are dlopen()ed, so not in any DT_NEEDED: libepoxy loads EGL/GLES by SONAME.
SEEDS="$OUT/wpe-engine $OUT/libexec/WPEWebProcess $OUT/libexec/WPENetworkProcess $OUT/lib/gio/modules/libgiognutls.so"
for so in libEGL.so.1 libGLESv2.so.2; do
    f=$(ls "$TARGET"/usr/lib/$so 2>/dev/null || true)
    [ -n "$f" ] || { echo "missing $so in target (Mesa?)" >&2; exit 1; }
    SEEDS="$SEEDS $f"
done

find_lib() {
    for d in "$TARGET/usr/lib" "$TARGET/lib" "$STAGING/usr/lib" "$STAGING/lib"; do
        [ -e "$d/$1" ] && { echo "$d/$1"; return 0; }
    done
    return 1
}

echo "== collect libraries"
todo="$SEEDS"
seen=' '
while [ -n "$todo" ]; do
    next=''
    for f in $todo; do
        for need in $("$READELF" -d "$f" 2>/dev/null | sed -n 's/.*(NEEDED).*\[\(.*\)\]/\1/p'); do
            case "$SYSTEM_LIBS" in *" $need "*) continue ;; esac
            case "$seen" in *" $need "*) continue ;; esac
            seen="$seen$need "
            path=$(find_lib "$need") || { echo "  ! not found: $need (needed by $(basename "$f"))" >&2; exit 1; }
            cp -L "$path" "$OUT/lib/$need"
            next="$next $OUT/lib/$need"
        done
    done
    todo=$next
done
for so in libEGL.so.1 libGLESv2.so.2; do cp -L "$TARGET/usr/lib/$so" "$OUT/lib/$so"; done
"$BR"/host/bin/*-strip --strip-unneeded "$OUT"/lib/*.so* "$OUT"/lib/gio/modules/*.so "$OUT"/libexec/* 2>/dev/null || true

echo "== fonts, certificates"
cp -r "$TARGET/usr/share/fonts" "$OUT/share/"
cp "$TARGET/etc/ssl/certs/ca-certificates.crt" "$OUT/etc/ssl/certs/"
cat > "$OUT/etc/fonts/fonts.conf" <<'EOF'
<?xml version="1.0"?>
<!DOCTYPE fontconfig SYSTEM "urn:fontconfig:fonts.dtd">
<fontconfig>
  <!-- Bundled fonts, then the TV's own (LG ships CJK and other scripts there). -->
  <dir prefix="relative">../../share/fonts</dir>
  <dir>/usr/share/fonts</dir>
  <dir>/mnt/fonts</dir>
  <cachedir prefix="xdg">fontconfig</cachedir>
  <alias><family>sans-serif</family><prefer><family>DejaVu Sans</family></prefer></alias>
  <alias><family>serif</family><prefer><family>DejaVu Serif</family></prefer></alias>
  <alias><family>monospace</family><prefer><family>DejaVu Sans Mono</family></prefer></alias>
</fontconfig>
EOF

echo "== check: nothing may need a glibc newer than the TV's 2.28"
bad=0
for f in "$OUT/wpe-engine" "$OUT"/libexec/* "$OUT"/lib/*.so* "$OUT"/lib/gio/modules/*.so; do
    v=$("$READELF" -V "$f" 2>/dev/null | grep -oE 'GLIBC_2\.[0-9]+' | sort -t. -k2 -n -u | tail -n 1)
    case "$v" in GLIBC_2.29|GLIBC_2.3[0-9]|GLIBC_2.4[0-9]) echo "  ! $(basename "$f") needs $v" >&2; bad=1 ;; esac
done
[ "$bad" = 0 ] || exit 1

echo "== staged: $(du -sh "$OUT" | cut -f1) in $OUT ($(ls "$OUT/lib" | wc -l) libraries)"
