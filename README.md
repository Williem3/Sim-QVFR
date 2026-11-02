# ace-quadview

Goal: an `lmu_quadview`-style OpenXR layer for **Assetto Corsa EVO** that makes the game render
a wide low-resolution outer view and a sharp focus view per eye, to cut GPU and VRAM use.

> **Experimental and unofficial. Use at your own risk.** ace-quadview is not made, approved or
> sanctioned by Kunos Simulazioni or the Assetto Corsa EVO developers. It hooks the game's code and
> changes how it renders, so a game update can break it, and it can crash the game. Use it in
> offline sessions only: Kunos validates content online. Please don't report problems you see
> while using it to the ACE developers.

## Status: alpha

The layer **changes how ACE renders**. It patches the game in memory, after checking the exe's
SHA-256 and code anchors. `[experiment] mode` in `ace-quadview.ini` picks the rendering path:

- `views4`, **Quad View Foveated Rendering (QVFR)**: one ACE frame renders four views (the two
  outer eye views at `periphery_scale` and two focus views at `focus_density` over `focus_percent`
  of the FOV). The layer blends each focus view into its eye at the headset's native size, following
  the eyes when eye tracking is available. This is the launcher's default.
- `quad`, **Two-Loop**: ACE runs two full frames per headset frame (A for the full view, B for the
  focus box), and the layer blends B into A. Frame generation (VectorXR) has to be off.
- `off`: rendering unchanged. Only the log-only probes run.
- Experiments for diagnosis, not for driving: `focus_only`, `two_loops`, `views3`, and
  `double_render`, which crashes ACE. The comments in `manifest/ace-quadview.ini` describe each one.

With `probes=1` (the default), the layer also logs the views, swapchains and frame submission. It
puts log-only hooks on ACE's VR functions, which log call stacks and per-frame call order, and runs
a D3D12 trace. The launcher's QVFR setup writes `probes=0`. See [docs/recon.md](docs/recon.md) for
the reverse-engineering notes.

In any other OpenXR app the DLL does nothing.

**Cheeky Foveated DLSS (spike, off by default).** With `[cheeky] enabled=1` the layer runs
[Cheeky Foveated DLSS](https://github.com/Williem3/CheekyFoveatedDLSS)'s resident runtime
(`CheekyFoveatedDLSSRuntime.dll`, GPL v3, not included: take it from a Cheeky release or build)
through Cheeky's runtime ABI, instead of Cheeky's `dxgi.dll` loader: it starts it at OpenXR start and
hands it ACE's D3D12 device each frame (`src/cheeky/cheeky_host.h`). Cheeky's settings stay in its own
`CheekyFoveatedDLSS.ini`. Every `report_s` seconds the log lists each DLSS view Cheeky sees with its
sizes and evaluations per frame. The keys are described in `manifest/ace-quadview.ini`.

## Build

MinHook and the OpenXR headers are vendored in `third_party/` (see `third_party/README.md`).
Licence: PolyForm Noncommercial 1.0.0 (`LICENSE.md`); third-party notices in `THIRD_PARTY_NOTICES.md`.
See [Licence](#licence) below.

```bash
cmake -S . -B build -G "Visual Studio 18 2026" -A x64
```
```bash
cmake --build build --config Release
```

The offline tests are the `build/Release/*_test.exe` executables (hook stub, code patch, compositor,
focus outline/cull/turn, launcher settings and plan, two-loop and quad flow, and others). Each one
exits non-zero on failure.

`package.ps1 -Version <version>` builds a release zip in `dist\` from a clean checkout of a commit,
after running every test. `packaging/README.txt` is the user guide that ships in the zip.

## Launch (ace-quadview-launcher)

`build/Release/ace-quadview-launcher.exe` opens a settings window and starts Assetto Corsa EVO in VR with the layer for that launch only;
nothing is installed or registered. It reads `ace-quadview.ini` next to it (the
layer's settings, plus a `[launcher]` section), finds the game through Steam, and writes a run
folder under `logs/` with the layer's manifest, a snapshot of the ini and `launch.txt` (what it
did). The layer's log for that launch is `logs/ace-<time>.log`. The window has these settings: rendering
method (QVFR or Two-Loop), eye-tracked or fixed foveation, outer and inner resolution, foveated
width and height, corner radius, feather, and a focus outline in teal, magenta or green.

OpenXR layer order: the layer is loaded nearest the game. Every enabled implicit layer that can be
switched off per launch (OpenXR Toolkit, VectorXR, ...) is loaded below it instead, in registry
order, so nothing can change what ACE renders behind the layer's back. VectorXR frame generation is
turned off for Two-Loop (`mode=quad`, and also `mode=two_loops`); it stays on below the layer for
QVFR. Layers that cannot be moved are listed as staying above.

```ini
[launcher]
; game_dir=K:\SteamLibrary\steamapps\common\Assetto Corsa EVO   (default: found through Steam)
; off_layers=XR_APILAYER_MBUCCHIA_toolkit                         (turned off for ACE)
```

`ace-quadview-launcher.exe --dry-run` writes the run folder and prints the plan without starting
the game; `--launch` launches without the window. Steam must be running and ACE closed.

## Run without the launcher (registered layer)

For development you can register the layer instead, so that ACE loads it when started from Steam.
It then runs whatever `build\Release\ace-quadview.ini` says, which may be the launcher's last
settings. For a log-only probe session, set `mode=off` and `probes=1` in `[experiment]` first.

1. Register the layer for your user (HKCU, removable):
   ```powershell
   powershell -ExecutionPolicy Bypass -File scripts\Register-ProbeLayer.ps1
   ```
   `-Machine` registers it in HKLM instead (run as administrator). Use that to order it against
   another machine-wide layer such as VectorXR.
2. Start SteamVR, then ACE from Steam as usual. Load a session in VR and drive for about a minute.
3. Quit ACE. The log is `build\Release\logs\ace-quadview.log`.
4. Unregister when done:
   ```powershell
   powershell -ExecutionPolicy Bypass -File scripts\Unregister-ProbeLayer.ps1
   ```

To skip the layer for one launch, set `DISABLE_XR_APILAYER_ACE_quadview=1`. Offline sessions only:
the layer patches game code, and Kunos validates content online.

## Tools

`tools/` holds Python helpers for the 101 MB exe: string cross-references (`xrefs.py`), function
disassembly with string notes (`disfunc.py`), chunk-to-entry resolution through chained unwind
info and incremental-link thunks (`func_entry.py`), RTTI vtable owners (`vtable_owner.py`),
struct-field displacement search (`disp_refs.py`) and callee labelling by referenced strings
(`label_callees.py`). `gen_launcher_theme.py` and `make_app_icon.py` generate the launcher's theme
header and icon.

## Development and Support

### Show your appreciation

If SIM QVFR has given you smoother VR, extra FPS, or room to turn up the resolution, please
consider donating.

[![Donate on Ko-fi](https://img.shields.io/badge/Donate-Ko--fi-FF69B4?style=for-the-badge&logo=ko-fi&logoColor=white)](https://ko-fi.com/williem3)

## Licence

ace-quadview is free to use, change and share for any **noncommercial** purpose under the
[PolyForm Noncommercial License 1.0.0](LICENSE.md). It is source-available, not open source:
commercial use needs the author's permission.

**Credit is required.** Anyone who shares any part of ace-quadview, changed or not, must pass on
`LICENSE.md` (or its URL) together with its `Required Notice:` line:

```
Required Notice: Copyright (c) 2026 Williem3 (https://github.com/Williem3/Sim-QVFR)
```

MinHook and the OpenXR headers keep their own licences (`THIRD_PARTY_NOTICES.md`).

## Acknowledgements

Thanks to [DSuzuki35](https://github.com/DSuzuki35) for the breakthrough in
[LMU-Quad-View-Mod](https://github.com/DSuzuki35/LMU-Quad-View-Mod) and for the starter launcher,
both of which helped the preliminary ACE quadview development.
