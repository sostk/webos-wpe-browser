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

Also seen: slot 4 `getServiceCertificateRequest` returns `08 04`; slot 8 `getOemCryptoBuildInfo` returns
`"oemcrypto build time : May 19 2022 …"`. Slots 12-17 behave consistently with
`removeProvisioning, listStoredLicenses, listUsageRecords, deleteUsageRecord, deleteAllUsageRecords,
getStatusForHdcpVersion`. Slots 0-2 are `onTimerExpired` and the destructor.

Storage names the CDM uses: `cert.bin` (device certificate), `usgtable.bin` (usage table),
`ksid<hex>.lic` (persisted licenses).

The provisioning URL is the one in the TV's `/usr/lib/libwvcdm_shared.so.16.3.0.0` (read it from there).

## Not yet verified

- Slot order after 19 (`load`, `update`, `getKeyStatuses`, `close`, `remove`, `decrypt`, ...). `cdm.h`
  declares up to slot 24 as EXPECTED and nothing past it.
- `IEventListener` slot order (the probe captured `onMessage` through a catch-all vtable).
- The v16 `InputBuffer`/`OutputBuffer` layout for `decrypt()`.
- Secure video path: whether decrypted frames can reach our LG media pipeline (`HW_SECURE_*` content).

## Gotchas

- `IClock::now()` must be real epoch milliseconds. The device certificate carries its creation time;
  a clock behind it makes every session call return `kNeedsDeviceCertificate`.
- Each app has its own storage and must provision once; LG's cached `cert.bin`
  (`/var/palm/jail/<app>/tmp/wam_ce_cdm/`) does not work in another process.
- There are no `nm`/`readelf`/`strace` on the TV; analyse pulled copies on the build machine.

`drm/ocdm-widevine/tests/abi_check.cpp` checks that `cdm.h` keeps every declared method in its verified
slot (run on the build machine and as an ARM binary under `qemu-arm-static`).
