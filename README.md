# auto-dark

Always-on "flashbang guard" for Windows. It enforces a brightness budget that depends on the
size of the bright area: big bright regions get dimmed hard, while small highlights and dark
content are left alone. You tune it live from a local web panel. See [PLAN.md](PLAN.md) for the design.

## Build

```bash
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

## Run

```bash
./build/Release/auto-dark.exe --open
```

Then use the control panel at http://127.0.0.1:7777/. The console window shows the log; close it
or press Ctrl+C to stop. Stopping removes all dimming immediately.

| Flag | Effect |
|---|---|
| `--open` | open the control panel in your browser |
| `--bypass` | start with filtering off |
| `--profile NAME` | start with a given profile |
| `--port N` | panel port (default 7777) |
| `--no-exclude` | debug: don't hide the overlay from capture (it will feed back into itself) |
| `--debug-layer` | enable the D3D11 debug layer |
| `--vblank` | old loop timing (wait for vblank), for latency A/B tests |
| `--probe X Y` | test: sample one desktop pixel per captured frame, served at `/api/probe` (use with `--no-exclude`) |

**Hotkeys:** `Ctrl+Alt+F12` bypass · `Ctrl+Alt+F11` next profile · `Ctrl+Alt+F10` cycle debug view.

## Tuning workflow

- **Budget curve:** sets the allowed mean brightness for a given bright-area size. The left end
  works as the highlight ceiling, and the right end is the whole-screen budget.
- **Debug views:** *Selection* shows what counts as bright. *Gain heatmap* shows where dimming happens
  and how strong it is. *Split* shows filtered on the left and raw on the right.
- **Profiles:** save variants (night, day, video...) and switch between them with `Ctrl+Alt+F11`.
- **Test pages:** http://127.0.0.1:7777/test/flash.html is a bright page to tune against.

## Adding a parameter or idea

1. Add an entry to `shaders/params.json`. A slider appears in the panel.
2. Declare it in the `Params` cbuffer in `shaders/common.hlsli` (same name).
3. Use it in any shader. Saving the file recompiles live. If compilation fails, the last good
   shaders keep running and the error shows in the panel.

## Latency (backend A: overlay)

- Capture runs on its own thread and device and wakes the render loop the moment Windows presents
  a new desktop frame; the overlay is presented ~1 ms later (shown as **lag** in the panel meters).
- Measured on a 144 Hz screen: content that changes inside an existing window still shows for about
  2 frames (~14 ms) before dimming, because Windows has usually started its next composition by the
  time it hands us a frame. The old vblank-paced loop showed 2–3 frames.
- **Start protected** pre-dims a window the instant it is shown or restored, before its first frame:
  measured 0 full-brightness frames for new windows. Optional triggers for foreground switches and
  title changes (browser tabs) trade brief dips on dark content for coverage.
- The remaining in-window lag can only be removed by backend B (DWM), which filters the frame
  before it is presented.

## Current limitations (backend A: overlay)

- The overlay can only darken.
- Exclusive-fullscreen apps and the secure desktop (UAC, lock screen) aren't covered.
- Per-pixel settings (`Finest scale` 0, `Selective scale` 0, `Protect darks`) can ghost while
  scrolling, because the overlay lags the content.
