# Games

Everything that belongs to one game lives in `src/games/<id>/`. The rest of Sim-QVFR (the OpenXR
layer, the composition, the menus, the launcher) is shared, and only talks to a game through two
headers here.

| File | What it is | Used by |
| --- | --- | --- |
| `game.h` | `GameInfo`: who a game is: name, exe, Steam folder and app id, launch arguments, supported builds | launcher and layer |
| `games.cpp` | The list of games, and lookups by exe | launcher and layer |
| `adapter.h` | `GameAdapter`: what the layer asks of a game's engine code: QVFR and the game's own experiments | layer |
| `adapters.cpp` | Picks a game's adapter; any other process gets one that does nothing | layer |

## What a game provides

- **Its identity** (`<id>/game.h`): a `GameInfo`, with the SHA-256 of every exe build its addresses were
  taken from. The launcher's launch checks and the layer refuse other builds, so a game update leaves the
  game running as normal instead of patching the wrong code. It also says how the launcher starts the game
  (its Steam folder, the exe's subfolder, the VR arguments, and an `openvr_api.dll` to check for a game
  that reaches OpenXR through OpenComposite), and which features beyond QVFR it has so far
  (`GameFeatures`: Foveated DLSS, the helmet, live resolution, the FOV crop). The launcher greys the rest
  out, the headset menu leaves it out, and the layer keeps it off whatever the ini says. The launcher's
  **Game** choice (`[launcher] game`) picks which game a launch starts.
- **Its QVFR** (`<id>/adapter.cpp`): an implementation of `GameAdapter`. QVFR (Quad View Foveated
  Rendering) means the game renders four views in one frame: two outer views at low resolution and two
  focus views at high density, which the layer then composes. Getting there is engine work: finding how
  the renderer counts views, lays them out, and fills their cameras, then patching it to render four.
  `GameAdapter::qvfr_*` is everything the layer needs back: the frame records (`Views4Frame`: what FOV
  each view was rendered with), the target sizes, and the live resolution and focus box going in.
- **Its addresses** (`<id>/abi.h` by convention): RVAs, anchor bytes and struct offsets. Each group gets one
  comment line saying what it is for, ending in a `[name]`; how it was found, what the engine does there and
  what was measured go into the game's recon notes under that `[name]` (step 5), not into the code.
  `src/engine/code_patch.h` has the shared tools for anchored code patches.
- **Optionally, its own experiments and diagnostics**: `GameAdapter::instance_created`,
  `session_created`, `session_destroyed` and `frame_end`, each reading the game's own `[experiment]`
  ini keys.

Two-loop (`mode=quad`) is legacy and not part of the adapter. A new game supports QVFR or nothing.

## Adding a game

0. Triage the exe: `python tools/triage_exe.py <exe> --strings` prints its build hash, graphics and VR APIs (and
   OpenXR extensions), anti-cheat, whether its code is encrypted on disk, its engine, upscalers, Steam's launch
   options and the rendering features it names. Its output starts the game's recon notes (step 5).
1. Make `src/games/<id>/` with `game.h` (its `GameInfo`, in a namespace named after the game). A game with
   anti-cheat lists its processes in `GameInfo::antiCheat`: while one runs, the layer stays log only.
2. Add it to `kGames` in `games.cpp`, and give it the identity-only adapter in `adapters.cpp`
   (`IdentityAdapter`, `identity_adapter.h`; derive from it for the game's log-only probes, as `lmu/adapter.cpp` does).
   The layer now switches on in the game but **only logs**: it runs as `mode=off` whatever the ini says,
   turns the session and swapchain logging on, and adds nothing to the game or the headset (no Cheeky,
   menu, overlay or gaze actions). Its log shows the game's OpenXR session: graphics binding, views,
   swapchain sizes and formats, frame submission. If the exe's code is encrypted on disk (SteamStub), this
   run also writes the decrypted image to the log folder (`<exe>-<hash>-image.dump`, `probe/image_dump.h`)
   for static analysis. `acc/` and `xplane/` started this way, and `lmu/` is at this stage now.
   `python tools/session_report.py <session log>` sums up any test run's log.
3. Implement `GameAdapter` in `<id>/adapter.cpp` and return it from `adapter_for` in `adapters.cpp`.
4. Add the new `.cpp` files to the shared library in `CMakeLists.txt` (`sim_qvfr_core`: the layer and the
   tests link it).
5. Keep the recon notes (the build the addresses are from, what is patched and why, how each address was
   found, measurements, the open questions) in `docs/recon-<id>.md`, with a `## abi.h` section holding a
   `### [name]` per group in `abi.h`. The file is local only: add it to `.gitignore` next to the other games'
   and never commit it. Code comments point there (`docs/recon-<id>.md "Section"`) instead of retelling it.

`ace/` is the worked example.

## Graphics APIs

The layer's own drawing works on the game's API, D3D12 (ACE) or D3D11 (ACC). `SessionGraphics`
(`src/quad/session_graphics.h`) holds the session's API and devices from `xrCreateSession`, and
`swapchain_textures` returns any swapchain's images as that API's textures, so a feature works on either
without knowing which:

- **In-headset menu and performance overlay:** drawn with GDI into pixels, uploaded by
  `make_panel_upload` (`src/quad/menu_panel.h`: D3D12 on the game's queue, D3D11 on its immediate
  context). Both APIs; in ACC, both open and draw in the headset (2026-11-03).
- **Composition:** one shader (`src/quad/compose_shader.h`). D3D12: `QuadCompositor` (ACE's frames).
  D3D11: `D3D11Compositor`, when the adapter says `qvfr_compose_d3d11` (one image holding all four views).
- **Helmet overlay:** both compositors, from one shader (`compose_shader.h`) and one piece loader
  (`src/quad/helmet_pieces.h`: each PNG decoded and measured once, each compositor keeping only its
  textures). Its own head-locked layer is a swapchain of the session's API.
- **D3D12 only for now:** the GPU frame timer (the overlay shows no GPU times on D3D11), the focus outline
  drawn on uncomposed views, Cheeky Foveated DLSS.

## Still tied to ACE

The log-only engine probes (`src/probe/probe.cpp`) hook ACE's `kProbeTargets`, and the CPU profiler
(`src/probe/cpu_profile.cpp`) names ACE's functions. Both check the game first and stay off anywhere
else. Moving their targets and labels into the adapter is the next step for them.

The launcher starts the game chosen under **Game** (`launcher_game()`, `[launcher] game`).
