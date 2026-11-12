# Widevine CE CDM on the TV: what we verified

Goal: Widevine EME in the WPE browser using the TV's own licensed CDM as a library (no extraction,
no modification). WebKit's Thunder EME backend talks to DRM only through 16 OpenCDM C functions
(`opencdm_*`); `drm/ocdm-widevine` implements those in-process on top of the CDM below, so WebKit
builds with `ENABLE_THUNDER=ON` without the Thunder framework.

## The library

- `/usr/lib/libwidevine_ce_cdm_shared.so.16.3.0.0` (soname `libwidevine_ce_cdm_shared.so`), ARM EABI5
  softfp, `std::__cxx11` strings. `widevine::Cdm::version()` returns `"16.3.0"`.
- Needs `libkadaptor.so.2` (LG's OEMCrypto/secure-crypto bridge). No separate `liboemcrypto.so`.
- Also used by LG's own apps through WAM (YouTube, the LG browser, ...) and shipped inside other app jails.

## Access from our app

The browser is a `native` app run under `/etc/jail_native_devmode.conf`, which adds the jail user to
`se` and `kadaptor` (the same secure groups as the Netflix jail) and mounts `/dev/lg` into the jail.
All checks below ran as that user (uid 6391, gid 5000, groups kmem, input, audio, video, compositor,
pulse-access, se, kadaptor, crashd).

## Verified on the TV (2026-11-13)

| Step | Result |
|---|---|
| `initialize(kNoSecureOutput, ClientInfo, storage, clock, timer, kErrors)` | `kSuccess` (needs non-empty product/company/model) |
| `create(listener, storage, privacy_mode=false)` | instance |
| `isProvisioned()` (slot 9) | `false` with empty storage, `true` after provisioning |
| `getProvisioningRequest(&req)` (slot 10) | 1139-char base64url request; no URL returned |
| POST `…/certificateprovisioning/v1/devicecertificates/create?key=…&signedRequest=<req>` | HTTP 200, 3940-byte JSON |
| `handleProvisioningResponse(<whole JSON body>)` (slot 11) | `kSuccess`, writes `cert.bin` (2814 bytes) |
| `createSession(kTemporary, &sid)` (slot 18) | `kSuccess`, `"ksid…"` |
| `generateRequest(sid, kCenc, <Widevine PSSH>)` (slot 19) | `kSuccess`, `onMessage(kLicenseRequest)` with a 2128-byte SignedMessage |

### License round trip (Shaka "Angel One" Widevine test content, Google's no-auth test proxy)

Run through the repo's `cdm.h` and `Host`, as the jail user:

| Step | Result |
|---|---|
| `generateRequest` | listener slot 0 (`onMessage`), type 0 (license request), 2155 bytes |
| POST to `https://cwip-shaka-proxy.appspot.com/no_auth` | HTTP 200, 1657-byte license |
| `update(sid, license)` (slot 21) | `kSuccess`; listener slot 1 (`onKeyStatusesChange`, has_new_usable_key = true) |
| `getKeyStatuses` (slot 24) | 5 keys, all `kUsable`, including the audio and video KIDs of the content |
| `getExpiration` (slot 23) | `kSuccess`, -1 (no expiry) |
| `close` (slot 27) | `kSuccess`; afterwards `getKeyStatuses` returns `kSessionNotFound` |

### Decryption: v16 batch API (slot 33)

v16 no longer has the old `decrypt(InputBuffer, OutputBuffer)`. Slot 33 is
`decrypt(const std::string& session_id, const DecryptionBatch& batch)`, found by disassembling the pulled
library: it is the one vtable method that calls the engine's virtual `DecryptV16`
(`wvcdm::CdmEngine::DecryptV16`, exported). Its code gives the layouts (32-bit):

- `DecryptionBatch` (36 bytes): samples\*, count, key_id\*, key_id_length, pattern encrypted/clear blocks
  (must be 0/0 for AES-CTR), `bool is_secure` (+24), scheme (+28: 1 CTR, 2 CBC), `bool is_video` (+32).
- `Sample` (36 bytes): iv\*, iv_length (16 when anything is protected; zero-pad 8-byte CENC IVs),
  data\*, data_length, subsamples\*, count (`{clear_bytes, protected_bytes}`), output\*, output_offset,
  output_length (`output_length - output_offset >= data_length`).
- With `kNoSecureOutput`, a batch with `is_secure` set is refused (`kNotSupported`).

Verified on the TV with the Angel One fragments (first encrypted fragment of each track; the first
fragments are clear lead): one batch of 188 audio samples and one of 100 video samples (subsamples
23 clear + 1472 protected) both returned `kSuccess`. Written back into the fragments on the build machine,
ffmpeg decoded them with **0 errors** (187 audio frames, 100 video frames, picture correct) against 416/210
errors for the still-encrypted fragments.

This content's video key allowed decryption into normal memory. Services that require hardware-secure
decoding for video (most HD/UHD content) are expected to refuse that; that is the secure-video-path question.

### Status codes are v16's, not the 2015 header's

`kSuccess` 0; EME-style errors 1-5 (`kTypeError`, `kNotSupported`, `kInvalidState`, `kQuotaExceeded`,
`kRangeError`); Widevine's own from 101: `kNeedsDeviceCertificate` 101 (seen: createSession before
provisioning), `kSessionNotFound` 102 (seen), `kDecryptError` 103, `kNoKey` 104,
`kKeyUsageBlockedByPolicy` 105; `kUnexpectedError` 99999 (seen: an empty provisioning reply).
Earlier probe output that showed "1"/"2" was the EME errors, not "needs certificate".

Also seen: slot 4 `getServiceCertificateRequest` returns `08 04`; slot 8 `getOemCryptoBuildInfo` returns
`"oemcrypto build time : May 19 2022 …"`. Slots 12-17 behave consistently with
`removeProvisioning, listStoredLicenses, listUsageRecords, deleteUsageRecord, deleteAllUsageRecords,
getStatusForHdcpVersion`. Slots 0-2 are `onTimerExpired` and the destructor. Library log strings show
`getRobustnessLevel` and `getResourceRatingTier` take an out-parameter and return a `Status`.

Storage names the CDM uses: `cert.bin` (device certificate), `usgtable.bin` (usage table),
`ksid<hex>.lic` (persisted licenses).

The provisioning URL is the one in the TV's `/usr/lib/libwvcdm_shared.so.16.3.0.0` (read it from there).

## Not yet verified

- `load` (20), `loadEmbeddedKeys` (22), the key-usage queries (25, 26), `remove`/`forceRemove` (28, 29:
  given a non-session they return `kSessionNotFound`, consistent with taking a session id), slots 30-32,
  and everything after `decrypt` (33). `cdm.h` declares nothing past slot 33.
- `IEventListener` slots 2-4 (`onRemoveComplete`, `onDeferredComplete`, `onDirectIndividualizationRequest`).
- Secure video path: content whose video key demands hardware-secure decoding, and whether its frames can
  reach our LG media pipeline.

## Gotchas

- `IClock::now()` must be real epoch milliseconds. The device certificate carries its creation time;
  a clock behind it makes every session call return `kNeedsDeviceCertificate`.
- Each app has its own storage and must provision once; LG's cached `cert.bin`
  (`/var/palm/jail/<app>/tmp/wam_ce_cdm/`) does not work in another process.
- There are no `nm`/`readelf`/`strace` on the TV; analyse pulled copies on the build machine.

`drm/ocdm-widevine/tests/abi_check.cpp` checks that `cdm.h` keeps every declared method in its verified
slot (run on the build machine and as an ARM binary under `qemu-arm-static`).
