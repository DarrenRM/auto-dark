# auto-dark

An always-on brightness guard for the Windows desktop: the larger a bright area, the less brightness it is allowed.

## What it is

A full-screen white page in a dark room is the "flashbang". auto-dark removes it at the pixel level,
for every window and app, without knowing anything about them. It gives each bright region a
brightness budget that shrinks as the region grows: a large white page is dimmed hard, while a small
highlight or dark content is left alone. It runs as a tray app, and you tune it live from a local
web panel. The design is in [PLAN.md](PLAN.md).

## What's interesting

- **A rule about area, not about apps.** The budget curve maps the size of a bright region to its
  maximum mean luminance. Its left end is a highlight ceiling and its right end is the whole-screen
  budget. A GPU luminance pyramid measures every scale at once. Gain is combined from coarse to fine
  and eroded at every level, so the fade lands in the dark surroundings instead of leaving a light
  rim inside the bright region.
- **Color only where it dims.** An optional night recipe (warmth, blue and green cuts, saturation)
  is applied only to regions being dimmed, scaled by how hard they are dimmed and by a sunset and
  bedtime schedule. An overlay can only blend, so the shader dims every channel by the smallest
  factor and adds the remaining color back from the captured pixel.
- **44 live parameters from one schema file.** `shaders/params.json` defines each parameter's
  range, group and help text, and the web panel generates its controls from it. Shader parameters
  bind to the constant buffer by name through shader reflection, so adding one takes a line of JSON
  and a declaration in HLSL, with no C++ or UI change.
- **Tuned on the real desktop.** Saving a `.hlsl` file, `common.hlsli` or `params.json` recompiles
  live. If compilation fails, the previous shaders keep running and the error shows in the panel,
  so tuning by eye never leaves the screen unguarded. Three on-desktop debug views (selection mask,
  gain heatmap, and a raw/filtered split) show what the rule is doing to the real screen.
- **Latency measured on the real desktop.** Capture runs on its own thread and device and wakes the
  render loop when Windows presents a frame. The overlay is presented about 1 ms after Windows
  hands over a frame, down from 10.6 ms before capture moved to its own event-driven thread. A pixel
  probe (`--probe X Y`) samples one desktop pixel per captured frame with QPC timestamps, which is
  how latency and flash frames are counted. A "start protected" pre-dim darkens a window the moment
  it appears, before its first frame.
- **Built to stay on.** It recovers from GPU device loss, broken capture hand-overs and stalled
  capture, pauses the overlay on a monitor owned by a fullscreen app, and has a bypass hotkey. The
  web server binds to 127.0.0.1 only, checks Host and Origin headers, and is off until you open the
  panel.

## How it works

```
Desktop Duplication (own thread + device, one per monitor)
        |
        v
  luma  ->  gain  ->  temporal  ->  apply            HLSL, Direct3D 11
  sRGB to   budget    instant      B-spline upsample,
  linear,   per       attack,      per-pixel response,
  mip       level,    slow         debug views
  pyramid   coarse    release
            to fine
        |
        v
Click-through topmost overlay (DirectComposition), excluded from capture;
it draws black with alpha = 1 - gain, so the desktop is multiplied by the gain

Web panel (127.0.0.1:7777) <-> HTTP/JSON <-> engine: params, profiles, previews, meters
```

The overlay can only darken, and it follows content by a frame or two, because Windows has usually
started its next composition by the time it hands over a frame. `PLAN.md` designs the next backend:
a hook inside the compositor that filters each frame before it is presented. Fullscreen games pause
the overlay on their monitor. Not covered: the secure desktop (UAC, lock screen), rotated monitors,
and monitors on a second GPU.

## Running it locally

Requirements: Windows 10 version 2004 or newer (developed on 22H2), x64, a Direct3D 11 GPU, Visual
Studio 2022 with the C++ workload and a Windows SDK, and CMake 3.20 or newer. No environment
variables are needed.

```bash
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
./build/Release/auto-dark.exe
```

It starts as a tray app with no window. Right-click the icon for Bypass, Profile, Open tuning
panel and Start with Windows. The executable reads `shaders/`, `web/` and `profiles/` from the
source tree, which is what makes hot reload work, so run it from a checkout. The log is
`logs/auto-dark.log`; `--console` shows it live.

| Flag | Effect |
|---|---|
| `--open` / `--panel` | start the tuning panel, with or without opening a browser |
| `--console` | show a console window with the log |
| `--bypass` / `--profile NAME` | start with filtering off / with a given profile |
| `--port N` | panel port (default 7777) |
| `--probe X Y` | sample one desktop pixel per captured frame, served at `/api/probe` (use with `--no-exclude`) |
| `--no-exclude` | let capture see the overlay (debug only: the dimming feeds back into itself) |
| `--debug-layer` | create the Direct3D device with the debug layer |

Hotkeys: `Ctrl+Alt+F12` bypass, `Ctrl+Alt+F11` next profile, `Ctrl+Alt+F10` cycle debug view.
With the panel running, open http://127.0.0.1:7777/test/flash.html for a bright page to tune against.

## Status

- Has run at startup as a tray app on Darren's PC since October 2026. Its tuned profile is
  `profiles/good.json`.
- C++20, Direct3D 11 and HLSL shaders, with a web panel in plain JavaScript.
- Measured with the pixel probe on a 144 Hz screen: content that changes inside an existing window
  shows for about 2 frames (about 14 ms) before dimming, and newly shown windows had 0
  full-brightness frames with "start protected" on.

## How it was built

Darren Mills designed and directed auto-dark, and AI coding agents wrote much of the code under that
direction; their `Co-Authored-By` trailers are in the history. He set the rule (regions over budget,
not apps). He chose live tuning on the real desktop over an offline algorithm lab, which is why the
schema-generated panel, hot reload and on-desktop debug views exist. The speed work and the recovery
paths came from his daily use. `PLAN.md` is the design and build order the work followed.

## License

MIT. See [LICENSE](LICENSE).
