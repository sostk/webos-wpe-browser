<p align="center"><img src="docs/screenshots/wren-icon.png" width="128" alt="Wren"></p>

# Wren: a web browser for LG webOS TVs

A native web browser for LG TVs: **WPE WebKit 2.50** with a JavaScript JIT, GPU drawing, **hardware video through
LG's own media pipeline** and **Widevine DRM** on the TV's built-in CDM. Made for the sofa: everything works with
the arrows, OK and Back of any LG remote.

| | |
|---|---|
| ![Home page](docs/screenshots/home.jpg) **Home**: search, shortcuts, recent pages, open tabs | ![Browsing](docs/screenshots/browsing.jpg) **Browsing**: ▲ at the top of a page shows the tabs and the toolbar |
| ![Quick actions](docs/screenshots/quick-actions.jpg) **Hold OK**: quick actions and page zoom | ![Side panel](docs/screenshots/side-panel.jpg) **Menu**: bookmarks, history, zoom, settings |

## Features

- Tabs, bookmarks, history, search engine choice, page zoom, "continue where you left off", sites' own icons
- Hardware video (H.264, HEVC, VP9; MSE, DASH, HLS) with HDR10/HLG, on LG's decoder
- Widevine DRM (EME) through the TV's own CDM
- YouTube opens as its fast TV app, full screen
- Remote-first: **▲** at the top of a page for tabs, **Hold OK** for quick actions, **CH ±** to switch tabs; the
  Magic Remote pointer and colour keys work too ([all keys](docs/usage.md))

## Install

**webOS 5 or newer** should work (2020 TVs onwards, webOS 22–26 included); only webOS 6 has been tested
([which TVs](docs/compatibility.md)). Download the ipk from [Releases](../../releases) and install it with the
Homebrew Channel or:

```sh
ares-install -d <your-device> org.sostk.wpebrowser_0.3.1_arm.ipk
```

No root is needed to run it.

## Reporting a bug

Plug a USB drive into the TV, then in Wren: hold OK → **Settings** → **Help and feedback** → **Save logs to USB**.
Attach the `wren-logs-….txt` file to a [bug report](https://github.com/sostk/webos-wpe-browser/issues/new?template=bug_report.yml).
Logs are cleared when the TV restarts, so save them first.

## Limitations

- **Widevine runs at software security level**, so some services cap the quality or refuse to play.
- Wren uses the TV's own provisioned Widevine CDM. This is outside the terms LG and Google ship it under, and the
  TV's Widevine certificate could in principle be revoked. **Use at your own risk.**
- Opus and Vorbis audio is decoded in software; only one hardware video plays at a time.

## More

[Documentation](docs/README.md): using the remote, launch parameters and logs, supported TVs, how it works,
building, patches. The development log is [NOTES.md](NOTES.md).

## Licence

GPL-3.0-or-later ([LICENSE](LICENSE)). Fonts: Manrope and Atkinson Hyperlegible (SIL OFL). Bundled libraries keep
their own licences ([details](docs/building.md#licences-in-the-package)). Widevine is not included: Wren uses the
CDM already on the TV. Built with the [webOS native app kit](https://github.com/Native-Webos/webos-native-app-kit).
