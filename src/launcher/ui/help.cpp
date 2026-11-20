// Each setting's help: the kHelp table, the ⓘ next to a label and the tip it shows under the pointer.
#include <algorithm>
#include <cmath>
#include <string>

#include <windows.h>

#include "window_state.h"

namespace simqvfr::launcher::ui::win {

// ---- Help --------------------------------------------------------------------------------------
// What a setting does and what it costs, by its label: an ⓘ after the label shows it under the pointer.
constexpr Help kHelp[] = {
    {I18N(L"Rendering method"),
     I18N(L"Quad View Foveated Rendering draws the outer and focus views in one game frame: the lightest on the CPU, and the one "
     L"with every feature. Two-loop (legacy) draws two whole game frames per headset frame: about twice the CPU work per frame, "
     L"and no frame generation.")},
    {I18N(L"Foveated mode"),
     I18N(L"Eye tracked: the sharp area follows your eyes, so it can be small and dense; it needs a headset with eye tracking. "
     L"Fixed: the area stays in the centre, so make it larger, since your eyes move off it.")},
    {I18N(L"Periphery resolution"),
     I18N(L"The wide view around the focus area, a share of the headset’s recommended resolution. It covers most of the view but "
     L"you see it with your side vision, so it is the first to lower: it saves GPU time and VRAM roughly with its pixels. "
     L"The preview’s Pixels per frame shows the total.")},
    {I18N(L"Focus density"),
     I18N(L"Pixel density inside the focus area, a share of the headset’s native density. Above 100% it renders more pixels than "
     L"the headset shows where you look: sharper text and edges. Its cost grows with the area’s size, so a small area can "
     L"take a high density cheaply. Dragging moves it in 5% steps; the arrow keys move it by 1%.")},
    {I18N(L"Object detail"),
     I18N(L"How detailed trees and objects are, and how far away they are still drawn. At 100% they are as sharp in the focus "
     L"region as its pixel density shows them; lower settings move toward the game’s own detail for the periphery resolution. "
     L"It costs CPU, not GPU: more detail means more objects for X-Plane’s main thread to prepare every frame, often what "
     L"limits the frame rate with QVFR. Lower it if frames stutter in dense scenery; the focus region shows coarser trees.")},
    {I18N(L"Eye off"),
     I18N(L"For a player who sees with one eye. The eye you choose shows black in the headset, and the game draws no focus view "
     L"for it, renders only a few pixels of its scene and skips DLSS on it, so the GPU does much less. The game still prepares "
     L"that eye’s frame, so the CPU saves little. With both eyes seeing, a black eye is uncomfortable: leave it at None.")},
    {I18N(L"Sharpening"),
     I18N(L"Crisper edges and text in the focus area; too much makes it shimmer. Raise it a little if the view looks soft (0% is "
     L"off); it costs a little GPU time and changes while you drive. How: AMD FidelityFX Contrast Adaptive Sharpening, "
     L"applied as Sim-QVFR resamples the focus view for the headset. Each pixel is sharpened by how much contrast room it "
     L"has, so soft detail gains the most and edges already sharp gain little. The percentage is CAS’s own sharpness, as "
     L"other CAS sharpeners show it: any setting above 0% already sharpens. Only the focus area is sharpened, fading out "
     L"across its feather so its edge stays even: your side vision sees shimmer more than detail. 0% is off; it changes "
     L"while you drive.")},
    {I18N(L"Night car lighting"),
     I18N(L"Makes the cars look lit at night instead of flat: LMU lights the paint with a flat wash from the floodlights. "
     L"Sim-QVFR relights each car after the game draws it, before the game's own post-processing: highlights where the "
     L"floodlights and headlights reflect off the paint, shading in creases and under the car, and the effects below. "
     L"The rest of the scene is untouched. It costs about 0.5 to 1 ms of GPU time with everything on. The switch takes "
     L"effect at the next launch; the values and effects change while you drive.")},
    {I18N(L"Quality"),
     I18N(L"How finely the effects are worked out: fewer samples and steps for the shadows, occlusion and smoothing "
     L"at Low, all of them at High. The switches below stay as you set them. Low takes about a third less GPU time than "
     L"High and looks a little grainier; the periphery always runs one step below the focus area, where it's least seen.")},
    {I18N(L"Fill light"),
     I18N(L"How much of the game's flat floodlight wash stays on the cars. Lower makes the shading and highlights stand "
     L"out more; 100% keeps the game's brightness.")},
    {I18N(L"Ambient occlusion"),
     I18N(L"Darkens creases, wheel arches and the underside, where light reaches less. 0% is off.")},
    {I18N(L"Highlights"),
     I18N(L"The strength of the floodlights' and headlights' reflections on the paint. 0% is off; above 100% is brighter "
     L"than the lights would make it.")},
    {I18N(L"Paint gloss"),
     I18N(L"How glossy the lacquer is: higher gives smaller, sharper highlights, lower broad soft ones.")},
    {I18N(L"Smoothing"),
     I18N(L"Smooths the surface direction over this distance where the game's own normals aren't available, which "
     L"hides faceted highlights on low-polygon parts. About 15 cm suits most cars; 0 is off.")},
    {I18N(L"Floodlight shadows"),
     I18N(L"A part of a car hidden from a floodlight, behind a wing or another car, loses that light and its highlight.")},
    {I18N(L"Paint detail"),
     I18N(L"Uses the paint's fine surface detail, orange peel and the carbon weave, in the highlights.")},
    {I18N(L"Lacquer reflections"),
     I18N(L"Mirrors what's around a car in its paint, strongest at shallow angles. Only what's on screen can be "
     L"reflected, and only where both eyes can see it, so both eyes show the same.")},
    {I18N(L"Headlight highlights"),
     I18N(L"Other cars' headlights, and yours in the mirrors, shine on the paint as the floodlights do.")},
    {I18N(L"Shadows on the ground"),
     I18N(L"Darkens the track and walls under and beside the cars, and casts soft floodlight shadows, so the cars sit on "
     L"the ground.")},
    {I18N(L"Night vision color"),
     I18N(L"As eyes do at night, dark areas lose their colour and turn slightly blue while lit areas keep theirs. A "
     L"matter of taste, off by default.")},
    {I18N(L"Color correction"),
     I18N(L"Adjusts the picture in the headset: exposure, brightness, contrast and saturation, for a game that looks too "
     L"dark, washed out or dull in your headset. Sim-QVFR applies it as it composes the views, the periphery and the "
     L"focus area alike, so the focus area never stands out; it costs almost no GPU time and changes while you drive. "
     L"Menus and loading screens show as the game draws them. Off, or every value at 0, leaves the picture as it was.")},
    {I18N(L"Exposure"),
     I18N(L"Scales all of the light, in stops (EV): +1.0 EV is twice as bright, -1.0 EV half, up to 3 stops either way. "
     L"Highlights brighten with the rest and can clip, so raise it a little at a time; Brightness lifts the darks "
     L"without touching them.")},
    {I18N(L"Brightness"),
     I18N(L"Lightens or darkens the mid tones and shadows while black stays black and white stays white, so an OLED "
     L"headset keeps its deep blacks: raise it to see into dark cockpits and night stints, lower it for a richer, "
     L"darker picture.")},
    {I18N(L"Contrast"),
     I18N(L"Spreads the picture’s tones away from mid grey (above 0) or draws them towards it (below 0), as a gentle "
     L"S-curve: black and white stay where they are and colours keep their hue. More contrast gives deeper shadows and "
     L"brighter highlights; less shows more detail in both.")},
    {I18N(L"Saturation"),
     I18N(L"How strong the colours are: above 0 more vivid, below 0 more muted, -100% grey. Brightness is kept as each "
     L"colour changes.")},
    {I18N(L"Accent brightness"),
     I18N(L"How light or dark the game’s colour is. The wheel picks its hue and how strong it is; the launcher keeps "
     L"its text and edges readable in light and dark mode.")},
    {I18N(L"Focus width"),
     I18N(L"The focus area’s width, a share of the view. Wider hides the edge between sharp and soft better; its pixels cost "
     L"density × width × height, so a wide area at a high density costs the most. It changes while you drive; wider "
     L"than the game started with (at your density) renders less dense until the next launch.")},
    {I18N(L"Focus height"),
     I18N(L"The focus area’s height, a share of the view. Taller covers more of the dash and mirrors; it costs pixels as the "
     L"width does. Taller than the game started with (at your density) renders less dense until the next launch.")},
    {I18N(L"Corner radius"), I18N(L"Rounds the focus area’s corners, so its edge is harder to spot. No cost: the area renders as a rectangle "
                       L"either way.")},
    {I18N(L"Feather"), I18N(L"The band where the focus area blends into the outer view. Wider hides the edge better; no measurable cost.")},
    {I18N(L"Gaze dead-zone"),
     I18N(L"How far your eyes move before the focus area follows. A little keeps it still against eye-tracker noise; too much "
     L"makes it lag when you look around. No GPU cost.")},
    {I18N(L"Gaze offset"), I18N(L"Moves the eye-tracked focus area up or down, in degrees, for a tracker that sits a little off. No cost.")},
    {I18N(L"Vertical position"), I18N(L"Moves the fixed focus area up or down, in degrees: towards the dash and mirrors, or the road. No cost.")},
    {I18N(L"Crop the view"),
     I18N(L"Cuts the edges a helmet or the lens hides before the game renders them. It saves GPU time and VRAM with the pixels cut; the "
     L"headset shows black there.")},
    {I18N(L"Top"), I18N(L"How much of the top of each eye’s view the game doesn’t render. Saves pixels in proportion.")},
    {I18N(L"Bottom"), I18N(L"How much of the bottom of each eye’s view the game doesn’t render. Saves pixels in proportion.")},
    {I18N(L"Outer side (temple)"), I18N(L"How much of each eye’s temple side the game doesn’t render. Saves pixels in proportion.")},
    {I18N(L"Inner side (nose)"), I18N(L"How much of each eye’s nose side the game doesn’t render; the other eye still sees most of it. Saves pixels in "
                    L"proportion.")},
    {I18N(L"Show a helmet"),
     I18N(L"Draws a helmet’s visor opening round the view, lit by the scene, in a layer of its own. It costs the headset a layer "
     L"to composite; nothing for the game.")},
    {I18N(L"Road focus"),
     I18N(L"The focus area renders in full detail only along the road ahead: about 10 m either side of the road and 3.5 m "
     L"above it, following its hills and bends. Trees, grandstands, hills and the sky beside and above that come from the "
     L"periphery’s image, as does the cockpit; where you look while you drive you shouldn’t see a difference. It saves "
     L"GPU time, more with a larger focus area. Turn it off if something beside the road looks softer than you want. "
     L"Changes while you drive.")},
    {I18N(L"Road width"),
     I18N(L"How far either side of the road’s centre stays in full detail with Road focus on. Lower keeps just the track "
     L"sharp and saves the most; higher brings the kerbs, run-off and barriers into the focus area and saves less. 10 m "
     L"suits most circuits. Changes while you drive.")},
    {I18N(L"DLAA on the road only"),
     I18N(L"With Road focus and Sim-QVFR’s DLSS on: the focus area’s DLAA runs only over the rows the road covers, the rest "
     L"takes the periphery’s DLAA. It saves more GPU time. Turn it off if you see a soft strip above or below the road "
     L"for a moment when you look up or down quickly. Changes while you drive.")},
    {I18N(L"Show focus outline"), I18N(L"Draws the focus area’s edge, to see where it is and how it follows your eyes. For setting up; "
                            L"turn it off to drive.")},
    {I18N(L"Use Cheeky Foveated DLSS"),
     I18N(L"Runs Cheeky Foveated DLSS inside the game, for the DLSS options below. Needs an NVIDIA RTX GPU, DLSS on in the game, and Cheeky’s "
     L"download. Applies at the next launch.")},
    {I18N(L"Cheaper outer views"),
     I18N(L"The outer views get Cheeky’s DLAA at a lower resolution instead of the game’s DLSS: GPU time saved on the views you see "
     L"least.")},
    {I18N(L"Outer DLAA scale"), I18N(L"The resolution of that cheaper pass. Lower saves more GPU time and softens the side view.")},
    {I18N(L"Focus supersampling"), I18N(L"Renders the focus views above their size before DLSS: a sharper centre, for more GPU time.")},
    {I18N(L"Frame generation"),
     I18N(L"Cheeky’s DLSS frame generation adds frames between the game’s: smoother motion when the game can’t hold the refresh rate, for some "
     L"GPU time and latency. Turn OpenXR Toolkit’s Turbo mode off with it, and run no other frame generator.")},
    {I18N(L"Center region"), I18N(L"The part of the view frame generation makes at full quality (it follows your eyes when eye tracked). "
                       L"Larger looks better and costs more GPU time.")},
    // Sections' reference text, by their titles (their footers keep a line).
    {I18N(L"Load order"),
     I18N(L"Nearest the game first. Each launch loads these below the Sim-QVFR layer in this order; a layer off in Windows can "
     L"still be loaded for the game alone. Locked switches keep the mod and the game working.")},
    {I18N(L"Describe the problem"),
     I18N(L"Zips the logs and the game’s crash reports from those sessions (names and account IDs taken out) into the logs "
     L"folder, then opens a new issue on GitHub with what you wrote, your settings, the last session and your PC filled in. "
     L"Add the steps there, and drag in the zip (it shows below) and any screenshots.")},
    {I18N(L"Busiest threads"),
     I18N(L"Each thread is a share of ONE core. A thread near 100% can hold the frame rate back even when Windows shows the whole "
     L"CPU at 15%; a thread that spins by design looks busy whatever limits it.")},
    {I18N(L"Frame rate"),
     I18N(L"The last session’s average against the headset’s refresh rate, the frame rate it asks for. Below it, the runtime "
     L"fills the gaps (motion smoothing or reprojection), which shows as wobble on moving edges.")},
    {I18N(L"Slow frames"),
     I18N(L"How long your slowest frames take: lower is smoother. One frame in ten took at least this long (the 90th "
     L"percentile). Above the frame’s budget (1000 ms divided by the headset’s refresh rate: 11.1 ms at 90 Hz) those frames "
     L"miss the headset, and you feel stutter even when the average frame rate looks fine.")},
    {I18N(L"Limited by"),
     I18N(L"What held the frame back most of the time: the game’s render thread on the CPU, the GPU, or nothing (the headset’s "
     L"refresh). Lowering resolution helps only when the GPU limits it.")},
    {I18N(L"Previous session"),
     I18N(L"The session before the last one for this game, and the settings changed in between, so you can see what a change "
     L"did. A change while you drive (here or in the headset menu) starts a new stint, kept as its own entry; each covers "
     L"the whole stint, with the moments the headset menu was open left out. Compare runs on the same track and car for "
     L"a fair difference.")},
    {I18N(L"Run with anti-cheat"),
     I18N(L"Starts the game through its anti-cheat (Easy Anti-Cheat), as Steam does, with Sim-QVFR running, so you can join online "
     L"servers. Sim-QVFR runs inside the game, and the anti-cheat may kick you or ban your account for it. Sim-QVFR and its "
     L"developer aren’t responsible for kicks or bans: turning it on asks you to accept that risk. Off starts the game without "
     L"its anti-cheat, for offline sessions only.")},
    {I18N(L"Layers for"),
     I18N(L"Launches from here: the VR add-ons (OpenXR layers) a launch from this launcher loads, below Sim-QVFR, in an order of "
     L"its own; nothing else changes. Every app: Windows’ own list, which every other app uses (and the game started from "
     L"Steam), saved in the registry when you apply, with a backup in the logs folder first.")},
    {I18N(L"CPU capture"),
     I18N(L"At the next launch, the CPU capture hotkey (Ctrl+Alt+C) while driving records for 20 s where the game’s busiest "
     L"threads spend their time, into a cpu-profile file in the logs folder. The headset menu shows when it starts and ends; "
     L"the game may stutter slightly while it records.")},
    {I18N(L"Frame capture"),
     I18N(L"Saves the game’s own image and the composed output, as PNG files in the captures folder inside the logs folder. Each "
     L"press saves one more frame, to compare the views where something looks wrong. In X-Plane 12, Assetto Corsa "
     L"Competizione and Le Mans Ultimate.")},
    {I18N(L"Next launch without QVFR"),
     I18N(L"Starts the game once with its own two views and QVFR off, still measuring, so Diagnostics can compare the same race with "
     L"and without QVFR. It turns itself off after that launch; the next one has QVFR again. The performance overlay works "
     L"in it, the headset menu does not.")},
    {I18N(L"Detailed logs"),
     I18N(L"Normal logs say what Sim-QVFR set up, decided and measured. Detailed logs add every step behind that, for a bug "
     L"report when the developer asks for one. They last three launches and then turn themselves off, so the logs stay "
     L"small. Send them with Report an issue, or Save the logs as a zip.")},
    {I18N(L"Logs folder"),
     I18N(L"Each launch adds a session log and a small run folder. The launcher keeps the newest 30 sessions and removes older "
     L"ones at each launch, and the newest 10 CPU captures. Frame captures and game image dumps stay until you clear them: "
     L"Clear captures moves them to the Recycle Bin.")},
    {I18N(L"Presets for eye tracked foveation"),
     I18N(L"Starting points that put the pixels in the centre: low outer views and a dense focus area. Eye tracked, the area "
     L"follows your eyes, so it can be small and very sharp. They set the resolution only and keep everything else.")},
    {I18N(L"Presets for fixed foveation"),
     I18N(L"Starting points that put the pixels in the centre: low outer views and a dense focus area. Fixed, your eyes move off "
     L"the centre, so the area is larger and a little less dense. They set the resolution only and keep everything else.")},
    {I18N(L"Your profiles"),
     I18N(L"A profile holds every setting of the picture: resolution, foveation and the focus overlay, and where the game has "
     L"them the view crop, the helmet and Foveated DLSS. Not the menu’s and overlay’s places, the hotkeys or the OpenXR "
     L"layers. Share copies one as a line of text to paste in a chat; Paste adds one from the clipboard. The headset menu’s "
     L"Profile line switches between them while you drive.")},
    {I18N(L"Headset menu"),
     I18N(L"A key’s first press also opens the menu. A wheel’s directions act only while the menu shows, so a switch the game "
     L"also uses doesn’t open it: use Open or close first.")},
    {I18N(L"In the game"),
     I18N(L"Keys work while the game window has focus; wheel, button box and controller buttons work whenever the game runs.")},
    {I18N(L"Field of view crop"),
     I18N(L"The outer view keeps its sharpness and the focus region its size. Changes reach a running game at once; with DLSS "
     L"on, or with less crop than at launch, the pixel count follows at the next launch.")},
    {I18N(L"Helmet overlay"),
     I18N(L"Built in, the visor opening follows the crop (Rim and Corners). Your own helmet: top.png, bottom.png and visor.png "
     L"with alpha in the helmet folder next to the layer; the log suggests the crop that matches them.")},
    {I18N(L"Focus shape"),
     I18N(L"Each corner moves toward the middle of the focus region, percent of its width (in) or height (down, up). Pull the top "
     L"corners in for a trapezoid: more of the dash sharp, less of the far field. The focus view still renders the whole "
     L"box, so the cost stays the same.")},
    {I18N(L"Cheeky Foveated DLSS"),
     I18N(L"No Cheeky loader goes in the game folder: Sim-QVFR loads Cheeky’s runtime into the game itself.")},
    {I18N(L"Frame generation (experimental)"),
     I18N(L"Outside the center region frames are warped with the game’s motion. Lower scales use less GPU time and memory. With "
     L"an eye-tracked focus (the Foveation card) the region follows your eyes too, through Cheeky’s OpenXR layer.")},
    {I18N(L"Placement"), I18N(L"The settings menu stays 0.8 m in front of your head, wherever you look.")},
    {I18N(L"Use DLSS"),
     I18N(L"Sim-QVFR evaluates NVIDIA’s DLSS on each view, with depth and motion it works out from the game’s own frames, "
     L"before the game’s post-processing. Needs an NVIDIA RTX graphics card. "
     L"Sim-QVFR includes NVIDIA’s nvngx_dlss.dll; Choose… takes another (a newer one from a game), Use included goes "
     L"back.")},
    {I18N(L"Use FSR"),
     I18N(L"Experimental. On a PC without an NVIDIA RTX graphics card, Sim-QVFR evaluates AMD FSR on each view in DLSS’s place, "
     L"with the same depth and motion: FSR 4 where AMD’s software offers it for your card (Radeon RX 9000, and newer "
     L"drivers on RX 7000 and 6000), FSR 3.1 elsewhere. Sim-QVFR includes AMD’s FSR files. Tested only as FSR 3.1 on an "
     L"NVIDIA card so far: tell us how it runs on yours.")},
    {I18N(L"Periphery preset"),
     I18N(L"Steadier or cheaper DLSS outside the focus area. K: a steadier picture, at a higher cost. F: about half the cost, a "
     L"little more flicker on edges. With eye tracking you rarely look there, so F is usually fine; with a fixed focus, K.")},
    {I18N(L"Texture sharpening"),
     I18N(L"DLSS rebuilds detail from frames drawn at a lower resolution, so textures are best sampled a step sharper while it "
     L"runs. Sim-QVFR does that through copies of the game’s shaders, in the focus area alone or in the periphery too. "
     L"Measured gains are modest (most in the periphery and on distant textures). Off at launch, nothing is installed for "
     L"it; turned on then, it starts at the next launch. Fine patterns may shimmer: then a step lower.")},
    {I18N(L"Updates"),
     I18N(L"Install downloads the release from GitHub, checks it against the SHA-256 GitHub gives, replaces the program files "
     L"and restarts the launcher; your settings, profiles and changed files stay, and the version you had is kept in the "
     L"update folder. Download opens the release’s page instead.")},
    {I18N(L"Changes now"), I18N(L"These settings reach the game while it runs, as soon as you change them. Without the game running, they "
              L"apply when it starts.")},
    {I18N(L"Now when lowered"),
     I18N(L"Lowering it reaches a running game at once, and so does raising it back up to what the game started with. Above "
     L"that, it takes effect at the next launch. With DLSS on in the game, the views keep their launch sizes until the "
     L"next launch.")},
    {I18N(L"Next game start"), I18N(L"These settings take effect the next time the game starts from the launcher. A running game keeps "
                     L"what it started with.")},
    {I18N(L"Show the clock"),
     I18N(L"The local time on a small card in the headset, apart from the performance overlay: on or off and moved live, here or "
     L"from the headset menu. It costs next to nothing; it redraws once a minute.")},
    {I18N(L"Show the overlay"),
     I18N(L"The performance overlay in the headset: frame rate, CPU and GPU time and what limits the frame. A little CPU time while "
     L"it shows.")},
    // The Picture tab's cards (picture.cpp): what their rows' one line of help leaves out.
    {I18N(L"Shape"),
     I18N(L"The shape of the sharp area. Rectangle: rounded by Corner radius. Custom shape: an outline of up to 10 points "
     L"you draw on the preview (click the area to add a point, drag one to move it, right-click one to remove it); trace "
     L"your windscreen, say, then place it with Vertical and Horizontal position. To hide the area’s edge, raise "
     L"Feather.")},
    {I18N(L"Outline color"), I18N(L"The outline’s colour in the headset. It shows while Show focus outline is on.")},
    {I18N(L"nvngx_dlss.dll"),
     I18N(L"The DLSS Sim-QVFR uses: the one it includes (NVIDIA’s), or another you choose, a newer one from a game, say (DLSS 310 "
     L"or newer). Use included goes back. Applies at the next launch.")},
    {I18N(L"Focus views"),
     I18N(L"DLAA draws them at their size. Ultra quality, Quality, Balanced and Performance draw them at 77, 67, 58 or 50% each "
     L"way and upscale them: less GPU time, softer distant textures. DLSS has no ultra quality model of its own: 77% runs its "
     L"quality one.")},
    {I18N(L"Focus preset"),
     I18N(L"The DLSS preset for the sharp focus views. With Sim-QVFR’s DLSS, Default is NVIDIA’s choice for the mode: K, and M at "
     L"Performance, which costs more. With Cheeky, Game default keeps the game’s.")},
    {I18N(L"Other cars’ motion"),
     I18N(L"Gives DLSS each other car’s own motion, so cars passing, alongside or ahead stay clean instead of smearing. Turn it off "
     L"for more headroom: in a field of cars it costs about 0.5 ms of CPU and 0.2 to 0.5 ms of GPU a frame. Applies at the "
     L"next launch, with DLSS on at launch; with DLSS off it costs nothing.")},
    {I18N(L"nvngx_dlssnr.dll"),
     I18N(L"NVIDIA’s DLSS neural rendering runtime. Sim-QVFR includes none: it is NVIDIA’s, and comes only with games that ship "
     L"DLSS-NR. Choose the one in such a game’s folder. Applies at the next launch.")},
    {I18N(L"Use DLSS-NR"),
     I18N(L"NVIDIA’s DLSS neural rendering on the views: its model reworks lighting and materials from the picture, its depth and "
     L"its motion. Experimental, and heavy: about 2 ms of GPU time and 130 MB of video memory for every million pixels it "
     L"works on, per view (RTX 5070 Ti). It turns itself off if frames fall under 10 a second with it on.")},
    {I18N(L"Intensity"), I18N(L"How much of the model’s change you get: 0% is the picture as it came.")},
    {I18N(L"Style"), I18N(L"The look the model aims for.")},
    {I18N(L"Preset"),
     I18N(L"The model: A is the original, B a faster one in newer DLLs (310.9.16 on). Default is the DLL’s own choice. A change "
     L"rebuilds DLSS-NR: a short stutter.")},
    {I18N(L"Local tone"), I18N(L"The strength of the model’s local contrast and lighting changes.")},
    {I18N(L"Local structure"), I18N(L"The strength of the model’s changes to fine structure.")},
    {I18N(L"Auto mask"), I18N(L"Lets the model find what it treats differently (skin) by itself.")},
    {I18N(L"Runs"),
     I18N(L"After DLSS works on the finished, anti-aliased views. Before DLSS works on the views as drawn, before anti-aliasing "
     L"and upscaling: fewer pixels when the focus views are upscaled, and DLSS then smooths its result.")},
    {I18N(L"Focus working size"),
     I18N(L"What the model works at, a share of the focus view. Lower costs less; the picture keeps its own detail.")},
    {I18N(L"Working size limit"),
     I18N(L"The most one view’s model works on, in megapixels, whatever the sizes. Each megapixel costs about 2 ms of GPU time and "
     L"130 MB of video memory, per view.")},
    {I18N(L"Edge blend"), I18N(L"The band over which the processed area fades into what isn’t processed around it.")},
    {I18N(L"Periphery too"),
     I18N(L"Off, only the focus views are processed: where you look. On, the periphery too, at its own working size and coverage. "
     L"It adds about 2 ms of GPU time per eye at least, however small you make it.")},
    {I18N(L"Periphery working size"), I18N(L"As the focus working size, for the periphery.")},
    {I18N(L"Periphery covered"), I18N(L"How much of each eye’s view the model covers, around the focus area.")},
    {I18N(L"Foveated DLSS"), I18N(L"Off leaves the game’s own DLSS on every view.")},
    {I18N(L"Outer preset"), I18N(L"The DLSS preset of the outer views’ own pass (Cheaper outer views).")},
    {I18N(L"Fix motion-vector blur"), I18N(L"Cheeky’s workaround for smearing with some presets. Adds GPU work.")},
    {I18N(L"DLSS-NR"), I18N(L"NVIDIA’s DLSS-NR (DLSS 5) on the focus views. Needs nvngx_dlssnr.dll next to the game.")},
    {I18N(L"DLSS-NR passes"), I18N(L"Each extra DLSS-NR pass costs about one more NR evaluation.")},
    {I18N(L"Multiplier"), I18N(L"Frames shown per frame the game renders: 2x adds one generated frame between each two of the game’s.")},
    {I18N(L"Center FG scale"), I18N(L"Frame generation’s resolution in the center region. Lower uses less GPU time and memory.")},
    {I18N(L"Periphery FG scale"), I18N(L"Frame generation’s resolution around the center region. Lower uses less GPU time and memory.")},
    {I18N(L"Release game"),
     I18N(L"When the game starts its next frame. Never: CPU and GPU overlap (most frames). Capture: after the GPU finishes. "
     L"Generated: after the generated frame.")},
    {I18N(L"Show the center region"), I18N(L"Outlines the region in the headset, to check that it follows your eyes.")},
    {I18N(L"Rim"), I18N(L"How far the helmet’s padding reaches into the view.")},
    {I18N(L"Corners"), I18N(L"Rounds the helmet’s visor opening.")},
    {I18N(L"Lighting"), I18N(L"How much the scene lights the helmet.")},
    {I18N(L"Visor"), I18N(L"Your own images only: closed, half or open. Ctrl+Alt+V in the game steps it.")},
    {I18N(L"Depth"), I18N(L"Your own images only: how far in front of your eyes, in cm. 0 is far away, with no depth.")},
    {I18N(L"Detail"), I18N(L"Your own images only: Detail (4096) is sharper, Performance (2048) costs less GPU time and memory.")},
    {I18N(L"Horizontal"), I18N(L"Degrees right of straight ahead. Negative is left. Drag it in Placement, or move it in the headset.")},
    {I18N(L"Vertical"), I18N(L"Degrees above eye level. Negative is below. Drag it in Placement, or move it in the headset.")},
    {I18N(L"Size"), I18N(L"How wide it is, in degrees of your view.")},
    {I18N(L"Show the hotkeys under the menu"),
     I18N(L"A line under the headset menu with the in-game hotkeys (overlay, visor, CPU capture) and their wheel buttons, as set "
     L"in Hotkeys. Also on the menu’s Headset page.")},
    {I18N(L"Open or close"), I18N(L"Opens the headset menu, or closes it.")},
    {I18N(L"Up"), I18N(L"Selects the line above in the headset menu.")},
    {I18N(L"Down"), I18N(L"Selects the line below in the headset menu.")},
    {I18N(L"Left"), I18N(L"Lowers the selected value in the headset menu.")},
    {I18N(L"Right"), I18N(L"Raises the selected value in the headset menu.")},
    {I18N(L"Overlay"), I18N(L"Turns the performance overlay on or off.")},
    {I18N(L"Helmet visor"), I18N(L"Steps the helmet’s visor: closed, half, open.")},
    {I18N(L"Helmet images"),
     I18N(L"Your own helmet: top.png, bottom.png and visor.png in the helmet folder next to the layer. Size and Height place them "
     L"(degrees of view, and above straight ahead).")},
};

// kHelp's index for a label as drawn (in the current language, or English), -1 without help.
int help_index(const std::wstring& label) {
    for (int i = 0; i < static_cast<int>(std::size(kHelp)); ++i)
        if (label == kHelp[i].label || label == tr(kHelp[i].label))
            return i;
    return -1;
}

// The help for a label as drawn, in the current language; nullptr without help.
const wchar_t* help_for(const std::wstring& label) {
    const int i = help_index(label);
    return i < 0 ? nullptr : tr(kHelp[i].text);
}

// The ⓘ for `label` at (x, y) (a 13-point circle, vertically centred on a body line); nothing without help.
void help_icon(const std::wstring& label, float x, float y) {
    if (!help_for(label))
        return;
    const Rect r{x, y + (t::kBody.lineHeight - 14) / 2, 14, 14};
    const int index = help_index(label);
    const Color c = col(app.tip == index ? app.c->accent : app.c->label_muted);
    app.canvas.circle(r.x + 7, r.y + 7, 6.25f, c, false, 1.3f);
    app.canvas.line(r.x + 7, r.y + 6.25f, r.x + 7, r.y + 10.25f, c, 1.4f);
    app.canvas.circle(r.x + 7, r.y + 4.2f, 0.85f, c, true);
    app.hits.push_back({kHelpIcon, {r.x - 5, r.y - 5, r.w + 10, r.h + 10}, index});
    if (app.tip == index)
        app.tipAnchor = r;
}

// The help under the pointer: a card under its ⓘ, kept inside the window, over everything but a pop-up menu.
void help_tip() {
    const bool own = app.tip == kTipText;  // a hit's own tip (Hit::tip): the top bar's, the rail's
    if ((!own && (app.tip < 0 || app.tip >= static_cast<int>(std::size(kHelp)))) || app.tipAnchor.w <= 0)
        return;
    const float pad = t::kSpace4;
    const std::wstring text = own ? app.tipText : std::wstring(tr(kHelp[app.tip].text));
    const float w = own ? std::min(320.0f, std::ceil(app.canvas.measure(text, t::kCallout)) + 2 * pad + 2) : 320.0f;
    const float th = app.canvas.measure_height(text, t::kCallout, w - 2 * pad);
    Rect card{app.tipAnchor.x - 12, app.tipAnchor.bottom() + 8, w, th + 2 * pad};
    card.x = std::clamp(card.x, 8.0f, app.canvas.width() - w - 8);
    if (card.bottom() > app.canvas.height() - 8)  // no room under it: above
        card.y = app.tipAnchor.y - 8 - card.h;
    app.canvas.fill(card, col(app.dark ? app.c->control_bg : app.c->group_bg), t::kRadiusGroup);
    app.canvas.stroke(card, col(app.dark ? app.c->control_hover : app.c->card_border), 1, t::kRadiusGroup);
    app.canvas.text(text, t::kCallout, {card.x + pad, card.y + pad, w - 2 * pad, th + 2}, col(app.c->label), Align::Leading, false,
                    true);
}

}  // namespace simqvfr::launcher::ui::win
