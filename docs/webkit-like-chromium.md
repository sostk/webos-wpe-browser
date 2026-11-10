# WebKit like Chromium on the TV: JIT and GPU drawing

How Chromium on webOS gets a JavaScript JIT and GPU drawing on the TV's own (softfp) libraries, and the plan to do
the same with WPE WebKit. Research done 2026-11-10 against webOS-ports/chromium151 (main, 2026-11-09) and WPE WebKit 2.50.5.

## 1. JavaScript JIT

### Chromium (V8)
- V8 keeps doubles in VFP registers (d0-d15) inside JIT code on every ARM ABI.
- Only at **C-call boundaries** does it convert, when the ABI is softfp (`src/v8/src/codegen/arm/macro-assembler-arm.cc`):
  - `MovToFloatParameter(s)`: `vmov r0, r1, d0` (and `r2, r3, d1`) before calling C.
  - `MovFromFloatResult`: `vmov dN, r0, r1` after it.
  - `CalculateStackPassedWords`: a double counts as two core-register words.
- `use_eabi_hardfloat()` is decided at run time (`--mfloat_abi=auto` → `base::OS::ArmUsingHardFloat()`).
- So V8 runs its JIT in a softfp process, next to the TV's softfp GPU, media and system libraries.

### WebKit (JavaScriptCore) today
- `PlatformEnable.h` turns the ARMv7 JIT on only for `CPU(ARM_THUMB2) && CPU(ARM_HARDFP) && OS(LINUX)`. On the TV
  (softfp) we fall back to the CLoop interpreter: no JIT, no WebAssembly.
- JSC has no softfp handling at all. It assumes hardfp wherever JIT code calls C with a double
  (`CCallHelpers.h`: "ARM (hardfp, which we require) passes FP arguments in FP registers").
- History: JSC did run its JIT on softfp. 32-bit iOS used the softfp convention, and a leftover proves it: the
  Darwin ARM_THUMB2 `defineUnaryDoubleOpWrapper` in `ThunkGenerators.cpp` is exactly `vmov r0,r1,d0; blx f; vmov d0,r0,r1`.
- Ruled out: marking every JIT-called C++ function `__attribute__((pcs("aapcs-vfp")))`. GCC 14 drops that attribute
  in templates ("ignoring attributes on template argument"), and JSC passes operation pointers through templates, so
  missed functions could not be found, and C++ calls through deduced pointers could silently use the wrong convention.

### The patch: `buildroot-external/staged-patches/wpewebkit/0004-jsc-arm-softfp-jit.patch`
Applies the V8 model at JSC's C-call boundaries:

| Boundary | Change |
|---|---|
| `WTF/PlatformCPU.h` | `CPU(ARM_SOFTFP_VFP)`: VFP present (`__ARM_FP`) but no `__ARM_PCS_VFP` |
| `WTF/PlatformEnable.h` | JIT allowed for ARM_THUMB2 + (HARDFP or SOFTFP_VFP) on Linux |
| `jit/CCallHelpers.h` arguments (Baseline, DFG, ICs) | Double → next even core pair (r0:r1, r2:r3), else 8-byte aligned stack slot with r3 left unused (AAPCS C.3). Executed after the GPR shuffle as FPR→pair moves (`moveDoubleToInts`); poke offsets count no FP argument registers |
| `jit/CCallHelpers.h` `setupResults(FPRReg)` | Double result read from r0:r1 (`moveIntsToDouble`) |
| `wasm/js/WasmToJS.cpp` | `operationConvertToF32/F64` results from r0 / r0:r1 |
| `runtime/OptionsList.h` | WebAssembly BBQ off by default on softfp (its C-call table still assumes hardfp); OMG is already off on ARMv7 |

Audited and unchanged:
- **Interpreters:** LLInt and IPInt `.asm` C calls pass only pointers and ints.
- **JIT-to-JIT conventions:** JSToWasm/WasmToJS argument shuffles don't involve the C ABI.
- **Math thunks:** the Linux math-thunk wrappers are null, so they're unused.
- **FP-returning operations:** all 36 reach their results through `callOperation*` → `setupResults` (inventory in NOTES.md).

Build: `make WPEB_SOFTFP_JIT=y` (external.mk) compiles WebKit alone as Thumb-2 (`-mthumb`), which JSC's ARMv7 JIT needs.
The rest of the softfp tree stays as it is.

Validation (host, QEMU; before any TV run):
1. `ninja jsc` in the WebKit build folder.
2. `tools/jsc-softfp/run.sh <jsc> <libs>` runs `boundary.js`, which covers every changed boundary: two-double calls, the
   int,int,int,double stack case, double results, Float64Array/DataView, dtoa, and WasmToJS F32/F64. It compares the
   interpreter (`--useJIT=false`) with forced Baseline and forced DFG, bit-exactly, from the same binary.
3. Optionally, JSTests stress files at tag wpewebkit-2.50.5 through the same runner (`JS_TESTS=...`).
4. `tools/js-probe.sh` against the staged engine: JIT speed and WebAssembly.

Later (phase 1b): BBQ for softfp, which means `CCallingConventionArmThumb2` F32/F64 → core registers and the BBQ
`emitCCall` result location.

## 2. GPU drawing

### Chromium on webOS
- `ui/ozone/platform/wayland/gpu/wayland_surface_factory.cc` → `GLOzoneEGLWayland::CreateViewGLSurface` →
  `GLSurfaceWayland` (`gl_surface_wayland.cc`): `wl_egl_window_create()` on the window's own root `wl_surface`, a normal
  EGL window surface on the vendor driver, and `eglSwapBuffers` straight to LG's compositor. **No readback, no copy.**
- That path needs the `WaylandConnection` (`connection_`), so GPU work runs in the process that owns the window.
  `GLSurfaceEglReadbackWayland` (read back to SHM) is used only for software GL.
- The webOS ozone extensions (`extensions/webos/host/`) add `webos_surface_group_*` and `webos_foreign_video_window*`:
  webOS's ways to compose surfaces from several processes (and hardware video holes) into one app window.

### Our engine today
- WebKit renders in WPEWebProcess.
- It reads every frame back into SHM (`glReadPixels`, about 16 ms at 720p on Mali, measured) and copies it to the UI,
  which uploads it as a texture.
- In GPU mode Mali composites, but the readback and two copies stay; Skia GPU *painting* corrupted tiles on Mali.

### The webOS route to "Chromium-style" presentation
The webOS 6 SDK ships `wayland-webos/webos-surface-group.xml` (`wl_webos_surface_group_compositor`: "a way to group
multi client surfaces together") and `webos-foreign.xml`. So:
1. The UI app (window owner) creates a named surface group on its window and a layer below its chrome
   (`create_surface_group`, `create_layer`, `set_z_index`). The page area of the UI is left transparent.
2. WPEWebProcess (softfp, so it can load Mali; needs part 1) connects to the compositor and creates its own `wl_surface` +
   `wl_egl_window`, `attach`es it to the group's layer, and renders WebKit's composited frame with Mali EGL +
   `eglSwapBuffers`. No readback, no copies, like `GLSurfaceWayland`.
3. Input stays as now (UI → engine IPC).

WebKit side: WPE 2.50's libwpe renderer-backend path already supports rendering into a native EGL window from the web
process (`wpe_renderer_backend_egl_target` → `EGLNativeWindowType`; wpebackend-rdk's wayland-egl backend does this for
set-top boxes). A small "wpebackend-webos" (target = `wl_egl_window` in the surface group) plus switching the engine from
the WPEPlatform SHM view to that backend replaces the readback path. Skia GPU painting needs its own Mali fix; until
then CPU painting + GPU compositing straight to the screen still removes the readback and both copies.

### Implementation (written 2026-11-10; not yet run on the TV)
| Piece | Where | What it does |
|---|---|---|
| libwpe backend | `engine/webos-backend/wpebackend-webos.c` → `engine/lib/libWPEBackend-webos.so` | Engine half: one `wpe_view_backend` per tab, plus a SEQPACKET channel to that page's target. Web-process half: connects to the compositor (`WPEB_WAYLAND_SOCKET`), creates a `wl_surface` + `wl_egl_window` per page (WebKit's `EGLNativeWindowType`), attaches it to the UI's group on ATTACH and detaches it on DETACH. Paces frames with `wl_surface.frame` (swap interval 0, 250 ms fallback), on WebKit's compositing thread through a GSource on a private `wl_event_queue`. Offscreen contexts (WebGL) fall back to pbuffers. |
| Window owner | `engine/webos-backend/owner.c` → `engine/lib/libwpeb-owner.so`, used by `browser/direct_owner.cpp` | `create_surface_group(SDL's wl_surface, "wpeb-<pid>")` + layer `page` at z -1. Finds the compositor socket with `getpeername`. The UI `dlopen`s it, so `wpebrowser`'s DT_NEEDED is unchanged. |
| Engine | `engine/src/direct.cpp`, `main.cpp` | `WPE_BROWSER_PRESENT=direct`: `wpe_loader_init("libWPEBackend-webos.so")` before any WPEPlatform call (WebKit runs one mode per process). Tabs are `WebKitWebView`s with a `WebKitWebViewBackend`. Input goes through libwpe events (device pixels). The TV keyboard uses a `WebKitInputMethodContext` subclass. Shown = attach + visible. |
| IPC v5 | `common/wpe_ipc.h` | `WPE_ENGINE_PRESENT {mode, status, w, h}`: mode at start (tab 0), then each tab's surface status. |
| UI | `browser/main.cpp`, `page_view.*` | Opt-in `WPE_BROWSER_PRESENT=direct` (with `WPE_BROWSER_RENDERER=gpu`). PageView clears its rectangle to alpha 0 once the tab reports FIRST_FRAME. Always renders at full scale. A negative status switches back to SHM (engine restart) for the rest of the run. |
| Build | `tools/build-webos-backend.sh` (host, webOS NDK), staged by `tools/stage-engine.sh` | Uses the NDK because the TV's libwayland is 1.11: code generated by newer scanners calls `wl_proxy_marshal_flags`, which it lacks. |

Checked on the PC: `tools/direct-probe.sh` runs the staged engine under QEMU with direct mode. The engine reports
DIRECT, WebKit loads the backend in the web process, the page target reports "no compositor" (-1) back through
engine → UI, the page's JavaScript runs, and no SHM frames are sent. The UI and both libraries cross-build;
the host tests pass.

Still open:
- Fonts: direct mode has no WPEPlatform settings, so grayscale text comes from `rgba=none` in the bundled fonts.conf.
  Check that WebKit honours it.
- Geometry: where LSM places a group member, and whether it scales it, is unknown. The page area is below the
  toolbar, so the surface may need an offset (sg-probe answers this).
- Hidden tabs: does detaching a surface whose last buffer is still committed hide it at once?

Unknowns to probe on the TV first (small C probe, two processes):
- Does LSM on webOS 6.5 accept `attach` from a second client of the same app (same uid, in the app's jail)?
- Positioning and z-order relative to the owner.
- Frame pacing.
- Whether Mali's wayland-egl works from a process started by the app.

## 3. Order of work
1. Finish the hard-float engine build (running; the fallback that's sure to work).
2. Phase 1: softfp JIT. Apply 0004, rebuild WebKit softfp + Thumb-2 with `WPEB_SOFTFP_JIT=y` (several hours; not in
   parallel with another WebKit build on 15 GB RAM), run the jsc differential tests and js-probe, then TV.
   If it passes, the hard-float private runtime is no longer needed and the TV's Mali is usable again.
3. Phase 2: surface-group probe on the TV, then wpebackend-webos for direct GPU presentation.
4. Phase 3: hardware video through LG's media stack from the UI process (webOS gives media access to the app's main
   executable); the engine passes compressed data over IPC.
