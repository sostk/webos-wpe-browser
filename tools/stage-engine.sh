#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Builds wpe-engine against the Buildroot output and stages it, with everything it needs at run time, into
# build/engine-stage/engine/ (packaged by browser/CMakeLists.txt). Runs inside the build container:
#   docker/run.sh 'sh /src/tools/stage-engine.sh'
#
# Layout of the staged folder (paths the UI app sets up in engine_client.cpp):
#   wpe-engine                         the engine (RUNPATH $ORIGIN/lib)
#   libexec/WPEWebProcess, WPENetworkProcess
#   lib/*.so*                          every library the above need, except the TV's glibc
#   lib/mesa/                          Mesa (libEGL/libGLESv2/libgallium/libLLVM: software GL for WPEWebProcess), only
#                                      for the hard-float engine or with STAGE_MESA=1: the softfp engine draws with the
#                                      TV's GPU (the default since 2026-11-11) and Mesa is 75 MB (30 MB of the ipk)
#   lib/gio/modules/libgiognutls.so    TLS for libsoup
#   lib/libWPEBackend-webos.so         direct GPU presentation, when build/webos-backend has it (softfp);
#   lib/libwpeb-owner.so               its window-owner half, dlopen()ed by the UI
#   etc/fonts/fonts.conf, share/fonts  bundled fonts (+ the TV's own font folders)
#   etc/ssl/certs/ca-certificates.crt  read directly via WPE_BROWSER_CA_FILE
# Hard-float Buildroot output (BR2_ARM_EABIHF, tools/build-hf.sh): glibc is bundled too (lib/ld-linux-armhf.so.3,
# libc, NSS modules for DNS, lib/gconv), and every executable's PT_INTERP is set to $LOADER_LINK, a link the UI
# creates in its runtime folder (browser/private_loader.cpp). The TV has no hard-float loader of its own.
# Softfp with STAGE_GLIBC=1 (the default): glibc 2.28 goes to lib/glibc/ (loader, libc & co., NSS modules, gconv/) and
# PT_INTERP is set to $LOADER_LINK too. The UI points that link at lib/glibc/ld-linux.so.3 on TVs whose glibc is older
# than 2.28 (webOS 4), else at the TV's /lib/ld-linux.so.3, so the bundled glibc is only used where it is needed.
set -eu

BR=${BR:-/build/br-out}
SRC=${SRC:-/src}
OUT=${OUT:-/src/build/engine-stage/engine}
TARGET=$BR/target
ENGINE_BUILD=${ENGINE_BUILD:-/build/engine-build}
HF=no
grep -qx 'BR2_ARM_EABIHF=y' "$BR/.config" && HF=yes
STAGE_GLIBC=${STAGE_GLIBC:-1}
[ "$HF" = yes ] && STAGE_GLIBC=0  # hard-float bundles glibc in lib/ (below)
if [ "$HF" = yes ]; then
    LOADER_LINK=${LOADER_LINK:-/tmp/org.sostk.wpebrowser/ld-linux-armhf.so.3}
else
    LOADER_LINK=${LOADER_LINK:-/tmp/org.sostk.wpebrowser/ld-linux.so.3}
fi
# gconv modules of the toolchain's glibc (Buildroot does not copy them to the target unless asked).
TOOLCHAIN_GCONV=${TOOLCHAIN_GCONV:-/build/x-tools/arm-webos-linux-gnueabi/arm-webos-linux-gnueabi/sysroot/usr/lib/gconv}
STAGING=$BR/staging
READELF=$BR/host/bin/arm-webos-linux-gnueabi-readelf
[ -x "$READELF" ] || READELF=$(ls "$BR"/host/bin/*-readelf | head -n 1)

# The TV provides these (glibc 2.28); bundling them would break the loader.
SYSTEM_LIBS=' ld-linux.so.3 libc.so.6 libm.so.6 libdl.so.2 libpthread.so.0 librt.so.1 libresolv.so.2 libutil.so.1 libanl.so.1 libnsl.so.1 libcrypt.so.1 '
# Sound: the TV's PulseAudio 9.0 (LG-patched, system mode) drops newer clients when they open a stream (our
# PulseAudio 17 libpulse: "Connection terminated", memfd/SHM off made no difference; TV, 2026-11-11). Its own
# libpulse.so.0 works with our GStreamer pulsesink, so the engine uses that one, with its own dependencies.
SYSTEM_LIBS="$SYSTEM_LIBS"'libpulse.so.0 '
# Hard-float: nothing comes from the TV; the loader itself is copied below and is in no DT_NEEDED closure.
[ "$HF" = yes ] && SYSTEM_LIBS=' ld-linux-armhf.so.3 '

echo "== build wpe-engine"
cmake -S "$SRC/engine" -B "$ENGINE_BUILD" -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="$BR/host/share/buildroot/toolchainfile.cmake" -DCMAKE_BUILD_TYPE=Release >/dev/null
cmake --build "$ENGINE_BUILD"

rm -rf "$OUT"
mkdir -p "$OUT/lib/gio/modules" "$OUT/libexec" "$OUT/etc/fonts" "$OUT/etc/ssl/certs" "$OUT/share"
cp "$ENGINE_BUILD/wpe-engine" "$OUT/"
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
STAGE_MESA=${STAGE_MESA:-$([ "$HF" = yes ] && echo 1 || echo 0)}
if [ -f "$TARGET/usr/lib/libgstreamer-1.0.so.0" ]; then
    echo "== stage private GStreamer plugins and scanner"
    mkdir -p "$OUT/lib/gstreamer-1.0" "$OUT/etc/pulse"
    for plugin in "$TARGET"/usr/lib/gstreamer-1.0/*.so; do
        [ -f "$plugin" ] || { echo "GStreamer has no plugins" >&2; exit 1; }
        # Older/incremental target trees may retain this unused desktop sink.
        # The UI owns Wayland, and the engine must not load a bundled Wayland ABI.
        case "$(basename "$plugin")" in libgstwaylandsink.so) continue ;; esac
        # Not used by WebKit or the browser: NVIDIA decoders, RTSP, AVI, DSD audio, debugging and tracing elements,
        # pipelines split across processes. (debugutilsbad stays: WebKit uses its fakevideosink; subenc too: webvttenc.)
        case "$(basename "$plugin")" in
            libgstnvcodec.so | libgstrtsp.so | libgstavi.so | libgstdsd.so | libgstbasedebug.so | libgstcoretracers.so | \
            libgstipcpipeline.so) continue ;;
        esac
        cp -L "$plugin" "$OUT/lib/gstreamer-1.0/"
        MEDIA_SEEDS="$MEDIA_SEEDS $OUT/lib/gstreamer-1.0/$(basename "$plugin")"
    done
    for required in coreelements typefindfunctions playback app isomp4 matroska libav videoparsersbad audioparsers vpx opus pulseaudio soup adaptivedemux2; do
        [ -f "$OUT/lib/gstreamer-1.0/libgst$required.so" ] || {
            echo "missing required media plugin: $required (build multimedia dependencies first)" >&2
            exit 1
        }
    done
    # The browser's hardware-video sinks (engine/gst), built with wpe-engine.
    if [ -f "$ENGINE_BUILD/libgstwpebmedia.so" ]; then
        cp "$ENGINE_BUILD/libgstwpebmedia.so" "$OUT/lib/gstreamer-1.0/"
        "$BR"/host/bin/*-strip "$OUT/lib/gstreamer-1.0/libgstwpebmedia.so" 2>/dev/null || true
        MEDIA_SEEDS="$MEDIA_SEEDS $OUT/lib/gstreamer-1.0/libgstwpebmedia.so"
    fi
    scanner="$TARGET/usr/libexec/gstreamer-1.0/gst-plugin-scanner"
    [ -x "$scanner" ] || { echo "missing GStreamer plugin scanner" >&2; exit 1; }
    cp "$scanner" "$OUT/libexec/"
    MEDIA_SEEDS="$MEDIA_SEEDS $OUT/libexec/gst-plugin-scanner"
    # PulseAudio client uses the existing sound server. Never autospawn a daemon.
    printf 'autospawn = no\n' > "$OUT/etc/pulse/client.conf"
    # Debugging tools, only with STAGE_GST_TOOLS=1.
    for tool in gst-inspect-1.0 gst-launch-1.0; do
        if [ "${STAGE_GST_TOOLS:-0}" = 1 ] && [ -x "$TARGET/usr/bin/$tool" ]; then
            cp "$TARGET/usr/bin/$tool" "$OUT/libexec/"
            MEDIA_SEEDS="$MEDIA_SEEDS $OUT/libexec/$tool"
        fi
    done
fi
for so in libEGL.so.1 libGLESv2.so.2; do
    [ "$STAGE_MESA" = 1 ] || break
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
rm -rf "$OUT/lib/mesa"
if [ "$STAGE_MESA" = 1 ]; then
    mkdir -p "$OUT/lib/mesa"
    for f in $MESA_SEEDS; do cp -L "$f" "$OUT/lib/mesa/$(basename "$f")"; done
    # Mesa's own closure (libgallium, libLLVM, ...), minus what the engine already shares from lib/.
    collect "$(for f in $MESA_SEEDS; do printf '%s ' "$OUT/lib/mesa/$(basename "$f")"; done)" "$OUT/lib/mesa"
else
    echo "== no Mesa (GPU rendering only; STAGE_MESA=1 bundles it)"
fi
if [ "$HF" = yes ]; then
    echo "== private glibc (hard-float)"
    cp -L "$TARGET/lib/ld-linux-armhf.so.3" "$OUT/lib/"
    # dlopen()ed by getaddrinfo/getpwuid through the TV's /etc/nsswitch.conf; the TV's own are softfp.
    NSS_SEEDS=''
    for so in libnss_files.so.2 libnss_dns.so.2; do
        cp -L "$TARGET/lib/$so" "$OUT/lib/"
        NSS_SEEDS="$NSS_SEEDS $OUT/lib/$so"
    done
    collect "$NSS_SEEDS" "$OUT/lib"
    gconv=$(ls -d "$TARGET"/usr/lib/gconv 2>/dev/null || true)
    [ -n "$gconv" ] || { echo "missing usr/lib/gconv (BR2_TOOLCHAIN_GLIBC_GCONV_LIBS_COPY)" >&2; exit 1; }
    cp -r "$gconv" "$OUT/lib/gconv"
    PATCHELF=$BR/host/bin/patchelf
    for exe in "$OUT/wpe-engine" "$OUT"/libexec/*; do
        "$READELF" -l "$exe" 2>/dev/null | grep -q 'Requesting program interpreter' || continue
        "$PATCHELF" --set-interpreter "$LOADER_LINK" "$exe"
    done
fi
if [ "$STAGE_GLIBC" = 1 ]; then
    echo "== bundled glibc (softfp, lib/glibc; used only on TVs with glibc older than 2.28)"
    mkdir -p "$OUT/lib/glibc"
    for so in $SYSTEM_LIBS libnss_files.so.2 libnss_dns.so.2; do
        case "$so" in libpulse*) continue ;; esac
        cp -L "$TARGET/lib/$so" "$OUT/lib/glibc/"
    done
    [ -d "$TOOLCHAIN_GCONV" ] || { echo "missing $TOOLCHAIN_GCONV (gconv modules)" >&2; exit 1; }
    cp -r "$TOOLCHAIN_GCONV" "$OUT/lib/glibc/gconv"
    PATCHELF=$BR/host/bin/patchelf
    for exe in "$OUT/wpe-engine" "$OUT"/libexec/*; do
        "$READELF" -l "$exe" 2>/dev/null | grep -q 'Requesting program interpreter' || continue
        "$PATCHELF" --set-interpreter "$LOADER_LINK" "$exe"
    done
    "$BR"/host/bin/*-strip --strip-unneeded "$OUT"/lib/glibc/*.so* "$OUT"/lib/glibc/gconv/*.so 2>/dev/null || true
fi
"$BR"/host/bin/*-strip --strip-unneeded "$OUT"/lib/*.so* "$OUT"/lib/mesa/*.so* "$OUT"/lib/gstreamer-1.0/*.so "$OUT"/lib/gio/modules/*.so "$OUT"/lib/injected-bundle/*.so "$OUT"/libexec/* 2>/dev/null || true

# Direct GPU presentation (engine/webos-backend, built on the host with the webOS NDK by
# tools/build-webos-backend.sh). dlopen()ed by the engine and by libwpe in WPEWebProcess; its libwayland-client
# and libwayland-egl are the TV's own (Mali), so they are deliberately not collected. softfp only.
BACKEND=$SRC/build/webos-backend/libWPEBackend-webos.so
if [ "$HF" = no ] && [ -f "$BACKEND" ]; then
    echo "== direct presentation backend"
    cp "$BACKEND" "$SRC/build/webos-backend/libwpeb-owner.so" "$OUT/lib/"
fi

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
  <!-- Grayscale antialiasing: subpixel (LCD) text shows coloured fringes on a TV panel. SHM mode also gets this
       from WebKit patch 0003; direct mode (libwpe API, no WPEPlatform settings) only from here. -->
  <match target="font"><edit name="rgba" mode="assign"><const>none</const></edit></match>
</fontconfig>
EOF

if [ "$HF" = yes ]; then
    echo "== check: hard-float everywhere, loader at $LOADER_LINK"
    bad=0
    for f in "$OUT/wpe-engine" "$OUT"/libexec/* "$OUT"/lib/*.so* "$OUT"/lib/mesa/*.so* "$OUT"/lib/gstreamer-1.0/*.so; do
        # A softfp object would be refused by the hard-float loader at run time; catch it here.
        if "$READELF" -h "$f" 2>/dev/null | grep -q 'soft-float ABI'; then echo "  ! soft-float: $(basename "$f")" >&2; bad=1; fi
    done
    for f in "$OUT/wpe-engine" "$OUT"/libexec/*; do
        i=$("$READELF" -l "$f" | sed -n 's/.*Requesting program interpreter: \(.*\)]/\1/p')
        [ "$i" = "$LOADER_LINK" ] || { echo "  ! $(basename "$f") loader is '$i'" >&2; bad=1; }
    done
    [ "$bad" = 0 ] || exit 1
fi
# Licences of the bundled Buildroot packages (WebKit, GStreamer, GLib, FFmpeg, ...): Buildroot's legal-info
# (`make ... legal-info`), as licenses/<package>-<version>/ plus manifest.csv (package, version, licence, files,
# source). Mesa and LLVM only when Mesa is bundled.
LEGAL="$BR/legal-info"
rm -rf "$OUT/licenses"
if [ -f "$LEGAL/manifest.csv" ]; then
    echo "== licences (Buildroot legal-info)"
    mkdir -p "$OUT/licenses"
    skip='^"(mesa3d|llvm)",'
    [ "$STAGE_MESA" = 1 ] && skip='^$'
    grep -vE "$skip" "$LEGAL/manifest.csv" > "$OUT/licenses/manifest.csv"
    for d in "$LEGAL"/licenses/*/; do
        name=$(basename "$d")
        pkg=$(printf '%s' "$name" | sed 's/-[0-9][^-]*$//')
        grep -qE "^\"$pkg\"," "$OUT/licenses/manifest.csv" || continue
        cp -r "$d" "$OUT/licenses/$name"
    done
    # This repository's own package (libocdm) is GPL-3.0-or-later, like the app; legal-info leaves a local
    # source out of its manifest, so it is added here from the source tree.
    if [ -f "$TARGET/usr/lib/libocdm.so.1" ] || [ -f "$TARGET/usr/lib/libocdm.so" ]; then
        rm -rf "$OUT"/licenses/ocdm-widevine-*
        mkdir -p "$OUT/licenses/ocdm-widevine"
        cp "$SRC/drm/ocdm-widevine/LICENSE" "$OUT/licenses/ocdm-widevine/"
        printf '"ocdm-widevine","","GPL-3.0-or-later","LICENSE","","this repository (drm/ocdm-widevine)"\n' >> "$OUT/licenses/manifest.csv"
    fi
    cp "$SRC/LICENSE" "$OUT/licenses/LICENSE"
else
    echo "  ! no $LEGAL/manifest.csv: run Buildroot's legal-info first; the engine's licences are not packaged" >&2
fi

if [ "$STAGE_GLIBC" = 1 ]; then
    echo "== check: every executable's loader is $LOADER_LINK"
    bad=0
    for f in "$OUT/wpe-engine" "$OUT"/libexec/*; do
        "$READELF" -l "$f" 2>/dev/null | grep -q 'Requesting program interpreter' || continue
        i=$("$READELF" -l "$f" | sed -n 's/.*Requesting program interpreter: \(.*\)]/\1/p')
        [ "$i" = "$LOADER_LINK" ] || { echo "  ! $(basename "$f") loader is '$i'" >&2; bad=1; }
    done
    [ -x "$OUT/lib/glibc/ld-linux.so.3" ] || { echo "  ! no lib/glibc/ld-linux.so.3" >&2; bad=1; }
    [ "$bad" = 0 ] || exit 1
fi

echo "== check: nothing may need a glibc newer than the TV's 2.28"
bad=0
for f in "$OUT/wpe-engine" "$OUT"/libexec/* "$OUT"/lib/*.so* "$OUT"/lib/mesa/*.so* "$OUT"/lib/gstreamer-1.0/*.so "$OUT"/lib/gio/modules/*.so "$OUT"/lib/injected-bundle/*.so; do
    v=$("$READELF" -V "$f" 2>/dev/null | grep -oE 'GLIBC_2\.[0-9]+' | sort -t. -k2 -n -u | tail -n 1)
    case "$v" in GLIBC_2.29|GLIBC_2.3[0-9]|GLIBC_2.4[0-9]) echo "  ! $(basename "$f") needs $v" >&2; bad=1 ;; esac
done
[ "$bad" = 0 ] || exit 1

echo "== staged: $(du -sh "$OUT" | cut -f1) in $OUT ($(ls "$OUT/lib" | wc -l) libraries)"
