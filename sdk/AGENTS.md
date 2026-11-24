# AGENTS.md: building a Sim-QVFR game mod

This file is for coding agents (Codex, Cursor, Copilot, Gemini, Claude Code and others) and for the people who drive
them. It is the one source of truth for how a Sim-QVFR mod should be built. `CLAUDE.md` only points here. Read all of
it before writing code.

## 1. What Sim-QVFR is for

Sim-QVFR adds **Quad View Foveated Rendering (QVFR)** to VR games. Each eye's picture is drawn as two views:

- a **periphery** (the wide view): the whole field of view at a lower resolution, and
- a **focus view**: a small area at full or higher sharpness, in the middle of the view or wherever eye tracking says
  the player looks.

Sim-QVFR then blends each eye's two views into one picture and hands it to the headset's software.

**The goal:** give the player more GPU frame time with no loss they can see. Every feature below serves that goal, and
a mod is judged by it. A mod that saves frames but makes the picture shimmer, swim or double up has failed.

Sim-QVFR has two parts:

- the **core**: an OpenXR API layer (`sim-qvfr.dll`) plus the launcher;
- **game mods**: one DLL per game (`mods/<id>/sim-qvfr-<id>.dll`) that tell the core how that game draws.

The core owns everything that isn't game-specific. A mod supplies only what only the game knows.

## 2. The integration rule: work at the API layer, don't hack the game

**Build your mod on the graphics API and OpenXR, not on the game's own code.** In order of preference:

1. **OpenXR only.** Ask for four views (or two plus multi-view), locate them, and compose. This works when the game's
   renderer already draws any number of views it is given.
2. **The D3D11 (or D3D12) API level.** Watch and adjust the game's own API calls: render targets, viewports,
   constant buffers and draws. Use the core's shared services for this: `d3d11_hooks_*` (one set of context hooks for
   the whole process), `multi_view_shaders_*` (every vertex shader gets a twin that draws the focus view in the same
   pass) and `nvapi_*` (NVIDIA multi-view). Each eye's pass then also draws its focus view, so the game's CPU cost
   stays the same.
3. **The game's own code** (`code_*`, `vtable_slot_*`, `hook` on game functions): **only when 1 and 2 can't work,**
   and only after the maintainer agrees.

**Why this order:**

- **Game updates.** An API-level mod keeps working when the game updates. A code patch breaks on every update, and the
  core then turns QVFR off for that build.
- **Online play and anti-cheat.** Patching or hooking a game's code is what anti-cheat looks for. API-level work
  touches only what any overlay or capture tool touches. Players get banned for this, so treat it seriously.
- **Relations with game makers.** Sim-QVFR stays welcome by not taking games apart.
- **One renderer for everyone.** Features in section 3 work in every game once the mod reports where the views are.

**Never:**

- write to the game's memory or exe without a hash check (`code_matches` before `code_write`);
- inject into other processes, or load code from outside your mod's folder;
- disable, bypass or hide from an anti-cheat;
- put game-specific reverse-engineering details in public files (addresses, offsets, shader hashes, anti-cheat
  internals). Keep them out of the repository, out of logs and out of issues.

**D3D12 games:** the core's compositor and its DLSS-NR and FSR backends already run on D3D12, but the per-draw services
above are D3D11 only for now. For a D3D12 game, raise it with the maintainer before you start.

## 3. Features the core provides, and what your mod must supply

Each feature lights up only when the mod gives the core what the feature needs. The names below are `sqvfr::Mod`
methods (`mod.hpp`). Each one mirrors an entry of the C table in `mod_api.h`, and the comments there are the full
contract. Any method you don't override answers "the core's default".

| Feature | What the player gets | What your mod supplies |
| --- | --- | --- |
| **QVFR itself** | Four views drawn, two composed | `has_qvfr`, `qvfr_start`, then a frame each time: `qvfr_take_frame` / `qvfr_latest_frame` (where each view sits), `qvfr_note_views`, `qvfr_frame_end`. Report multi-view with `qvfr_multi_view`. |
| **Live periphery and focus resolution** | Resolution changes while driving, from the headset menu or Auto-fit | Answer `qvfr_live_resolution` with 1, then apply `qvfr_set_live_layout` each time it's called. The game keeps the size it launched with; you draw into the part the new size needs. Only lowering works live; raising above the launch size waits for the next start. |
| **Focus area size and position** | The sharp box's size and offset, changed live | Apply `qvfr_set_live` (the focus box's share of the eye, width and height). |
| **Eye tracking** | The focus box follows the player's eyes | Answer `wants_gaze` with 1 and aim the focus views from `qvfr_set_gaze`. The core reads the tracker, applies the dead-zone, and makes both eyes' boxes meet where the eyes converge. |
| **Foveated DLAA / DLSS (and FSR, DLSS-NR)** | DLAA on the periphery, DLAA or upscaling on the focus views | Your views need what the upscaler needs: colour, depth and motion vectors per view, and sub-pixel jitter. Apply `qvfr_jitter_views` to the cameras you draw with, and `qvfr_set_dlss` / `qvfr_set_nr` for the settings. `qvfr_target_sizes` reports render and output sizes. |
| **Focus mask and corner radius** | The focus views skip pixels outside the focus shape (a rounded box, or a free-form outline) | Apply `qvfr_set_focus_mask` (the focus area and a margin): keep the focus views from rendering outside the area, for example with a depth mask drawn before the scene so its draws skip those pixels (early depth). Stay a margin outside the shape, so nothing that is shown, or that DLSS, sharpening or ambient occlusion read next to it, is lost. The rounding and the feather come with `qvfr_set_look`. |
| **View crop** | The game stops drawing the edges a helmet would hide | Nothing extra when the game renders the field of view OpenXR gives it: the core narrows it. A game with fixed cameras has to apply the layout's `cropWidth` / `cropHeight`. |
| **Helmet overlay** (racing-style games) | A visor round the view, lit by the scene | Nothing: the core draws it over the composed eyes. It is offered for games whose Sim-QVFR entry lists it; ask the maintainer to turn it on for yours. |
| **CAS sharpening, feather, colour correction** | A crisper focus area, a soft blend at its edge, exposure, contrast and saturation | Nothing when the core composes (the default). If your game composes the views itself (`qvfr_composable` 0), these are yours to match. |
| **Headset menu, performance overlay, launcher tab** | Every core setting, live, in the headset; frame times | Nothing for the core's settings. Your own settings go in `settings.json` (section 5). |
| **Hang watch, logs, captures** | A dump when the game hangs; one log per game | `hang_watch_s` (seconds of no frames before it fires) and `host->log`. |

The less your mod does, the more of this works without extra code. The order to aim for:

1. a mod that identifies the game and leaves it alone (the example);
2. QVFR at a fixed size;
3. live resolution and eye tracking;
4. the focus mask;
5. the upscalers.

## 4. The ABI rules

- **C only across the boundary.** `mod_api.h` is plain C. `mod.hpp` is a header-only C++ wrapper that builds the C
  table from a class. Don't pass C++ types, STL containers or exceptions through the table.
- **One export:** `sqvfr_mod_entry(const SqvfrHost* host)`. Check `host->abi == SQVFR_ABI_VERSION`. Pick the game
  from `host->gameId` and return a table whose `gameId` is that game. Return null to refuse; log why first.
- **Append-only.** Both tables start with their own `size`. Read a host entry only when `host->size` covers it
  (`mod.hpp`'s helpers do this). The core does the same with yours. A breaking change is a new `SQVFR_ABI_VERSION`,
  and the core refuses a table of another version.
- **ABI 1 is not frozen yet** while the mod manager is on the nightly channel. Expect appends.
- **Threads:** table entries are called on the game's threads (frame end, the render thread). Don't block in them.
  Anything that observes D3D11 calls runs on the game's thread before its call goes on, and must not call the hooked
  functions itself.
- **Static C runtime.** A mod is loaded with only its own folder and System32 searched for DLLs, so link the runtime
  in. `sqvfr_add_mod` does this for you.

## 5. Your mod's settings

List your settings in `settings.json` (schema: `schemas/settings.schema.json`). It allows up to 16 settings, each a
toggle or a choice of 2 to 4 options. They show on the Game tab's **Mod settings** card for the games in your
`mod.json`, and are saved in `[mod_<id>]` of `sim-qvfr.ini`.

Read them in the mod with `sqvfr::setting_on(host, "key", default)` and
`sqvfr::setting_choice(host, "key", "default")`. A mod sees only its own section. You get your default when the player
never changed a setting, or when the core is too old to have this service. Settings are read when the game starts, so
a change applies at the next launch.

## 6. Packaging: what "Add a mod" accepts

A player adds a mod by dropping its zip on the launcher, or with **Settings > Mods > Add a mod…**. The zip must hold:

- `mod.json` (schema: `schemas/mod.schema.json`), with an `id` of a-z, 0-9, `_` and `-` (up to 32 characters);
- the DLL that `mod.json`'s `dll` names;
- optionally `settings.json`, your assets, and a `SHA256.txt` (lines of `<sha256>  <file>`; when present, every file
  must match it).

Put the files at the zip's root, or in one folder. Use one mod per zip, under 64 MB. `sqvfr_add_mod`'s
`sqvfr_mod_zip_<name>` target makes this zip for you.

## 7. Trust: what players see

- A mod not made by Sim-QVFR runs **only after the player approves** that exact file in Settings > Mods. A rebuilt
  DLL needs approving again.
- It runs **in an online, anti-cheat-protected launch** only when the player also turns on **Allow online** for it,
  **and** the DLL carries a valid Authenticode signature of **your own**. Unsigned mods never run online.
- Sim-QVFR's own signature never covers your mod. Logs and reports label it "third-party".

## 8. Building and testing

- Build the example first (`examples/hello`, see `README.md`), add its zip through the launcher, approve it, and check
  the game's log for its line. That proves your tool chain, packaging and trust steps before any rendering work.
- Then grow it one feature at a time (section 3's order). After each step, launch with **Game › Diagnostics › Next
  launch without QVFR** and compare: frames gained, and nothing visibly worse.
- Turn on **Detailed logs** for `SQVFR_LOG_DEBUG` lines. Check `host->log_debug_on()` before building costly ones.
- Keep a mod's own tests runnable without the game: pure maths in headers, tested on their own.

## 9. When working on this as an agent

- Follow sections 2 and 4 strictly. If a feature seems to need a code patch, stop and say so; don't write one.
- Keep public files free of game internals (section 2, "Never").
- Change one thing at a time, and say what the player should see change.
- Don't edit `mod_api.h` or `mod.hpp` in a mod. They are the core's contract: a change there is a change to
  Sim-QVFR, made in its repository.
