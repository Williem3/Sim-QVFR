# Headset setup

Sim-QVFR works with any PC VR headset whose software supports **OpenXR**, the standard way games talk to VR
headsets today. You don't install anything extra for Sim-QVFR: start your headset's software as you normally
would, then start the game from Sim-QVFR's launcher.

The one thing to know: your PC has one **active OpenXR runtime**, the program games send their VR picture to. It's
either your headset's own software or SteamVR. Both work with Sim-QVFR. Your headset's own software usually runs a
little lighter; SteamVR is the one to use if you need it for base-station tracking or trackers.

[Back to the README](../../README.md)

## Contents

- [Virtual Desktop](#virtual-desktop-quest-pico) (Quest, Pico)
- [Quest Link and Air Link](#quest-link-and-air-link)
- [Pimax and Varjo](#pimax-and-varjo)
- [SteamVR headsets](#steamvr-headsets) (Valve Index, Bigscreen, Reverb and others)
- [Eye tracking](#eye-tracking)
- [Using other VR tools alongside](#using-other-vr-tools-alongside)

## Virtual Desktop (Quest, Pico)

Quest 2, Quest 3 and Pico 4 have no eye tracking: the sharp area stays in the middle of your view. Quest Pro has eye
tracking (see [Eye tracking](#eye-tracking)).

1. Start the Virtual Desktop Streamer on the PC and connect from the headset.
2. In the Streamer's settings, set the **OpenXR runtime** to **VDXR** (Virtual Desktop's own). SteamVR works too,
   but VDXR skips it and usually runs lighter.
3. Open Sim-QVFR's launcher and click **Launch**. You can do this from inside the headset, in Virtual Desktop's view
   of your PC's desktop: the game then opens in VR.

## Quest Link and Air Link

1. In the Meta Quest Link app on the PC, set Meta Quest Link as the active OpenXR runtime (**Settings › General**).
2. Connect the headset, open Sim-QVFR's launcher and click **Launch**. You don't need SteamVR.

## Pimax and Varjo

Start Pimax Play or Varjo Base, then launch from Sim-QVFR. Eye tracking on the Pimax Crystal and Varjo headsets is
used when it's turned on in their software.

In every game, Sim-QVFR puts the picture together itself, as on any headset, except in Le Mans Ultimate:

**Runtime composition** (Le Mans Ultimate only, automatic, nothing to set): Pimax's and Varjo's software puts the
sharp area on the display itself, at its full resolution. Sim-QVFR hands it its views instead of putting the picture
together itself, which saves the memory and GPU time of that step.

- Tested on Pimax's software; Varjo's should work the same way but hasn't been tried yet (reports welcome).
- **Keep Pimax Play's or Varjo Base's own foveated area at least as large as Sim-QVFR's focus area.** The headset
  only shows the sharp area inside its own region. **Game › Diagnostics** in the launcher tells you when it's
  too small.
- The headset's software places the sharp area here, so Sim-QVFR's gaze dead-zone and its position sliders
  (**Vertical position**, **Horizontal position**, **Gaze offset**, **Horizontal gaze offset**) don't apply. **To use
  them, turn Quad-Views Foveated Rendering off in Pimax Play or Varjo Base:** Sim-QVFR then puts the picture together
  itself. The headset menu leaves these sliders out while the headset's software composes; the launcher leaves them
  out after a session in which it did. The custom shape and Feather apply either way.
- With frame generation (VectorXR, Cheeky's) or another OpenXR tool in between, Sim-QVFR puts the picture together
  itself as usual. A report from **Game › Diagnostics** says what the last launch did.

## SteamVR headsets

Make SteamVR the active OpenXR runtime (SteamVR **Settings › OpenXR**, **Set SteamVR as OpenXR runtime**), start
SteamVR, then launch from Sim-QVFR.

## Eye tracking

With eye tracking the sharp area follows your eyes, so it can be smaller and you save more. Sim-QVFR uses it when
your headset's software passes it on to games (for Quest Pro through Virtual Desktop, turn on eye-tracking
forwarding in Virtual Desktop's settings). No other tool is needed.

In the launcher, **Picture › Foveation** sets whether the sharp area follows your eyes (**Eye tracked**) or stays
in the middle (**Fixed**). With **Fixed**, make it larger, since your eyes move off it.

## Using other VR tools alongside

- **OpenXR Toolkit, VectorXR and similar tools** can run alongside. The launcher's **Game › OpenXR layers** lists
  them and **Launch checks** warns about an order that won't work. Use one frame generator at a time.
- **Don't run another quad-views or foveated-rendering mod at the same time** (Quad-Views-Foveated,
  LMU-Quad-View-Mod, a game's own foveated rendering). Sim-QVFR does that job.
