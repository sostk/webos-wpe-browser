// SPDX-License-Identifier: GPL-3.0-or-later
// Exercises every JIT -> C boundary that patch 0004 (JSC softfp JIT) changes. Prints one deterministic line
// per case; tools/jsc-softfp/run.sh compares the output of the interpreter (--useJIT=false) with the
// output after forcing Baseline and DFG compilation. Any difference means a double crossed the C ABI wrongly.
"use strict";
const out = [];
function emit(name, v) { out.push(name + "=" + (Object.is(v, -0) ? "-0" : String(v))); }
const N = 2000;                       // enough iterations to reach the DFG with the thresholds in run.sh
const xs = [0.5, -0.5, 1.5, 2.5, -2.5, 1e-310, 1.7976931348623157e308, -0, 3.141592653589793, 1e21, -7.25, 123456.789];

// double, double -> double (operationMathPow: both arguments in pairs r0:r1, r2:r3)
function pow(a, b) { return a ** b; }
// fmod (Math::fmodDouble: two doubles), floor/ceil/round/trunc slow paths, sqrt, abs, fround, f16round
function mod(a, b) { return a % b; }
function rounding(a) { return Math.floor(a) + 2 * Math.ceil(a) + 3 * Math.round(a) + 5 * Math.trunc(a); }
function roots(a) { return Math.sqrt(Math.abs(a)) + Math.fround(a) + (Math.f16round ? Math.f16round(a) : 0); }
// operationArithMinMultipleDouble / Max: buffer + count, double result
function minmax(a, b, c, d) { return Math.min(a, b, c, d) * 3 + Math.max(a, b, c, d); }
// int, int, int, double: the double cannot use r3 and goes on the stack (AAPCS C.3), e.g.
// operationPutDoubleByValBeyondArrayBounds(globalObject, object, index, double)
function beyond(arr, i, v) { arr[i] = v; return arr; }
// double -> int (operationToInt32 with a double argument)
function toInt(a) { return (a * 1e10) | 0; }

// Hash of the exact bit patterns (NaN, -0 and last-bit differences all count).
const bits = new DataView(new ArrayBuffer(8));
let hash = 17;
function mix(v) { bits.setFloat64(0, v); hash = Math.imul(hash ^ bits.getUint32(0), 16777619) ^ bits.getUint32(4); }
for (let it = 0; it < N; ++it) {
    const a = xs[it % xs.length], b = xs[(it * 7 + 3) % xs.length] || 0.75;
    mix(pow(Math.abs(a) % 10 + 0.5, b % 3)); mix(mod(a, b)); mix(rounding(a * 1.25)); mix(roots(a));
    mix(minmax(a, b, -a, b * 0.5)); mix(toInt(a));
    if (it % 400 === 399) emit("hash" + it, hash >>> 0);
}
for (const a of xs) {
    emit("pow" + a, pow(2.5, a % 5)); emit("mod" + a, mod(a, 0.7)); emit("round" + a, rounding(a));
    emit("roots" + a, roots(a)); emit("minmax" + a, minmax(a, 1.5, -a, 2.25)); emit("int" + a, toInt(a));
}
for (let it = 0; it < N; ++it) {
    const arr = [1.5, 2.5];
    beyond(arr, 5 + (it & 3), xs[it % xs.length] * 1.0001);
    if (it % 500 === 499) emit("beyond" + it, arr.join(","));
}
// Float64Array / DataView: doubles through typed-array slow paths
const f64 = new Float64Array(64), dv = new DataView(new ArrayBuffer(64));
for (let it = 0; it < N; ++it) { f64[it & 63] = xs[it % xs.length] / 3; dv.setFloat64((it & 7) * 8, f64[it & 63], it & 1); }
emit("f64", Array.from(f64.slice(0, 12)).join(","));
emit("dv", [0, 1, 2, 3, 4, 5, 6, 7].map(i => dv.getFloat64(i * 8, i & 1)).join(","));
// Number <-> string conversions take doubles through C++ (dtoa) on the slow path.
let s = "";
for (let it = 0; it < 200; ++it) s = String(xs[it % xs.length] * (it + 1) / 7);
emit("str", s); emit("parse", parseFloat("3.25e-5") * 2);

// WebAssembly (IPInt + WasmToJS): an imported JS function returning non-number values forces
// operationConvertToF64 / operationConvertToF32 (results in r0:r1 / r0 under softfp).
if (typeof WebAssembly === "object") {
    // (module (import "m" "f" (func $f (result f64))) (import "m" "g" (func $g (result f32)))
    //   (func (export "run") (result f64) (f64.add (call $f) (f64.promote_f32 (call $g)))))
    const bytes = new Uint8Array([0,97,115,109,1,0,0,0,1,13,3,96,0,1,124,96,0,1,125,96,0,1,124,2,13,2,1,109,1,102,0,0,1,109,1,103,0,1,3,2,1,2,7,7,1,3,114,117,110,0,2,10,10,1,8,0,16,0,16,1,187,160,11]);
    try {
        const inst = new WebAssembly.Instance(new WebAssembly.Module(bytes), { m: { f: () => "2.75", g: () => ({ valueOf() { return 0.5; } }) } });
        let w = 0;
        for (let it = 0; it < N; ++it) w += inst.exports.run();
        emit("wasm", w);
    } catch (e) { emit("wasm", "error " + e); }
} else emit("wasm", "unavailable");

print(out.join("\n"));
