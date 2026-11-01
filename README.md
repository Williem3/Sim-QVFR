# ace-quadview

Goal: an `lmu_quadview`-style OpenXR layer for **Assetto Corsa EVO** that makes the game render
a wide low-resolution outer view and a sharp focus view per eye, to cut GPU and VRAM use.

> **Experimental and unofficial. Use at your own risk.** ace-quadview is not made, approved or
> sanctioned by Kunos Simulazioni or the Assetto Corsa EVO developers. It hooks the game's code and
> changes how it renders, so a game update can break it, and it can crash the game. Use it in
> offline sessions only: Kunos validates content online. Please don't report problems you see
> while using it to the ACE developers.

## Status: probe build (log only)

The current DLL **does not change rendering**. It is an OpenXR layer that forwards every call
unchanged. In ACE it:

- logs the views, swapchains and frame submission the game uses;
- after checking the exe's SHA-256 and code anchors, puts log-only hooks on seven of ACE's VR
  functions (camera update, FOV setter, frustum, locate/acquire, begin/end frame, single-pass
  decision). Each hook logs the full call stack for its first calls, and every 1800 frames it logs
  three frames of call order.

That reveals the per-eye render loop, which static analysis cannot reach because it runs through
ACE's task system. See [docs/recon.md](docs/recon.md) for what is known so far.

In any other OpenXR app the DLL does nothing.

## Build

MinHook and the OpenXR headers are vendored in `third_party/` (see `third_party/README.md`).
Licence: PolyForm Noncommercial 1.0.0 (`LICENSE.md`); third-party notices in `THIRD_PARTY_NOTICES.md`.

```bash
cmake -S . -B build -G "Visual Studio 18 2026" -A x64
```
```bash
cmake --build build --config Release
```

`build/Release/stub_test.exe` checks the hand-assembled hook stub (arguments, results, stack walk).

## Launch (ace-quadview-launcher)

`build/Release/ace-quadview-launcher.exe` opens a settings window (rendering mode QVFR or Two-Loop,
foveated size, focus and periphery resolution, feather, eye tracking or fixed, a debug outline in
green, magenta or teal) and starts Assetto Corsa EVO in VR with the layer for that launch only;
nothing is installed or registered. It reads `ace-quadview.ini` next to it (the
layer's settings, plus a `[launcher]` section), finds the game through Steam, and writes a run
folder under `logs/` with the layer's manifest, a snapshot of the ini and `launch.txt` (what it
did). The layer's log for that launch is `logs/ace-<time>.log`.

OpenXR layer order: the layer is loaded nearest the game. Every enabled implicit layer that can be
switched off per launch (OpenXR Toolkit, VectorXR, ...) is loaded below it instead, in registry
order, so nothing can change what ACE renders behind the layer's back. VectorXR is turned off in
`mode=quad`. Layers that cannot be moved are listed as staying above.

```ini
[launcher]
; game_dir=K:\SteamLibrary\steamapps\common\Assetto Corsa EVO   (default: found through Steam)
; off_layers=XR_APILAYER_MBUCCHIA_toolkit                         (turned off for ACE)
```

`ace-quadview-launcher.exe --dry-run` writes the run folder and prints the plan without starting
the game; `--launch` launches without the window. Steam must be running and ACE closed.

## Run one probe session

1. Register the layer for your user (HKCU, removable):
   ```powershell
   powershell -ExecutionPolicy Bypass -File scripts\Register-ProbeLayer.ps1
   ```
2. Start SteamVR, then ACE from Steam as usual. Load a session in VR and drive for about a minute.
3. Quit ACE and send `build\Release\logs\ace-quadview.log`.
4. Unregister when done:
   ```powershell
   powershell -ExecutionPolicy Bypass -File scripts\Unregister-ProbeLayer.ps1
   ```

To skip the layer for one launch, set `DISABLE_XR_APILAYER_ACE_quadview=1`. Offline sessions only.
The probe hooks game code, and Kunos validates content online.

## Tools

`tools/` holds Python helpers for the 101 MB exe: string cross-references (`xrefs.py`), function
disassembly with string notes (`disfunc.py`), chunk-to-entry resolution through chained unwind
info and incremental-link thunks (`func_entry.py`), and RTTI vtable owners (`vtable_owner.py`).

## Acknowledgements

Thanks to [DSuzuki35](https://github.com/DSuzuki35) for the breakthrough in
[LMU-Quad-View-Mod](https://github.com/DSuzuki35/LMU-Quad-View-Mod) and for the starter launcher,
both of which helped the preliminary ACE quadview development.
