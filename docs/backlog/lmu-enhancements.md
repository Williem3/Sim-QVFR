# Le Mans Ultimate: enhancements backlog

Ideas for LMU's QVFR path that aren't bugs: the image is right without them. The engine details behind them are in the
local recon notes (`docs/recon-lmu.md`, not in the repository).

## Survive game updates: learn the game's values from the API instead of its memory

**Idea.** Take every value Sim-QVFR still reads from LMU's memory from what the game hands OpenXR and Direct3D 11
instead, so a game update that moves LMU's data doesn't turn QVFR off.

**Why.** Since 2026-11-14 LMU's path writes nothing into the game and calls none of its code, but it still reads a few
values at fixed places in LMU's memory. Those places can move with any update, so the layer only starts on the one exe
build Sim-QVFR was made for (its SHA-256): after an update QVFR stays off until a Sim-QVFR release, and testers can't
try the new build. With no memory reads left, the build check could become a warning, and a routine update (a
recompile, new content, physics) would most likely keep working.

**The reads, and where each could come from.**

| Read today | Learn it from | Effort |
|---|---|---|
| Recommended eye size | OpenXR already gives it (`xrEnumerateViewConfigurationViews`); LMU only stores the runtime's value | Trivial |
| Render scale | The main targets are the recommended size x the scale, and the pass tracker sees their size; a live change shows as new targets | Easy |
| Which eye a pass draws | Pass order, or which eye's field of view the pass's projection fits | Easy |
| Eye projections | Found once in the scene pass's constant buffers as the 4x4 that fits OpenXR's fields of view, then kept by buffer slot and offset (the post census already dumps these buffers) | Medium |
| FSR on or off | The eye renders smaller than its output, and FSR's upscale runs | Medium |
| View matrix for DLSS | Beside the projection in the same constant buffer: a rotation and translation, confirmed by its rotation following the head | Hardest |

**Self-correcting.** Each learned place is checked against the truth (does the matrix there still fit OpenXR's fields of
view?). When it stops fitting, after an update or a settings change, it is learned again; meanwhile the eyes render
alone at full resolution, the fallback that exists today.

**Not this way.** Scanning LMU's code for byte patterns that refer to each value: it still depends on the code's layout,
still reads the game's memory, and would mean shipping a memory scanner.

**What stays fragile.** The API route depends on how LMU renders (its pass order, matrices in constant buffers, YEBIS
sizing itself from the viewport), not on where its data sits. A patch that reworks the renderer could still need a fix.

**Order.**
1. Recommended size, render scale, eye of pass and FSR: four of the five addresses gone, QVFR likely on after an update.
2. The eye projections.
3. The DLSS view matrix (until then DLSS waits on a known build).

**Measure.** One live run per step: the learned value logged next to the memory read until they agree, then the read
removed. The real proof is the first LMU update.

**Interim option.** An opt-in launcher toggle to run on an untested build, with each read checked against OpenXR (the
recommended size, the projections' fields of view, a sane render scale, the renderer inside LMU's image) through
guarded reads; on a failed check QVFR stays off and the log names the read.
