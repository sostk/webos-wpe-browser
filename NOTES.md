# WebKit browser for LG webOS (native app, WPE WebKit + webos-native-app-kit)

## Target
- Development TV: LG OLED65C1PTB, webOS 6.5.3 (firmware 03.53.45), kernel 4.4.84 aarch64, **32-bit ARM userland
  (softfp)**, glibc 2.28, Mali-G51, 2.6 GB RAM (about 490–660 MB available), rooted (Homebrew Channel).
- Reach it with `ares-shell -d tv -r '<cmd>'`, `ares-push` and `ares-pull` (profile `tv` in ~/.webos/ose/novacom-devices.json holds the key and passphrase).
- Support floor: **webOS 6+** (glibc 2.28). User approved this on 2026-11-09.

## Safety rules (from the user, still in force)
- No firmware changes, no eMMC/bootloader writes, no kexec, no LG Developer Mode app (it breaks root).
- Large files go to USB (`/tmp/usb/sda`) or /tmp.
- **Ask before** installing an ipk, changing any service, or writing outside /tmp or USB on the TV.

## Layout
| Path | What |
|---|---|
| `browser/` | the app (kit-based, C++), to be written |
| `third_party/webos-native-app-kit/` | kit **submodule** (github.com/Native-Webos/webos-native-app-kit @312f365, 2026-10-30). Local changes live as patches in `third_party/patches/webos-native-app-kit/`; after `git submodule update --init`, run `third_party/patches/apply-kit-patches.sh`. |
| `toolchain/ct-ng.defconfig` | crosstool-ng config for the cross toolchain |
| `buildroot-external/` | BR2_EXTERNAL tree: `configs/webos_browser_defconfig`, overrides in `external.mk` |
| `docker/` | `Dockerfile` (ubuntu 24.04 + build tools + crosstool-ng 1.27.0); `run.sh '<cmd>'` runs in it as the host user |
| `~/webos-browser-build/` (outside the project) | toolchain (`x-tools/`), Buildroot source and output, downloads, ccache |

## Decisions
- 2026-11-09 **Engine: WPE WebKit**, starting with Buildroot's 2.50.5. Bump to 2.52.x/2.54.x once the pipeline works.
  The integration API is **WPEPlatform** (headless platform; the app receives SHM buffers and uploads them in a kit `GlView`).
  CPU (Skia) rendering first; GPU rendering later, once the TV's Mali EGL capabilities are known.
- 2026-11-09 **The NDK can't be used for WebKit**: its sysroot is glibc 2.12 (it targets old webOS). Instead:
  crosstool-ng with GCC 14.2, **glibc 2.28**, Linux 4.4.302 headers, tuple `arm-webos-linux-gnueabi`.
  Its target flags match the NDK (`-mcpu=cortex-a9 -mfpu=neon-fp16 -mfloat-abi=softfp -marm`, aapcs-linux).
  glibc's minimum kernel is set to 3.2.0 because the TV kernel 4.4.84 is older than 4.4.302. We don't ship glibc anyway: the TV's is used.
- 2026-11-09 Everything except libEGL/libGLESv2 (and other firmware libraries the kit dlopens) is **bundled in the ipk**. The TV's glib (2.58), ICU 62,
  libsoup 2.4 and harfbuzz are too old or variable.
- 2026-11-09 Builds run in Docker (`docker/run.sh`) because the host lacks bison, flex, meson and similar tools, and `sudo` from the prompt doesn't work.
- 2026-11-09 Mesa (softpipe, EGL + GLES) is in Buildroot only to provide EGL/GLES headers and link targets. It isn't shipped.
- Version 1 scope: address bar (kit TextField + TV keyboard), back/forward/reload/home, D-pad via WebKit spatial navigation,
  Magic Remote pointer as a mouse (needs a pointer hook added to the kit's Delegate), 720p render scaled to the screen.
  Not in version 1: video/audio, tabs, bookmarks.

## Log
- 2026-11-09 TV checks (read-only): MemAvailable 658 MB, appstore partition 1.4 GB free, USB at /tmp/usb/sda,
  TV libs: glib 2.58, ICU 62, libsoup-2.4, gnutls 28, GStreamer 1.16, harfbuzz, cairo, webp 7; no WPE/libsoup3/epoxy.
- 2026-11-09 Docker image `webos-browser-build:1` built. ct-ng config resolved: binutils 2.43.1, GCC 14.2.0, glibc 2.28, Linux 4.4.302.
- 2026-11-09 Buildroot master @cdd3776b (2026-11-08) cloned to ~/webos-browser-build/buildroot: wpewebkit 2.50.5, libwpe 1.16.3,
  wpebackend-fdo 1.16.1, libsoup3 3.7.3, ICU 78.2, cog 0.18.5.
- 2026-11-09 Buildroot has no neon-fp16 FPU choice, so it uses `BR2_ARM_FPU_NEON`, which gives `-mcpu=cortex-a9 -mfpu=neon -mfloat-abi=softfp` (same calling convention).
- 2026-11-09 **Kit change (local; offer upstream):** `Delegate::on_pointer(Context&, const input::PointerEvent&)` gets every pointer event
  before `Ui::handle_pointer`, and returning true consumes it. Files: app/include/wnk/app/app.hpp, app/src/app.cpp. See `git -C third_party/webos-native-app-kit diff`.
- Plan for text in web pages (v1): when a page input is focused, open a kit TextField dialog, then put the text back with JS
  (set the value and dispatch an `input` event). Native IME pass-through to WebKit can come later.
- 2026-11-09 **Finding (WPE 2.50.5 source, AcceleratedSurface.cpp SwapChain):** even "software" rendering composites with GL in WPEWebProcess.
  WebKit uses SHM buffers only when PlatformDisplay is **Surfaceless** (EGL_MESA_platform_surfaceless) and no dma-buf export is available.
  The TV's Mali EGL can't provide that, so we **bundle Mesa llvmpipe (LLVM 23) for the web process**. The UI process keeps Mali (SDL/kit) for the screen.
  Open item: make WPEWebProcess load the bundled Mesa libEGL while the UI process loads Mali's (same SONAME libEGL.so.1). Options: a RUNPATH on
  libepoxy pointing to a mesa/ subdir (the UI process already has Mali loaded via SDL), or a wrapper/env for the web process only.
- 2026-11-09 WPEPlatform is opt-in in 2.50 (`ENABLE_WPE_PLATFORM` defaults to developer mode); enabled in external.mk with DRM/Wayland off.
  App design: subclass WPEDisplay (connect, create_view, get_keymap; get_egl_display returns an error, so SHM is used) and WPEView (render_buffer,
  where the WPEBufferSHM data is uploaded with glTexSubImage2D in a kit GlView, then wpe_view_buffer_rendered/released).
- 2026-11-09 **Architecture decision: two processes.**
  1. `browser/` = UI app built with the **stock webOS NDK** and the kit, unchanged. It has to be: SDL-webOS must compile against the NDK's
     wayland 1.11 headers and libhelpers/pbnjson. Newer wayland headers generate calls such as wl_proxy_marshal_flags that the TV's libwayland lacks.
  2. `engine/` = **wpe-engine** host built with the glibc 2.28 toolchain and linked to WPE WebKit. It implements a custom WPEPlatform
     display/view/toplevel/IME and has **no EGL** (so WebKit uses SHM). It spawns WPEWebProcess/WPENetworkProcess.
  - IPC: the app spawns the engine with a socketpair (SOCK_SEQPACKET) on fd 3. Frames go in a memfd ring (sent once by SCM_RIGHTS) with
    per-frame messages and acks for pacing. Commands (load/back/forward/reload/resize/input/IME text) and events (uri/title/progress/
    can-go/IME focus/crash) are fixed-size binary messages (`common/wpe_ipc.h`).
  - Benefits: kit rules unchanged; Mesa is loaded only in the engine's children (LD_LIBRARY_PATH set at spawn), so there's no clash with Mali;
    a WebKit crash doesn't take the UI down. Cost: one memcpy per frame (about 3.7 MB at 720p).
  - The engine bundles GCC 14's libstdc++.so.6 (WebKit needs GLIBCXX_3.4.33). It's isolated to the engine's processes; the UI app keeps static libstdc++.
  - That makes the WEBKIT_EXEC_PATH patch essential (helpers live in the app package).
- 2026-11-09 Toolchain built (crosstool-ng exit 0, about 17 min) at ~/webos-browser-build/x-tools/arm-webos-linux-gnueabi.
  **Verified on the TV:** a test binary ran from /tmp (memfd_create works on kernel 4.4.84, gnu_get_libc_version()=2.28, highest symbol GLIBC_2.27, softfp).
- 2026-11-09 gnutls trust store fixed to /tmp/wpe-browser/ca-certificates.crt (external.mk). The UI copies the bundled CA list there before spawning the engine.
- 2026-11-09 Code written:
  - `common/wpe_ipc.h`: fixed-size binary protocol, memfd frame ring with acks.
  - `engine/src/platform.{h,cpp}`: WebOSDisplay/View/Toplevel/Ime (WPEPlatform subclasses); `engine/src/main.cpp`: IPC loop, WebKitWebView
    (spatial navigation, persistent cookies, popups in the same view), input → WPEEvents; `engine/CMakeLists.txt`.
  - `browser/`: kit app with toolbar + address bar, PageView (GLES2 texture, damaged-row uploads, BGRA swizzle), EngineClient
    (posix_spawn with env, SCM_RIGHTS memfd), remote/pointer/TV-keyboard routing, engine auto-restart (3x).
  - `tools/stage-engine.sh`: builds the engine against Buildroot and stages engine/ (helpers, dependency closure minus the TV's glibc,
    Mesa EGL/GLES, giognutls, fonts + fonts.conf, CA bundle), then fails if anything needs a glibc newer than 2.28.
  - Host build of the UI + smoke test pass; screenshot shows the toolbar and "engine not found". NDK ipk builds (1.4 MB), check-package passes.
- 2026-11-09 **Kit change 2 (local; offer upstream):** `GLIBC_RUNTIMES` in wnk_add_webos_package / `--glibc-runtime` in check-package.py:
  a separate-process tree that may use the TV's glibc (FIRMWARE_GLIBC) and bundle Mesa libEGL/libGLESv2; other LG libraries stay denied.
  Tests added in tests/tools/test_packaging.py (PrivateRuntimeRuleTests); the kit's packaging suite passes (25 + 2 new tests).
- 2026-11-09 Toolchain test: `arm-webos-linux-gnueabi-gcc` C code needs -D_GNU_SOURCE for memfd_create (C++ defines it).
- 2026-11-09 **UI redesign (user request): Chromium-style dark chrome with tabs.**
  - `browser/chrome.{hpp,cpp}`: TabStrip/TabView (monogram, ellipsized title, close ×, active tab joined to the toolbar,
    separators), round IconButtons (Material icons, `browser/icons/*.svg`, Apache-2.0, with `-dim` variants), pill Omnibox
    (lock/info/search icon, scheme hidden for https), ProgressLine, native NewTabPage (search pill + 8 shortcut tiles).
    The palette is Chromium dark (#202124 frame, #35363A toolbar, #8AB4F8 accent/focus ring).
  - Tabs: the app owns the tab model. **Protocol v2**: the header carries a tab id; TAB_OPEN/CLOSE/ACTIVATE. The engine keeps one
    WebKitWebView per live tab, all **related views sharing one web process**. Only the active one is visible/renders, and input goes to the active tab.
    Our display must be passed to related views too (wpe_display_get_default() would create a different platform's display).
  - **Tab discarding** (Chromium-like): at most 4 live tabs. The LRU background tab is closed in the engine and keeps URL/title, reloading when shown.
    12 tabs max. Home = the tab's New Tab page (no engine view).
  - Keys: Red omnibox, Green reload/stop, Yellow next tab, Blue new tab; Back in a page = history or omnibox; Back in the chrome = back to content.
  - **Kit change 3 (local; offer upstream):** `TextField::set_chromeless(bool)` skips the field's own fill/border so a container
    (the omnibox pill) can draw it. The kit test suite passes (109/109).
  - Verified: host build + smoke test, screenshots of the NTP and 3 tabs; NDK ipk 1.45 MB, check-package passes. The engine v2 is not compiled yet (waiting for WebKit).
- 2026-11-10 **Installed on the TV at the user's request** (`ares-install -d tv`, app id org.sostk.wpebrowser, UI only; no engine yet).
  It launches, the window is shown and focused, and it renders correctly at 1920x1080 (GPU max texture 8192). A TV screenshot was taken by
  running the binary directly with APPID + APP_WPEBROWSER_SHOT (busybox `timeout -t N` syntax).
  luna-send in ares-shell exits 0 but prints nothing (even redirected), so the capture service isn't usable from it.
  Fixed: startup focus went to the toolbar omnibox; it's now settled on the first frame (NTP search box). Reinstalled and relaunched.
  Remove with: `ares-install -d tv -r org.sostk.wpebrowser`.
- 2026-11-10 **Bug fixed:** wnk::platform::storage_candidates() returns shared *base* folders (/media/developer on the TV,
  $XDG_DATA_HOME on the host). The app had used them directly and created /media/developer/webkit/cache on the TV (empty; removed with rmdir)
  and ~/.local/share/{webkit,profile} on the PC (removed). Data now goes in `<base>/org.sostk.wpebrowser.data/` (profile/, webkit/).
- 2026-11-10 **Settings, clear browsing data, history, bookmarks, bookmarks bar (user request), version 0.2.0.**
  - `browser/profile.{hpp,cpp}`: settings / bookmarks / history (max 5000, newest first, consecutive visits merged) / session as JSON
    in `<data>/profile/`, atomic writes on the kit worker pool (at most every 2 s), synchronous flush on suspend and stop.
  - `browser/screens.{hpp,cpp}`: chrome://-style pages on the Navigator: Settings (bookmarks bar, page zoom 100-200 %,
    search engine DuckDuckGo/Google/Bing/Startpage/Ecosia, on startup NTP / continue where you left off, privacy links, about),
    Clear browsing data (time range, history / cookies+site data / cache), History (grouped by day, per-entry remove), Bookmarks
    (remove). The ⋮ menu is a Chromium-style drop-down: New tab, History, Bookmarks, Clear browsing data, Settings, Close tab, Exit.
  - Chrome: a star in the omnibox (blue when bookmarked) and a BookmarksBar of chips + "All bookmarks". Session restore brings tabs back discarded.
  - **Protocol v3**: APP_CLEAR_DATA {what, seconds} → webkit_website_data_manager_clear (cookies, session/local storage, IndexedDB,
    service workers, DOM cache, HSTS, app cache / memory+disk cache) → ENGINE_DATA_CLEARED. APP_SET_ZOOM applies to all tabs and new ones.
  - Back on the browser screen: `BrowserScreen::on_back` (pushed pages pop normally).
  - **Kit change 4 (local; offer upstream):** Dialog: empty title adds no title line; `set_panel_width`, `set_panel_padding`,
    `anchor_top_right(Point)` (drop-down menus). Kit tests 109/109.
  - Verified: host screenshots (menu, settings, history, bookmarks, clear data); a scripted "clear all time" removed 6/6 history
    entries and saved 0 to disk; NDK ipk 0.2.0 (1.55 MB) passes check-package; installed and launched on the TV, profile at
    /media/developer/org.sostk.wpebrowser.data/profile.
- 2026-11-10 Git repository initialised (branch `main`, one commit per component); remote `origin` = https://github.com/sostk/webos-wpe-browser (private). The kit's working copy keeps the 4 local changes uncommitted; they are tracked as patches (regenerate with `git -C third_party/webos-native-app-kit diff -- <files>` when a kit change is edited).
- 2026-11-10 Buildroot built every dependency (LLVM 23, Mesa 26.2.5 with libEGL + libgallium, libsoup3, ICU 78, ...). WPE WebKit failed at
  its first file: "DFG and FTL JIT require baseline JIT to be enabled". JSC's 32-bit ARM JIT needs the **hard-float** ABI
  (__ARM_PCS_VFP) and webOS is softfp. **Decision:** CLoop interpreter (JIT/DFG/FTL/WebAssembly/sampling profiler off) via external.mk,
  as Buildroot does for ARMv5/v6/MIPS. Cost: slower JavaScript and no WebAssembly.
  **Possible later speed-up:** build the engine tree hard-float with its own glibc + loader (a true private runtime). WebKit's helpers
  can't be exec'd with a system PT_INTERP, but PT_INTERP can point at a fixed path such as /tmp/wpe-browser/ld-linux-armhf.so.3 that the UI
  sets up at start (like the CA bundle). That would enable the ARMv7 JIT. It means rebuilding the toolchain and all packages.
  **2026-11-10:** the first WebKit compile was stopped at step 3491/7656: 6 parallel jobs exhausted the PC's 15 GB (cc1plus killed). Resumed with `make BR2_JLEVEL=4` (`~/webos-browser-build/br-build3.log`).
- 2026-11-10 **Compile error at WebCore/platform/graphics/gbm/MemoryMappedGPUBuffer.cpp: `linux/dma-buf.h` missing.** That uapi header
  appeared in Linux 4.6; the toolchain has 4.4 headers. Fix: `toolchain/extra-headers/linux/dma-buf.h` (copy of the uapi header) copied into
  Buildroot's sysroot `br-out/host/arm-buildroot-linux-gnueabi/sysroot/usr/include/linux/`. crosstool-ng leaves `x-tools/.../sysroot` read-only,
  so after a toolchain rebuild or a fresh `br-out`, copy it again (`chmod u+w` the directory first for x-tools). The code that uses it only runs for
  GBM/dma-buf buffers, which the engine never creates. Alternative for a clean rebuild: `-DUSE_GBM=OFF -DUSE_LIBDRM=OFF` (needs reconfigure = full rebuild).
  Resumed with `br-build4.log`.
- 2026-11-10 **WPE WebKit 2.50.5 built** (br-build4.log, exit 0). Engine compiled (one fix: a lambda inside G_CALLBACK broke the macro) and
  staged by tools/stage-engine.sh: 200 MB, 57 libraries (largest: libWPEWebKit 58 MB, libLLVM 58 MB, libicudata 32 MB, libgallium 17 MB).
- 2026-11-10 **TV smoke test from USB** (no install; engine at /tmp/usb/sda/sda1/engine, run with the installed 0.2.0 UI and
  `WPE_BROWSER_ENGINE_DIR`): wpe-engine and WPEWebProcess load with the bundled libraries; the engine reports
  "ready: WPE WebKit 2.50.5, page 1280x610" to the UI in about 1.1 s. The USB stick is tfat (no symlinks; execution works).
  Not yet tested: loading a page. The script step `text@...` did not type into the NTP search box on the TV (works on the host);
  use D-pad steps instead. The kit script's final `wait@` does not hold: add a trailing `wait@10` step.
  **User asked to pause TV testing (2026-11-10).**
- 2026-11-10 **Full ipk with the engine builds and passes check-package**: `browser/build/webos-release/dist/org.sostk.wpebrowser_0.2.0_arm.ipk`,
  94.5 MB (200 MB unpacked). One fix: Mesa's libEGL linked libwayland-client/-server (Buildroot adds the wayland EGL platform because
  wpewebkit selects wayland); external.mk now passes `-Dplatforms= -Dlegacy-wayland=` to Mesa (surfaceless + drm only). 55 libraries in engine/lib.
  Not installed on the TV (TV testing paused at the user's request).

## 2026-11-10 installation review
- User requested review of Claude's completed work and installation of the bundled-runtime IPK, resuming TV testing.
- Installed `browser/build/webos-release/dist/org.sostk.wpebrowser_0.2.0_arm.ipk` successfully with `ares-install -d tv`, then brought it to the foreground with `ares-launch`.
- Inspected archive: 212,144,123 bytes unpacked, 93 engine files; includes wpe-engine, both WebKit helpers, Mesa under engine/lib/mesa, and the UI GPU selector. Software remains the default.
- Package check passed with `--expected-needed browser/expected-needed.txt --glibc-runtime engine`. Firmware loader inventory check passed 6/6 available images for webOS 6+; this checks UI loader compatibility, not page behavior.
- TV evidence (configured LG OLED65C1PTB/webOS 6.5.3 target): installed engine executable and helper processes running; `/tmp/engine.log` reports `ready: WPE WebKit 2.50.5, page 1280x610`. Engine environment confirms `WPE_BROWSER_RENDERER=software` and bundled lib + lib/mesa paths.
- Actual launch runs as uid 6391. Root-owned `/tmp/org.sostk.wpebrowser` from earlier direct tests is not usable by that uid; the current engine log falls back to `/tmp/engine.log`. The older log in `/tmp/org.sostk.wpebrowser` is stale.
- Remaining issues observed, not fixed in this installation task: missing `/usr/lib/wpe-webkit-2.0/injected-bundle/libWPEInjectedBundle.so`; TLS property warnings; libsoup reports `attempt to write a readonly database`.
- Confirmed TLS mixing through WPENetworkProcess maps: bundled libgio/libglib, but firmware `/usr/lib/gio/modules/libgiognutls.so` and `/usr/lib/libgnutls.so.28.43.4` inside the app jail. The bundled GIO module setting alone is not isolating the TLS backend.
- Browser left running for user testing. Page rendering, successful HTTPS navigation, and GPU mode have not been verified by this review. No source fixes or firmware/service changes made.


## 2026-11-10 GPU and browser-media continuation (local only)
- Current user instruction: **do not connect to, deploy to, launch on, or test on the TV** while their partner is watching. This supersedes earlier testing permissions. No device commands have been run in this continuation.
- Found Claude's incremental GPU WebKit build already completed (`br-wk-rebuild.log`, exit 0). Reverse dry-run confirms exported patch 0002 matches the build source. It tries default EGL + offscreen/pbuffer when Mesa surfaceless is absent. Hardware access and timings remain unmeasured.
- Kept software default and Mesa fallback. Built and staged the patched engine, cross-compiled updated probe to `build/egl-probe`, and built IPK locally; check-package passed. This package still has the old **video-disabled** WebKit until the multimedia rebuild is staged.
- Probe now requests WebKit's RGBA8/stencil8 configuration, supports forced pbuffer, checks API binding and GL errors, and labels lower-precision RGBA4 timings. Host Mesa surfaceless and forced-pbuffer pixel checks pass; host results are not TV evidence.
- Full-browser video requirement includes MSE. Chose the existing WebKit GStreamer URL/MSE backends, not the URL-only external-holepunch stub. Defconfig enables multimedia, libav software decoders, MP4/WebM parsers, VP9/Opus/Vorbis, HTTP, adaptive HLS/DASH and PulseAudio client. External holepunch stays off. GL video sink stays off for the initial software-decoding baseline; this does not disable Skia GPU painting.
- Runtime sets bundled GStreamer plugin/scanner paths and a writable per-profile registry, overriding inherited/firmware paths. PulseAudio client configuration disables daemon autospawn; it must use the existing server. Audio routing/jail permission still need later device validation.
- Staging includes plugin entry points (dlopened, absent from DT_NEEDED), scanner, diagnostic tools, and their dependency closure. It excludes the unused Wayland sink so it cannot drag a second Wayland ABI into the engine.
- Added real child-process environment integration test for software and GPU launch; both it and the browser UI smoke test pass locally. Added self-generated media fixtures and manual playback/MSE page at `tools/media-tests/`.
- Multimedia rebuild started in local Docker using 3 jobs to reduce memory pressure. Log: `~/webos-browser-build/br-media.log`. Do not label it built until this log and subsequent staging/package checks pass.
- Still required for the requested default-browser experience: finish multimedia build/package; verify URL and MSE playback, sound, seek, fullscreen and live/adaptive behavior; integrate LG hardware decoding for useful resolution/power performance (GStreamer encoded sinks + hole-punch quirks can retain upstream MSE; study Rialto quirk rather than replacing MSE with a URL player). No LG hardware media bridge exists in this change. DRM/CDM integration and site compatibility are separate and not promised.


## 2026-11-10 TV GPU measurements and updated instruction
- User temporarily resumed TV tests, then explicitly said **Stop tv testing**. TV tests are stopped again; do not connect/deploy/run/inspect on the TV without a later resumption. No installation or service/firmware changes were made in this continuation. Probe binaries and generated test profiles/logs are under `/tmp` only.
- OLED65C1PTB, webOS 6.5.3, kernel 4.4.84: firmware libEGL/GLES resolve to Mali-G51 r9p0, EGL 1.4, GLES 3.2. Default-display surfaceless and forced-pbuffer FBO pixel checks passed. Neither dma-buf import nor Mesa dma-buf export is advertised. Measured clear + 1280x720 RGBA8 readback: surfaceless avg 16.4 ms/best 15.1 ms; pbuffer avg 15.8 ms/best 14.9 ms (30 runs each). This is not a full browser frame-time benchmark.
- Headless WebKit probe (root, no UI window) confirmed Mali mappings and a working WebGL canvas. Default Skia GPU painting corrupted ordinary page tiles (background BGRA 0,255,0,0 instead of 160,80,32,255); disabling GPU worker threads did not fix it. Mali compositing + CPU Skia painting passed both background and WebGL pixel checks. The software control also passed. GPU mode in EngineClient now retains CPU tile painting; the overall default remains software pending broader validation.
- Test logs: `/tmp/wpe-engine-probe-NKmuOi` (GPU failure), `fEvji1` (Mali composite/CPU paint pass), `QYk5MO` (software pass), `oNHQJW` (GPU with no painting workers, failure). Probe captures a frame.ppm in newer runs. Installed WebKit matched local GPU-patched library checksum. The attempted installed-jail/app-UID comparison did not start: the earlier browser process/jail root no longer existed. Do not label the root test as a jailed-app verification.
- Confirmed requirement: Netflix, Disney+ and other DRM services are essential. TV has `libwidevine_ce_cdm_shared.so.16.3.0.0` and `libplayreadycdm.so`; library presence does not establish a usable browser CDM. Existing kit research has partial platform DRM results but no working Widevine path for this browser. See `third_party/webos-native-app-kit/docs/research/DRM-FEASIBILITY.md`.
- Local EME interfaces are enabled for the ongoing DRM work (Thunder off). No proprietary CDM factory/secure media bridge has been implemented; Netflix/Disney+ are not supported or verified by this change. Google documents Widevine's licence-agreement requirement: https://developers.google.com/widevine/drm/overview . The experimental webos-drm-bridge currently targets webOS 25, not this webOS 6 TV: https://github.com/lennylxx/webos-drm-bridge .
- Local ARM QEMU checks passed: MP4 H.264/AAC and WebM VP9/Opus decode + seek; loopback HTTP indexed MP4, WebM, HLS and DASH decode + seek. Both streams must produce decoded buffers. Fragmented MP4 remained the MSE fixture; indexed MP4 is the HTTP seek control. These checks do not test browser MSE/EME APIs, sound output, GPU or service compatibility. Browser host smoke + real child-environment tests pass 2/2.
- Long multimedia compilation was interrupted during user/environment steering, not completed. `tools/build-media.sh start` resumes it in a detached local container, then stages the engine, checks staged ARM media playback through QEMU, builds an IPK in `build/media-dist/` and runs the package checker. Status: `tools/build-media.sh status`, `~/webos-browser-build/br-media-current.status` and `.log`. This script has no TV operations. Until status is `exit=0` and the final PASS is present, do not claim a completed multimedia IPK.


## 2026-11-10 multimedia installation
- Multimedia pipeline completed successfully (exit 0), including ARM QEMU media checks and package validation. User requested installation; `build/media-dist/org.sostk.wpebrowser_0.2.0_arm.ipk` was installed successfully.

## 2026-11-10 TLS certificate repair
- Root diagnostics left `/tmp/wpe-browser` owned by root with mode 0700. The installed browser (uid 6391) could not traverse it to read the public CA file. Clock was correct; the network process used bundled GIO/GnuTLS and reported unknown-CA errors for public sites.
- With explicit user approval, changed the directory to 0755 and restarted WPE Browser. Website success awaits user confirmation.
- Permanent fix reads the packaged CA bundle directly through WPE_BROWSER_CA_FILE, using secure_getenv in private GnuTLS and retaining its configured fallback for older launchers. Missing/unreadable bundles fail startup with an actionable error. Clear inherited GIO_EXTRA_MODULES so firmware TLS modules cannot override the bundled backend. Certificate and hostname checks remain enabled.
- Local validation: GnuTLS rebuild/staging, host tests (2/2), and rebuilt IPK/package checks passed. ARM QEMU loopback checks in tools/test-tls.py accepted a trusted issuer and rejected an untrusted issuer and wrong hostname. The new IPK is prepared locally and has not been installed.


## 2026-11-10 GPU and media source publication
- Committing GPU default-EGL support, opt-in Mali compositing with CPU Skia painting, private GStreamer media packaging, local probes/fixtures and process-environment regression coverage. Software remains the default. No working DRM/CDM integration is included.
- Validation: browser host tests passed 2/2; completed multimedia pipeline reports exit=0 with staged ARM decoder/URL checks and IPK checks passing. The later TLS-fixed IPK also passed packaging checks. Existing kit working changes match all four tracked patch files; the upstream submodule reference is unchanged. No TV operations were performed for this commit.


## 2026-11-10 profile ownership, text rendering (local only; no TV operations)
- **Injected bundle warning:** already fixed by 1d08f1e (`WEBKIT_INJECTED_BUNDLE_PATH`, bundle staged in engine/lib/injected-bundle). The TV log came from the earlier ipk.
- **libsoup "attempt to write a readonly database":** WebKit's SQLite cookie storage is libsoup's SoupCookieJarDB. Root test runs left root-owned
  folders/files under `/media/developer/org.sostk.wpebrowser.data/`; the app (uid 6391) accepted the folder because only the base folder was probed
  and `make_dir` treats EEXIST as success. Fix: `browser/storage.{hpp,cpp}`: `usable_tree()` walks the profile (dirs rwx, files rw for this uid);
  `own_usable_dir()` falls back to `<folder>-<uid>` and leaves the other user's files alone. Host test `browser.storage` added (3/3 host tests pass).
- **User report from the TV run:** pages had "weird colour" and soft text; YouTube stayed on its loading skeleton.
  - Pixel path re-checked: WebKit reads back with `glReadPixels(GL_BGRA)`, the UI uploads as RGBA and swaps `.bgr` in the shader: consistent.
  - Cause found for coloured/soft text: WPEPlatform always reports a subpixel layout (default RGB), so `SystemSettings::antialiasMode()` is always
    `Subpixel` (LCD AA). On an OLED WRGB panel, after the 2/3 render scale is stretched ×1.5 with GL_LINEAR, that shows as colour fringes.
    WebKit patch **0003** maps an unknown layout byte to "none" (grayscale AA, kUnknown_SkPixelGeometry); the engine sets 0xFF.
  - Remaining softness is the 2/3 render scale (1280-wide page, linear ×1.5 upscale). Native 1920 rendering would be sharp but costs 2.25× pixels
    in CPU raster and the per-frame memcpy (~6.7 MB). Not changed; needs a decision.
  - YouTube: not diagnosed (needs TV logs). Most likely cause is JavaScript speed: CLoop interpreter (no JIT on softfp) running the desktop
    Polymer app. Candidates: engine log + remote inspector on the next TV run; a lighter YouTube front end via a per-site UA; the hard-float private
    runtime that would allow the ARMv7 JIT (see the 2026-11-10 CLoop entry).
- Built: WebKit rebuilt with patch 0003 (`br-font-fix.log`, exit 0), engine restaged (241 MB, 87 libraries, glibc check passes), NDK ipk
  `browser/build/webos-release/dist/org.sostk.wpebrowser_0.2.0_arm.ipk` (115 MB; includes the media engine, the TLS fix and the profile fix)
  passes check-package. Not installed; grayscale text not yet seen on the TV.
- Installed on the TV at the user's request (ares-install -d tv, 2026-11-10). Not launched by Claude.
- 2026-11-10 21:41 TV log check (read-only, at the user's request; the user launched the app at 21:38):
  - Engine ready in the normal way; no "readonly database" message this run. /media/developer/org.sostk.wpebrowser.data is 0777 throughout, with
    some root-owned entries (cookies.sqlite, storage/, mediakeys/), so `usable_tree()` accepts it and the profile stays in place.
  - One WPEWebProcess ran at 99 % CPU for ~3 min (load average 17) on a Google Images search; it had settled by 21:41:30. RAM: 536 MB available, 338 MB swap in use.
  - UI: steady 60 fps (median 16.7 ms), 47.8 MB RSS; worst draw 72.9 ms while the page was busy.
  - **Audio is broken:** libpulse refuses `XDG_RUNTIME_DIR=/tmp/xdg` (root-owned) and `/tmp/xdg/pulse` is empty, so the TV's PulseAudio socket is
    elsewhere. Needs: find the system socket, then set `PULSE_SERVER` (and a private XDG_RUNTIME_DIR) for the engine.
  - `WPEWebProcess: WebProcess didn't exit as expected after the UI process connection was closed` (g_error → abort) for an old web process after
    tab close/reopen. Benign for the user, but leaves an abort in the log.
  - Harmless: no D-Bus machine-id; platform resource 'missingImage'.

## 2026-11-10 "Full working browser": hard-float engine with JavaScript JIT (user request: "do what is required")
- **Decision:** build the engine tree hard-float + Thumb-2 with its **own glibc 2.28 and loader**, so JavaScriptCore gets the
  baseline + DFG JIT and WebAssembly (PlatformEnable.h: `CPU(ARM_THUMB2) && CPU(ARM_HARDFP) && OS(LINUX)`). FTL stays off (32-bit).
  The UI app stays softfp (NDK + kit). The softfp tree (`br-out`, `build/engine-stage`) is kept as a fallback.
  - Cost: **GPU (Mali) mode is impossible in this engine**: the firmware EGL/GLES are softfp and can't load into a hard-float
    process. EngineClient forces software (llvmpipe) when the engine has a private loader. Any future LG hardware-decoder
    bridge must be a separate softfp process.
- Pieces:
  - `toolchain/ct-ng-hf.defconfig` (tuple `arm-webos-linux-gnueabihf`), `buildroot-external/configs/webos_browser_hf_defconfig`
    (EABIHF, Thumb-2, gconv copied), external.mk keeps the CLoop/JIT-off block for softfp only.
  - `tools/build-hf.sh start|status`: detached container; toolchain (if missing) → dma-buf.h into the x-tools sysroot →
    Buildroot `O=/build/br-out-hf` (BR2_JLEVEL=4) → stage to `build/engine-stage-hf/engine`. Log `~/webos-browser-build/br-hf.log`, status `br-hf.status`.
  - `tools/stage-engine.sh` hard-float mode (auto from `BR2_ARM_EABIHF`): bundles libc & co., `ld-linux-armhf.so.3`, libnss_files/dns
    (getaddrinfo dlopens them via the TV's nsswitch.conf; the TV's are softfp), `lib/gconv`; sets PT_INTERP of every executable to
    `/tmp/org.sostk.wpebrowser/ld-linux-armhf.so.3`; fails on any soft-float ELF or a wrong loader.
  - `browser/private_loader.{hpp,cpp}`: the UI reads wpe-engine's PT_INTERP; if it lies in the app's runtime folder (kit:
    `/tmp/<app id>`, owned by the app, symlinks rejected), it links it to `engine/lib/<loader>` (atomic rename). Env adds
    `GCONV_PATH`, clears `LD_PRELOAD`. Host test `browser.private_loader`.
  - check-package: `--glibc-runtime engine` accepts the tree as is (every DT_NEEDED resolves inside it).
- **Sharp text setting** (default on): protocol v4 `WPE_APP_SET_SCALE`; the engine calls `wpe_toplevel_scale_changed` so pages keep
  their CSS size (1280 wide) but render at 1.5x = native 1920. Damage rects are in buffer pixels (as in WPE's Wayland platform), so
  partial uploads stay correct. Costs 2.25x raster + copy; Settings → Appearance → "Sharp text" turns it off. Not yet measured on the TV.
- Engine log now records `load started/committed/finished after N ms: <uri>` per tab.
- Audio: TV unreachable (timeout) when trying to locate the PulseAudio socket; still open.
- `tools/js-probe.{c,sh}`: host-native IPC driver; the ARM engine runs through binfmt QEMU (softfp needs `QEMU_LD_PREFIX=<br-out sysroot>`;
  hard-float uses the same /tmp/org.sostk.wpebrowser loader link as the TV). Fixed JS workload (fib(27) + 200k sort), WebAssembly
  check, optional device scale. **Softfp CLoop baseline under QEMU: 1651 ms, no WebAssembly**; scale 1500 gives 960-px frames for a 640-CSS-px page (pass).
  QEMU timings are only for comparing builds, not TV speed.
- Hard-float build fixes: `BR2_ARM_EABIHF` depends on `BR2_ARM_CPU_HAS_FPU`, which on Cortex-A9 needs `BR2_ARM_ENABLE_VFP=y`; without it
  Kconfig silently fell back to EABI ("Incorrect ABI setting"). After an ABI change the output folder must be fresh (`rm -rf br-out-hf`).
  Toolchain `x-tools/arm-webos-linux-gnueabihf` built OK. Buildroot restarted 2026-11-10 ~22:20.

## 2026-11-10 Research: Chromium's JIT and GPU path → plan for WebKit (user request)
- Full write-up: `docs/webkit-like-chromium.md`. Sources: webOS-ports/chromium151 (V8 `macro-assembler-arm.cc`,
  ozone `wayland_surface_factory.cc`, `gl_surface_wayland.cc`, `extensions/webos/host/`), WPE WebKit 2.50.5, webOS NDK protocol XML.
- **JIT:** V8 converts doubles only at C-call boundaries on softfp. JSC assumes hardfp there; it once supported softfp (32-bit iOS
  leftovers). The `pcs("aapcs-vfp")` shortcut is unsafe: GCC 14 drops the attribute in templates (tested). Patch prepared:
  `buildroot-external/staged-patches/wpewebkit/0004-jsc-arm-softfp-jit.patch` (applies cleanly to 2.50.5; NOT yet compiled).
  Build switch: `make WPEB_SOFTFP_JIT=y` (external.mk; WebKit as Thumb-2). Tests: `tools/jsc-softfp/{boundary.js,run.sh}`
  (interpreter vs forced Baseline/DFG, bit-exact hashes; wasm module validated with node). WebAssembly BBQ off on softfp for now.
  FP-returning JIT operations: 36; all 32-bit call sites go through callOperation*/setupResults except WasmToJS (patched).
- **GPU:** Chromium renders with EGL into `wl_egl_window` on the window's own surface (no readback); needs GPU work in the
  window-owning process. webOS 6 SDK has `wl_webos_surface_group_compositor` (multi-client surfaces in one window) and
  `wl_webos_foreign`. Plan: web process (softfp after phase 1) attaches its own EGL surface to the UI's surface group;
  WebKit via a libwpe renderer backend ("wpebackend-webos"). Needs a TV probe first.
- Disk: removed `~/webos-browser-build/ctng-work` (crosstool-ng scratch, ~17 GB incl. tarball cache copies) during the
  hard-float build; free space went from 25 GB to 42 GB.
- 2026-11-10 ~23:00 **Hard-float build stopped at the user's request** (was building host-llvm; `br-out-hf` left partial,
  status file marked "stopped"). **softfp JIT build started:** patch 0004 moved into `patches/wpewebkit/`;
  `tools/build-softfp-jit.sh start` → `wpewebkit-dirclean` + rebuild in `br-out` with `WPEB_SOFTFP_JIT=y`
  (CMake: ENABLE_JIT=ON, DFG=ON, FTL=OFF, C_LOOP=OFF, WEBASSEMBLY=ON, `-mthumb`; patches 0001-0004 applied cleanly),
  then `ninja jsc`, then staging to `build/engine-stage-jit/engine`. Log `br-jit.log`, status `br-jit.status`.
  The interpreter engine in `build/engine-stage` remains the fallback.
- 2026-11-10 Decision (user): stay on WPE WebKit 2.50.5 until the JIT and GPU drawing work; upgrade (2.54.2 is current stable, 2026-10-23) afterwards.
- 2026-11-10 GPU phase prep (local): `tools/sg-probe/{sg-probe.c,build.sh}` → `build/sg-probe/sg-probe` (NDK, softfp, needs only
  libwayland-client/-egl, libEGL, libGLESv2, glibc 2.4). Owner = webOS app window (`APPID` env) that creates surface group
  "wpebprobe" + layer "page" (z -1/+1) and leaves its middle transparent; it forks "client" = separate Wayland connection whose
  wl_surface is `attach`ed to the layer and animated with Mali EGL, logging frame/eglSwapBuffers times. Run on the TV (needs
  the user's go-ahead): `APPID=org.sostk.wpebrowser ./sg-probe owner below 20` (and `above`). Not yet run.

## 2026-11-10 Direct GPU presentation, code written (user request: "start writing code for direct gpu"; no TV operations)
- Design and file list: docs/webkit-like-chromium.md, part 2, "Implementation". In short: WPEWebProcess draws
  each page with the TV's EGL into its own wl_surface. The surface is attached to a webOS surface group that the
  UI creates on its window (layer "page", z -1). No glReadPixels, no SHM copies, no texture upload.
- WebKit 2.50.5 still has the libwpe renderer-backend path (AcceleratedSurface::RenderTargetWPEBackend). It is
  chosen per process: once a WPEDisplay exists, views with a libwpe backend are refused, so the engine decides at
  start (WPE_BROWSER_PRESENT).
- Opt-in for testing: the UI needs WPE_BROWSER_RENDERER=gpu and WPE_BROWSER_PRESENT=direct. A negative surface
  status makes it restart the engine in SHM mode for the rest of the run.
- Build: `bash tools/build-webos-backend.sh` (host, NDK; the TV's libwayland is 1.11, no wl_proxy_marshal_flags),
  then stage-engine.sh copies both .so into engine/lib.
- PC check: `bash tools/direct-probe.sh` PASS. The engine runs in direct mode, the web process loads the backend,
  the target reports -1 (no compositor on the PC) back to the UI, page JS runs, and no SHM frames are sent.
- TV (needs the user's go-ahead): run sg-probe first, then the browser with the two env variables.
- **TV run, 2026-11-10 (user said the TV is on; sg-probe from /tmp/sg-probe, nothing installed).** Screenshots via
  `script -q -c "luna-send ... com.webos.service.capture/executeOneShot ..." /dev/null`: luna-send only answers with
  a pty, which is why it printed nothing before. /dev/fb0 can't be read, even as root.
  - Attaching a second process' surface to the group is accepted. Mali EGL in it renders at 60 fps (16.7 ms
    interval, eglSwapBuffers ~1.3 ms).
  - A **bare surface is never drawn**. With a window role (wl_shell toplevel + webOS shell surface, same appId) it
    is drawn. z -1 puts it below the owner, and it shows through the owner's alpha-0 area; z +1 puts it on top.
  - LSM **scales the member surface to the whole owner window** (1280x720 filled 1920x1080). It can't be placed
    in a sub-rectangle, so the page surface is full-window and the chrome must overlay it.
  - The backend now gives each page surface that window role (WPEB_APP_ID).
- **Overlay toolbar (user chose the recommendation, 2026-11-10).** The page fills the screen in both modes, and the
  chrome (tabs, toolbar, bookmarks) lies over its top. It slides away (160 ms) while the page has focus, and a
  thin progress line shows while it's hidden. It comes back on Red, on Back with no history, or with the pointer
  at the top edge. Down from the toolbar or bookmarks bar returns to the page. The New Tab page keeps the chrome and
  starts below it. Hidden views can't take focus, so focus_omnibox() shows the chrome first.
  Checked on the PC with the QEMU engine: screenshots of the hidden toolbar, Red, Back, and Down back to the page.
- 2026-11-11 **Softfp JIT build finished** (`tools/build-softfp-jit.sh status`: exit=0, staged `build/engine-stage-jit/engine`, 250 MB).
  - `tools/jsc-softfp/run.sh`: boundary.js identical in Baseline and DFG vs the interpreter (45 JIT compilations).
  - js-probe (QEMU, needs `QEMU_LD_PREFIX`=br-out sysroot): same engine 680 ms with JIT vs 1095 ms with `JSC_useJIT=false`;
    WebAssembly works only with the JIT on. QEMU understates the gain; real numbers need the TV.
  - ipk built from it (`-DWPEB_ENGINE_STAGE=.../engine-stage-jit/engine`), 120 MB, check-package passes. The kit's checker got
    `--system-lib` / `SYSTEM_LIBS` so the direct-presentation libraries may need the TV's libwayland-client / libwayland-egl.
    Not installed.
- 2026-11-11 **TV test of the JIT + direct-presentation ipk** (installed with the user's go-ahead).
  - App storage was full (0 bytes free): 8 WPEWebProcess core dumps (1.36 GB) from 2026-11-10 in the app folder. Two
    (core.6302, core.15329) pulled to ~/webos-browser-build/tv-cores/wpeb-cores, all removed from the TV. Not analysed yet.
    Core dumps must not land in the app folder (set RLIMIT_CORE 0 or a cwd in /tmp for the engine).
  - New launch parameters (browser/main.cpp): {"target"|"url": URL} opens a page (also on relaunch);
    {"wpebRenderer":"gpu","wpebPresent":"direct"} replace the environment variables an installed app never gets.
  - Normal mode (software, SHM): Wikipedia full screen, toolbar hidden, 60 fps, UI rss 67 MB, load finished 13.4 s.
  - JIT on the TV: fib(27)+200k sort 644–686 ms over 6 runs, WebAssembly ok (needs the JIT).
  - **Direct mode works**: group wpeb-<pid>/page, web process connected (compositor, shell, webos shell, surface groups),
    1280x720 surface attached, status FIRST_FRAME, page shown under the UI with correct placement; UI 60 fps, UI rss 41 MB
    (no SHM slots), load finished 6.2 s.
  - Bugs seen in both modes: CSS-mask/SVG icons drawn as coloured vertical stripes (same artifact as the gradient stripes
    under QEMU, so a rendering bug of our build, not QEMU); a green block at the top-right of Wikipedia's header.
  - file:// pages show as plain text (no MIME type detection for local files); data: targets are refused ("URL can't be shown").
- 2026-11-11 **Striped icons/gradients fixed: patch 0005** (`buildroot-external/patches/wpewebkit/0005-skia-arm32-gcc-raster-pipeline-abi.patch`).
  - Cause: Skia's raster pipeline on ARM32+NEON marks its Stage pointer type `pcs("aapcs-vfp")`; with GCC/softfp the calls and
    the stage functions disagree on where r,g,b,a go, so each 4-pixel group gets mixed lanes/channels. Affected: CSS and canvas
    gradients, CSS masks (Wikipedia's icons), colour filters (`filter: invert` drew nothing). Images, solid fills, opacity were fine.
  - Fix: the attribute only with clang; GCC keeps NEON with its default ABI. Output is pixel-identical to forcing SKRP_CPU_SCALAR.
  - New PC tool: `bash tools/page-shot.sh ENGINE_DIR URL OUT.png [W H [SETTLE_S]]` (QEMU engine, saves the page frame).
  - Applied in the existing br-out tree by hand (ninja + `make wpewebkit-reinstall`), engine-stage-jit restaged; ipk installed;
    Wikipedia on the TV (direct mode) shows its icons correctly. The green block in Wikipedia's header was the same bug.
- 2026-11-11 **Crash dumps / crash logging**: the two TV cores (old non-JIT build, 2026-11-10) are in WebKit (core.6302 SIGABRT
  from a WebKit assertion/abort, core.15329 SIGSEGV), different places; that build's unstripped library no longer exists
  (br-out was rebuilt with the JIT), so they can't be symbolised. Not reproduced yet. Now:
  - the engine sets RLIMIT_CORE 0 for itself and the WebKit processes (WPE_BROWSER_CORE_DUMPS=1 keeps dumps);
  - the UI keeps the previous run's log as engine.prev.log next to engine.log (/tmp/org.sostk.wpebrowser on the TV);
  - "web process crashed" is logged with UTC time and page URL. The current br-out build is unstripped, so new crashes
    can be symbolised against it.
  - Omnibox: `data:` URLs were turned into https://data:...; now passed through.
- 2026-11-11 **Toolbar peeks over the page** (user request: show it at the top of the page and on Back):
  - engine: per-view user script in its own world ("wpeb") posts at-top changes (document scroll and large scrolling
    elements, captured scroll events, rechecked after pageshow for back/forward) → WPE_ENGINE_SCROLL (IPC v6). Each view
    now gets its own WebKitUserContentManager (related views would share the first tab's). WPE_BROWSER_DEBUG_SCROLL=1 logs.
  - UI: with the page focused, the chrome shows while the page is at its top and for 2.5 s after Back (focus stays on
    the page, keys still go to it); it slides away when the page scrolls. Red / Back without history / pointer at the top
    edge still move focus into it.
  - Verified on the PC (real UI + QEMU engine, long test page): top → shown, PageDown → hidden, Back → shown, then hidden
    after 2.5 s on the restored scrolled page. Note: the peeking toolbar covers the top ~136 px of the page.
  - Kit script gotcha: a trailing wait@ has no effect (the shot is taken when the steps end); end with a dummy key (F24).
  - 2026-11-11 user tested the peeking toolbar on the TV (direct mode): works.
- 2026-11-11 **GPU drawing is the default on the TV**: webOS builds (WNK_SDL_WEBOS) default to renderer gpu + direct presentation; launch params {"wpebRenderer":"software"} / {"wpebPresent":"shm"} or the env vars override; host builds stay software + SHM. Built and package-checked; not yet installed.
- 2026-11-11 **Direct mode vs web-process switches** (found when GPU drawing became the default: YouTube went black):
  - WebKit moved the tab into a new web process (YouTube's COOP header; WPE also swaps on cross-site navigation). The new
    process's surface could not attach ("Layer already attached", Wayland protocol error → SHM fallback). A layer handoff
    in the backend (old target detaches, new one attaches after it confirms or after 500 ms) fixed the error, but LSM then
    hid the whole app (NL_VSC visible:false; screenshot pure black), with or without removing the old window role.
  - Fix: direct mode keeps a view in one web process. WebKit patch 0006 lets WPE honour WPE_BROWSER_NO_PROCESS_SWAP (GTK
    has a property for it), and the engine turns the CrossOriginOpenerPolicy feature off (COOP swaps take their own
    path). All tabs already share one web process (related views), so little isolation is lost. Checked on the PC
    (direct probe: YouTube and a cross-site redirect stay in one process) and on the TV: YouTube draws in direct mode.
  - The backend keeps the handoff and now gives the surface its window role only while attached (safer for crash restarts).
- 2026-11-11 **Page-load speed investigation** (TV: 4x Cortex-A55, 2.6 GB RAM, ~1 GB free, all 4 cores allowed, no cgroup limit):
  - Engine logs Navigation Timing per page ("tab N timing: dns/connect/ttfb/html | interactive dcl fcp load | resources").
  - Launch parameter {"wpebEnv": {"WEBKIT_*"/"JSC_*"/"WPE_BROWSER_DEBUG*": "..."}} passes engine environment for
    experiments; WPE_BROWSER_DEBUG_SETTINGS="prop=value;..." sets WebKitSettings properties (e.g. user-agent,
    enable-javascript-markup).
  - Wikipedia (desktop): network ~0.6 s, DOM interactive ~1.0 s, first paint 3.5-4.2 s (run-to-run noise ~0.4 s).
    Page JS off: no change. example.com: first paint 0.23-0.5 s. The main thread is at 100% of one core from
    navigation to first paint; painting/JIT threads mostly idle. UI uses ~15% CPU constantly (60 fps redraw).
  - tools/tv-sampler (ptrace sampler pushed to /tmp on the TV, report.py on the PC against the unstripped WebKit):
    before first paint the main thread is in layout 25%, text/fonts 18% (fontconfig, FreeType, ICU, HarfBuzz),
    waiting 15%, style 12%, parsing/DOM 8%, JS 6%: no single hotspot. After load, layout+style continue (Wikipedia's
    deferred scripts). gdbserver over a network port / SSH was refused by the permission system; ares-shell's stdin
    pipe works for gdb but interrupt-based sampling did not.
  - GPU painting (WebKit hybrid) now renders correctly (the 2026-11-10 "Mali tile corruption" was the Skia ABI bug,
    patch 0005) and is the GPU-mode default; first paint ~10% earlier in back-to-back runs.
  - Mobile sites are much cheaper: YouTube desktop interactive 10.3 s / load 10.6 s / 23 core-s vs m.youtube.com
    2.8 s / 3.7 s / 11 core-s. Wikipedia's mobile redirect needs another UA string.
  - WebKit is compiled for -mcpu=cortex-a9 (Buildroot BR2_GCC_TARGET_CPU); the TV is Cortex-A55. Division helpers
    are 0% of samples; re-tuning would mainly help scheduling (estimate ~10%, ~5 h rebuild).
- 2026-11-11 **More WebKit threads** (GPU mode defaults now): WEBKIT_SKIA_CPU_PAINTING_THREADS=4,
  WEBKIT_SKIA_GPU_PAINTING_THREADS=2, JSC_numberOfGCMarkers=4. Wikipedia, 3 runs each: default first paint 4.09 s /
  load 4.59 s; with the threads 3.57 / 4.03 (A/B via wpebEnv) and 3.80 / 4.25 after installing them as defaults:
  ~7-13 % faster. Layout/style/parsing stay on the one main thread (WebKit design; same in Chrome).
  - Maximum threads (8 CPU + 4 GPU painting): first paint 3.83 s / load 4.38 s vs 3.73 / 4.18 with 4+2 (4 runs each):
    slower (painting threads crowd the main thread on 4 cores). GPU-affine hybrid strategy: 3.80 / 4.24; with 4 GPU
    threads 3.63 / 4.20: within noise. Kept 4 CPU + 2 GPU, WPE's default CPU-affine strategy.
- 2026-11-11 **YouTube video** (user: "fixing video playback on youtube"; TV tests with the user's go-ahead):
  - **Sound** never worked: libpulse looked in XDG_RUNTIME_DIR=/tmp/xdg (root's). The TV's PulseAudio (9.0, LG-patched,
    system mode, auth-anonymous) is at /var/run/pulse/native, mounted in the app jail. Our PulseAudio 17 libpulse is
    dropped by it on stream creation ("Connection terminated"; memfd/SHM off made no difference), the TV's own
    libpulse.so.0 works: stage-engine.sh no longer bundles it (system lib). PULSE_SERVER + PULSE_SINK=pmedia
    (LG's policy gives streams on the hardware sink pcm_output 0 % volume; pmedia/pdefaultapp 100 %, pndk 0 %),
    PULSE_RUNTIME_PATH in our runtime folder. GST_REGISTRY_UPDATE=no (inherited from webOS) is unset.
  - **Black screen mid-video = webOS killed the web process**: SIGTERM from `sam` (seen with gdb over the ares-shell
    stdin pipe: `target remote | ares-shell -d tv -r "gdbserver - --attach PID"`, NDK gdb). memchute
    (com.webos.memorymanager, getCurrentMemState/getUnitList via luna-send in a pty) went "medium" with the web
    process at ~800-900 MB. Hybrid GPU painting held 407 MB of Mali buffers vs 140 MB with CPU painting on the same
    page (Skia GPU caches, 256 MB per context, no limit set by WebKit): GPU mode now paints on the CPU again
    (WEBKIT_SKIA_ENABLE_CPU_RENDERING=1; GPU composites and presents). YouTube's watch page alone is ~600 MB.
  - Software decoding: YouTube chose VP9 1080p60 (dropped frames, memory). Engine user script (all frames, page
    world) answers MediaSource.isTypeSupported / canPlayType / mediaCapabilities "no" above 1280x720@30 and for
    VP9/AV1 when decoding is in software; WPE_BROWSER_DEBUG_MEDIA="off" | "PIXELS@FPS[+vp9]". Still not smooth.
  - **Hardware video (works, user: "The video is smooth")**: engine/gst/wpebmediasink.c registers `rialtomsevideosink`
    and `rialtomseaudiosink`; WebKit 2.50's generic "rialto" quirks (WEBKIT_GST_QUIRKS=rialto,
    WEBKIT_GST_HOLE_PUNCH_QUIRK=rialto, WEBKIT_GST_USE_PLAYBIN3=1) make decodebin stop at encoded H.264/HEVC/VP9 and
    use them as the hole-punch video sink (no WebKit rebuild). The sink (sync to the pipeline clock) streams access
    units over a socket (common/wpe_media_ipc.h, $runtime/media.sock) to the UI process, whose MediaServer feeds the
    kit's NdlBackend: LG's media stack registers bus names under the app's luna role, which is bound to the
    `wpebrowser` executable, not WPEWebProcess. Picture on the video plane under the page (page surface below the
    UI window, both transparent there). First run: VP9 854x480 NDL load 2.2 s, Init 0.4 s, SetAppState -> -1 (logged
    only), area follows WebKit's rectangle. Audio still software (pulsesink in our audio sink): one NDL session per
    app, page sounds must not take it. Off: launch parameter {"wpebVideo":"software"} or non-direct presentation.
  - Open: A/V sync (audio separate from NDL), pipeline pause/resume every ~8 s in the log (buffering?), web process
    ~85 % CPU on YouTube's desktop page, fullscreen, seeking (NDL splice), tab switch/background with video.
- 2026-11-11 **Hardware video: comparison with webOS-ports/chromium151** (Chromium 151 on LG's webOS OSE Chromium 120 port):
  - Page overlays: our NDL video is a punch-through LG places directly below the app's main window, which cuts out
    the page layer below it (YouTube controls vanished). Raising the page above the UI fixed the picture but gave
    the page surface keyboard focus; `focus_owner` (owner or client side) and `set_key_index -1` are rejected by
    LG's compositor (group destroyed / Wayland error 22). Page raising is now opt-in ({"wpebRaisePage":"1"}).
  - Chromium never has this problem: page and controls are one surface, the video element becomes a viz
    VideoHoleDrawQuad (transparent hole), and the video plane is bound to an **exported window** created on that
    surface with LG's `wl_webos_foreign.export_element(surface, video_object=0)` (ui/ozone/platform/wayland/
    extensions/webos/host/webos_foreign_video_window*.cc). `window_id_assigned` gives the id; geometry goes through
    `wl_webos_exported.set_exported_window(src, dst)` or `set_crop_region`; `set_property("mute", on/off)` hides it.
  - The id goes to LG's media pipeline as `option.windowId` (media/neva/webos/umediaclient_impl.cc, uMediaServer
    `uMediaClient` for URL/HLS playback; MSE on OSE goes through media/gpu/webos `mcil` decoding into GL textures,
    which this TV does not have: no libmcil; it has libplayerAPIs (StarfishMediaAPIs), libumedia_api, libNDL*).
  - So the Chromium-equivalent for this TV: export a video window on **our page surface** (backend, web process),
    send its window id over the media socket, and play through the kit's StarfishBackend (libplayerAPIs; already
    uses option.windowId with SDL-webOS exported windows; needs a "use this window id" option) in the UI process.
    That keeps the page above its own video, no z-order flip or focus problem, and Starfish takes compressed audio
    (AAC/Opus) and has pause/seek/position, unlike NDL v2.
- 2026-11-11 **Hardware video now Chromium-style** (user: "Can we update ours to match chromium"):
  - Backend (web process) binds wl_webos_foreign and exports `wpeb_webos_video_open/place/close`: export_element
    (video_object) on the shown page surface, window_id_assigned awaited (1 s, event on the target's queue),
    set_exported_window(src = coded picture, dst = WebKit's hole rectangle). The media sink (same process,
    dlopen RTLD_NOLOAD of libWPEBackend-webos.so) opens it at caps time and sends the id in WPEB_MEDIA_LOAD (IPC v2).
  - UI MediaServer: a load with a window id plays through the kit's StarfishBackend (libplayerAPIs) with
    StarfishOptions::window_id (kit patch 0006: use an external exported window, never create/place/destroy it);
    without one, NDL as before. TV: test page H.264 720p30 via Starfish, window _Window_Id_18, ~30 fps shown,
    0 dropped after start; overlays draw over the video with the page *below* the UI (no z flip, no focus issue);
    loop restart used LG's Seek(0). YouTube: Starfish VP9 854x480 on its own page window.
  - Page raising (wpebRaisePage) is no longer needed; kept opt-in. Audio still PulseAudio (next: AAC/Opus into the
    same Starfish load for A/V sync).
- 2026-11-11 **No kit Starfish: own LG media pipeline client** (user: "Donot use kit starfish do what chromium does"):
  - webOS-ports/chromium151: URL playback = uMediaServer uMediaClient (libumedia_api) with option.windowId = the
    exported window (media/neva/webos/umediaclient_impl.cc). Its MSE path is mcil (media/gpu/webos), absent on this
    TV; uMediaClient has no buffer feed (checked the TV's libumedia_api exports). User chose: Chromium's
    uMediaServer path for URLs (not done yet: needs WebKit to hand the URL over) + our own StarfishMediaAPIs client
    for MSE. Kit patch 0006 removed again.
  - browser/lg_media_player.{hpp,cpp}: links libplayerAPIs through the NDK's starfish-media-pipeline header
    (StarfishMediaAPIs in a 64 KiB block, never freed); BUFFERSTREAM Load with option.windowId, appId, codec,
    envelope from smp::util::getMaxVideoResolution; Feed {"bufferAddr","bufferSize","pts" ns,"esData":1}; media
    (stream) time from the sink; Seek(ms) at the first unit after a load/flush; real Pause/Play; Unload on a worker.
  - MediaServer: per-stream feeder thread with a queue (generation counter dropped on flush/load), so a "BufferFull"
    decoder (paused) never blocks pause/play/seek messages (one run stuck after a pause before this).
  - TV: test page H.264 720p plays, loops via Seek(0); YouTube VP9 480p on _Window_Id_23 with YouTube's controls
    drawn over the video (screenshot), first frame 0.6 s after Load. A Pause at 0:11 without user input: open.
- 2026-11-11 **User agent**: "WPEBrowser-webOS/0.2" -> "WPEBrowser/0.2". Sites' mobile checks list "webOS" (Palm phones):
  webtv.nettv.com.np (client-side, index-*.js: /iPhone|iPad|...|webOS|Tablet|.../i, blockMobileBrowser) showed its
  mobile page; with the new agent it loads the desktop site (login page, TV 2026-11-11).
- 2026-11-11 **YouTube findings**: desktop YouTube stalled ~3 s in (only the first segment arrives; then 5.1 E-AC-3 audio
  also held video 3 s ahead: Dolby now refused by the media-limits script, pulsesink provide-clock off). m.youtube.com
  with an Android Chrome agent played smoothly on the LG pipeline with its controls over the video, then YouTube's
  player showed "Something went wrong" at media time 60 s (twice; network idle, no console errors): likely YouTube's
  bot check (PO token) rejecting a WebKit engine claiming to be Chrome. iPhone Safari agent: streams VP9 360p over MSE;
  the 60 s mark not yet tested. dash.js sample (Akamai BBB, MSE) plays on the LG pipeline (starts at 320x180).
- 2026-11-11 **window.open / target=_blank as new tabs** (user: Google sign-in opens a window):
  - Engine on_create makes a real related view (tab ids from WPE_ENGINE_TAB_ID_BASE 0x40000000, IPC v7
    WPE_ENGINE_TAB_CREATED {opener}); "close" (window.close) -> WPE_ENGINE_TAB_CLOSE. UI adds the tab after its opener
    and shows it; on window.close it shows the opener and closes the view 1.5 s later; closing a shown tab shows the
    next one first.
  - webOS minimised the whole browser (black screen, user's "crash" on tab switch and after Google sign-in) whenever
    a page surface's window role was removed or a surface with one was destroyed (even unmapped): webOS counts every
    page surface as an app window. Backend now: role kept for the surface's life; hidden = detach + unmap (NULL
    buffer); closed pages' surfaces (with role) are kept in a per-process pool and reused. Also: pages free their
    surface-group handle with wl_proxy_destroy (never the destroy request), one lock serialises attach/detach in a
    process, and an attach first detaches any other page of the process holding the layer ("Layer already attached").
  - TV: popup open -> close -> second popup (reused surface): no minimise, opener shown again (screenshots).

## 2026-11-11 Web-process crashes (user request: "fix the stability and smoothness")
- The "black screen after Google sign-in" and later crashes are SIGSEGV in the web process, not memory kills. LG's
  crash reporter keeps a report per crash (text, registers, maps, backtraces): /var/log/reports/librdx/RDXDErr2_WPEWebProcess_*.gz
  (no core dumps needed). Symbolize with the unstripped br-out/build/wpewebkit-2.50.5/lib/libWPEWebKit-2.0.so.1.6.10
  (same .text as the staged one) and the container's addr2line (offset = pc - r-xp base - 1).
- All 5 reports (11:25-12:14): JSC::JSObject::allocateMoreOutOfLineStorage, from JIT, LLInt and C++ (DOM prototype
  creation) callers. A butterfly was bump-allocated at the end of the last 16 KB MarkedBlock of a 1 MB mapping, i.e.
  a free-list interval ran past its block (heap corruption earlier). Open upstream 32-bit ARM bug: webkit.org/b/295780
  (bisected to the MarkedBlock footer->header change, 32-bit alignment suspected) and b/275099. No fix upstream.
- Mitigations: JSC_useRecursiveJSONParse=false (the only reported workaround, b/275099 c3). The UI reloads a crashed
  tab automatically (once per 20 s; background tabs when shown).
- Black screen after a crash (fixed, TV-tested with SIGSEGV/SIGKILL on Wikipedia, dash.js, popups, hardware video):
  webOS minimises an app when a surface with the app's appId goes away (not crashd: SIGKILL does it too, and the
  minimise comes ~200 ms before WebKit reports the crash). Page surfaces now carry "<appId>.page" (override:
  WPE_BROWSER_DEBUG_PAGE_APPID); they still show in the group, nothing appears in recents, the foreground app stays
  ours. The compositor also kept the dead surface on the layer ("Layer already attached", fatal Wayland error 71 for
  the next page): after a crash the UI makes a new layer "page-N" (wpeb_owner_renew_layer) and tells the engine
  (WPE_APP_SET_LAYER, IPC v8), which re-attaches its views there.
  Tried and dropped: _WEBOS_WINDOW_TYPE_OVERLAY (still minimised); relaunching itself (applicationManager/launch is
  accepted but a minimised window stays minimised); SDL_RestoreWindow / fullscreen toggling (LG's SDL ignores them;
  SDL_GetWindowWMInfo gives no shell surface to call set_state on).
- Second crash, during hardware video (fixed in WebKit patch 0007): SIGSEGV on the main thread, fault 0x1,
  g_object_set_valist -> param_spec_pool_hash. WebKit set urisourcebin's guint64 "ring-buffer-max-size" with an int
  (2 * MB): on ARM32 the next property name is read from stack garbage. Runs for playbin3 + blob: (MSE) + preload auto.

## 2026-11-11 LG's built-in browser (user request: why NetTV is unstyled there; how its media works)
- com.webos.app.browser 6.5.6 = `chrome` launcher + lib/libcbe.so (103 MB, softfp, Chrome/87.0.4280.88): webOS "neva"
  Chromium (neva_app_runtime, webos::WebViewBase) with LG's UI ("crow") and LG's closed webos_tv_impl media code.
  WebAppMgr uses /usr/lib/libcbe.so (Chrome/79). Copies for analysis: scratchpad lgcbe/ (not in the repo).
- NetTV is unstyled there because it ships Tailwind v4 CSS: everything inside @layer (Chrome 99+), color-mix/oklch
  (111+), :has (105+), dvh (108+). Chrome 87 drops the @layer blocks. Its system log has nothing useful (PmLog only
  shows SM_APPDRM lines).
- Its media (from libcbe strings / source paths):
  - MSE: WebMediaPlayerMSE -> MediaPlatformAPIWebOSTV (webos_tv_impl/media/webos_tv/media_platform_api_webos_tv.cc):
    Chromium demuxes, then feeds BOTH audio and video ES to StarfishMediaAPIs (BUFFERSTREAM, FeedBuffer/FeedAudio,
    bufferAddr/bufferSize), with video_preroll_msec, max_feed_ahead_msec, SetPlayRate, SeekInternal, HDR metadata,
    DRM (WIDEVINE_MODULAR/PLAYREADY, svpVersion), windowId from a wl_webos_foreign video window
    (ozone/media/video_window_controller_impl.cc, foreign_video_window_provider.cc); HoleFrameVideoDecoder punches the hole.
  - Plain URLs (src=file.mp4, HLS): WebMediaPlayerUMS -> UMediaClientImpl -> uMediaServer::uMediaClient::loadAsync
    (libumedia_api) with the URL, appId and windowId: LG's own pipeline fetches and plays the file.
  - Also DirectMediaPlayer ("DIRECTMEDIA-ES-PLAYER"), TV/HbbTV/camera/WebRTC-game pipelines.

## 2026-11-11 Audio into LG's pipeline (lip sync)
- As LG's own browser (MediaPlatformAPIWebOSTV feeds audio and video ES to one StarfishMediaAPIs): rialtomseaudiosink
  is now a bin that decides on its first caps. AAC of a playback whose video is on our hardware sink (window, Load
  under 800 ms ago) and not muted -> WpebAudioHw: ADTS headers from codec_data, sync + 1 s ahead like the video, sent
  over the video sink's connection (media IPC v3: AUDIO_FORMAT / AUDIO_NONE / AUDIO / TRACKS / VOLUME). Everything else
  (page sounds, Opus, muted autoplay, audio-only) -> decodebin ! audioconvert ! audioresample ! pulsesink. Dynamically
  added sinks have async=false (a prerolling one put the playing pipeline back to PAUSED: 18 s start on dash.js).
- UI: the Load waits up to 1 s for the audio answer (not when TRACKS says no audio), then Loads video + AAC with the
  kit's verified audio settings (needAudio, audioSync, 1 MB audio buffer, no low-delay); separate audio/video queues.
- TV, dash.js BBB: one Load "H264 + AAC", resources VDEC + ADEC, audioSinkType main_sound, audio and video fed.
  Volume/mute while on LG's pipeline: setVolume/setAudioMute JSON keys are guesses (LG's browser never sets them).

## 2026-11-13 Widevine playback on the TV (user: "Tv is on")
- Built and installed the ipk with WebKit ENABLE_THUNDER=ON + libocdm (engine-stage-jit, check-package passes).
- **First run: every key system failed** (NotSupportedError, ClearKey too, even `[{}]`): requestMediaKeySystemAccess
  asks the UI process with a WebKitMediaKeySystemPermissionRequest, and WebKit's default permission-request handler
  denies it. Fixed: the engine allows media-key-system requests (`on_permission_request`, logged per tab); other
  requests still go to the default (denied).
- Test kit: `tools/eme-test/` (page served from the TV's loopback by a static `minihttpd`; results come back as
  /log beacons). Angel One (Widevine, Google's no-auth proxy) with Shaka 4.11.7:
  - requestMediaKeySystemAccess + createMediaKeys OK for com.widevine.alpha with robustness "", SW_SECURE_CRYPTO,
    SW_SECURE_DECODE; HW_SECURE_ALL → NotSupported. **ClearKey now NotSupported** (no permission request reaches the
    UI, so the ClearKey factory isn't offered with Thunder on; not investigated).
  - **H.264/AAC (cenc) with ABR: played the whole 60 s to `ended`**, 5 keys usable, license 2170 → 1657 bytes,
    1502 frames, 2 dropped, rendition switches 576p/240p/144p kept playing.
  - **VP9/Opus (webm) with ABR: played to `ended`**, 1496 frames, 0 dropped, switched 240p → 576p.
  - Stalls: switching codec mid-stream (MP4/H.264 → WebM/VP9, selectVariantTrack) froze video after ~8 s (time ran
    on to the buffered end, then stopped). Real sites rarely do this.
  - videoWidth/videoHeight stay at the first rendition's size after ABR switches (hardware sink doesn't report
    new caps?). Picture quality on screen not yet checked.

## 2026-11-13 NetTV (webtv.nettv.com.np) live cricket with Widevine
- `WPE_BROWSER_DEBUG_CONSOLE=1` (launch param {"wpebEnv": {...}}): the page's console, uncaught errors, EME/MSE
  calls, media element events, canPlayType/isTypeSupported answers and manifest/licence requests go to engine.log
  as "tab N page: ...". Used because the remote inspector would need a port open to the LAN.
- Failure: Video.js "No compatible source" (code 4). NetTV's Safari test is AppleWebKit && !/Chrome|CriOS|Edg|OPR|
  Firefox|FxiOS/; it then sends X-Client-Browser: Safari, gets an fps.ezdrm.com (FairPlay) licence for HLS.
  Fix: site quirk, a Chrome token in navigator.userAgent on *.nettv.com.np only (HTTP header unchanged).
- With it: DASH (live-ottint.nettv.com.np/npl/live_match3/index.mpd), Widevine licence from widevine-dash.ezdrm.com
  200, keys usable, played ~40 s of the live match (t 59 → 97).
- Then the web process crashed (5 reports 18:09–18:21, all JSC::JSObject::allocateMoreOutOfLineStorage, from
  putDirect via JIT ops and DOM prototype creation) = the 32-bit ARM JSC heap bug of 2026-11-11 (webkit.org/b/295780).
  Symbolize: copy /var/log/reports/librdx/RDXDErr2_WPEWebProcess_* to /tmp with simple names, ares-pull, offsets
  from the report's memory map, addr2line on br-out/build/wpewebkit-2.50.5/lib/libWPEWebKit-2.0.so.1.6.10.
- **Root cause found (not heap corruption):** the faulting instruction is `ldr r2, [r8, #-4]` = the butterfly's
  IndexingHeader::vectorLength, loaded at the top of allocateMoreOutOfLineStorage although the source reads it only
  in the indexed-type cases of IndexingHeader::indexingPayloadSizeInBytes(); GCC hoisted it. Objects without
  indexed properties have no header: their butterfly points just past the allocation. bmalloc chunks are 1 MB; a
  16 KB MarkedBlock at a chunk's end puts the allocation's end on the boundary, and the load hits the next,
  unmapped chunk (fault = boundary + 4 in all reports). Explains upstream's bisect to the MarkedBlock
  footer→header change (a footer kept the bytes after the last cell mapped) and why clang builds rarely see it.
- Fix: WebKit patch 0008 (IndexingHeaderInlines.h): the header pointer passes through an empty `asm volatile` in
  the branches that need it (GCC, 32-bit only), so the load can't be hoisted. Checked in the disassembly: the
  function now only computes r8-8 up front and loads [r2,#4] after the type checks.
- 2026-11-13 **TV verification of 0.2.0** (87 MB ipk, patch 0008 + LG-Chromium media changes): Angel One Widevine
  plays; a mid-play seek (15 s -> 40 s) does flush -> 1, Seek(40000 ms) -> 1, plays again 1.6 s later; at the end
  pushEOS -> 1 and LG reports ENDOFSTREAM (857 frames). Tears of Steel H.264 3840x1714 (Widevine): 700 frames,
  0 dropped. **NetTV: the user watched live TV for an hour without a web-process crash** (before patch 0008 it
  crashed within minutes): the JSC fix holds. Not yet seen: the envelope reload on an FHD->UHD step (Shaka
  would not step in the test), HDR setHdrInfo, hardware Opus.
- 2026-11-14 **YouTube investigation (TV).** www.youtube.com is the desktop app ("Kevlar": Polymer/Lit web components,
  `<ytd-app>`, client WEB) with a ~10 MB JS bundle; our UA (Safari on Linux) gets the ES5 build (10.8 MB +
  custom-elements-es5-adapter) where Chrome/Mac Safari get the 2021 build (9.7 MB). On the C1: interactive after
  10.4-14.7 s (network ~1.5 s, the rest is JS parse/compile), web process ~600 MB and one core busy, TV down to ~260 MB
  available with 450 MB in swap. YouTube's TV app (youtube.com/tv, "Kabuki", leanback) is served to us too: 1.1 MB
  base JS, interactive 1.6 s, fcp 3.2 s, 177 MB, idle after load. The Home tile now opens youtube.com/tv.
- **Video never started** on YouTube: its audio is Opus in WebM and WebKit's MSE append pipeline needs `opusparse`
  (gst-plugins-bad "opus", was off): no audio data, readyState stuck at HAVE_METADATA. Enabled
  BR2_PACKAGE_GST1_PLUGINS_BAD_PLUGIN_OPUS: desktop site plays VP9 480p30 on LG's pipeline (Opus in software).
  The same bug as the WebM Opus/Vorbis demux failure of 2026-11-13.
- **TV app video stayed black:** the media server held LG's Load for the audio side's answer (up to 1 s) but only
  checked that timeout when poll() went idle; the TV app streams without pause, so it never loaded. Now checked on
  every unit too. TV app: VP9 1080p60 on LG's pipeline, 60 fps, web process ~316 MB / 44% CPU.
- The glass panel no longer stays while a page is at its top (web apps that never scroll kept it over their
  header and video); it peeks for 2.5 s after a load and after Back, and comes back with Up at the top edge.
- LG's screen capture (executeOneShot DISPLAY) leaves out the hardware video plane (black); VIDEO is permission-denied.

## 2026-11-14 Older and newer webOS (phase 1)
- Firmware inventory (webosbrew fw-symbols, one image per release, `~/.cache/plxnative/fwsym/data`): glibc 2.14 (webOS 1), 2.19 (2–3),
  2.24 (4.x), **2.28 (5.x and 6.x)**, 2.31 (7–8 = webOS 22/23), newer after. So webOS 5 meets the engine's glibc 2.28 floor;
  the old "webOS 6+" floor was too strict. libpulse 0.20, libwayland-client 0.3.0 and Mali EGL are the same on 5 and 6.
  Widevine CE CDM: none usable on 4.0–4.4, 14.0 on 4.5, 15.1 on 5, 16.3 on 6, 16.4 on 7–8, 17.x on 9+. No libcurl before webOS 5.
- The staged engine needs glibc symbols up to GLIBC_2.28 (fcntl64, statx, thrd_exit; 2.27 memfd_create, expf/logf/powf...;
  2.25 getrandom/getentropy) in 27 files, so webOS 4 (2.24) needs a bundled glibc (plan phase 2: softfp glibc 2.28 + the
  private-loader link, pointed at the bundled loader only when the TV's glibc is older than 2.28).
- `tools/fw-check.py` on the 0.3.0 ipk: the UI failed to load on webOS 1–4 **and on webOS 26 (11.2.0)**: libplayerAPIs lacked
  setHdrInfo/setAudioMute before webOS 5, and webOS 26 changed `smp::util::getMaxVideoResolution` to take `const std::string &`.
  lg_media_player.cpp now looks these four up with dlsym (both getMaxVideoResolution manglings) and logs which exist
  ("player APIs: ..."). After: UI loads on webOS 4.4 → 26 (9/14 images). webOS 1–3 still lack the cxx11 `Feed` (not a target).
- Save logs to USB (Settings → Help and feedback) and .github/ISSUE_TEMPLATE/bug_report.yml added; version 0.3.1.

## 2026-11-14 Phase 2: bundled softfp glibc, chosen at launch (tested on the C1)
- `tools/stage-engine.sh` (STAGE_GLIBC=1, default for softfp): glibc 2.28 from the Buildroot target (ld-linux.so.3,
  libc/libm/libpthread/..., libnss_files/dns) plus the toolchain's gconv modules go to `engine/lib/glibc/` (14 MB, 12 MB of
  it gconv; ipk 87.8 → 92.9 MB). PT_INTERP of wpe-engine and libexec/* is `/tmp/org.sostk.wpebrowser/ld-linux.so.3`.
- `browser/private_loader.cpp`: the UI points that link at `engine/lib/glibc/ld-linux.so.3` when the TV's glibc
  (gnu_get_libc_version) is older than 2.28, else at the TV's `/lib/ld-linux.so.3`. Launch parameter
  `{"wpebGlibc": "bundled" | "system"}` forces either. With the bundled one: LD_LIBRARY_PATH starts with lib/glibc,
  GCONV_PATH = lib/glibc/gconv, LD_PRELOAD empty. GPU stays on (softfp, unlike the hard-float tree). Log line
  "engine loader: ...". Host test browser.private_loader covers the choices.
- TV (OLED65C1, webOS 6.5.3), 2026-11-14:
  - auto: "the TV's glibc (2.28)", link → /lib/ld-linux.so.3, Widevine H.264 eme-test played to the end.
  - bundled forced: wpe-engine, WPEWebProcess and WPENetworkProcess all map engine/lib/glibc/libc.so.6; the TV's Mali
    EGL/GLES load into them (direct presentation, GPU rendering); TV Widevine CDM: H.264/AAC (cenc) played to the end,
    1493 frames, 2 dropped; VP9 on LG's decoder + Opus through the TV's libpulse: a PulseAudio sink input
    (s16le 2ch 48 kHz), 874 frames, 0 dropped. DNS/TLS (Shaka from jsDelivr, Google's licence proxy) worked through
    the bundled NSS modules.
  - Not covered: TV libraries built against an older glibc (only a webOS 4 TV can show that), gconv conversions.
- So on webOS 4 the remaining blockers are the TV itself: no usable Widevine before 4.5 and no libcurl (licence
  requests), lower RAM. Phase 3 needs a webOS 4 tester.

## 2026-11-14 DRM and the app on older webOS: feasibility (offline, firmware symbol inventory)
- **Correction:** webOS 1–4 do ship libcurl, as `libcurl.so.5` (LG's soname; same `curl_easy_*`); 5+ has `libcurl.so.4`.
  libocdm now tries both (provisioning).
- Widevine CE CDM entry points: 14.0 (4.5) and 15.1 (5) export the same `initialize`/`create`/`version` as 16.x, but
  14.0 still has the old `decrypt(InputBuffer, OutputBuffer)` (`CdmEngine::Decrypt`, no `DecryptV16`), so its vtable
  differs from cdm.h; 15.1 is stripped (144 symbols), layout unknown. 17.1 (webOS 24+) dropped `ClientInfo` from
  `initialize`, so our mangled name is missing there. **libocdm now loads only 16.x** (`kSupportedMajor`;
  `OCDM_WIDEVINE_ANY_VERSION=1` skips the check); other versions report the key system unsupported instead of calling
  into a foreign vtable.
- Chromium's public CDM interface (`CreateCdmInstance`, `cdm::ContentDecryptionModule_N`) is on every TV from 3.4:
  `_4` on 3.4 (libwvcdm_shared 2.x), **`_8` on 3.9, 4.4 and 4.10** (libwvcdm_shared 3.0.5 / 3.2.1 / 14.0, driven by
  LG's Pepper plugin libwidevinecdmadapter.so), **`_10` on 5–8** (libwvcdm_shared 15.1–16.4) and on 9–11 (in
  libwidevine-wrapper.so, which also needs luna-service2). LG's `WvContentDecryptionModule_8` constructor takes LG
  `IhIClock/IhIStorage/IhITimer`; whether `CreateCdmInstance` builds those from `Host_8` needs the binary.
  A `_10` backend in libocdm could cover webOS 5–26 and can be developed on the C1 (6.4 has `_10`); `_8` would then
  add 3.5–4.5 (untestable here). Same limit as now: only keys allowing decryption outside the secure video path.
- App on webOS 1–3 (fw-check + the engine's TV-library symbols): 3.x lacks only the cxx11 `StarfishMediaAPIs::Feed`
  (old-ABI `Feed` exists) for the UI, and `wl_proxy_marshal_constructor_versioned` (wayland < 1.10) for the engine;
  libpulse is fine everywhere; bundled glibc 2.28 needs kernel ≥ 3.2 (memfd_create ≥ 3.17 is a risk). webOS 1–2 also lack
  the wayland read-queue API (`wl_display_prepare_read_queue`...) and have other Starfish `Load`/constructor
  signatures: not a target.
