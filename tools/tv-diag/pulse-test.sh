#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Wren: can this TV's PulseAudio play Wren's sound? Runs pulse-probe (tools/tv-diag/pulse-probe.c) with Wren's
# bundled C library and the TV's own libpulse, the way Wren's engine does, first as root, then as Wren's app user.
# Each run lists the sound server's sinks and plays a short quiet tone (440 Hz) on each sink it opens.
#
# Needs Wren installed (0.3.4 alpha 6 or later). Run as root over SSH, with pulse-probe next to this script:
#   sh pulse-test.sh            (add --plain to skip the format stream, which makes PulseAudio 2.1 on webOS 3.9
#                                quit; --silent for no tones; --all-sinks to try every sink)
# PulseAudio's process id is shown before and after each run: a new id means the sound server restarted.
# The report goes to /tmp/wren-pulse-test.txt as well as the screen. Read-only apart from the sound.

dir=$(cd "$(dirname "$0")" && pwd)
probe=$dir/pulse-probe
app=/media/developer/apps/usr/palm/applications/org.sostk.wpebrowser
out=/tmp/wren-pulse-test.txt
[ -x "$probe" ] || chmod +x "$probe" 2>/dev/null
[ -f "$probe" ] || { echo "pulse-probe not found next to this script"; exit 1; }

# Wren's bundled glibc on TVs older than glibc 2.28 (webOS 3 and 4), else the TV's own.
if [ -x "$app/engine/lib/glibc/ld-linux.so.3" ]; then
    run="$app/engine/lib/glibc/ld-linux.so.3 --library-path $app/engine/lib/glibc:/usr/lib:/lib:/usr/lib/pulseaudio"
else
    run="/lib/ld-linux.so.3 --library-path /usr/lib:/lib:/usr/lib/pulseaudio"
fi

# Wren's app user and groups, from its log if it has run since the TV started (else the usual ones).
as=5601:5000:29,44,505,509,777
log=/tmp/org.sostk.wpebrowser/events.log
if [ -r "$log" ]; then
    line=$(grep -a -m1 '\[gamepad\].*uid' "$log")
    uid=$(echo "$line" | sed -n 's/.*uid \([0-9]*\).*/\1/p')
    gid=$(echo "$line" | sed -n 's/.* gid \([0-9]*\).*/\1/p')
    groups=$(echo "$line" | sed -n 's/.*groups \[\([0-9,]*\)\].*/\1/p')
    [ -n "$uid" ] && [ -n "$gid" ] && as=$uid:$gid:$groups
fi

{
    echo "Wren PulseAudio test, $(date -u '+%Y-%m-%d %H:%M UTC' 2>/dev/null)"
    cat /etc/starfish-release 2>/dev/null
    echo
    echo "== as root (PulseAudio pid before: $(pidof pulseaudio))"
    PULSE_SERVER=unix:/var/run/pulse/native $run "$probe" "$@" 2>&1
    echo "PulseAudio pid after: $(pidof pulseaudio)"
    # If it restarted, give it time to take connections again.
    for i in 1 2 3 4 5 6 7 8 9 10; do [ -S /var/run/pulse/native ] && pidof pulseaudio >/dev/null && break; sleep 1; done
    sleep 2
    echo
    echo "== as Wren's app user ($as) (PulseAudio pid before: $(pidof pulseaudio))"
    PULSE_SERVER=unix:/var/run/pulse/native $run "$probe" --as "$as" "$@" 2>&1
    echo "PulseAudio pid after: $(pidof pulseaudio)"
} | tee "$out"
echo
echo "Report written to $out"
