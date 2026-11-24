# Le Mans Ultimate: enhancements backlog

Ideas for LMU's QVFR path that aren't bugs: the image is right without them. The engine details behind them are in the
local recon notes (`docs/recon-lmu.md`, not in the repository).

## Survive game updates: learn the game's values from the API instead of its memory

**Done (2026-11-24).** Every value LMU's path needs (offered eye size, render scale, FSR on or off, eye projections,
the eye a pass draws, DLSS's and night car lighting's camera) comes from what the game hands OpenXR and Direct3D 11,
on every build. Nothing is read in LMU's memory and `src/games/lmu/abi.h` is gone: the exe's SHA-256 only tells the
launcher whether a build was tested. The real proof is still the first LMU update.

