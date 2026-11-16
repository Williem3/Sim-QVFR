# X-Plane 12

[Back to the README](../../README.md)

## Getting started

1. **Steam's X-Plane 12, version 12.4.3-r2.** Only the Steam version works for now; an X-Plane installed from
   Laminar's own installer runs without Sim-QVFR. On any other build, Sim-QVFR stays off and X-Plane runs as usual.
   After an X-Plane update, Sim-QVFR stays off until a release supports the new build (watch the
   [Releases page](https://github.com/Williem3/Sim-QVFR/releases)), and your headset shows a message for the first
   30 seconds.
2. **Start your headset's software** (see [Headset setup](headsets.md)).
3. **Start Steam, pick X-Plane 12 in the launcher and click Launch.** The launcher starts X-Plane in VR mode for
   you. Sim-QVFR has to be loaded before X-Plane starts, so X-Plane started from Steam or its own shortcut runs
   without it.

## What to expect

**X-Plane is often limited by its main thread** (the CPU), especially over big airports and dense scenery. Sim-QVFR
saves graphics-card work, so it helps most where the GPU holds you back: high headset resolutions, clouds and
weather. The performance overlay (Ctrl+Alt+P) shows which one is busy. To see your own gain, use **Game ›
Diagnostics › Next launch without QVFR** and fly the same flight both ways.

**Object detail** (the launcher's **Picture › Resolution**, and the headset menu) is Sim-QVFR's own X-Plane setting.
It sets how much detail trees and objects get in the sharp area. At 100%, the default, they get the detail the
sharp area needs; lower values cost X-Plane's main thread less, at the price of coarser trees where you look. Lower
it if frames stutter in dense scenery. X-Plane's own graphics settings still apply as usual.

- **Changes while you fly:** the resolution sliders and the focus area's size change straight away, in the launcher
  or the headset menu.
- **Sharpening:** X-Plane sharpens the picture itself too, so start low.
- **Not in X-Plane:** the view crop, the helmet overlay and Foveated DLSS.
- **Hotkeys:** joystick and throttle buttons can open the headset menu (**Headset › Hotkeys**).
- **Online flying:** see [Online play and anti-cheat](online-play.md).

How Sim-QVFR works in X-Plane: [How it works](how-it-works.md#x-plane-12).
