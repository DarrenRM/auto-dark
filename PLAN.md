# auto-dark — flashbang guard

An always-on **spatial luminance limiter** for the Windows desktop. The rule it enforces is
"the larger a bright area, the lower its allowed brightness". It works on pixels and regions,
with no knowledge of windows or apps. Tuning happens live from a local web control panel.

## Machine context
- Windows 10 22H2 (19045), RTX 3080 Ti, VS 2022 + Windows SDK 26100, CMake, Node 22.
- Monitors: Samsung LS28AG700N 4K @175% (primary, HDR capable but off) + ASUS VG248 1080p 144 Hz.
- SDR pipeline for now; all shader math is done in linear light so HDR can be added later.
- Games are out of scope.

## Architecture

```
Web UI (http://127.0.0.1:7777)  ⇄  HTTP/JSON  ⇄  Engine (C++20 / D3D11)
  generated sliders, curve editor,                ├─ core: params + HLSL shader chain (hot reload)
  profiles, debug views, preview, meters          ├─ backend A: Desktop Duplication + overlay (no admin)
                                                  └─ backend B: hook inside DWM (admin, later)
```

- **Generated sliders.** `shaders/params.json` defines every parameter (range, group, help).
  The engine maps parameters into the shader `Params` cbuffer by name, using shader reflection.
  To add a parameter: add a line to params.json, declare it in `common.hlsli`, then use it.
  No C++ or UI change is needed.
- **Hot reload.** Edits to `shaders/*.hlsl(i)` and `params.json` recompile live. If compilation
  fails, the last good shaders keep running and the error shows in the UI.
- **Profiles** are JSON files in `profiles/`.
- **Safety.** `Ctrl+Alt+F12` toggles bypass. Closing the engine removes the overlay immediately.

## Algorithm (v1)
1. **luma**: sRGB → linear. `Y` = luminance, `m` = soft mask "counts as bright" (threshold/softness),
   `E = Y·m` (bright energy). Written to an RGBA16F pyramid; GenerateMips gives box means per scale.
2. **gain** (coarse → fine, per level k in [minLevel, maxLevel]):
   `g_k = min(1, budget(area_k) / mean_k(E))`, then a min-filter ("spread") so dimming
   transitions land in the dark surroundings instead of inside bright regions. Combined with
   the B-spline-upsampled coarser result: `c_k = min(g_k, up(c_{k+1}))`.
3. **temporal** (on the finest-level grid): instant attack, exponential release.
4. **apply**: B-spline-upsampled gain, then the per-pixel response: *selective* (dimming applied in
   proportion to bright-mask coverage at `selectScale`, so neighbouring dark content is spared and the
   budget is met exactly by the bright pixels), protect-darks, strength and min-gain. Debug views.
   - Backend A draws premultiplied black with `alpha = 1 − gain` on a click-through topmost
     overlay excluded from capture. The result is multiplicative dimming, 1–2 frames late.
   - Backend B will run the same chain on the composed frame before present (no latency).

`area_k ≈ (2·2^k)² / (W·H)` is the effective neighbourhood size of pyramid level k as a fraction
of the monitor. The **budget curve** maps log10(area) → max mean linear luminance (1 = full white).
Its left end acts as the highlight ceiling; its right end is the whole-screen budget.

## Build order
1. [x] Engine skeleton: device, per-monitor DDA capture, overlay windows, bypass hotkey, frame loop.
2. [x] HTTP API + web control panel: schema-driven sliders, curve editor, profiles, preview, meters.
3. [x] Algorithm v1 (above), tuned live from there.
4. [ ] Backend B (DWM): a hook DLL inside dwm.exe, running the same shader chain before present.
   The design below follows the research on ledoge/dwm_lut (GPL-3). It assumes a clean-room
   reimplementation of the technique unless we decide to fork under GPL.
   - **Injection:** an admin injector enables SeDebugPrivilege and copies the DLL somewhere the
     `Window Manager\DWM-N` account can read. It then calls
     `CreateRemoteThread(LoadLibrary)` to load it and `CreateRemoteThread(FreeLibrary)` to unload.
     If DWM crashes, winlogon restarts it clean, so failures recover on their own.
   - **Hooks** (MinHook, in dwmcore.dll):
     - `COverlayContext::Present` runs the chain.
     - `IsCandidateDirectFlipCompatbile` and `OverlaysEnabled` return false, which turns off
       MPO and DirectFlip so every frame goes through composition.
     - Resolve the function addresses from public dwmcore.pdb symbols in the injector (DbgHelp)
       rather than byte patterns, so monthly 19045 updates don't break it.
   - **Per monitor** (keyed by the `DeviceClipBox` origin): an *unfiltered shadow* texture, the
     luma and gain pyramids, and the temporal state.
   - **Each Present:**
     1. Copy the dirty rects from the backbuffer into the shadow.
     2. Run the pyramid, gain and temporal passes.
     3. Apply full screen from the shadow into the backbuffer.
     4. Widen the dirty rects to the full screen.
   - **Context state:** isolate DWM's context state with `SwapDeviceContextState`.
   - **Static screen:** DWM stops presenting when nothing changes, so temporal release would stall.
     Force presents during release with a tiny invalidated window.
   - **Params:** a shared-memory section (DACL grants `S-1-5-90-0`) read with a seqlock, so the hook
     never blocks. A heartbeat from the engine means the hook bypasses itself if the engine dies.
     The web UI and profiles work unchanged.
   - **Skip** HDCP-protected frames, and any formats other than BGRA8 or RGBA16F.
   - **Admin:** either the user runs the injector from an elevated terminal, or the user creates a
     one-time highest-privilege scheduled task that is triggered with `schtasks /run`.
   - **Risks:** a bug blanks the desktop (recovers via restart), unload races (use an in-hook
     refcount), lost MPO/DirectFlip (games are out of scope), and AV / anti-cheat flagging.

## HTTP API (engine ⇄ UI contract)
All responses are JSON unless noted. The server binds 127.0.0.1 only.

| Method | Path | Body / query | Response |
|---|---|---|---|
| GET | `/` , `/app.js`, `/style.css` | | static files from `web/` |
| GET | `/api/schema` | | contents of `shaders/params.json` |
| GET | `/api/state` | | see below |
| POST | `/api/params` | `{ "<id>": number, ..., "curve": [[x,y],...] }` (partial) | `{ "ok": true }` |
| GET | `/api/stats` | | see below |
| GET | `/api/preview?mon=0&mode=0` | mode 0 output, 1 input, 2 selection, 3 gain | `image/jpeg` (204 if none yet) |
| GET | `/api/profiles` | | `{ "profiles": ["default", ...], "active": "default" }` |
| POST | `/api/profile/load` | `{ "name": "x" }` | `{ "ok": true }` |
| POST | `/api/profile/save` | `{ "name": "x" }` (saves current params, becomes active) | `{ "ok": true }` |
| POST | `/api/profile/delete` | `{ "name": "x" }` | `{ "ok": true }` |
| POST | `/api/control` | `{ "bypass": bool }` and/or `{ "resetTemporal": true }` | `{ "ok": true }` |

`GET /api/state`:
```json
{
  "profile": "default", "dirty": false, "bypass": false, "schemaVersion": 3,
  "params": { "threshold": 0.2, "minLevel": 2, "curve": [[-7,1],[-3.5,1],[-2,0.6],[-1,0.3],[0,0.15]] },
  "monitors": [ { "index": 0, "name": "\\\\.\\DISPLAY2", "width": 3840, "height": 2160, "primary": true,
                  "levels": [ { "level": 0, "logArea": -6.32 }, ... ] } ],
  "shaderError": null, "captureExcluded": true
}
```

`GET /api/stats`:
```json
{ "bypass": false, "shaderError": null,
  "monitors": [ { "index": 0, "fps": 143.8, "frameMs": 0.9, "inMean": 0.42, "outMean": 0.12, "minGain": 0.18 } ] }
```

Hotkeys: `Ctrl+Alt+F12` bypass · `Ctrl+Alt+F11` next profile · `Ctrl+Alt+F10` cycle debug view.
