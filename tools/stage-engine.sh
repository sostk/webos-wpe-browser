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
#   etc/ssl/certs/ca-certificates.crt  read directly via WPE_BROWSER_CA_FILE
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
# GPU_PROCESS is enabled by this WebKit configuration; retain its helper too.
GPU_SEED=''
if [ -x "$LIBEXEC/WPEGPUProcess" ]; then
    cp "$LIBEXEC/WPEGPUProcess" "$OUT/libexec/"
    GPU_SEED="$OUT/libexec/WPEGPUProcess"
fi
cp "$TARGET"/usr/lib/gio/modules/libgiognutls.so "$OUT/lib/gio/modules/"

BUNDLE=$(ls "$TARGET"/usr/lib/wpe-webkit-*/injected-bundle/libWPEInjectedBundle.so)
mkdir -p "$OUT/lib/injected-bundle"
cp "$BUNDLE" "$OUT/lib/injected-bundle/"
SEEDS="$OUT/wpe-engine $OUT/libexec/WPEWebProcess $OUT/libexec/WPENetworkProcess $OUT/lib/gio/modules/libgiognutls.so $OUT/lib/injected-bundle/libWPEInjectedBundle.so $GPU_SEED"
# Mesa (software GL for the web process) is dlopen()ed by libepoxy by SONAME, so it is in no DT_NEEDED.
# It goes to lib/mesa/, which the UI adds to LD_LIBRARY_PATH only in software mode; in GPU mode the
# web process picks up the TV's own libEGL.so.1 / libGLESv2.so.2 (Mali) from the default search path.
MESA_SEEDS=''
MEDIA_SEEDS=''
if [ -f "$TARGET/usr/lib/libgstreamer-1.0.so.0" ]; then
    echo "== stage private GStreamer plugins and scanner"
    mkdir -p "$OUT/lib/gstreamer-1.0" "$OUT/etc/pulse"
    for plugin in "$TARGET"/usr/lib/gstreamer-1.0/*.so; do
        [ -f "$plugin" ] || { echo "GStreamer has no plugins" >&2; exit 1; }
        # Older/incremental target trees may retain this unused desktop sink.
        # The UI owns Wayland, and the engine must not load a bundled Wayland ABI.
        case "$(basename "$plugin")" in libgstwaylandsink.so) continue ;; esac
        cp -L "$plugin" "$OUT/lib/gstreamer-1.0/"
        MEDIA_SEEDS="$MEDIA_SEEDS $OUT/lib/gstreamer-1.0/$(basename "$plugin")"
    done
    for required in coreelements typefindfunctions playback app isomp4 matroska libav videoparsersbad audioparsers vpx opus pulseaudio soup adaptivedemux2; do
        [ -f "$OUT/lib/gstreamer-1.0/libgst$required.so" ] || {
            echo "missing required media plugin: $required (build multimedia dependencies first)" >&2
            exit 1
        }
    done
    scanner="$TARGET/usr/libexec/gstreamer-1.0/gst-plugin-scanner"
    [ -x "$scanner" ] || { echo "missing GStreamer plugin scanner" >&2; exit 1; }
    cp "$scanner" "$OUT/libexec/"
    MEDIA_SEEDS="$MEDIA_SEEDS $OUT/libexec/gst-plugin-scanner"
    # PulseAudio client uses the existing sound server. Never autospawn a daemon.
    printf 'autospawn = no\n' > "$OUT/etc/pulse/client.conf"
    for tool in gst-inspect-1.0 gst-launch-1.0; do
        if [ -x "$TARGET/usr/bin/$tool" ]; then
            cp "$TARGET/usr/bin/$tool" "$OUT/libexec/"
            MEDIA_SEEDS="$MEDIA_SEEDS $OUT/libexec/$tool"
        fi
    done
fi
for so in libEGL.so.1 libGLESv2.so.2; do
    f=$(ls "$TARGET"/usr/lib/$so 2>/dev/null || true)
    [ -n "$f" ] || { echo "missing $so in target (Mesa?)" >&2; exit 1; }
    MESA_SEEDS="$MESA_SEEDS $f"
done

find_lib() {
    for d in "$TARGET/usr/lib" "$TARGET/usr/lib/pulseaudio" "$TARGET/lib" "$STAGING/usr/lib" "$STAGING/lib"; do
        [ -e "$d/$1" ] && { echo "$d/$1"; return 0; }
    done
    return 1
}

# collect <seeds> <dest-dir>: copies the DT_NEEDED closure of the seeds into dest-dir, skipping the TV's
# glibc and anything already collected (the global $seen), so a second call only adds what is new.
seen=' '
collect() {
    todo="$1"
    dest="$2"
    while [ -n "$todo" ]; do
        next=''
        for f in $todo; do
            for need in $("$READELF" -d "$f" 2>/dev/null | sed -n 's/.*(NEEDED).*\[\(.*\)\]/\1/p'); do
                case "$SYSTEM_LIBS" in *" $need "*) continue ;; esac
                case "$seen" in *" $need "*) continue ;; esac
                seen="$seen$need "
                path=$(find_lib "$need") || { echo "  ! not found: $need (needed by $(basename "$f"))" >&2; exit 1; }
                cp -L "$path" "$dest/$need"
                next="$next $dest/$need"
            done
        done
        todo=$next
    done
}

echo "== collect libraries"
collect "$SEEDS $MEDIA_SEEDS" "$OUT/lib"
mkdir -p "$OUT/lib/mesa"
for f in $MESA_SEEDS; do cp -L "$f" "$OUT/lib/mesa/$(basename "$f")"; done
# Mesa's own closure (libgallium, libLLVM, ...), minus what the engine already shares from lib/.
collect "$(for f in $MESA_SEEDS; do printf '%s ' "$OUT/lib/mesa/$(basename "$f")"; done)" "$OUT/lib/mesa"
"$BR"/host/bin/*-strip --strip-unneeded "$OUT"/lib/*.so* "$OUT"/lib/mesa/*.so* "$OUT"/lib/gstreamer-1.0/*.so "$OUT"/lib/gio/modules/*.so "$OUT"/lib/injected-bundle/*.so "$OUT"/libexec/* 2>/dev/null || true

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
for f in "$OUT/wpe-engine" "$OUT"/libexec/* "$OUT"/lib/*.so* "$OUT"/lib/mesa/*.so* "$OUT"/lib/gstreamer-1.0/*.so "$OUT"/lib/gio/modules/*.so "$OUT"/lib/injected-bundle/*.so; do
    v=$("$READELF" -V "$f" 2>/dev/null | grep -oE 'GLIBC_2\.[0-9]+' | sort -t. -k2 -n -u | tail -n 1)
    case "$v" in GLIBC_2.29|GLIBC_2.3[0-9]|GLIBC_2.4[0-9]) echo "  ! $(basename "$f") needs $v" >&2; bad=1 ;; esac
done
[ "$bad" = 0 ] || exit 1

echo "== staged: $(du -sh "$OUT" | cut -f1) in $OUT ($(ls "$OUT/lib" | wc -l) libraries)"
