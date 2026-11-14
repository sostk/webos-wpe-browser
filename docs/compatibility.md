# Which TVs

Developed and tested on an LG OLED65C1 (webOS 6.5.3, firmware 03.53.45). Other models are untested; reports are
very welcome ([bug report form](https://github.com/sostk/webos-wpe-browser/issues/new?template=bug_report.yml)).

| webOS (internal release) | Years | glibc | Wren |
|---|---|---|---|
| 1.x – 3.x | 2014–2017 | 2.14 – 2.19 | not supported |
| 4.4 – 4.10 | 2018–2019 | 2.24 | untested, experimental: the engine uses its own bundled glibc 2.28 there; no DRM |
| 5.x | 2020 | 2.28 | should work; no DRM yet (Widevine 15.1) |
| **6.x** | 2021 | 2.28 | **tested** (Widevine 16.3) |
| 22, 23 (7.x, 8.x) | 2022–2023 | 2.31 | should work; Widevine 16.4 untested |
| 24, 25, 26 (9.x – 11.x) | 2024–2026, and older TVs upgraded by webOS Re:New | newer | should work; no DRM yet (Widevine 17) |

How this was checked: the web engine needs glibc 2.28 or newer. It uses the TV's own where that is new enough; on
older TVs it uses a glibc 2.28 bundled in the package (`engine/lib/glibc`; tested by forcing it on webOS 6). An offline check of the
package against LG firmware images (the kit's `tools/fw-check.py`, using the webosbrew firmware symbol inventory)
finds every library and function the interface needs on webOS 4.4 to 26. That shows the app would start, not how
well it runs.

DRM: Wren uses the Widevine CDM already on the TV, through the interface of version 16 (webOS 6: 16.3, tested;
webOS 22–23: 16.4, untested). Other versions are not used, because their interface differs: webOS 4.5 has 14.0,
webOS 5 has 15.1 and webOS 24+ has 17.x; webOS 4.0–4.4 has none of this kind. DRM sites then report that the key
system is unavailable. The TVs from webOS 3.5 on also carry Widevine behind Chromium's public CDM interface
(version 8 up to webOS 4.5, version 10 from webOS 5), which may later bring DRM to them; see NOTES.md.

Details and measurements: [NOTES.md](../NOTES.md), "2026-11-14 Older and newer webOS".
