Sim-QVFR for Assetto Corsa EVO
==================================

Quad views with eye-tracked foveated rendering for Assetto Corsa EVO in VR: the game renders a
lower-resolution full view plus a sharp focus view where you look, and Sim-QVFR blends them
into the headset image. Nothing is installed: the launcher starts the game with Sim-QVFR for
that launch only.

OFFLINE ONLY. Sim-QVFR changes how the game renders by patching it in memory. Do not use it
in online sessions.

Requirements
------------
- Assetto Corsa EVO (Steam), a PC VR headset with an OpenXR runtime, Steam running.
- Eye tracking (XR_EXT_eye_gaze_interaction) for the focus area to follow your eyes; without it
  the focus area stays in the centre.
- Turn the game's own foveated rendering off; Sim-QVFR replaces it.

Start
-----
1. Unzip anywhere (a folder you can write to; logs go to logs\ next to the launcher).
2. Start Steam and your VR runtime.
3. Run sim-qvfr-launcher.exe. Its window holds the settings below; Launch ACE saves them and
   starts the game in VR. If it cannot find the game, check that Assetto Corsa EVO is installed
   through Steam and Steam is running, or use Report an issue.

The window
----------
Settings save as you change them and apply at the next launch; while the game runs, most apply at once.
An (i) after a setting's name shows, under the pointer, what it does and what it costs.
- Profiles: the presets Performance, Balanced and Quality (they set the resolution only: the outer
  views, the focus density and the focus area's size, with low outer views and a dense centre; one set for
  eye tracked foveation, with a small area that follows your eyes, and one for fixed, with a larger area)
  and your own profiles, which hold every setting of
  the picture (resolution, foveation, the focus overlay, the view crop and helmet, Foveated DLSS) but not
  the menu's and overlay's places, the hotkeys or the OpenXR layers. Use one, save the settings you have as
  one (in the profiles folder next to the launcher), Share one as a line of text to paste in a chat, or
  Paste one someone shared. The headset menu's first line, Profile, switches between them while you drive.
- Hotkeys: a key and a wheel, button box or controller button for opening and closing the headset
  menu, moving through it (up, down, lower, raise), the performance overlay, the helmet's visor and a CPU
  capture. Click a binding, then press the keys or the wheel button (Esc cancels, Delete clears); the pane
  lists the wheels and controllers it finds. Keys work while the game window has focus, wheel buttons
  whenever the game runs; a wheel's menu buttons act only while the menu shows, so bind Open or close too.
  Changes apply to a running game at once. Defaults: Ctrl+Alt+arrows, Ctrl+Alt+P, Ctrl+Alt+V, Ctrl+Alt+C.
- Launch checks: what would stop Sim-QVFR at the next launch, each with a fix: a game update
  (Sim-QVFR supports one AssettoCorsaEVO.exe build; after an update it stays off and the game runs
  as normal, and the headset says so for the first 30 seconds), Steam not running, the game already
  running, Cheeky's dxgi.dll or version.dll loader in the game folder, and with Cheeky Foveated DLSS on:
  its runtime and OpenXR layer, OpenXR Toolkit's place above Cheeky's layer, and Toolkit's Turbo mode
  with frame generation. The bar at the bottom shows the first problem (click it for the list), and
  Launch asks before starting a game that would crash or run without Sim-QVFR.
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
  view), Corners and Lighting apply live. A helmet comes in layer\helmet: top.png (brow), bottom.png
  (chin bar) and visor.png, PNGs with alpha of the whole face from inside the helmet. Replace them
  with your own to change it (delete them for the drawn rim instead). They're shown as their own layer over the game, so frame generation
  isn't disturbed. Size and Height place them, Depth sets how far in front of your eyes they sit
  (0 = far away, no depth), Detail picks Performance (2048, cheaper) or Detail (4096, sharper); the Visor is Closed, Half or Open (its hotkey, Ctrl+Alt+V). The
  headset menu's Helmet page has them all. The log suggests the crop that matches their opening.
- Foveation: Foveated width and Foveated height, each a share of the outer view, and Corner radius
  (rounds the focus region's corners; 0 = square, 100% = fully round ends). Vertical position (fixed) or
  Gaze offset (eye tracked) moves the region up or down; Gaze dead-zone is how far your eyes move before
  it follows (it keeps still against eye-tracker noise). Focus shape: Box, or Corners, each corner pulled
  in on its own (a trapezoid, say, for more of the dash and less of the sky).
- Focus overlay: Show focus outline, its color (teal, magenta, green) and Feather (the blend band).
- Foveated DLSS: optional Cheeky Foveated DLSS (see below), its runtime and OpenXR layer status, and
  its settings (cheaper outer views, their DLAA scale and preset, the focus preset). The settings
  apply live; the headset menu's second page has them too.
- Headset menu: where the in-headset settings menu floats, and the performance overlay (on/off,
  where it floats and its size; drag either in the preview): frame rate and pacing, CPU and GPU time
  with their headroom and which one limits the frame, VRAM, frame generation and eye tracking.
  Its hotkey (Ctrl+Alt+P) turns it on and off in the game; the headset menu's Overlay and menu page moves both.
  Show the hotkeys under the menu (on by default) lists the in-game hotkeys and their wheel buttons in a
  line under the headset menu; the menu's Overlay and menu page turns it on and off too.
- OpenXR layers: the OpenXR layers (OpenXR Toolkit, VectorXR, Cheeky's...) in the order the game loads
  them below Sim-QVFR, each on or off for the game. With the launcher changes them for its launches
  only; Without the launcher (Windows) edits Windows' own list, which a game started from Steam uses.
- Diagnostics: the last session's numbers (frame rate, frame time, CPU and GPU time, what limited the
  frame, ACE's busiest threads), to compare settings or to ask for help: Copy summary, Open logs folder.
  Next launch without QVFR runs the game once with its own two views, still measuring, so the page can
  compare the same race with and without QVFR. CPU capture: at the next launch, its hotkey (Ctrl+Alt+C)
  while driving records for 20 seconds where ACE's busiest threads spend their time, into the logs folder.
- Report an issue: pick the kind of problem and describe it; Report an issue zips the newest logs, the
  settings, CPU captures and hang dumps, and opens a GitHub issue with the details filled in, for you to
  attach the zip and send.
- About: the version, and Check for updates when the launcher starts (off by default): it asks GitHub
  for the newest release, nothing else, and the bar at the bottom says when one is available; Check now
  is there too. Install puts a newer release in place for you (close the game first): the launcher
  downloads it from GitHub, checks it, replaces its own files and restarts. Your settings, profiles, logs,
  Cheeky's folder and any file you changed (your own helmet images) stay; the version you had is kept in
  the update folder, to copy back by hand if you ever need it. Download opens the release's page instead.
- The preview shows the focus region and the resulting view sizes. Pixel sizes appear after the
  first launch, once the headset has reported its recommended resolution (runtime.ini).
- Restore Defaults returns to the kit's defaults (not the hotkeys). Return launches the game.

The headset menu
----------------
Ctrl+Alt+arrows (or your hotkeys) while the game has focus, or a click of the left controller stick,
opens it; up and down select, left and right change the value; it hides after a few seconds. Its pages:
the method's settings (with Profile first and, for Quad View Foveated Rendering, Optimize for frame rate
last: Right measures while you drive and finds the resolution that holds the frame rate, Left undoes it),
Focus shape, View crop, Helmet, Foveated DLSS and Frame generation (with Cheeky running), and Overlay and
menu. Changes apply at once and save to the launcher's settings.

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
2. From the ace.2 ZIP, copy the CheekyFoveatedDLSS folder into this package's layer folder, so that
   you have layer\CheekyFoveatedDLSS\CheekyFoveatedDLSSRuntime.dll. You can also keep it elsewhere and
   pick the DLL with Choose... in the pane.
3. Don't put Cheeky's dxgi.dll or version.dll in the game folder (remove them if they are there).
   Sim-QVFR loads Cheeky itself.
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

Assetto Corsa Competizione (alpha)
----------------------------------
Choose Assetto Corsa Competizione under Game on the Rendering page, then Launch. ACC's VR has to run
through OpenComposite (one-time setup: docs/acc-opencomposite.md on the project page,
https://github.com/Williem3/Sim-QVFR); Launch checks tells you if it's missing. Turn DLSS off in ACC.
Not in ACC yet, so greyed out: Foveated DLSS, the helmet, the field of view crop, and resolution
changes while the game runs (in ACC they apply at the next launch).

Other OpenXR layers (OpenXR Toolkit, VectorXR, ...)
---------------------------------------------------
The launcher loads Sim-QVFR nearest the game and moves every other installed OpenXR layer
below it for that launch, so tools like OpenXR Toolkit cannot change what the game renders behind
its back. Turn OpenXR Toolkit's upscaling and foveated rendering off for this game (Sim-QVFR
does the foveation); its other features are fine. VectorXR frame generation works below it.

Problems
--------
Send the newest logs\ace-*.log and the matching logs\run-*\launch.txt.

sim-qvfr-launcher.exe --dry-run shows what a launch would do without starting the game.

Removing it: delete the folder. Nothing else was changed on your PC.

Licence: PolyForm Noncommercial 1.0.0 (LICENSE.md): free for noncommercial use; commercial use needs
the author's permission. If you share any part of it, changed or not, pass on LICENSE.md with its
line "Required Notice: Copyright (c) 2026 Williem3 (https://github.com/Williem3/Sim-QVFR)".
Third-party notices: THIRD_PARTY_NOTICES.md. Cheeky Foveated DLSS (optional, separate download) is
by ClarkCheekyKent, GNU GPL v3: https://github.com/ClarkCheekyKent/CheekyFoveatedDLSS
