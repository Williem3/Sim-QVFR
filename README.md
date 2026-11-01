# ace-quadview

Goal: an `lmu_quadview`-style OpenXR layer for **Assetto Corsa EVO** that makes the game render
a wide low-resolution outer view and a sharp focus view per eye, to cut GPU and VRAM use.

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

Shares MinHook and the OpenXR headers with `../lmu-quadview/rebuild/third_party`.

```bash
cmake -S . -B build -G "Visual Studio 18 2026" -A x64
```
```bash
cmake --build build --config Release
```

`build/Release/stub_test.exe` checks the hand-assembled hook stub (arguments, results, stack walk).

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
