#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Differential test for the softfp JIT patch (0004): same jsc binary, interpreter vs forced JIT tiers.
# Host-only (QEMU user mode), no TV.
#   bash tools/jsc-softfp/run.sh JSC_BINARY LIB_DIR [SYSROOT]   (extra .js files: JS_TESTS="a.js b.js")
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
jsc=$1; libs=$2; sysroot=${3:-$HOME/webos-browser-build/br-out/staging}
tests=("$here/boundary.js" ${JS_TESTS:-})
run() { QEMU_LD_PREFIX="$sysroot" LD_LIBRARY_PATH="$libs" timeout 900 qemu-arm-static -L "$sysroot" "$jsc" "$@"; }
# Low thresholds: Baseline after 1 run, DFG after 10; no concurrent compilation, so tier-up is deterministic.
forced=(--useConcurrentJIT=false --thresholdForJITAfterWarmUp=1 --thresholdForJITSoon=1
        --thresholdForOptimizeAfterWarmUp=10 --thresholdForOptimizeAfterLongWarmUp=10 --thresholdForOptimizeSoon=10)
fail=0
for t in "${tests[@]}"; do
    name=$(basename "$t")
    ref=$(run --useJIT=false "$t" 2>&1) || { echo "FAIL $name: interpreter run failed"; echo "$ref" | tail -5; fail=1; continue; }
    for mode in baseline dfg; do
        if [ $mode = baseline ]; then extra=(--useDFGJIT=false); else extra=(); fi
        got=$(run "${forced[@]}" "${extra[@]}" "$t" 2>&1) || { echo "FAIL $name/$mode: crashed"; echo "$got" | tail -5; fail=1; continue; }
        if [ "$got" = "$ref" ]; then echo "ok   $name/$mode ($(echo "$ref" | wc -l) lines identical)"
        else echo "FAIL $name/$mode: output differs"; diff <(echo "$ref") <(echo "$got") | head -10; fail=1; fi
    done
    # Show that the JIT really ran: JSC prints compilation counts with --reportCompileTimes... keep it cheap:
    run "${forced[@]}" --reportBaselineCompileTimes=true --reportDFGCompileTimes=true "$t" 2>&1 >/dev/null \
        | grep -c -E "Baseline|DFG" | sed "s/^/     $name: JIT compilations logged: /" || true
done
exit $fail
