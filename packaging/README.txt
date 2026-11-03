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
- Hotkeys: a key and a wheel, button box or controller button for opening and closing the headset
  menu, moving through it (up, down, lower, raise), the performance overlay, the helmet's visor and a CPU
  capture. Click a binding, then press the keys or the wheel button (Esc cancels, Delete clears); the pane
  lists the wheels and controllers it finds. Keys work while the game window has focus, wheel buttons
  whenever the game runs; a wheel's menu buttons act only while the menu shows, so bind Open or close too.
  Changes apply to a running game at once. Defaults: Ctrl+Alt+arrows, Ctrl+Alt+P, Ctrl+Alt+V, Ctrl+Alt+C.
- Launch checks: what would stop ace-quadview at the next launch, each with a fix: a game update
  (ace-quadview supports one AssettoCorsaEVO.exe build; after an update it stays off and the game runs
  as normal, and the headset says so for the first 30 seconds), Steam not running, the game already
  running, Cheeky's dxgi.dll or version.dll loader in the game folder, and with Cheeky Foveated DLSS on:
  its runtime and OpenXR layer, OpenXR Toolkit's place above Cheeky's layer, and Toolkit's Turbo mode
  with frame generation. The bar at the bottom shows the first problem (click it for the list), and
  Launch asks before starting a game that would crash or run without ace-quadview.
- Rendering: Rendering method (Quad View Foveated Rendering, or Two-loop Foveated Interlaced: two
  game frames per headset frame; turn frame generation off) and Foveated mode (Eye tracked or Fixed).
- Resolution: Outer resolution (the full view) and Inner resolution (pixel density in the focus
  region), each a percent of the headset's recommended resolution and set independently. In
  Two-loop the inner resolution follows from the outer resolution and the foveated size.
  Field of view crop (Quad View Foveated Rendering only, off by default): Top, Bottom, Outer side
  (toward the temple) and Inner side (toward the nose), each a share of the eye's view and set
  independently. The game doesn't render the cropped edges and the headset shows black there, so
  pixels a helmet would cover aren't wasted. The outer view keeps its sharpness and the focus region
  its size. It applies live, also from the headset menu's View crop page; with DLSS on, the pixel
  savings follow at the next launch.
  Helmet overlay (Quad View Foveated Rendering only, off by default): draws a helmet's visor opening
  around the view, following the crop, lit by the scene. Rim (how far the padding reaches into the
  view), Corners and Lighting apply live. A helmet comes in ace\helmet: top.png (brow), bottom.png
  (chin bar) and visor.png, PNGs with alpha of the whole face from inside the helmet. Replace them
  with your own to change it (delete them for the drawn rim instead). They're shown as their own layer over the game, so frame generation
  isn't disturbed. Size and Height place them, Depth sets how far in front of your eyes they sit
  (0 = far away, no depth), Detail picks Performance (2048, cheaper) or Detail (4096, sharper); the Visor is Closed, Half or Open (its hotkey, Ctrl+Alt+V). The
  headset menu's Helmet page has them all. The log suggests the crop that matches their opening.
- Foveation: Foveated width and Foveated height, each a share of the outer view, and Corner radius
  (rounds the focus region's corners; 0 = square, 100% = fully round ends).
- Focus overlay: Show focus outline, its color (teal, magenta, green) and Feather (the blend band).
- Foveated DLSS: optional Cheeky Foveated DLSS (see below), its runtime and OpenXR layer status, and
  its settings (cheaper outer views, their DLAA scale and preset, the focus preset). The settings
  apply live; the headset menu's second page has them too.
- Headset menu: where the in-headset settings menu floats, and the performance overlay (on/off,
  where it floats and its size; drag either in the preview): frame rate and pacing, CPU and GPU time
  with their headroom and which one limits the frame, VRAM, frame generation and eye tracking.
  Its hotkey (Ctrl+Alt+P) turns it on and off in the game; the headset menu's Overlay and menu page moves both.
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
  focus_shape=box      box, or corners: each corner of the focus area pulled in on its own, e.g. a
                       trapezoid (top corners in) for more of the dash and less of the far field.
  shape_top_left_in=0, shape_top_left_down=0, shape_top_right_in=0, shape_top_right_down=0,
  shape_bottom_left_in=0, shape_bottom_left_up=0, shape_bottom_right_in=0, shape_bottom_right_up=0
                       with focus_shape=corners: how far each corner moves toward the middle,
                       percent of the area's width (in) or height (down, up), 0..45.
  gaze=1               focus area follows your eyes (0 = fixed in the centre).
  debug_border=1       outlines the focus area (to check where it is).
  debug_border_color=  green, magenta or teal.
[launcher]
  game_dir=            folder holding AssettoCorsaEVO.exe (default: found through Steam).
  off_layers=          OpenXR layers to turn off for the game, comma-separated,
                       e.g. XR_APILAYER_MBUCCHIA_toolkit.

Foveated DLSS (Cheeky Foveated DLSS, optional)
----------------------------------------------
The DLSS features come from Cheeky Foveated DLSS by ClarkCheekyKent, integrated with his permission.
With DLSS on in the game and the QVFR method, the outer views get a cheaper DLAA pass while the focus
views keep full DLSS. It also offers focus-view supersampling, DLSS-NR and frame generation. It is a
separate download (GNU GPL v3), not included. You need an NVIDIA RTX GPU.

Downloads:
- CheekyFoveatedDLSS-0.6.3-Standalone-ace.2.zip, from
  https://github.com/Williem3/CheekyFoveatedDLSS/releases/tag/v0.6.3-ace.2
  (Cheeky v0.6.3 with the fixes ACE's quad views need; official v0.6.3 shows the outer views shrunk).
- CheekyFoveatedDLSS-0.6.3-Standalone-TEST-SIGNED.zip, from the official release
  https://github.com/ClarkCheekyKent/CheekyFoveatedDLSS/releases/tag/v0.6.3
  (only for OpenXR\CheekyOpenXRSetup.exe, Cheeky's OpenXR layer installer).

Steps:
1. Run OpenXR\CheekyOpenXRSetup.exe from the official ZIP.
2. From the ace.2 ZIP, copy the CheekyFoveatedDLSS folder into this package's ace folder, so that
   you have ace\CheekyFoveatedDLSS\CheekyFoveatedDLSSRuntime.dll. You can also keep it elsewhere and
   pick the DLL with Choose... in the pane.
3. Don't put Cheeky's dxgi.dll or version.dll in the game folder (remove them if they are there).
   ace-quadview loads Cheeky itself.
4. If you use OpenXR Toolkit, it must be above Cheeky's layer in the OpenXR layer order (earlier in
   the list, e.g. in OpenXR API Layers GUI). Otherwise the game crashes at start.
5. In the Foveated DLSS pane, tick Use Cheeky Foveated DLSS and launch. The pane shows whether it
   finds the runtime and the layer, and after a launch Cheeky's status and version.
Frame generation (experimental, in the same pane): Cheeky's DLSS-G between the game's frames. Turn
OpenXR Toolkit's Turbo mode off (the game crashes with it), and don't run another frame generator
(such as VectorXR's) with it. Show the center region outlines the full-quality region.
With DLSS on, resolution changes apply at the next launch (DLSS sizes its views when the game starts).
The pane's other settings apply live. The headset menu's Foveated DLSS and Frame generation pages
set them too.

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

Licence: PolyForm Noncommercial 1.0.0 (LICENSE.md): free for noncommercial use; commercial use needs
the author's permission. If you share any part of it, changed or not, pass on LICENSE.md with its
line "Required Notice: Copyright (c) 2026 Williem3 (https://github.com/Williem3/Sim-QVFR)".
Third-party notices: THIRD_PARTY_NOTICES.md. Cheeky Foveated DLSS (optional, separate download) is
by ClarkCheekyKent, GNU GPL v3: https://github.com/ClarkCheekyKent/CheekyFoveatedDLSS
