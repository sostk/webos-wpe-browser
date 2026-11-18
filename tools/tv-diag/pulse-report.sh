#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Wren: how PulseAudio runs on this TV. Read-only: it only reads files and lists processes.
# Run as root over SSH:  sh pulse-report.sh   (the report goes to /tmp/wren-pulse.txt)
out=/tmp/wren-pulse.txt
: > "$out"
say() { echo "$*" >> "$out"; }
run() { say "\$ $*"; ( eval "$*" ) >> "$out" 2>&1; say ""; }

say "Wren PulseAudio report"
run "cat /etc/starfish-release 2>/dev/null || cat /etc/os-release"

say "== PulseAudio processes (user, group, command line)"
for d in /proc/[0-9]*; do
    name=$(cat "$d/comm" 2>/dev/null)
    case "$name" in *pulse*|*audiod*) ;; *) continue ;; esac
    pid=${d#/proc/}
    say "pid $pid: $name"
    say "  command: $(tr '\0' ' ' < "$d/cmdline" 2>/dev/null)"
    grep -E '^(Uid|Gid|Groups):' "$d/status" >> "$out" 2>/dev/null
    # Only the variables that say where PulseAudio listens; nothing else from its environment.
    tr '\0' '\n' < "$d/environ" 2>/dev/null | grep -E '^(PULSE_|XDG_RUNTIME_DIR=|HOME=)' | sed 's/^/  env /' >> "$out"
    say "  open sockets:"
    ls -l "$d/fd" 2>/dev/null | grep -c socket | sed 's/^/    count /' >> "$out"
    say ""
done

say "== PulseAudio sockets and folders"
run "ls -la /tmp/pulseaudio* /tmp/pulse* /var/run/pulse* /run/pulse* /var/lib/pulse* 2>/dev/null"
run "find /tmp /var/run /run -maxdepth 3 \\( -name native -o -name '*pulse*' \\) 2>/dev/null | head -n 40"
run "grep -E 'pulse|native' /proc/net/unix | awk '{print \$NF}' | sort -u"
say "Who may open each listening socket and its folder:"
for s in $(grep -E 'pulse|native' /proc/net/unix | awk '{print $NF}' | sort -u); do
    ls -ld "$s" "$(dirname "$s")" >> "$out" 2>&1
done
say ""

say "== Groups (sound and media related)"
run "grep -iE 'pulse|audio|media|^[^:]*:[^:]*:50[0-9]:' /etc/group"

say "== Configuration"
for f in /etc/pulse/system.pa /etc/pulse/default.pa /etc/pulse/client.conf /etc/pulse/daemon.conf; do
    [ -r "$f" ] || continue
    say "--- $f (lines that are not comments)"
    grep -vE '^\s*(#|;|$)' "$f" >> "$out" 2>&1
    say ""
done
run "ls /etc/pulse /etc/pulse/*.d 2>/dev/null"

say "== Wren's own process (if Wren is running)"
for d in /proc/[0-9]*; do
    [ "$(cat "$d/comm" 2>/dev/null)" = "wpebrowser" ] || continue
    say "pid ${d#/proc/}:"
    grep -E '^(Uid|Gid|Groups):' "$d/status" >> "$out" 2>/dev/null
done

say "== end"
echo "Report written to $out"
