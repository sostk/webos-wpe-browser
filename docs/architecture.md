# How Wren works

## Two processes

The UI and the engine need different toolchains, so they are separate programs:

1. **`browser/`, the UI app** (`org.sostk.wpebrowser`): C++ on the
   [webOS native app kit](https://github.com/Native-Webos/webos-native-app-kit), built with the stock webOS NDK.
   It owns the window, the browser chrome, input, and LG's media pipeline (which must run under the app's own
   process to get its media resources), and starts the engine.
2. **`engine/`, the engine** (`wpe-engine` + WebKit's `WPEWebProcess`/`WPENetworkProcess`): built with a
   GCC 14 / glibc 2.28 cross toolchain and Buildroot, because WebKit needs a far newer userland than the NDK's.
   It talks to the UI over a socket (`common/wpe_ipc.h`); pages are drawn by the GPU into a surface below the UI
   (`engine/webos-backend`, direct presentation), or sent as shared-memory frames.

The engine brings its own GLib, libsoup, GStreamer, ICU, fonts and TLS certificates; only the TV's glibc, GPU
drivers, Wayland, PulseAudio and Widevine CDM are used.

## Engine

WPE WebKit 2.50.5 (Skia), with the Baseline/DFG JavaScript JIT running on webOS's softfp ABI (patch 0004). Pages are
painted on the CPU and composited with the TV's Mali GPU straight to the screen. See [patches.md](patches.md).

## Media

WebKit's GStreamer pipeline hands encoded video (H.264, HEVC, VP9) and AAC to the browser's own sinks
(`engine/gst/wpebmediasink.c`), which send them over `common/wpe_media_ipc.h` to the UI process. The UI feeds LG's
pipeline (`StarfishMediaAPIs`; `browser/media_server.cpp`, `browser/lg_media_player.cpp`) on the TV's video plane,
the same player LG's own Chromium uses. WebKit punches a hole in the page where the video is. HDR10/HLG metadata, end
of stream, seeks and resolution changes are handled the way LG's Chromium does them
([lg-chromium-media.md](lg-chromium-media.md)). Opus and Vorbis are decoded in software and played through
PulseAudio.

LG player functions that not every firmware has (setHdrInfo, setAudioMute, setVolume, getMaxVideoResolution) are
looked up at run time.

## DRM

`drm/ocdm-widevine` (`libocdm.so`) implements the OpenCDM API on the Widevine CE CDM that ships on the TV
(`libwidevine_ce_cdm_shared.so`, loaded with dlopen), so WebKit's EME works for Widevine sites
([widevine-cdm-abi.md](widevine-cdm-abi.md)).

## Limitations in detail

- **Widevine at software security level:** samples are decrypted by the CDM into normal memory and fed to the
  hardware decoder in the clear, so `HW_SECURE_*` robustness is not offered. Services that require hardware
  security, or licence servers that cap software clients, give SD or refuse; their webOS apps (LG's Chromium) get
  hardware security. Sending encrypted samples to LG's secure video path is undocumented and not attempted
  ([lg-chromium-media.md](lg-chromium-media.md) §4).
- ClearKey does not work while the Widevine (Thunder) backend is on; persistent licences and
  `setServerCertificate` are not implemented.
- Audio other than AAC (Opus, Vorbis) is decoded in software. A codec change mid-stream can stall.
- One hardware video at a time per app (LG's pipeline); a second one loses its picture.
- A site that only has a `favicon.ico` shows a letter instead of its icon (WPE has no favicon API; icons come from
  the page's icon links).

## Repository layout

| Path | What |
|---|---|
| `browser/` | the UI app: `wren_ui.*` (the glass interface: Home, tab row, overlays), `chrome.*` (buttons, address bar), screens (settings, history, bookmarks), engine client, LG media player, log export, profile, tests |
| `browser/branding/` | the Wren icon sources (SVG at three detail levels: 512 master, 130 and 80 px) |
| `engine/src/` | `wpe-engine`: WebKit views, settings, site quirks, debug console, IPC with the UI |
| `engine/gst/` | GStreamer sinks that send encoded audio/video to LG's pipeline |
| `engine/webos-backend/` | libwpe backend for direct GPU presentation on webOS (wl_webos foreign surfaces) |
| `common/` | the UI ↔ engine and media IPC protocols |
| `drm/ocdm-widevine/` | `libocdm.so`: OpenCDM on the TV's Widevine CE CDM |
| `buildroot-external/` | Buildroot tree: configs (softfp, hard-float), the `ocdm-widevine` package, package patches |
| `toolchain/` | crosstool-ng configs (GCC 14.2, glibc 2.28, Linux 4.4 headers, `arm-webos-linux-gnueabi`) |
| `docker/` | the build container (`run.sh '<cmd>'`) |
| `third_party/webos-native-app-kit/` | the kit (submodule) and `third_party/patches/` (local kit patches) |
| `tools/` | build scripts, engine staging, on-TV probes and tests (`eme-test`, `media-tests`, …) |
| `docs/` | these documents |
| `NOTES.md` | the full development log: decisions, TV findings, measurements |
