#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Wren: what an LG webOS TV offers for hardware video (for webOS 3 and 4, where Wren has no video window yet).
#
# Read-only: it starts and changes nothing on the TV; it only reads files and asks two system services for
# information. The only file it writes is its report. LG's libraries stay on the TV: the report lists which of
# the function names Wren would use they contain, not the libraries themselves.
#
# Run as root (SSH) or from the Developer Mode shell:
#   sh video-caps.sh
# The report goes to /media/internal/wren-video-caps.txt (or /tmp if that is not writable). It contains the TV
# model and firmware but no serial number, network or account details. Please look through it before posting.
# If Wren has run since the TV was switched on, it also includes the media lines of Wren's logs.

out=${WREN_DIAG_OUT:-/media/internal/wren-video-caps.txt}
( : > "$out" ) 2>/dev/null || out=/tmp/wren-video-caps.txt
: > "$out"

say() { echo "$*" >> "$out"; }
section() { say ""; say "== $*"; }
run() { say "\$ $*"; ( eval "$*" ) >> "$out" 2>&1; }

# The library a soname resolves to in the usual places ("" if none).
find_lib() {
    for d in /usr/lib /lib /usr/lib/arm-webos-linux-gnueabi; do
        for f in "$d/$1" "$d/$1".*; do
            [ -e "$f" ] && { echo "$f"; return; }
        done
    done
}

# Which of these names appear in the file (searched as text: no nm on the TV). -w: whole names only, so
# NDL_DirectVideoPlay does not match inside NDL_DirectVideoPlayWithCallback.
has_names() {
    file=$1; shift
    for n in "$@"; do
        if grep -q -a -w "$n" "$file" 2>/dev/null; then say "  yes $n"; else say "  no  $n"; fi
    done
}

# luna-send prints nothing without a terminal on some TVs (ares-shell -r); script(1) gives it one.
luna() {
    say "\$ luna-send -n 1 -f $1 '$2'"
    if command -v script >/dev/null 2>&1; then
        script -q -c "luna-send -n 1 -w 3000 -f $1 '$2'" /dev/null >> "$out" 2>&1
    else
        luna-send -n 1 -w 3000 -f "$1" "$2" >> "$out" 2>&1
    fi
}

say "Wren video capabilities report, $(date -u '+%Y-%m-%d %H:%M UTC' 2>/dev/null)"
say "script version 2"

section "TV"
for f in /etc/starfish-release /etc/webos-release /etc/os-release; do [ -r "$f" ] && run "cat $f"; done
run "uname -a"
if command -v nyx-cmd >/dev/null 2>&1; then
    run "nyx-cmd OSInfo query webos_release webos_build_id webos_imagename"
    run "nyx-cmd DeviceInfo query board_type device_name"
fi
if command -v luna-send >/dev/null 2>&1; then
    luna luna://com.webos.service.tv.systemproperty/getSystemInfo '{"keys":["modelName","firmwareVersion","sdkVersion","boardType","UHD"]}'
fi
run "grep -E '^(Hardware|CPU part|model name)' /proc/cpuinfo | sort | uniq -c; grep -c ^processor /proc/cpuinfo"
run "grep -E '^(MemTotal|MemAvailable|SwapTotal)' /proc/meminfo"
run "id"

section "C libraries"
run "ls -l /lib/libc.so.6 /lib/ld-linux*.so.3 /usr/lib/libstdc++.so.6* /lib/libstdc++.so.6* 2>/dev/null"

section "Video libraries present"
run "ls -l /usr/lib 2>/dev/null | grep -iE 'acb|ndl|lgnc|playerapis|starfish|umedia|mediaclient|directmedia|resource' "

section "ACB (libAcbAPI): Starfish's display window and app state on webOS 3 and 4"
lib=$(find_lib libAcbAPI.so)
if [ -n "$lib" ]; then
    say "library: $lib"
    has_names "$lib" AcbAPI_create AcbAPI_initialize AcbAPI_setMediaId AcbAPI_setSinkType AcbAPI_setMediaVideoData \
        AcbAPI_setMediaAudioData AcbAPI_setState AcbAPI_setDisplayWindow AcbAPI_setCustomDisplayWindow \
        AcbAPI_setDisplayWindowEx AcbAPI_setWindowZOrder AcbAPI_resetScreenSaverTimer AcbAPI_setScreenSaverAutoMode \
        AcbAPI_startMute AcbAPI_stopMute AcbAPI_finalize AcbAPI_destroy
else
    say "libAcbAPI: not found"
fi
lib=$(find_lib libAcb.so)
[ -n "$lib" ] && run "grep -a -o 'luna://[A-Za-z0-9./_-]*' $lib | sort -u"

section "Starfish media pipeline (libplayerAPIs)"
lib=$(find_lib libplayerAPIs.so)
if [ -n "$lib" ]; then
    say "library: $lib"
    run "grep -a -o '_ZN17StarfishMediaAPIs[A-Za-z0-9_]*' $lib | sort -u"
else
    say "libplayerAPIs: not found"
fi

section "NDL DirectMedia"
lib=$(find_lib libNDL_directmedia.so)
if [ -n "$lib" ]; then
    say "library: $lib"
    say "API 2 (webOS 5+):"
    has_names "$lib" NDL_DirectMediaLoad NDL_DirectMediaUnload NDL_DirectMediaSetWindowId NDL_DirectVideoPlay
    say "API 1 (webOS 3.5 - 4.x; Init, Quit, GetError, SetAppState, SetArea and AudioPlay exist in API 2 too):"
    has_names "$lib" NDL_DirectMediaInit NDL_DirectMediaQuit NDL_DirectMediaGetError NDL_DirectMediaSetAppState \
        NDL_DirectVideoOpen NDL_DirectVideoClose NDL_DirectVideoSetCallback NDL_DirectVideoPlayWithCallback \
        NDL_DirectVideoSetArea NDL_DirectVideoStop NDL_DirectAudioOpen NDL_DirectAudioClose NDL_DirectAudioPlay
else
    say "libNDL_directmedia: not found"
fi

section "LGNC direct video (webOS 1 - 3.4)"
lib=$(find_lib liblgncopenapi.so)
if [ -n "$lib" ]; then
    say "library: $lib"
    has_names "$lib" LGNC_DIRECTVIDEO_Open LGNC_DIRECTVIDEO_Play LGNC_DIRECTVIDEO_Close LGNC_DIRECTAUDIO_Open
else
    say "liblgncopenapi: not found"
fi

section "Compositor interfaces (names found in the compositor's files)"
for f in /usr/sbin/surface-manager /usr/bin/surface-manager /usr/lib/libluna-surfacemanager*.so* \
         /usr/lib/libwebos-surface*.so* /usr/lib/libwayland-webos*.so* /usr/lib/libStarfishCompositor*.so*; do
    [ -e "$f" ] && [ ! -L "$f" ] || continue
    say "$f:"
    grep -a -o 'wl_webos_[a-z_]*\|wl_starfish_[a-z_]*' "$f" 2>/dev/null | sort -u | sed 's/^/  /' >> "$out"
done

section "Media services"
run "ls /etc/umediaserver 2>/dev/null"
for f in /etc/umediaserver/*.conf /etc/umediaserver/*.config; do
    [ -r "$f" ] || continue
    say "--- $f (first 200 lines)"
    head -n 200 "$f" >> "$out" 2>&1
done
run "ls /usr/share/luna-service2/services.d /usr/share/dbus-1/system-services /usr/share/ls2/system-services 2>/dev/null | grep -iE 'media|acb|video|audio|ums|starfish|display'"
run "ps 2>/dev/null | grep -iE 'umediaserver|starfish|acb|videooutput|audiod|surface' | grep -v grep"
if command -v luna-send >/dev/null 2>&1; then
    luna luna://com.webos.service.videooutput/getStatus '{}'
fi

section "GStreamer plugins on the TV"
run "ls /usr/lib/gstreamer-1.0 2>/dev/null | head -n 120"

section "Wren"
run "ls -d /media/developer/apps/usr/palm/applications/org.sostk.wpebrowser 2>/dev/null"
run "cat /media/developer/apps/usr/palm/applications/org.sostk.wpebrowser/appinfo.json 2>/dev/null"
L=/tmp/org.sostk.wpebrowser
if [ -d "$L" ]; then
    run "grep -E 'start app_id|\\[device\\]|\\[direct\\]|\\[lgmedia\\]|\\[media\\]|\\[engine\\]|hardware video' $L/events.log | head -n 150"
    run "grep -E 'video window|wpebmediasink|gst|media' $L/engine.log | head -n 80"
else
    say "no Wren logs (Wren has not run since the TV was switched on)"
fi

say ""
say "== end"
echo "Report written to $out"
