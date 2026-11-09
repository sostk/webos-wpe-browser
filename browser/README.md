# WPE Browser

`org.sostk.wpebrowser`, a native webOS TV app on the [webOS native app kit](https://github.com/Native-Webos/webos-native-app-kit),
made with its `tools/new-app.py`. The kit is not copied in: this folder points at it through
`WNK_KIT_DIR` (environment or `-DWNK_KIT_DIR=`), else `../third_party/webos-native-app-kit`.

```sh
./build.sh            # host build and smoke test (e2e.wpebrowser.starts_and_quits)
./build.sh run        # on the desktop: arrow keys, Enter, Escape
./build.sh webos      # build/webos-release/dist/org.sostk.wpebrowser_0.1.0_arm.ipk, package-checked
ARES_DEVICE=tv ./build.sh install && ./build.sh launch
```

The first build of each configuration also builds the kit's libraries and dependencies (SDL on webOS,
libwebp, libass), into this folder's `build/`; later builds reuse them.

Next steps:

- Read the kit's `docs/AGENT-APP-GUIDE.md`: the rules, then the map from a feature to the kit's API.
- `icon.png` (80x80) and `largeIcon.png` (130x130) are placeholders; replace them with real artwork.
- Network access: `config.http = true`. Settings that survive a relaunch: `config.storage = true`
  and `REQUIRED_PERMISSIONS database.operation` in `wnk_add_webos_package`.
- Media playback: uncomment `set(WNK_FFMPEG ON)` in CMakeLists.txt and follow the guide's
  "Media playback" section (more libraries to link, `BUNDLED_LIBS`, the player's `expected-needed.txt`).
- If the executable's libraries change, the package check fails until `expected-needed.txt` is
  reviewed and updated.
- To keep the kit with the app, add it as a git submodule (`git submodule add <kit url> kit`) and
  set the default in CMakeLists.txt and build.sh to `kit`.
