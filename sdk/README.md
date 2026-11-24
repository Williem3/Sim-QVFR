# Sim-QVFR mod SDK

Everything you need to build a game mod for Sim-QVFR: one DLL per game that tells Sim-QVFR's core how that game draws,
so the core can give it quad-view foveated rendering and everything built on it.

**Start with [AGENTS.md](AGENTS.md).** It explains what a mod is for, the rule for how it hooks into a game, and what
to fill in for each feature. It is written for coding agents and people alike.

> The mod manager is on Sim-QVFR's **nightly** channel. The ABI (version 1) can still grow, so rebuild your mod when
> a nightly says it changed.

## What's here

| Path | What it is |
| --- | --- |
| `AGENTS.md` | The rules and the feature guide (`CLAUDE.md` points to it) |
| `include/mod_api.h` | The C contract between the core and a mod |
| `include/mod.hpp` | A header-only C++ wrapper: derive from `sqvfr::Mod`, override what your game does |
| `cmake/SqvfrMod.cmake` | `sqvfr_add_mod()`: builds the DLL and the zip players add |
| `schemas/` | JSON schemas for `mod.json` and `settings.json` |
| `examples/hello/` | The smallest mod: it identifies the game, reads its settings and logs a line |

In the Sim-QVFR repository the headers are in `src/mod_api/`. The packaged SDK copies them to `include/`.

## Making a mod, step by step

You need Windows, Visual Studio 2022 (the "Desktop development with C++" workload) and CMake 3.21 or newer.

1. **Copy `examples/hello`** to a folder of your own and rename it. In `mod.json`, set your own `id`, `name`,
   `author`, `version`, `dll` (`sim-qvfr-<id>.dll`) and `games`. In `CMakeLists.txt`, point `include(...)` at this
   SDK's `cmake/SqvfrMod.cmake`.
2. **Build it and make the zip:**
   ```
   cmake -S . -B build -G "Visual Studio 17 2022" -A x64
   cmake --build build --config Release --target sqvfr_mod_zip_hello
   ```
   This writes `build/sqvfr-mod-<id>-<version>.zip`.
3. **Add it.** Drop the zip on the Sim-QVFR launcher window, or use **Settings > Mods > Add a mod…**. Confirm.
4. **Approve it.** In **Settings > Mods**, press **Approve** on your mod. Approval covers this exact file, so each
   rebuild needs approving again.
5. **Run the game** from the launcher, then open the game's log (**Game › Get help › Open the logs folder**). The
   example writes `hello mod: running in ats, feeling calm`.
6. **Change a setting** on the Game tab's **Mod settings** card, launch again, and watch the log line change.

From there, climb AGENTS.md's ladder (section 4) one rung at a time. For a new game, add its entry to `mod.json`
(AGENTS.md section 7): the launcher then lists it.

## Shipping it

- Hand players the zip. They add it the same way you did.
- To let players run it in online, anti-cheat-protected sessions, sign the DLL with **your own** code-signing
  certificate (Authenticode). Players must also turn on **Allow online** for it. Unsigned mods never run online.
- Optional: put a `SHA256.txt` in the zip (`<sha256>  <file>` per line), so a damaged download is refused.
