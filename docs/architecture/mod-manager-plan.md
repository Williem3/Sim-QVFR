# Mod manager plan (short)

The full plan, with the reasoning, is the doc "Sim-QVFR mod manager plan":
https://claude.ai/artifact/J96YZDxfot26NXZDr3X7PE. This file is its summary in the repo, plus what each phase left in
the code.

## Goal

Sim-QVFR splits into a signed **core** and one **mod DLL per game**:

- The core is `sim-qvfr.dll` (the OpenXR layer, the compositors, the headset menu and overlay, the gaze) and the
  launcher.
- Each mod (`sim-qvfr-<id>.dll`) has its own manifest, its own ini and its own logs folder.
- First-party games use the same plugin path as community mods.
- The launcher gives each installed mod its own tab, and a mod can be updated on its own.

## Decisions

1. **Plugin ABI: a versioned C function table, wrapped in a header-only C++ class for mod authors.** We don't
   export the C++ `GameAdapter` class itself:
   - a C++ class ties every mod to our compiler and to the exact layout of `adapter.h`;
   - the core builds with the static CRT, so each DLL has its own heap and STL, and nothing but plain C can cross.
2. **Trust:**
   - *Official* means the Authenticode signer matches a certificate thumbprint pinned in the core. The core checks
     this inside the game process, before `LoadLibraryExW`, so it holds however the game was started.
   - *Community* means everything else. Nothing in `mod.json` can claim Official.
   - An anti-cheat launch loads Official mods only.
   - A community mod loads only after the user approves that exact file (its SHA-256 is stored in `[mods]`).
   - `package.ps1` signs only a fixed list of first-party files.
3. **First game through the seam: SCS Software's adapter** (ATS and ETS2), not ACC, which is being remade. LMU
   comes last.

Layout on disk:

- `mods/<id>/`: code that an update replaces. It holds `mod.json`, `settings.json`, the DLL and the mod's assets.
- `games/<id>/`: user data that no update touches. It holds `sim-qvfr-<id>.ini` and `logs/`.
- `sim-qvfr.ini`: global keys only.

## Phases

Each phase leaves the app working.

1. **Seams, no behaviour change.** Status: done, see below.
2. **Per-game ini and logs.** `games/<id>/` folders, `SIM_QVFR_INI` plus a global ini path, per-game log folders,
   and a one-time migration that deletes nothing.
3. **Trust and loading.** The pinned-thumbprint check in the core, approval by hash, refusal under anti-cheat, safe
   `LoadLibraryExW`, and a packaging guard.
4. **Extract the games into their own DLLs.** The SCS games first (after `lmu/mvr_probe` becomes an engine service),
   then X-Plane, ACC once it is remade, ACE, and LMU last.
5. **Dynamic tabs.** The launcher scans `mods/*/mod.json` and builds each tab from `settings.json`. It never loads a
   mod DLL itself.
6. **The SDK and its docs.** An `sdk/` folder with `mod_api.h`, `mod.hpp`, helpers that hold no global state, a
   CMake function, JSON schemas, an example identity-only mod, and a making-a-mod guide.
7. **Per-mod updates.** Release tags `mod-<id>-v<version>`, the `mods/<id>/` folder swapped whole, the old folder
   kept for rollback.

## Phase 1: what is in the code

All new code is in `src/mod_api/`:

- **`mod_api.h`**: the C ABI. Its version is `SQVFR_ABI_VERSION` 1. It has two tables:
  - `SqvfrHost` (core to mod): the game this process is (`gameId`), log, the launch's ini, the captures folder,
    hooks through the core's one MinHook, code patches, `readable`, and `request_game_exit`.
  - `SqvfrMod` (mod to core): `GameAdapter` entry for entry. Each entry takes `void* self`.

  Both tables start with `size` and `abi`. New entries are only ever appended. An entry beyond the table's size, a
  null entry, or a query answered below 0 means the core uses `GameAdapter`'s default. A table of another ABI
  version, a table too short to hold its own header, or a table for another game is refused.
- **`mod.hpp`**: the SDK's `sqvfr::Mod` base class, plus `sqvfr::mod_table`, which builds the C table from an
  instance.
- **`plugin_adapter.{h,cpp}`**: `PluginAdapter`, which presents a mod's table to the layer as a `GameAdapter`.
- **`host_services.{h,cpp}`**: the core's `SqvfrHost`.
- **`convert.h`**: converts the core's types to the ABI's structs and back. This is core only.
- **`adapter_mod.h`**: `AdapterMod`, which wraps a built-in `GameAdapter` as a mod. It is the bridge for games that
  are still built into the DLL.

The SCS games go through the table even though they are still built into the DLL:

- `src/games/scssoftware/mod_entry.cpp` is one mod for two games. It reads `SqvfrHost::gameId` ("ats" or "ets2")
  and returns that game's table, each forwarding to `scs::adapter(game)`.
- `adapter_for()` returns a `PluginAdapter` for ATS and for ETS2. If the core refuses the mod, the game falls back
  to log only.
- The other games are unchanged, ACC included.
- `tests/mod_api_test.cpp` covers:
  - the refusals and the defaults beyond a table's size;
  - a round trip of every struct;
  - the game named in the host table;
  - that ATS and ETS2 answer the same through the tables as they do directly.

**One mod, several games.** The manifest needs a `games` array instead of one identity. Each entry has its own `id`,
names, exe, Steam folder and app id, builds and defaults. `features`, `qvfrImage`, `dll`, `version` and `update`
are shared at the top level, and an entry can override them. The launcher makes one tab per entry. The layer finds
the entry by exe and passes its `id` as `SqvfrHost::gameId`. Each game keeps its own `games/<id>/` ini and logs.

## What phase 4 has to solve first

Found while sizing the boundary:

- **The SCS games** use:
  - logging (`logf`, `log_line`, `log_directory`);
  - `kDevBuild` (dev-only diagnostics);
  - `games/lmu/mvr_probe` (shader twins, the backend, and the private-data GUIDs);
  - `engine/context_hooks` (`install`, `rescan`, `uninstall`, `Entry`, the draw kinds);
  - `engine/nvapi_stereo` (`set_two_views`, `query`, `status_name`);
  - `engine/d3d11_state` (`D3D11StateGuard`) and `engine/dlss_math`;
  - `probe/aftermath` (GPU crash dumps), `probe/hang_watch` (`hang_watch_on_fire`), `probe/d3d11_capture`
    (`write_png_bgra`);
  - `quad/showcase` (`stock_wanted`, `set_stock_active`, `note_scene_frame`), `quad/d3d11_fixation` and
    `quad/corner_fit`.

  Unlike ACC they hook no game code: everything is at the D3D11 and OpenXR level. But `context_hooks`, `mvr_probe`,
  `aftermath`, the hang watch and the showcase flags all hold process-wide state. Each needs a host entry, or has to
  move into the core, before SCS can be its own DLL.
- **Shared state in inline headers** would split per DLL: `quad/showcase.h`, `quad/car_lighting.h`,
  `quad/eye_off.h` and `probe/frame_parts.h`. These need host entries before LMU, SCS or ACE can move out.
- **The SCS games are built on `games/lmu/mvr_probe`.** It has to become an engine service first.
- **ACE's API path** is called from the layer directly: `games/ace/replay_probe.h` is included in 7 layer files, and
  the layer checks `id == "ace"`. `probe/probe.cpp` is an ACE probe table. All of this has to go behind the adapter
  first.
- **X-Plane's early load** runs in `DllMain` under the loader lock. Its mod can't be loaded with `LoadLibrary` at
  that point.
