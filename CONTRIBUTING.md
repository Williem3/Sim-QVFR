# Contributing to Sim-QVFR

This is the technical guide for developers working on Sim-QVFR. Players should start with the
[README](README.md): everything a player sets is in the launcher, so the ini keys below are for
development and diagnosis only.

Sim-QVFR is an `lmu_quadview`-style OpenXR layer for **Assetto Corsa EVO** that makes the game
render a wide low-resolution outer view and a sharp focus view per eye, to cut GPU and VRAM use. In any
other OpenXR app the DLL does nothing.

## How it works

The layer **changes how ACE renders**. It patches the game in memory, after checking the exe's
SHA-256 and code anchors. `[experiment] mode` in `sim-qvfr.ini` picks the rendering path:

- `views4`, **Quad View Foveated Rendering (QVFR)**: one ACE frame renders four views (the two
  outer eye views at `periphery_scale` and two focus views at `focus_density` over `focus_percent`
  of the FOV). The layer blends each focus view into its eye at the headset's native size, following
  the eyes when eye tracking is available. This is the launcher's default.
- `quad`, **Two-Loop** (legacy): ACE runs two full frames per headset frame (A for the full view, B for the
  focus box), and the layer blends B into A. Frame generation (VectorXR) has to be off. The launcher
  no longer offers it (its method picker shows only to switch an ini still set to `quad` over to
  QVFR) and shared profiles can't select it; the code stays, but new work targets QVFR only.
- `off`: rendering unchanged. Only the log-only probes run.
- Experiments for diagnosis, not for driving: `focus_only`, `two_loops`, `views3`, and
  `double_render`, which crashes ACE. The comments in `manifest/sim-qvfr.ini` describe each one.

With `probes=1` (the default), the layer also logs the views, swapchains and frame submission. It
puts log-only hooks on ACE's VR functions, which log call stacks and per-frame call order, and runs
a D3D12 trace. The launcher's QVFR setup writes `probes=0`. The reverse-engineering notes are kept
out of the repository, one file per game (`docs/recon-<game>.md`, listed in `.gitignore`).

### Games

Everything specific to one game is in `src/games/<id>/`: its identity (exe, builds, Steam id), its
addresses, and its QVFR engine code behind `GameAdapter`. The layer and the launcher only use
`src/games/game.h` and `src/games/adapter.h`. To add a game, start with
[src/games/README.md](src/games/README.md); `src/games/ace/` is the worked example.

### Features

**Cheeky Foveated DLSS (`[cheeky]`, off by default).** The layer loads Cheeky's runtime
(`CheekyFoveatedDLSSRuntime.dll`) through its runtime interface instead of Cheeky's `dxgi.dll` loader.
Each frame it hands the runtime ACE's D3D12 device, and it sets Cheeky's quad view roles and the pane's
settings live (`src/cheeky/`). Integrated with ClarkCheekyKent's permission; the credit he asked for
("CheekyFoveatedDLSS - v0.6.3") shows wherever the UI controls Cheeky (`src/cheeky/cheeky_settings.h`).
**Release ACE** (`fg_release`) sets when Cheeky lets ACE start its next frame with frame generation:
Never (5) lets ACE's CPU and the GPU overlap for the most frames, Capture (8, the default) waits for the
GPU to finish the previous frame, and Generated (3, Cheeky's own default) waits for the generated frame.
When Cheeky runs, the layer also writes `logs\cheeky-snapshot.json`.

**Performance overlay (`[osd]`, off by default).** A card in the headset view (`src/quad/osd.h`) shows
ACE's frame rate and pacing, its CPU and GPU time with their headroom and which one limits the frame,
VRAM, frame generation and eye tracking. The GPU time is the whole GPU's, from Windows' GPU engine
counters (`src/probe/gpu_load.h`), so the runtime's compositor and frame generation count too, split by
who used it. The CPU time includes ACE's busiest thread (`src/probe/cpu_load.h`) as well as the render
thread.

**Field-of-view crop (`[crop]`, QVFR only, off by default).** Narrows each eye's field of view before
ACE sees it (`src/quad/fov_crop.h`). The outer views shrink with the crop at the same pixel density, the
focus box keeps its size, and the headset shows black outside the crop. Live without DLSS; with DLSS the
outer views' pixels follow at the next launch.

**Helmet overlay (`[helmet]`, QVFR only, off by default).** Draws a helmet's visor opening around each
eye (`src/quad/helmet.h`), lit by the frame along each side. The package ships `assets/helmet/`
(installed to `layer\helmet`): `top.png`, `bottom.png` and `visor.png` with alpha. They're drawn into a
head-locked layer of their own that the headset composites after any frame generation, so the scene keeps
the FOV ACE rendered and Cheeky's frame generation stays aligned. Only the bands the pieces draw on are
rendered and composited, at 2048 or, with `detail`, 4096 pixels a side. The log suggests the crop that
matches the images' opening.

## Build

### Requirements

- Windows 10 or 11, x64.
- **Visual Studio 2026**, or the free **Build Tools for Visual Studio 2026**, with the **Desktop development
  with C++** workload, including these components (check they're ticked in the installer):
  - MSVC v14.50 x64/x86 build tools,
  - a Windows 11 SDK,
  - **C++ CMake tools for Windows**: the CMake and **Ninja** the build uses.

Nothing else: MinHook and the OpenXR headers are vendored in `third_party/` (see `third_party/README.md`).
A separately installed CMake (3.24 or newer) works too.

**DLSS in LMU (optional): NVIDIA's DLSS SDK, local only.** DLSS in LMU builds only with NVIDIA's DLSS
SDK ([github.com/NVIDIA/DLSS](https://github.com/NVIDIA/DLSS)), which is **never committed or published**: its
licence allows it only in object code inside an application, so its headers, libraries and DLLs stay on your
machine. `.gitignore` ignores its folder and its files (`nvsdk_ngx*.h`, `nvsdk_ngx*.lib`, `nvngx_*.dll`) anywhere in
the tree, and `package.ps1` refuses a commit holding any of them. Put these files side by side in one folder, of the
same SDK release:
- `include\*.h` (the SDK's `include` folder),
- `nvsdk_ngx_s.lib` and `nvsdk_ngx_s_dbg.lib` (from `lib\Windows_x86_64\x64`),
- `nvngx_dlss.dll` (from `lib\Windows_x86_64\rel`: the release DLL, not `dev`),
- `LICENSE.txt`.

That folder is either `third_party\nvidia-dlss-sdk\` in your checkout (gitignored; CMake finds it there) or anywhere
outside it, given to CMake as `-DNGX_SDK_DIR=<folder>`. Without it everything builds and DLSS answers that it isn't
available. With it, the build copies `nvngx_dlss.dll` beside `sim-qvfr.dll`, as a release has it.

### Building

From any PowerShell:

```powershell
powershell -ExecutionPolicy Bypass -File build.ps1
```
```powershell
powershell -ExecutionPolicy Bypass -File build.ps1 -Test
```

The first command builds Release into `build\Release`. The second also runs the offline tests.

Ninja needs Visual Studio's build environment (`cl.exe`, the SDK's headers and libraries on `PATH`, `INCLUDE`
and `LIB`), which an ordinary shell doesn't have, so `build.ps1` loads it itself (`tools\vs_env.ps1`):
- It finds Visual Studio with `vswhere`, runs `vcvars64.bat` and keeps what it changed in
  `%LOCALAPPDATA%\sim-qvfr-dev`, so later builds start in under a second.
- It configures `build\` with the `Ninja Multi-Config` generator on the first run.
- It builds and tests at below-normal priority, so a game in the headset keeps its frames.

Options:
- `-Filter lmu` runs only the tests whose names match.
- `-Target sim_qvfr_launcher` builds one target.
- `-Jobs 8` caps the parallel jobs.

A `build\` folder made by the Visual Studio generator is switched to Ninja: only CMake's cache
(`CMakeCache.txt`, `CMakeFiles\`) is removed, so `build\Release` keeps the development launcher's settings,
logs and profiles.

In a **Developer PowerShell for VS 2026** or an **x64 Native Tools Command Prompt**, the environment is
already there, and plain CMake works the same way:

```bash
cmake -S . -B build -G "Ninja Multi-Config"
```
```bash
cmake --build build --config Release
```
```bash
ctest --test-dir build -C Release -j 16 --output-on-failure
```

The Visual Studio generator (`-G "Visual Studio 18 2026" -A x64`) still works, but rebuilds more slowly.

On an 8-core, 16-thread machine:
- a clean build takes under a minute,
- a change to one source file rebuilds in 2 to 3 seconds,
- a no-op build takes under a second.

Ninja runs about one job per logical processor across the whole build. The big targets precompile the
standard and Windows headers.

### Tests

The offline tests are the `build/Release/*_test.exe` executables (hook stub, code patch, compositor,
focus outline/cull/turn, launcher settings and plan, two-loop and quad flow, and others). Each one
exits non-zero on failure. ctest runs them all side by side, in about 12 seconds.

Each test gets its own TEMP folder and log, under `%TEMP%\sim-qvfr-tests\`. A test executable's first
run after it changes takes about a second longer, because Windows Defender checks every new executable.
The tests link deterministically, so only the ones a change reaches are new.

### Packages

`package.ps1 -Version <version>` builds a release zip in `dist\` from a clean checkout of a commit,
after running every test. It needs the same Visual Studio setup and loads its environment the same way.
`packaging/README.txt` is the user guide that ships in the zip. Without
`-Version` it packages the release planned in `CMakeLists.txt` (`SIM_QVFR_RELEASE`), the one the
launcher's About pane names in a development build. Update that line when the next release is planned.

Every package says whether it has DLSS: `-NgxSdk <folder>` (the SDK folder above) builds with it and ships
`nvngx_dlss.dll` beside the layer, with NVIDIA's licence in `licenses\`; `-NoDlss` builds without it. The package
never carries the SDK's headers or libraries (`package.ps1` checks the zip), only its library compiled into
`sim-qvfr.dll`, the DLL and the licence. The DLL must be one of the SDK's release DLLs that `package.ps1` lists
(`$dlssReleases`, by git blob hash in github.com/NVIDIA/DLSS): never the development one (`dev`), which has the same
signature and version. For a newer SDK, add its `rel` DLL's blob hash there.

A release with DLSS goes out only after NVIDIA has been notified (developer.nvidia.com/sw-notification) and has
approved in writing how the About page, the README and the release notes name NVIDIA and DLSS. Its RTX SDKs licence
asks for both (its supplement, sections 4 and 7). NVIDIA's names stand on their own: "Sim-QVFR uses NVIDIA DLSS",
never "Sim-QVFR's DLSS". Until then, release with `-NoDlss`.

## The launcher

`build/Release/sim-qvfr-launcher.exe` opens a settings window and starts Assetto Corsa EVO in VR with
the layer for that launch only; nothing is installed or registered. It reads `sim-qvfr.ini` next to
it (the layer's settings, plus a `[launcher]` section), finds the game through Steam, and writes a run
folder under `logs/` with the layer's manifest, a snapshot of the ini and `launch.txt` (what it did). The
layer's log for that launch is `logs/session-<time>.log`.

**Launch checks** (`src/launcher/preflight.h`): the exe build (SHA-256, hashed in the background), Steam
running and ACE closed, Cheeky's `dxgi.dll` or `version.dll` loader in the game folder, and with Cheeky on
its runtime, its OpenXR layer, OpenXR Toolkit's place above Cheeky's layer and Toolkit's Turbo mode with
frame generation. After a game update the layer refuses to patch, ACE runs unchanged, and the headset
shows a notice for the first 30 seconds.

**Profiles (`profiles\`, `[profiles]`).** Presets (Performance, Balanced, Quality) set only the
resolution, with a set per foveated mode. A saved profile is every key the launcher writes for the
picture, in `profiles\<name>.ini` (`src/profile/profiles.h`); the panels' places, hotkeys and OpenXR
layers are not in one. Share copies a profile as `QVFR1:` and base64 of only what differs from the
defaults; Paste takes in only a profile's keys.

**Updates (`src/launcher/update_check.h`, `src/launcher/install.h`).** `[launcher] update_check` (off by
default) asks GitHub's API for the newest release. Install (packaged installs only, not while ACE runs)
downloads the release zip, checks it against GitHub's SHA-256, unpacks it with `tar.exe`, checks every
file against `SHA256.txt`, then replaces the program files, keeping `sim-qvfr.ini`, files the release
doesn't have and files the user changed. Replaced files go to `update\backup-<old version>\`, everything
rolls back if a step fails, and the launcher restarts.

**Hotkeys (`[hotkeys]`, `src/input/hotkeys.h`).** Keys act while ACE has focus; wheel buttons are read
through DirectInput on a thread of the layer's own whenever one is bound (`src/input/pad_input.h`),
including D-pad hats. A wheel's menu directions act only while the menu shows. `[menu] hotkey_footer`
(on by default) lists them under the headset menu.

**OpenXR layer order.** The layer is loaded nearest the game. Every enabled implicit layer that can be
switched off per launch (OpenXR Toolkit, VectorXR, ...) is loaded below it instead, in registry order,
so nothing can change what ACE renders behind the layer's back. VectorXR frame generation is turned off
for Two-Loop (`mode=quad`, and also `mode=two_loops`); it stays on below the layer for QVFR. Layers that
cannot be moved are listed as staying above.

```ini
[launcher]
; game_dir=K:\SteamLibrary\steamapps\common\Assetto Corsa EVO   (default: found through Steam)
; off_layers=XR_APILAYER_MBUCCHIA_toolkit                         (turned off for ACE)
```

`sim-qvfr-launcher.exe --dry-run` writes the run folder and prints the plan without starting
the game; `--launch` launches without the window. Steam must be running and ACE closed.

## Run without the launcher (registered layer)

For development you can register the layer instead, so that ACE loads it when started from Steam.
It then runs whatever `build\Release\sim-qvfr.ini` says, which may be the launcher's last
settings. For a log-only probe session, set `mode=off` and `probes=1` in `[experiment]` first.

1. Register the layer for your user (HKCU, removable):
   ```powershell
   powershell -ExecutionPolicy Bypass -File scripts\Register-ProbeLayer.ps1
   ```
   `-Machine` registers it in HKLM instead (run as administrator). Use that to order it against
   another machine-wide layer such as VectorXR.
2. Start SteamVR, then ACE from Steam as usual. Load a session in VR and drive for about a minute.
3. Quit ACE. The log is `build\Release\logs\sim-qvfr.log`.
4. Unregister when done:
   ```powershell
   powershell -ExecutionPolicy Bypass -File scripts\Unregister-ProbeLayer.ps1
   ```

To skip the layer for one launch, set `DISABLE_XR_APILAYER_SIM_qvfr=1`. Offline sessions only:
the layer patches game code, and Kunos validates content online.

## Tools

`tools/` holds Python helpers for the 101 MB exe: string cross-references (`xrefs.py`), function
disassembly with string notes (`disfunc.py`), chunk-to-entry resolution through chained unwind
info and incremental-link thunks (`func_entry.py`), RTTI vtable owners (`vtable_owner.py`),
struct-field displacement search (`disp_refs.py`) and callee labelling by referenced strings
(`label_callees.py`). `gen_launcher_theme.py` and `make_app_icon.py` generate the launcher's theme
header and icon.

## Licence

Contributions are under the project's [PolyForm Noncommercial License 1.0.0](LICENSE.md). MinHook and
the OpenXR headers keep their own licences ([THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)). NVIDIA's DLSS SDK
parts in releases are under NVIDIA's terms (the same file), and its licence forbids reverse engineering them: debug
DLSS through its API and NGX's log, never by disassembling `nvngx_dlss.dll` or the NGX library. Cheeky
Foveated DLSS is GPL v3 and is never bundled: the layer loads it at runtime from the user's own download.
