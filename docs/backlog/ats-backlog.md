# American Truck Simulator: backlog

Open items in ATS's QVFR path (the same code serves Euro Truck Simulator 2). The image is right without them unless an
item says otherwise. The measurements behind them are in the local recon notes (`docs/recon-ats.md`, not in the
repository).

## Bug: violet ground markers flash in one eye (Euro Truck Simulator 2)

**Symptom.** In a company yard, glowing violet outlines (like the game's parking-spot markers) pop in for single frames,
in one eye only (2026-11-15, ETS2 at 100% Scaling, RTX 5070 Ti, Pimax runtime). One burst caught it: right eye, a frame
drawing the left eye first, in that eye's own picture and its focus view alike; the left eye and the next frame clean.
The markers aren't in the G-buffer: a forward or emissive draw after lighting puts them there.

**What is known.**
- Sim-QVFR's: with QVFR off (`scs_qvfr=0`) none in about 2,600 frames.
- Not the focus view: they stay with Ctrl+Alt+G (focus view off), with NVIDIA's multi-view rendering and with instancing.
- Not the rewrite of constants by value: the mirror pass and the per-draw rule took only the eye's target sizes in
  10,800 frames (zero pairs of other sizes), and a strict size test (a diagnostic, since removed) didn't stop them.

**Suspect.** The mirror copies going stale: the scene draws read our copy of the game's large constant buffers. Writes
from another thread now make the next bind copy again (a per-buffer generation, since the code review of 2026-11-15),
and the summary counts them. An ATS run with the counter (2026-11-15 06:04) had 8,629 writes from other threads, none
while the eyes were in the corner, which speaks against this suspect. ETS2 at the yard not run since.

**Probably fixed (2026-11-15 06:36).** The first ETS2 run with the copies refreshed after other threads' writes: no
flashes seen at the yard (the user, about 8,000 frames, 8,492 such writes). The flash was rare (about 1 frame in 100
recorded), so one run is good evidence, not proof.

**Next step.** Watch for it over the next sessions of driving. If it comes back: the dev build's Ctrl+Alt+N (mirrors
off) against on, then the viewport and scissor cuts. Once a few more sessions are clean, remove Ctrl+Alt+N and the
other-thread counter, and close this item.

## Not yet run on an AMD or Intel GPU

Sim-QVFR's own multi-view rendering (instancing, the overlay's SMVR) has only run on an NVIDIA RTX card with instancing
selected, where it matched NVIDIA's multi-view rendering in picture and frame rate (2026-11-14). The drivers it is meant
for have not run it.

**Next step.** One session's log from an AMD or Intel machine: the line naming the way picked at session start, the
count of draws doubled, the count of draws without a twin, and the frame rate.

## A freeze inside the runtime, twice

**Symptom.** The game stopped responding mid-drive, twice in one evening on a Pimax runtime (2026-11-14), both after
the performance overlay and the headset menu were switched on for ATS. Dozens of runs before that never froze.

**What is known.** The second freeze left a dump: the render thread was inside frame submit, in the overlay card's
update, inside the runtime and the GPU driver; the game's frame-wait thread was waiting on a runtime lock behind it.
Which of the overlay's runtime calls it was is not known.

**Third freeze (2026-11-15 06:57, ATS, Pimax).** The hang watch named the stuck call: the runtime's xrEndFrame. The
dump has the render thread in our xrEndFrame, then Pimax's, then its client library, spinning inside d3d11.dll; no other
thread was in our code. Just before, the game ended and started its OpenXR session three times in 13 s (on its main
thread, while its render thread kept drawing). Each start ran our install there, and the install issues commands on the
game's immediate context (a Flush, one of ours, multithread protection switched on and off), which isn't thread-safe;
each end released what the render thread's hooks were using. Both now wait for the render thread's next xrEndFrame
(service_lifecycle), with a test of the restart sequence. Whether that was the freeze's trigger isn't known: the two
earlier freezes had no such restarts on record.

**Next step.** Repeat what led to the third freeze (a menu, a graphics setting applied: the session restarts) on the
current build, on the Pimax runtime. A freeze again with the same stack: the Pimax runtime's own; then a long ATS
session on SteamVR, and a log of the D3D11 calls the runtime makes through our hooks inside xrEndFrame.

## Indirect draws are not counted

Instancing doubles a draw's instance count, which an indirect draw keeps in a buffer. An indirect draw in a scene pass
would reach the eye and be missing from the focus view. One captured frame had none; nothing counts them at run time.

**Next step.** Count indirect draws in the eye passes and log them with the instancing summary.

## Anti-aliasing jitter differs between the eye and its focus view

The game's temporal anti-aliasing jitters by a fraction of a pixel of the target. The focus view's pixels are a
different size on screen than the eye's, so the same jitter moves the two views by different amounts. Nobody has seen an
effect of it; TAA and SMAA both looked right in the headset.

**Next step.** Compare a still scene's focus box edge with temporal anti-aliasing on and off before spending time here.

## The desktop mirror shows the working layout

The game's desktop window shows each eye in the corner of its image with the focus region beside it, since the mirror
is copied before the layer puts the picture together for the headset.

**Next step.** Find where the mirror takes its picture from and whether it can be given the composed one. Cosmetic.

## The menu's "in effect" notes follow another path's rule

The headset menu notes what a size is held to when it asks for more than fits. For ATS those notes still follow the
rule of the four-view path. The sizes themselves apply correctly (the focus region keeps what it asks for, the
periphery takes the room left).

**Next step.** Give the notes this path's rule: periphery at most 100% minus the focus box's width times its density.

## Other ambient occlusion and anti-aliasing settings

Checked in the headset: TAA and SMAA, with the game's ambient occlusion on. The focus view takes its ambient occlusion
from the eye, so other occlusion settings should hold, but they have not been looked at. Scaling above 100% multiplies
the eye's size and was only run at 100%.

## The game's Scaling must be 100%

With Scaling at another value (seen in Euro Truck Simulator 2 at 75%, 2026-11-15) the game draws each eye smaller or
larger than the headset's image and resamples at the end. The path doesn't handle that step: it showed a low-resolution
picture with the focus regions blinking. It now stays off while the sizes differ and says so in the log.

**Next step.** Either handle the resampling blit (cut its source to the corner, replay it for the focus region), or
tell the player in the headset why QVFR is off. Scaling below 100% is what the periphery setting already does better.

## Cheeky's layer is loaded for a game without Foveated DLSS

The launcher puts Cheeky's OpenXR layer into ATS's layer chain, set to pass everything through. ATS has no Foveated
DLSS, so it should not be loaded at all. It sat between Sim-QVFR and the runtime in the freeze's dump.

**Next step.** Leave it out of the chain for a game without the feature.
