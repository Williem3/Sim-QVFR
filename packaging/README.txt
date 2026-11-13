Sim-QVFR
========

Quad views with eye-tracked foveated rendering in VR for Assetto Corsa EVO, Assetto Corsa
Competizione, X-Plane 12 and Le Mans Ultimate: the game renders a lower-resolution full view plus a sharp focus view
where you look, and Sim-QVFR blends them into the headset image. Nothing is installed: the launcher
starts the game with Sim-QVFR for that launch only.

OFFLINE ONLY. Sim-QVFR changes how the game renders by patching it in memory. Do not use it
in online sessions. Le Mans Ultimate's opt-in Run with anti-cheat is at your own risk (see Le Mans
Ultimate below).

Requirements
------------
- One of the games: Assetto Corsa EVO (Steam), Assetto Corsa Competizione (Steam, its VR through
  OpenComposite: see below), X-Plane 12 (12.4.3-r2, from Steam or elsewhere) or Le Mans Ultimate
  (1.4.2.0, Steam).
- A PC VR headset with an OpenXR runtime. Steam running for the Steam games.
- Eye tracking (XR_EXT_eye_gaze_interaction) for the focus area to follow your eyes; without it
  the focus area stays in the centre.
- In Assetto Corsa EVO, turn the game's own foveated rendering off; Sim-QVFR replaces it.

Start
-----
1. Unzip anywhere you can write to, not under Program Files (the launcher can't save its settings
   there; logs go to logs\ next to the launcher).
2. Start Steam and your VR runtime.
3. Run sim-qvfr-launcher.exe with a normal double-click. Pick the game in the list on the left of the
   window; Launch starts it in VR with Sim-QVFR. Always start the game from the launcher:
   started from Steam, it runs without Sim-QVFR.
   As administrator (Run as administrator, or UAC turned off) it works too. The game then starts as
   administrator as well, and its OpenXR reads layers only from Windows' machine-wide list, so each
   launch adds Sim-QVFR's own entries to HKLM\SOFTWARE\Khronos\OpenXR\1\ApiLayers\Explicit and takes
   them out when the game closes. Nothing else there changes; logs\openxr-registrations.log lists
   each entry added and taken out. elevated_registration=0 in [launcher] of sim-qvfr.ini turns it off.
4. If the launcher can't find a game, Launch checks offers Choose folder...: pick the game's folder,
   the folder its exe is in, or steamapps\common.

The window
----------
The games are a list on the left (Ctrl+Tab switches); the selected one is the yellow block. Four tabs
across the top hold the settings in cards: Overview, Picture, Headset and Game. The top bar also has
the theme button (follow Windows, light or dark), Find a setting (Ctrl+K: type a setting's name and
press Return, it opens the card and highlights it), the profile menu, the status and Launch.
Settings save as you change them. Each card says when its settings apply: Changes now, Now when
lowered, or Next game start. Settings that depend on a switch show only while it is on (their values
are kept). An (i) after a name shows, under the pointer, what it does and what it costs. After a
change a bar at the bottom says what changed, with Undo; Ctrl+Z and the profile menu undo it too.
Each card's ... menu has Reset this card... (it asks first).

Each game keeps its own settings: resolution, focus area, object detail, crop, helmet, DLSS or
Foveated DLSS and the profile in use. Picking another game switches them. Hotkeys, the overlay, the
menu's and overlay's places and the launcher's own settings are shared by every game.

Overview
- The selected game's profile: Performance, Balanced or Quality (they set the resolution only: the
  periphery, the focus density and the focus area's size, with a low periphery and a dense centre; one
  set for eye tracked foveation, with a small area that follows your eyes, and one for fixed, with a
  larger area), or Custom. The last session: frame rate against the headset's refresh, slow frames,
  the pixels saved and the last five sessions as a chart. A summary card for each part opens its card.
- The profile menu (top bar): Save current settings... as your own profile (it holds every setting of
  the picture: resolution, foveation, the outline, the view crop and helmet, DLSS or Foveated DLSS, but
  not the menu's and overlay's places, the hotkeys or the OpenXR layers; in the profiles folder),
  Paste a shared profile, Update or Revert a profile you changed after picking it, Manage profiles...
  (Use, Share one as a line of text to paste in a chat, Delete), and Restore defaults for <game>...
  The headset menu's first line, Profile, switches between them while you play.
- The version at the bottom of the game list opens About (a dot there means an update is out).

Picture
- Resolution: Periphery resolution (the full view) and Focus density (pixel density in the focus
  area, 50-400%), each a percent of the headset's recommended resolution and set independently.
  Lowering them applies live; raising them past the size the game started with waits for the next
  launch, and so does any change with Foveated DLSS on.
  Sharpening (every game, 0% = off, the default): AMD FidelityFX Contrast Adaptive Sharpening as
  Sim-QVFR puts the views together, the same strength in the focus area and the periphery. It sharpens
  soft detail most and edges already sharp least, and shows most on a soft image (a low focus density,
  a lower-resolution headset); the periphery shimmers a little more with it. X-Plane runs an FSR
  sharpening pass of its own, so start low there. It applies live, also from the headset menu.
  Object detail (X-Plane only): how much detail trees and objects get. 100% (the default) is the
  detail the sharp focus area needs; lower costs X-Plane's main thread less, which often limits the
  frame rate with QVFR; 0% is X-Plane's own detail for the render size. It applies live.
- Foveation: Eye tracked (the focus area follows your gaze) or Fixed (it stays at the centre of each
  lens). Focus width and Focus height, each a share of the periphery, Corner radius (0 = square, 100%
  = fully round ends) and Feather (softens its edge). Vertical position (fixed) or Gaze offset (eye
  tracked) moves the area up or down; Gaze dead-zone is how far your eyes move before it follows (it
  keeps still against eye-tracker noise). The shape is Rectangle, or Custom corners, each corner pulled
  in on its own (a trapezoid, say, for more of the dash and less of the sky). Show focus outline and
  its colour (teal, magenta, green).
- Preview: the focus area and the resulting view sizes, for the left eye, the right eye or both.
  Pixel sizes appear after the first launch, once the headset has reported its recommended resolution
  (runtime.ini).
- View & helmet (ACE, ACC and LMU): Field of view crop (off by default): Top, Bottom, Outer side
  (temple) and Inner side (nose), each a share of the eye's view and set independently. The game
  doesn't render the cropped edges and the headset shows black there, so pixels a helmet would cover
  aren't wasted. The periphery keeps its sharpness and the focus area its size. It applies live, also
  from the headset menu's View crop page; with Foveated DLSS on, the pixel savings follow at the next
  launch.
  Helmet overlay (off by default): draws a helmet's visor opening around the view, following the crop,
  lit by the scene. Rim (how far the padding reaches into the view), Corners and Lighting apply live.
  A helmet comes in layer\helmet: top.png (brow), bottom.png (chin bar) and visor.png, PNGs with alpha
  of the whole face from inside the helmet. Replace them with your own to change it (delete them for
  the drawn rim instead). They're shown as their own layer over the game, so frame generation isn't
  disturbed. Size and Height place them, Depth sets how far in front of your eyes they sit (0 = far
  away, no depth), Detail picks Performance (2048, cheaper) or Detail (4096, sharper); the Visor is
  Closed, Half or Open (its hotkey, Ctrl+Alt+V). The headset menu's Helmet page has them all. The log
  suggests the crop that matches their opening.
- Rendering: Let the runtime compose (Pimax, Varjo; LMU only, on by default): on a Pimax or Varjo
  OpenXR runtime, which composes quad views itself, Sim-QVFR hands it the four views and the focus area
  reaches the display at its full resolution (sharper above 100% focus density). Sim-QVFR's resolution
  settings apply; its focus area is drawn inside the runtime's own focus region, centred where the
  runtime puts it (its eye tracking), so keep the runtime's focus area (Pimax Play, Varjo Base) at
  least as large as Sim-QVFR's: Diagnostics says when it isn't. The gaze dead-zone and vertical offset
  don't apply then. On SteamVR, with frame generation, or with another layer between Sim-QVFR and the
  runtime, Sim-QVFR composes the image as before. Applies at the next launch; the card says what the
  last launch did. A profile still on the legacy Two-loop method shows Rendering method here too; once
  you switch to Quad View Foveated Rendering, Two-loop isn't offered again.
- DLSS and DLSS-NR (LMU, NVIDIA RTX): see Le Mans Ultimate below.
- Foveated DLSS and Frame generation (ACE and ACC): optional Cheeky Foveated DLSS (see below), its
  runtime and OpenXR layer status, and its settings (cheaper outer views, their DLAA scale and preset,
  the focus preset). The settings apply live; the headset menu's Foveated DLSS page has them too.

Headset
- Headset menu: where the in-headset settings menu floats and its size; Show the hotkeys under the
  menu (on by default) lists the in-game hotkeys and their wheel buttons in a line under it.
- Performance overlay: on/off, where it floats and its size: frame rate and pacing, CPU and GPU time
  with their headroom and which one limits the frame, VRAM, frame generation and eye tracking. In ACC
  and X-Plane the GPU tile shows the whole GPU's time (GPU (all work)); in LMU, which multi-view path
  runs (MVR, SMVR, + DLSS). Its hotkey (Ctrl+Alt+P) turns it on and off in the game; the headset
  menu's Overlay and menu page moves the menu and the overlay.
- Placement: drag the menu or the overlay in the preview (the arrow keys move them too).
- Hotkeys: a table of actions with a key and a wheel, button box or controller button each: opening
  and closing the headset menu, moving through it (up, down, lower, raise), the performance overlay,
  the helmet's visor, a CPU capture and a frame capture. Click a binding, then press the keys or the
  wheel button (Esc cancels, Delete clears); a clash shows a warning on both actions, and the card
  lists the wheels and controllers it finds. Keys work while the game window has focus, wheel buttons
  whenever the game runs; a wheel's menu buttons act only while the menu shows, so bind Open or close
  too. Changes apply to a running game at once. Defaults: Ctrl+Alt+arrows, Ctrl+Alt+P, Ctrl+Alt+V,
  Ctrl+Alt+C, Ctrl+Alt+F.

Game
- Launch checks: what would stop Sim-QVFR at the next launch, each with a fix: a game update
  (Sim-QVFR supports specific builds of each game; after an update it stays off and the game runs
  as normal), the game not found, Steam not running, the game already running, the launcher running as
  administrator (a note: each launch registers Sim-QVFR for the game, see Start), ACC without OpenComposite
  or with a DLSS too old for Foveated DLSS, Cheeky's dxgi.dll or version.dll loader in the game
  folder, and with Cheeky Foveated DLSS on: its runtime and OpenXR layer, OpenXR Toolkit's place above
  Cheeky's layer, and Toolkit's Turbo mode with frame generation; with DLSS on in LMU: multi-view
  rendering, and its nvngx_dlss.dll found and DLSS 310 or newer. The status in the top bar shows the
  first problem (click it for the checks), and Launch asks before starting a game that would crash or
  run without Sim-QVFR.
- Online racing: offline sessions only, or for LMU, Run with anti-cheat (see Le Mans Ultimate below).
- Get help: the last session's likely causes, and Report an issue...: pick the kind of problem and
  describe it; it zips the newest logs, the settings, CPU captures, hang dumps, the game's OpenXR loader
  output and, for ACC, the game's own crash reports (with your Steam ID, account, PC and Windows user
  names and login IDs taken out), and opens a GitHub issue with the details filled in (a free GitHub
  account is needed). Drag the zip's tile onto the issue form to attach it, then send. Under Other ways
  to share, Save the logs as a zip makes the same zip in the logs folder without opening an issue, for
  when someone asks you for your logs; Copy summary and Open logs folder are there too.
- Diagnostics: the last session's numbers (frame rate against the headset's, slow frames against the
  frame's budget, CPU and GPU time, what limited the frame, the game's busiest threads, and how much
  of the session the headset runtime ran the game at half rate, as Varjo's motion smoothing does), to
  compare settings or to ask for help. Previous session sets the last session against the one before
  it (for example "+6.5 fps, after Periphery 60% -> 40%"), and against the newest run without QVFR; Recent sessions lists the last 12 and what changed each time. Next launch without QVFR
  runs the game once with its own two views, still measuring, so the card can compare the same session
  with and without QVFR. Behind More, the capture tools:
  CPU capture: at the next launch, its hotkey (Ctrl+Alt+C) while playing records for 20 seconds where
  the game's busiest threads spend their time, into the logs folder; its Slow frames part shows what
  each thread spends the extra time on in the frames that take longer than the rest.
  Frame capture (ACC, X-Plane and LMU): at the next launch, its hotkey (Ctrl+Alt+F) saves the frame the
  headset gets, the game's own image and the composed output, as PNG files in the logs folder's
  captures folder; each press saves one more.
- OpenXR layers: a line saying what loads, and behind More the OpenXR layers (OpenXR Toolkit, VectorXR,
  Cheeky's...) in the order the game loads them below Sim-QVFR, each on or off for the game. Layers
  for: Launches from here changes them for the launcher's launches only; Every app edits Windows' own
  list, which a game started from Steam uses. Reset this card... goes back to Windows' order.
- Games in the rail: a switch for each game in the list on the left, and its colour. Only games found
  on this PC (through Steam or a folder you chose) are listed; the others are tagged Not found, and
  their switch shows them anyway. Pick a colour preset, or Custom... for any colour.

About (the version at the bottom of the game list): Check for updates when the launcher starts (the
first start asks once): it asks GitHub for the newest release, nothing else, and the status says when
one is available; Check now is there too. Install puts a newer release in place for you (close the
game first): the launcher downloads it from GitHub, checks it, replaces its own files and restarts.
Your settings, profiles, logs, Cheeky's folder and any file you changed (your own helmet images) stay;
the version you had is kept in the update folder, to copy back by hand if you ever need it. Download
opens the release's page instead.

After Launch the status follows the game: starting, running with Sim-QVFR, started without Sim-QVFR
(with the OpenXR loader's reason), crashed or stopped responding (opens Report an issue), or closed
(with that launch's frame rate; opens Diagnostics). Return presses the focused button, and Launch
anywhere else.

The headset menu
----------------
Ctrl+Alt+arrows (or your hotkeys) while the game has focus, or a click of the left controller stick,
opens it; up and down select, left and right change the value; it hides after a few seconds. Its pages:
the method's settings (with Profile first, Object detail after Focus density in X-Plane, Sharpening
after them, and Optimize for frame rate last: Right measures while you play and finds the resolution
that holds the frame rate, Left undoes it), Focus shape, View crop, Helmet, Foveated DLSS and Frame generation (with Cheeky
running), and Overlay and menu. A game shows only the pages it has (X-Plane: the settings, Focus shape,
and Overlay and menu). Changes apply at once and save to the game's settings.

Foveated DLSS (Cheeky Foveated DLSS, optional; ACE and ACC)
-----------------------------------------------------------
The DLSS features come from Cheeky Foveated DLSS by ClarkCheekyKent, integrated with his permission.
With DLSS on in the game and the QVFR method, the outer views get a cheaper DLAA pass while the focus
views keep full DLSS. It also offers focus-view supersampling, DLSS-NR and frame generation. It is a
separate download (GNU GPL v3), not included. You need an NVIDIA RTX GPU.

Downloads:
- CheekyFoveatedDLSS-0.6.3-Standalone-sim-qvfr-support.zip, from
  https://github.com/Williem3/CheekyFoveatedDLSS/releases/tag/v0.6.3-sim-qvfr-support
  (Cheeky v0.6.3 with the fixes both games need: official v0.6.3 shows ACE's outer views shrunk and
  crashes ACC at start; older builds such as v0.6.3-ace.2 don't work with ACC).
- CheekyFoveatedDLSS-0.6.3-Standalone-TEST-SIGNED.zip, from the official release
  https://github.com/ClarkCheekyKent/CheekyFoveatedDLSS/releases/tag/v0.6.3
  (only for OpenXR\CheekyOpenXRSetup.exe, Cheeky's OpenXR layer installer).

Steps:
1. Turn DLSS on in the game's graphics settings: Foveated DLSS works on the game's own DLSS.
2. Run OpenXR\CheekyOpenXRSetup.exe from the official ZIP.
3. From the sim-qvfr-support ZIP, copy the CheekyFoveatedDLSS folder into this package's layer
   folder, so that you have layer\CheekyFoveatedDLSS\CheekyFoveatedDLSSRuntime.dll. You can also keep
   it elsewhere and pick the DLL with Choose... in the Foveated DLSS card.
4. Don't put Cheeky's dxgi.dll or version.dll in the game folder (remove them if they are there).
   Sim-QVFR loads Cheeky itself.
5. If you use OpenXR Toolkit, it must be above Cheeky's layer in the OpenXR layer order (earlier in
   the list, e.g. in OpenXR API Layers GUI). Otherwise the game crashes at start.
6. In Picture > Foveated DLSS, turn on Use Cheeky Foveated DLSS and launch. The card shows whether it
   finds the runtime and the layer, and after a launch Cheeky's status and version.
In ACC, also replace ACC's DLSS (2.2.15, too old: the focus area works but the cheaper outer views
don't) with DLSS 310 or newer: nvngx_dlss.dll in
Assetto Corsa Competizione\AC2\Plugins\DLSS\Binaries\ThirdParty\Win64 (a tool such as DLSS Swapper
does it; Steam's Verify integrity of game files puts the original back). Launch checks tells you when
it's too old. For DLSS-NR, put NVIDIA's nvngx_dlssnr.dll in the same CheekyFoveatedDLSS folder (ACE
has one in its own folder; ACC doesn't come with one).
Frame generation (experimental, ACE only for now, the card under Foveated DLSS): Cheeky's DLSS-G between the
game's frames. Turn OpenXR Toolkit's Turbo mode off (the game crashes with it), and don't run another
frame generator (such as VectorXR's) with it. Show the center region outlines the full-quality region.
With DLSS on, resolution changes apply at the next launch (DLSS sizes its views when the game starts).
The cards' other settings apply live. The headset menu's Foveated DLSS and Frame generation pages
set them too.

Assetto Corsa Competizione
--------------------------
Pick Assetto Corsa Competizione in the game list, then Launch. ACC's VR has to run through OpenComposite
(one-time setup: docs/acc-opencomposite.md on the project page, https://github.com/Williem3/Sim-QVFR);
Launch checks tells you if it's missing. The field of view crop, the helmet overlay, resolution
changes while the game runs, ACC's own DLSS and Foveated DLSS work in ACC; Cheeky's frame generation
doesn't yet. Eye tracking works without another OpenXR layer.
The two eyes' focus areas point at what you're looking at (ACC measures how far away it is), so
they line up on the dash as well as down the track. Bloom and lens flares look as they do without
QVFR (the focus area takes its glow from the view around it). Contact shadows are off while QVFR
runs: they came out differently in the focus area than around it.

X-Plane 12
----------
Pick X-Plane 12 in the game list, then Launch. The launcher starts X-Plane in OpenXR VR and loads Sim-QVFR
before X-Plane starts, which X-Plane needs: started from Steam or its own shortcut, X-Plane runs
without QVFR. Sim-QVFR supports X-Plane 12.4.3-r2; on any other build QVFR stays off and X-Plane runs
as usual. The resolution sliders and the focus size change while you fly, and Object detail (above)
trades scenery detail for X-Plane's main thread. Not in X-Plane: the field of view crop, the helmet
overlay and Foveated DLSS (X-Plane has no DLSS). If X-Plane isn't from Steam, Launch checks asks for
its folder.

Le Mans Ultimate
----------------
More frames in LMU: LMU-OPTIMIZATION-GUIDE.md, in this folder, explains which settings cost CPU and GPU
time (DLSS, other cars' motion, MSAA, resolution) and what to turn down first.
Pick LMU in the game list, then Launch. Started from Steam, LMU runs without QVFR. Sim-QVFR supports LMU
1.4.2.0 (the Steam build of 23 October 2026); on any other build QVFR stays off and LMU runs as usual.
Eye tracking, the resolution sliders, the focus size, the field of view crop, the helmet overlay, the
headset menu and the performance overlay work in LMU, and menus and the HUD are drawn sharp in the
focus area.
LEAVE LMU'S OWN VR FOV SCALE AT 100% x 100%. With any other value QVFR shows a zoomed-in picture or a
picture inside the picture (a known bug, to be fixed). To trim the view's edges for speed, use
Sim-QVFR's View crop instead (Picture > View & helmet, or the headset menu): if you had LMU's Y scale
at 90%, about 8% off the top and bottom gives a similar view.
Multi-view rendering (always on in LMU, no option): each eye's pass also draws and finishes that eye's
focus view, so LMU renders only its own two eye passes a frame. It is the only way Sim-QVFR runs in LMU;
the earlier four passes, made by changing LMU's code, are gone. NVIDIA RTX 20 series and newer use
NVIDIA's multi-view rendering; other cards (AMD, Intel, older NVIDIA) use Sim-QVFR's own, which draws
each draw twice in one call (instancing) and needs a driver with Direct3D 11.3's viewport choice in the
vertex shader. The performance overlay shows which runs beside its CPU heading: MVR, SMVR, + DLSS.
A yellow Eyes only there means the focus views aren't being made: both eyes render at full resolution,
a correct, sharp picture that costs more. Sim-QVFR tries again by itself, at once after you change a
graphics setting in LMU and otherwise every 20 seconds at most (a failed try shows as a brief soft
blink); on a card that can't do multi-view rendering it stays that way. The log says why.
Sim-QVFR doesn't change, hook or call LMU's own code: it works through OpenXR and Direct3D 11, reads a
few values in LMU's memory (the cameras, the eye being drawn, the render size) and writes two (the
render scale and the eye render size).
LMU's own render scale and FSR (its VR graphics settings) work with QVFR: they lower every view's
resolution on top of Sim-QVFR's sliders, and FSR upscales each view. MFAA (NVIDIA Control Panel) works
with multi-view rendering. Let the runtime compose (Picture > Rendering, above) applies to LMU.
VectorXR works below Sim-QVFR in LMU (its frame generation and overlay were tested; turn it on in
Game > OpenXR layers). Cheeky Foveated DLSS isn't for LMU (it works on a game's own DLSS).
DLSS in LMU (experimental, NVIDIA RTX 20 series and newer): LMU has no DLSS, so Sim-QVFR runs NVIDIA
DLSS on its views itself, through NVIDIA's NGX, without changing LMU's code. Picture > DLSS (Use DLSS,
or the headset menu's DLSS page) turns it on and picks the focus area's mode: DLAA, or drawn smaller and
upscaled (Ultra quality 77%, Quality 67%, Balanced 58%, Performance 50% each way: less GPU time, softer
distant textures), and the presets: the periphery K (steadier) or F (about half the cost, more
aliasing; suits eye tracking). Changes apply while you drive, each with a short stutter.
Texture sharpening (off by default; Focus only, or Focus and periphery) samples textures sharper where
DLSS works; the gain is subtle, most in the periphery. On at launch it changes while you drive, without
a stutter; turned on from Off it starts at the next launch (off, it costs nothing). If fine patterns
shimmer, a step lower.
With DLSS on, LMU's MSAA can go off. Other cars' motion (on by default, the DLSS card's advanced
options) gives DLSS each other car's motion, so passing cars don't smear; in a full field about 0.5 ms
of CPU and 0.2-0.5 ms of GPU a frame. Applies at the next launch.
DLSS-NR (experimental, off by default; the DLSS-NR card under DLSS, or the headset menu's DLSS-NR page):
NVIDIA's neural rendering on the same views, with DLSS on. You need your own nvngx_dlssnr.dll (it isn't
in NVIDIA's public SDK, so it can't be included; a game that ships DLSS-NR has it): Choose... points
Sim-QVFR at it. About 2 ms of GPU time per million pixels it works on (RTX 5070 Ti), so it covers the
focus area only unless you turn on Periphery too. Not tested in the headset yet.
NVIDIA's nvngx_dlss.dll is included (layer\nvngx_dlss.dll, DLSS 310.9.1). To use a newer one, Choose...
it in a game's folder on the DLSS card (DLSS 310 or newer); Use included goes back. Launch checks says
when the one chosen is missing or too old.
Uses NVIDIA DLSS through the NVIDIA DLSS SDK (licenses\NVIDIA-RTX-SDKs-LICENSE.txt). NVIDIA, RTX and
DLSS are trademarks of NVIDIA Corporation; Sim-QVFR is not sponsored or endorsed by NVIDIA.
Anti-cheat: by default the launcher starts LMU without Easy Anti-Cheat, so offline only. Run with
anti-cheat (Game > Online racing) starts LMU through Easy Anti-Cheat with Sim-QVFR running, at the next
launch; it asks you to accept the risk first. AT YOUR OWN RISK: Sim-QVFR works inside LMU while it
runs. LMU's code stays unchanged, but Sim-QVFR hooks LMU's graphics calls and reads and writes LMU's
memory. The anti-cheat may kick you or ban your account for that. Turning it on, you knowingly take
responsibility for that. Sim-QVFR doesn't hide from, bypass or change the anti-cheat. Cheeky stays off
with it on.
LMU-Quad-View-Mod (by Dan Suzuki, https://github.com/DSuzuki35/LMU-Quad-View-Mod) inspired this work;
Sim-QVFR's LMU support is written separately and works differently (four views rendered in the game
and composed by Sim-QVFR itself, or by a Pimax or Varjo runtime, nothing else to install). Try both
on your PC and keep the one that runs best; don't run both at once.

Other OpenXR layers (OpenXR Toolkit, VectorXR, ...)
---------------------------------------------------
The launcher loads Sim-QVFR nearest the game and moves every other installed OpenXR layer
below it for that launch, so tools like OpenXR Toolkit cannot change what the game renders behind
its back. Turn OpenXR Toolkit's upscaling and foveated rendering off for this game (Sim-QVFR
does the foveation); its other features are fine. VectorXR frame generation works below it.

Problems
--------
Start with Launch checks (the Game tab). If the game starts on the desktop instead of in the headset, or crashes,
use Report an issue... in Game > Get help (or Save the logs as a zip when someone asks for your logs). The logs are in logs\: each
session's session-*.log, and each launch's run-*\ folder with launch.txt and openxr-loader.log.

sim-qvfr-launcher.exe --dry-run shows what a launch would do without starting the game.

Removing it: delete the folder. Nothing else was changed on your PC.

Licence: PolyForm Noncommercial 1.0.0 (LICENSE.md): free for noncommercial use; commercial use needs
the author's permission. If you share any part of it, changed or not, pass on LICENSE.md with its
line "Required Notice: Copyright (c) 2026 Williem3 (https://github.com/Williem3/Sim-QVFR)".
Third-party notices: THIRD_PARTY_NOTICES.md. Cheeky Foveated DLSS (optional, separate download) is
by ClarkCheekyKent, GNU GPL v3: https://github.com/ClarkCheekyKent/CheekyFoveatedDLSS
NVIDIA's DLSS SDK parts (the NGX library in sim-qvfr.dll and layer\nvngx_dlss.dll, for DLSS in LMU)
are NVIDIA's and not covered by the PolyForm licence. Your terms for them are in THIRD_PARTY_NOTICES.md
(NVIDIA's licence: licenses\NVIDIA-RTX-SDKs-LICENSE.txt). In short: use them only as part of Sim-QVFR;
don't modify, reverse engineer or redistribute them (to share Sim-QVFR, share the link to its
release); NVIDIA gives no warranty for them.
