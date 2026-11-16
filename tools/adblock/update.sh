#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Refreshes the ad blocker's filter lists (before a release); tools/stage-engine.sh converts and compiles them.
# EasyList and EasyPrivacy: https://easylist.to, dual-licensed GPL-3.0-or-later and CC BY-SA 3.0.
set -eu
cd "$(dirname "$0")/lists"
for l in easylist easyprivacy; do
    curl -fsSL -o "$l.txt.new" "https://easylist.to/easylist/$l.txt"
    mv "$l.txt.new" "$l.txt"
    echo "$l: $(grep -m1 '^! Version:' "$l.txt")"
done
