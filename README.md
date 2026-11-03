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

**Foveated DLSS with Cheeky Foveated DLSS (optional, off by default).** ace-quadview can run
[Cheeky Foveated DLSS](https://github.com/ClarkCheekyKent/CheekyFoveatedDLSS) by
[ClarkCheekyKent](https://github.com/ClarkCheekyKent) inside ACE, integrated with his permission. See
[Foveated DLSS setup](#foveated-dlss-setup-cheeky-foveated-dlss) below.

**Performance overlay (`[osd]`, off by default).** A card in the headset view
(`src/quad/osd.h`) shows ACE's frame rate and pacing, its CPU and GPU time with their headroom and
which one limits the frame, VRAM, frame generation and eye tracking. The GPU time is the whole GPU's,
from Windows' GPU engine counters (`src/probe/gpu_load.h`), so the runtime's compositor and frame
generation count too, split by who used it. The CPU time includes ACE's busiest thread
(`src/probe/cpu_load.h`) as well as the render thread. Turn it on in the
launcher's Headset menu pane, from the headset menu, or with Ctrl+Alt+P in the game; the pane and the
headset menu's Overlay and menu page also place it, as they place the menu.

**Field-of-view crop (`[crop]`, QVFR only, off by default).** Narrows each eye's field of view before
ACE sees it, so the edges a helmet covers (a helmet mod's visor, for example) are never rendered
(`src/quad/fov_crop.h`). Top, bottom, outer (temple) and inner (nose) sides are set independently.
The outer views shrink with the crop at the same pixel density, the focus box keeps its size, and
the headset shows black outside the crop. Set it in the launcher's Resolution pane or on the headset
menu's View crop page, live: the view crops at once, and the outer views' pixels follow without DLSS
(with DLSS on, at the next launch).

**Helmet overlay (`[helmet]`, QVFR only, off by default).** The layer draws a helmet's visor opening
around each eye (`src/quad/helmet.h`). Built in, the opening is the crop's rectangle with rounded
corners and the padded rim reaches a little into the view with a soft edge. It's lit by the frame: each side takes the light of the rendered image along it, so it darkens
in tunnels and brightens under a bright sky. Your own helmet can replace the built-in look: `top.png`
(brow), `bottom.png` (chin bar) and `visor.png` with alpha in a `helmet` folder next to the layer DLL,
fixed to the head with a size, height and depth (how far in front of your eyes, for stereo depth).
They're drawn into a head-locked layer of their own that the headset composites after any frame
generation, so the scene keeps the FOV ACE rendered and Cheeky's frame generation stays aligned. Only
the bands the pieces draw on are rendered and composited, at 2048 or, with `detail`, 4096 pixels a side.
The visor slides closed, half or open (Ctrl+Alt+V in the game), and the log suggests the crop that
matches the images' opening. It applies live, from the launcher's Resolution pane or the headset
menu's Helmet page.

## Foveated DLSS setup (Cheeky Foveated DLSS)

The DLSS features come from [Cheeky Foveated DLSS](https://github.com/ClarkCheekyKent/CheekyFoveatedDLSS)
by [ClarkCheekyKent](https://github.com/ClarkCheekyKent), who gave permission to integrate it into
ace-quadview. With QVFR it gives:

- **Foveated DLSS:** the outer views get Cheeky's cheaper DLAA pass at a lower resolution, and the focus
  views keep full DLSS.
- **Focus-view options (experimental):** supersampling, a fix for motion-vector blur, and DLSS-NR.
- **Frame generation (experimental):** Cheeky's DLSS-G between ACE's frames, guided by ACE's own depth and
  motion. A centre region gets full quality and follows your eyes with an eye-tracked focus.

Cheeky Foveated DLSS is a separate download under the GNU GPL v3. ace-quadview doesn't include it. The
layer loads Cheeky's runtime (`CheekyFoveatedDLSSRuntime.dll`) through its runtime interface instead of
Cheeky's `dxgi.dll` loader. Each frame it hands the runtime ACE's D3D12 device, and it sets Cheeky's
quad view roles and the pane's settings live (`src/cheeky/`).

### What you need

| Download | From | Why |
| --- | --- | --- |
| `CheekyFoveatedDLSS-0.6.3-Standalone-ace.2.zip` | [Cheeky Foveated DLSS v0.6.3-ace.2](https://github.com/Williem3/CheekyFoveatedDLSS/releases/tag/v0.6.3-ace.2) | Cheeky v0.6.3 with the D3D12 output-size fixes ACE's quad views need ([pull request](https://github.com/ClarkCheekyKent/CheekyFoveatedDLSS/pull/76)). Official v0.6.3 shows the outer views shrunk ("picture in picture"). |
| `CheekyFoveatedDLSS-0.6.3-Standalone-TEST-SIGNED.zip` | [Cheeky Foveated DLSS v0.6.3](https://github.com/ClarkCheekyKent/CheekyFoveatedDLSS/releases/tag/v0.6.3) (official) | Only for `OpenXR\CheekyOpenXRSetup.exe`, which installs Cheeky's OpenXR layer. The layer is unchanged in the fork build, which doesn't include it. |

Once a Cheeky release includes the fixes, its own Standalone ZIP replaces both.

You also need an NVIDIA RTX GPU with DLSS turned on in ACE. Use the launcher's QVFR method (Rendering pane).

### Steps

1. **Install Cheeky's OpenXR layer.** From the official v0.6.3 ZIP, run `OpenXR\CheekyOpenXRSetup.exe`.
2. **Put Cheeky's runtime next to the layer.** From the ace.2 ZIP, copy the `CheekyFoveatedDLSS` folder into
   the folder that holds `ace-quadview.dll` (`ace\` in the release ZIP). You should then have
   `ace\CheekyFoveatedDLSS\CheekyFoveatedDLSSRuntime.dll`. You can also keep it anywhere and pick the DLL with
   **Choose…** in the Foveated DLSS pane.
3. **Don't use Cheeky's loader.** Leave `dxgi.dll` and `version.dll` from Cheeky's ZIP out of the game folder,
   and remove them if they are there. ace-quadview loads Cheeky itself, and Cheeky refuses to run twice.
4. **Check the OpenXR layer order** if you use OpenXR Toolkit. Toolkit must be above Cheeky's layer, that is
   earlier in the implicit layer list (OpenXR API Layers GUI). Otherwise ACE crashes at start.
5. **Turn it on.** In the launcher's **Foveated DLSS** pane, tick **Use Cheeky Foveated DLSS**. The pane shows
   whether it finds the runtime and the OpenXR layer. After a launch, it also shows Cheeky's status and version
   ("CheekyFoveatedDLSS - v0.6.3").
6. **Launch.** The other settings in the pane apply live. The headset menu's Foveated DLSS and Frame
   generation pages set the same options.

**Frame generation:** turn on **Frame generation** in the same pane. Turn OpenXR Toolkit's **Turbo mode**
off, because ACE crashes with it. Don't run another frame generator, such as VectorXR's, at the same time.
**Show the center region** outlines the full-quality region, so you can check that it follows your eyes.
**Release ACE** (`[cheeky] fg_release`) sets when Cheeky lets ACE start its next frame: Never (5) lets
ACE's CPU and the GPU overlap for the most frames, Capture (8, the default) waits for the GPU to finish
the previous frame, and Generated (3, Cheeky's own default) waits for the generated frame.

**Notes:**
- With DLSS on, resolution changes apply at the next launch, because DLSS sizes its views when the game starts.
- The settings are the `[cheeky]` keys in `manifest/ace-quadview.ini`.
- For problems, send the newest `logs\ace-*.log`. When Cheeky runs, ace-quadview also writes
  `logs\cheeky-snapshot.json`.

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

Thanks to [ClarkCheekyKent](https://github.com/ClarkCheekyKent) for
[Cheeky Foveated DLSS](https://github.com/ClarkCheekyKent/CheekyFoveatedDLSS), which provides the Foveated
DLSS, DLSS-NR and frame generation features, and for permission to integrate it into ace-quadview.
Cheeky Foveated DLSS is free software under the GNU GPL v3. ace-quadview loads its runtime as a separate
download and doesn't include it. The launcher and the headset menu show its version wherever they
control it.
