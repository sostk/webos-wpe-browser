# How LG's Chromium plays media on the TV, compared with ours

Source: static analysis of `/usr/lib/libcbe.so` from the LG OLED65C1 (webOS 6.5.3, Chromium 79, used by WebAppMgr for
every web app), copied to the PC on 2026-11-11: its linked libraries, imported symbols and strings (source file
names, log messages, JSON keys). No code was run on the TV for this. Points marked *(inferred)* come from strings
only, not from tracing the code.

## 1. Two playback paths

| Page uses | LG Chromium | Ours (WPE) |
|---|---|---|
| MSE (`MediaSource`, Shaka, dash.js, hls.js, Video.js VHS) | `WebMediaPlayerMSE` → Chromium's own demuxer → `ExternalRenderer` / `MediaPlatformAPIWebOSTV` feeds elementary streams to **`StarfishMediaAPIs`** (libplayerAPIs) | WebKit's GStreamer MSE → our `rialtomsevideosink` / `rialtomseaudiosink` → IPC → UI process `LgMediaPlayer` → **`StarfishMediaAPIs`** |
| Plain `<video src=…>` (MP4, HLS, DASH URL) | `WebMediaPlayerUMS` → **uMediaServer** (`uMediaClient::loadAsync`, `setUri`, `play/seek`): LG's own player fetches and demuxes the URL (`HLS-LG`, `MPEG-DASH` handled natively) | WebKit's GStreamer pipeline (souphttpsrc + qtdemux/hlsdemux) → same sinks → `StarfishMediaAPIs` |
| WebRTC / camera | `rtc_video_decoder_webos`, `StarfishCameraPlayer` | not done |

So for MSE we already use **the same low-level player (`StarfishMediaAPIs`, BUFFERSTREAM) as LG's Chromium**. The
differences are in what we tell it and which calls we use.

## 2. Load payload

Chromium builds it in `MediaPlatformAPIWebOSTV::MakeLoadPayload`. Keys found in libcbe compared with our
`browser/lg_media_player.cpp`:

| Key | Chromium | Ours |
|---|---|---|
| `mediaTransportType` | `BUFFERSTREAM` (MSE); `DIRECTMEDIA-ES-PLAYER` for a direct ES player | `BUFFERSTREAM` |
| `externalStreamingInfo.contents.codec` | video + audio codec | video + `"AAC"` only |
| `esInfo` | `ptsToDecode`, `seperatedPTS`, `videoWidth/Height`, `pixelAspectRatio` | `ptsToDecode:0`, `seperatedPTS:true`, `pauseAtDecodeTime` |
| `opusInfo` (`channels`, `sampleRate`, `streamHeader`) | yes: **Opus goes to the hardware pipeline** | Opus is decoded in software → PulseAudio |
| DRM keys `encryptionType` (`WIDEVINE_MODULAR` / `PLAYREADY`), `svpVersion`, `trackType`, `profileId` (parent key name not in the strings) | yes: **encrypted samples go to the pipeline (secure video path)** | none: we decrypt in software first |
| `adaptiveStreaming` `maxWidth/maxHeight/maxFrameRate` | from `smp::util::getMaxVideoResolution` (same as ours) | same, but our H.264 envelope comes from the **first** rendition |
| `transmission.contentsType` | `WEBRTC` seen for WebRTC; the MSE value isn't visible in the strings | always `LIVE` |
| `lowDelayMode`, `queryPosition` | used | used |
| `videoRenderQueueThreshold` | yes | no |
| `hardwareMemoryChecker`, `DolbyHdrInfo` | yes | no |
| `seekMode: keep-rate`, `trickType: client-side` | trick play *(inferred)* | no |

## 3. Runtime calls

| `StarfishMediaAPIs` call | Chromium | Ours |
|---|---|---|
| `Load`, `Play`, `Pause`, `Seek`, `Feed`, `Unload`, `getMediaID`, `setVolume`, `getCurrentPlaytime` | ✓ | ✓ |
| `flush()` | ✓ on seek (`FlushRenderBuffer`; log "smp::flush() fail") | ✗: our `flush()` only re-seeks on the next feed |
| `pushEOS()` | ✓ end of stream | ✗: the pipeline never learns the stream ended (we rely on WebKit's own EOS) |
| `getVideoRenderQueueLength`, `getAudioBufferSize` | ✓ for feed pacing (`max_feed_ahead_msec_`, `video_preroll_msec_`) | ✗: we pace by the GStreamer clock (sync sink) and retry on `BufferFull` |
| `setHdrInfo` | ✓ HDR10/HLG mastering metadata (`displayPrimaries*`, `whitePoint*`, `maxContentLightLevel`, `smpte2084`, `arib-std-b67`) | ✗: HDR streams play without metadata (the TV may show them with wrong tone mapping) |
| `SetPlayRate` | ✓ `playbackRate` | ✗ |
| `setExternalContext` | ✓ events on Chromium's GLib context | ✗: events on LG's own thread |
| `notifyForeground` | ✓ | ✗ |
| `setSink`, `setServersideMode` | ✓ (`videoSink`/`audioSink`/`avSink`, server-side trick play) | ✗ |

## 4. DRM: the biggest difference

**LG Chromium:** EME key systems come from Pepper CDM plugins (`application_x-ppapi-widevine-cdm`, plugin
`wvcdm_shared`; `playreadycdm`) registered by `key_systems_util_webos.cc`. All robustness levels up to
`HW_SECURE_ALL` are advertised, which our earlier probe confirmed. The renderer passes **still-encrypted** samples
to Starfish, with `encryptionType` and `svpVersion` in the load payload. Decryption and decoding then happen in the TV's secure
video path (OEMCrypto in `libkadaptor`, TEE), and clear frames never reach normal memory *(inferred from the payload
keys)*. That is why web apps get L1-grade Widevine and HD/4K licences.

**Ours:** libocdm uses the same CE CDM library, but decrypts each sample **into normal memory**
(`opencdm_gstreamer_session_decrypt_buffer`) and feeds clear access units. That works for content allowed at
software robustness, but:
- we can't honestly advertise `HW_SECURE_*`, so services that require it refuse or cap the resolution;
- licence servers that check the security level may give only SD keys.

Closing this would mean feeding encrypted samples with LG's DRM payload keys and letting the secure pipeline decrypt them.
That needs a reverse-engineered secure-buffer/session-handoff protocol between the CDM session and Starfish, which
LG doesn't document. It also has the same licence and revocation risk as before. **Not recommended**, at least not
without a separate decision.

## 5. Codec coverage

| | Chromium → hardware | Ours → hardware |
|---|---|---|
| H.264, HEVC, VP9 | ✓ | ✓ |
| AV1 | ✓ when `enableAV1` / `is_av1_decoder_enabled_` | enum exists, not wired |
| AAC | ✓ | ✓ |
| Opus | ✓ (`opusInfo`) | software (PulseAudio) |
| AC-3 / E-AC-3, Dolby Atmos | ✓ when `is_supported_dolby_atmos` | software / none |
| Vorbis | rejected ("not supported audio codec") → software | software, and the WebM audio demux is currently broken for us |
| VP9 with `enableVP9DMA` | DMA path for VP9 *(inferred)* | normal feed |

Chromium also keeps software decoders (FFmpeg, libvpx) as a fallback for what the hardware refuses.

## 6. Video plane

Both use LG's video plane through a punched hole: Chromium uses `media_layer_id` / `windowId` from
wayland-webos-foreign, and we do the same from our backend. Chromium can also render video **into a texture**
through `libvt` (`VT_CreateVideoWindow`, `VT_UpdateOutputTexture`), used for video in WebGL/canvas and for CSS
transforms. We have no texture path, so video can't be drawn to a canvas or rotated.

## 7. Per-app preferences

`media_platform_prefs.cc` reads per-app options from the app's `appinfo.json` media block:
- `maxAudioSourceBuffer`, `maxVideoSourceBuffer`;
- `videoPrerollTime`, `minVideoInterbufferDistance`;
- `enableSourceBufferFlush`;
- `ignoreBufferLowEvent`;
- `enableClientSideTrick`, `enableServerSideTrick`;
- `enableAV1`, `enableVP9DMA`;
- `disableVideoIntrinsicSize`.

It also has app-specific workarounds (e.g. "Hikari 4K"). These are the controls LG found it needed. Buffer sizes
and preroll are the ones most relevant to our stalls.

## 8. What to take from this (ordered by value and effort)

1. **Envelope from the largest rendition, not the first.** Chromium gets it from `getMaxVideoResolution` and the
   rendition size; we squeeze H.264 ≤1080 into a 1920×1080 envelope, which may break a jump to 4K. Small change in
   `LgMediaPlayer::load`; possibly ask WebKit for the largest size or always declare the codec maximum.
2. **Call `flush()` on seek** (then `Seek`), as Chromium does, instead of only re-anchoring. This may fix the
   mid-stream codec-switch and seek stalls.
3. **`pushEOS()` at end of stream**, so LG's pipeline drains and reports `ENDOFSTREAM`.
4. **`setHdrInfo`** from the caps' colorimetry/mastering metadata, for correct HDR10/HLG.
5. **Opus (and AC-3/E-AC-3) to the hardware** with `opusInfo` + `streamHeader`: keeps A/V sync in LG's pipeline
   and fixes Opus timing. It doesn't fix the WebM demux bug, which is ours.
6. **Pace by `getVideoRenderQueueLength`** rather than only the GStreamer clock and `BufferFull`.
7. `SetPlayRate` for `playbackRate`.
8. HW_SECURE DRM: see section 4. A separate, risky decision.
