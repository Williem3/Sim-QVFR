ace-quadview for Assetto Corsa EVO
==================================

Quad views with eye-tracked foveated rendering for Assetto Corsa EVO in VR: the game renders a
lower-resolution full view plus a sharp focus view where you look, and ace-quadview blends them
into the headset image. Nothing is installed: the launcher starts the game with ace-quadview for
that launch only.

OFFLINE ONLY. ace-quadview changes how the game renders by patching it in memory. Do not use it
in online sessions.

Requirements
------------
- Assetto Corsa EVO (Steam), a PC VR headset with an OpenXR runtime, Steam running.
- Eye tracking (XR_EXT_eye_gaze_interaction) for the focus area to follow your eyes; without it
  the focus area stays in the centre.
- Turn the game's own foveated rendering off; ace-quadview replaces it.

Start
-----
1. Unzip anywhere (a folder you can write to; logs go to logs\ next to the launcher).
2. Start Steam and your VR runtime.
3. Run ace-quadview-launcher.exe. Its window holds the settings below; Launch ACE saves them and
   starts the game in VR. If it cannot find the game, set game_dir in the [launcher] section of
   ace-quadview.ini.

The window
----------
Settings save as you change them and apply at the next launch.
- Rendering: Rendering method (Quad View Foveated Rendering, or Two-loop Foveated Interlaced: two
  game frames per headset frame; turn frame generation off) and Foveated mode (Eye tracked or Fixed).
- Resolution: Outer resolution (the full view) and Inner resolution (pixel density in the focus
  region), each a percent of the headset's recommended resolution and set independently. In
  Two-loop the inner resolution follows from the outer resolution and the foveated size.
- Foveation: Foveated width and Foveated height, each a share of the outer view, and Corner radius
  (rounds the focus region's corners; 0 = square, 100% = fully round ends).
- Focus overlay: Show focus outline, its color (teal, magenta, green) and Feather (the blend band).
- The preview shows the focus region and the resulting view sizes. Pixel sizes appear after the
  first launch, once the headset has reported its recommended resolution (runtime.ini).
- Restore Defaults returns to the kit's defaults. Return launches the game.

Settings (ace-quadview.ini, read when the game starts; the window writes them)
-------------------------------------------------------------------------------
[experiment]
  periphery_scale=40   resolution of the outer (full) view, percent of the headset's native.
  focus_density=100    sharpness of the focus area, percent of native pixel density.
  focus_percent=31     width of the focus area, percent of the field of view.
  focus_height_percent=31  its height (default: the same as the width).
  The three are independent. Unset, periphery and focus density follow eye_scale
  (periphery = eye_scale, focus density = eye_scale / focus_percent).
  periphery_scale + focus_density x focus_percent must stay under about 1.0 on a ~4000-pixel-wide
  headset (the runtime's 8192-pixel limit).
[quad]
  feather_percent=20   blend band at the edge of the focus area.
  corner_radius_percent=0  rounds its corners (percent of half the shorter side).
  gaze=1               focus area follows your eyes (0 = fixed in the centre).
  debug_border=1       outlines the focus area (to check where it is).
  debug_border_color=  green, magenta or teal.
[launcher]
  game_dir=            folder holding AssettoCorsaEVO.exe (default: found through Steam).
  off_layers=          OpenXR layers to turn off for the game, comma-separated,
                       e.g. XR_APILAYER_MBUCCHIA_toolkit.

Other OpenXR layers (OpenXR Toolkit, VectorXR, ...)
---------------------------------------------------
The launcher loads ace-quadview nearest the game and moves every other installed OpenXR layer
below it for that launch, so tools like OpenXR Toolkit cannot change what the game renders behind
its back. Turn OpenXR Toolkit's upscaling and foveated rendering off for this game (ace-quadview
does the foveation); its other features are fine. VectorXR frame generation works below it.

Problems
--------
Send the newest logs\ace-*.log and the matching logs\run-*\launch.txt.

ace-quadview-launcher.exe --dry-run shows what a launch would do without starting the game.

Removing it: delete the folder. Nothing else was changed on your PC.

Licence: PolyForm Noncommercial 1.0.0 (LICENSE.md). Third-party notices: THIRD_PARTY_NOTICES.md.
