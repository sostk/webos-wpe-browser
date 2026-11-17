# Patches

## WPE WebKit 2.52.6 (`buildroot-external/patches/wpewebkit/`)

Buildroot still packages 2.50.5; `tools/patch-buildroot.sh` applies
`buildroot-external/buildroot-patches/0001-package-wpewebkit-bump-to-2.52.6.patch` (version and hashes) to the
Buildroot checkout; the build scripts run it before configuring. 2.52 is the last series with the ARMv7 JavaScript
JIT: 2.54 switched it off and its 32-bit Baseline inline caches are not implemented (see NOTES.md, 2026-11-17).

| Patch | Why |
|---|---|
| `0001-honour-WEBKIT_EXEC_PATH-in-release-builds` | WebKit's helper processes ship inside the app package, not at the compile-time path |
| `0002-use-the-default-EGL-display-without-Mesa-surfaceless` | the TV's Mali EGL has no `EGL_MESA_platform_surfaceless`: use its default display with a surfaceless context or a 1×1 pbuffer |
| `0004-jsc-arm-softfp-jit` | JavaScriptCore's ARMv7 JIT on webOS's softfp ABI (V8's model: doubles in core registers only at C-call boundaries); without it JSC falls back to the slow interpreter |
| `0005-skia-arm32-gcc-raster-pipeline-abi` | Skia's `pcs("aapcs-vfp")` stage ABI mismatches under GCC softfp: gradients, masks and colour filters were drawn as stripes |
| `0006-wpe-optional-process-swap-on-navigation` | lets the browser keep a view in its web process (`WPE_BROWSER_NO_PROCESS_SWAP`): the webOS compositor can't move a view's layer between processes |
| `0007-gstreamer-ring-buffer-max-size-is-guint64` | an `int` passed for a `guint64` property through varargs crashed the web process on 32-bit ARM when setting up MSE video |
| `0008-jsc-32bit-gcc-no-speculative-indexing-header-loads` | GCC hoisted an `IndexingHeader` load above its type check; past a 1 MB heap chunk's end it faulted (SIGSEGV in `allocateMoreOutOfLineStorage`, [webkit.org/b/295780](https://bugs.webkit.org/show_bug.cgi?id=295780)) |
| `0009-shm-readback-without-bgra-read-support` | Mali-400 accepts BGRA textures but cannot read BGRA pixels: use RGBA readback and convert the SHM frame to BGRA when the GLES read-format extension is absent |

Dropped for 2.52: `0003-allow-grayscale-font-antialiasing` (2.52 added `WPE_SETTINGS_SUBPIXEL_LAYOUT_NONE`, which the
engine now sets).

## GnuTLS (`buildroot-external/patches/gnutls/`)

| Patch | Why |
|---|---|
| `0001-use-the-app-CA-bundle-at-its-runtime-path` | read the app's packaged CA bundle (`WPE_BROWSER_CA_FILE`, via `secure_getenv`); verification is unchanged |

## webOS native app kit (`third_party/patches/webos-native-app-kit/`)

Applied on top of the submodule by `third_party/patches/apply-kit-patches.sh`; candidates for upstreaming.

| Patch | Why |
|---|---|
| `0001-Delegate-on_pointer-hook` | the app sees every pointer event first (the page under the Magic Remote pointer) |
| `0002-GLIBC_RUNTIMES-private-runtime-tree` | package a private runtime (the engine) that uses the TV's glibc, with its own package check |
| `0003-TextField-set_chromeless` | a text field without its own box, for the address-bar pill |
| `0004-Dialog-drop-down-menu-support` | dialog width, padding and top-right anchoring, for the browser menu |
| `0005-check-package-system-lib` | name further TV system libraries the private runtime may link |
