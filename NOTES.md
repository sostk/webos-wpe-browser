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
- 2026-11-10 Git repository initialised (branch `main`, one commit per component). The kit's working copy keeps the 4 local changes uncommitted; they are tracked as patches (regenerate with `git -C third_party/webos-native-app-kit diff -- <files>` when a kit change is edited).
- 2026-11-10 Buildroot built every dependency (LLVM 23, Mesa 26.2.5 with libEGL + libgallium, libsoup3, ICU 78, ...). WPE WebKit failed at
  its first file: "DFG and FTL JIT require baseline JIT to be enabled". JSC's 32-bit ARM JIT needs the **hard-float** ABI
  (__ARM_PCS_VFP) and webOS is softfp. **Decision:** CLoop interpreter (JIT/DFG/FTL/WebAssembly/sampling profiler off) via external.mk,
  as Buildroot does for ARMv5/v6/MIPS. Cost: slower JavaScript and no WebAssembly.
  **Possible later speed-up:** build the engine tree hard-float with its own glibc + loader (a true private runtime). WebKit's helpers
  can't be exec'd with a system PT_INTERP, but PT_INTERP can point at a fixed path such as /tmp/wpe-browser/ld-linux-armhf.so.3 that the UI
  sets up at start (like the CA bundle). That would enable the ARMv7 JIT. It means rebuilding the toolchain and all packages.
  **2026-11-10:** the first WebKit compile was stopped at step 3491/7656: 6 parallel jobs exhausted the PC's 15 GB (cc1plus killed). Resumed with `make BR2_JLEVEL=4` (`~/webos-browser-build/br-build3.log`).
