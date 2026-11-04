# Games

Everything that belongs to one game lives in `src/games/<id>/`. The rest of ace-quadview (the OpenXR
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
  game running as normal instead of patching the wrong code.
- **Its QVFR** (`<id>/adapter.cpp`): an implementation of `GameAdapter`. QVFR (Quad View Foveated
  Rendering) means the game renders four views in one frame: two outer views at low resolution and two
  focus views at high density, which the layer then composes. Getting there is engine work: finding how
  the renderer counts views, lays them out, and fills their cameras, then patching it to render four.
  `GameAdapter::qvfr_*` is everything the layer needs back: the frame records (`Views4Frame`: what FOV
  each view was rendered with), the target sizes, and the live resolution and focus box going in.
- **Its addresses** (`<id>/abi.h` by convention): RVAs, anchor bytes and struct offsets, each with a
  comment saying how it was found. `src/engine/code_patch.h` has the shared tools for anchored code
  patches.
- **Optionally, its own experiments and diagnostics**: `GameAdapter::instance_created`,
  `session_created`, `session_destroyed` and `frame_end`, each reading the game's own `[experiment]`
  ini keys.

Two-loop (`mode=quad`) is legacy and not part of the adapter. A new game supports QVFR or nothing.

## Adding a game

1. Make `src/games/<id>/` with `game.h` (its `GameInfo`, in a namespace named after the game).
2. Add it to `kGames` in `games.cpp`, and give it the identity-only adapter in `adapters.cpp` (`NoQvfr`).
   The layer now switches on in the game but **only logs**: it runs as `mode=off` whatever the ini says,
   turns the session and swapchain logging on, and adds nothing to the game or the headset (no Cheeky,
   menu, overlay or gaze actions). Its log shows the game's OpenXR session: graphics binding, views,
   swapchain sizes and formats, frame submission. `acc/` started this way.
3. Implement `GameAdapter` in `<id>/adapter.cpp` and return it from `adapter_for` in `adapters.cpp`.
4. Add the new `.cpp` files to the shared library in `CMakeLists.txt` (`ace_qv_core`: the layer and the
   tests link it).
5. Write `<id>/README.md`: the build the addresses are from, what is patched, and the open questions.

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
- **D3D12 only for now:** the helmet overlay (in `QuadCompositor`), the GPU frame timer (the overlay
  shows no GPU times on D3D11), the focus outline drawn on uncomposed views, Cheeky Foveated DLSS.

## Still tied to ACE

The log-only engine probes (`src/probe/probe.cpp`) hook ACE's `kProbeTargets`, and the CPU profiler
(`src/probe/cpu_profile.cpp`) names ACE's functions. Both check the game first and stay off anywhere
else. Moving their targets and labels into the adapter is the next step for them.

The launcher starts one game per release (`launcher_game()`).
