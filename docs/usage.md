# Using Wren

## The remote

Wren works with the arrows, OK and Back of any LG remote; colour keys are optional shortcuts.

| Key | Does |
|---|---|
| Arrows | move between links and controls on the page (WebKit spatial navigation) |
| ▲ at the top of a page | the glass panel: the tab row above the toolbar; ◀ ▶ and OK switch tabs |
| **Hold OK** | quick actions (new tab, bookmark, tabs, reload, close tab, home, settings, toolbar) and page zoom |
| CH + / − | previous / next tab |
| Back | back in the page's history, or to the panel |
| Menu | side panel: bookmarks, history, zoom, settings, exit |
| Red / Green / Yellow / Blue | all tabs / side panel / quick actions / new tab |

The Magic Remote pointer works as a mouse and its wheel scrolls.

**TV mode:** on TV web apps (a `tv.` host or a `/tv` path, such as YouTube's TV app) Wren hides its panel and
progress line so the page fills the screen. Hold OK → **Toolbar** brings the tabs back. youtube.com links open
YouTube's TV app; the engine variable `WPE_BROWSER_DESKTOP_YOUTUBE=1` keeps the desktop site.

## Launch parameters

`ares-launch -d <device> org.sostk.wpebrowser -p '<json>'`:

| Parameter | Effect |
|---|---|
| `{"target": "https://…"}` | open this URL |
| `{"wpebRenderer": "gpu" \| "software"}` | GPU (default) or software rendering (needs an engine built with `STAGE_MESA=1`) |
| `{"wpebPresent": "direct" \| "shm"}` | draw pages straight to the screen (default) or via shared memory |
| `{"wpebShow": "home" \| "panel" \| "quick" \| "side" \| "zoom" \| "tabs"}` | open that screen a few seconds after start (screenshots, tests) |
| `{"wpebEnv": {"NAME": "value"}}` | environment for the engine, e.g. `WPE_BROWSER_DEBUG_CONSOLE=1` (page console, EME/MSE calls and media events to `engine.log`), `WPE_BROWSER_DESKTOP_YOUTUBE=1`, `GST_DEBUG`, `WEBKIT_*`, `WPEB_HW_OPUS=1` (experimental hardware Opus) |

## Logs

On the TV, in `/tmp/org.sostk.wpebrowser/` (cleared when the TV restarts):

- `engine.log`: the web engine, this run; `engine.prev.log`: the run before (the crash, if the app was reopened)
- `events.log`: the interface

**Settings → Help and feedback → Save logs to USB** writes all three to a USB drive as `wren-logs-<date>.txt`, with
query strings and fragments cut from web addresses. On a rooted TV they can also be fetched over SSH:

```sh
ssh root@<tv-ip> 'cd /tmp/org.sostk.wpebrowser && tail -c 1500000 engine.log engine.prev.log events.log' > wren-logs.txt
```

The logs list the addresses of pages you visited; read them before posting them publicly.
