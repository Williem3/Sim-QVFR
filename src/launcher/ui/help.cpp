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
    {L"Rendering method",
     L"Quad View Foveated Rendering draws the outer and focus views in one game frame: the lightest on the CPU, and the one "
     L"with every feature. Two-loop (legacy) draws two whole game frames per headset frame: about twice the CPU work per frame, "
     L"and no frame generation."},
    {L"Foveated mode",
     L"Eye tracked: the sharp area follows your eyes, so it can be small and dense; it needs a headset with eye tracking. "
     L"Fixed: the area stays in the centre, so make it larger, since your eyes move off it."},
    {L"Outer resolution",
     L"The wide view around the focus area, a share of the headset’s recommended resolution. It covers most of the view but "
     L"you see it with your side vision, so it is the first to lower: it saves GPU time and VRAM roughly with its pixels. "
     L"The preview’s Pixels per frame shows the total."},
    {L"Inner resolution",
     L"Pixel density inside the focus area, a share of the headset’s native density. Above 100% it renders more pixels than "
     L"the headset shows where you look: sharper text and edges. Its cost grows with the area’s size, so a small area can "
     L"take a high density cheaply. Dragging moves it in 5% steps; the arrow keys move it by 1%."},
    {L"Object detail",
     L"How detailed trees and objects are, and how far away they are still drawn. At 100% they are as sharp in the focus "
     L"region as its pixel density shows them; lower settings move toward the game’s own detail for the outer resolution. "
     L"It costs CPU, not GPU: more detail means more objects for X-Plane’s main thread to prepare every frame, often what "
     L"limits the frame rate with QVFR. Lower it if frames stutter in dense scenery; the focus region shows coarser trees."},
    {L"Sharpening",
     L"AMD FidelityFX Contrast Adaptive Sharpening, applied as Sim-QVFR resamples the outer and focus views for the headset "
     L"(when the runtime composes them, on a copy of the outer view). Each pixel is sharpened by how much contrast room it "
     L"has, so soft detail gains the most and edges already sharp gain little. Both views get the same strength, so the focus "
     L"area’s edge stays even. It costs a little GPU time, and the low outer resolution shimmers more with it. 0% is off; "
     L"it changes while you drive."},
    {L"Brightness",
     L"How light or dark the game’s colour is. The wheel picks its hue and how strong it is; the launcher keeps "
     L"its text and edges readable in light and dark mode."},
    {L"Foveated width",
     L"The focus area’s width, a share of the view. Wider hides the edge between sharp and soft better; its pixels cost "
     L"density × width × height, so a wide area at a high density costs the most."},
    {L"Foveated height",
     L"The focus area’s height, a share of the view. Taller covers more of the dash and mirrors; it costs pixels as the "
     L"width does."},
    {L"Corner radius", L"Rounds the focus area’s corners, so its edge is harder to spot. No cost: the area renders as a rectangle "
                       L"either way."},
    {L"Feather", L"The band where the focus area blends into the outer view. Wider hides the edge better; no measurable cost."},
    {L"Gaze dead-zone",
     L"How far your eyes move before the focus area follows. A little keeps it still against eye-tracker noise; too much "
     L"makes it lag when you look around. No GPU cost."},
    {L"Gaze offset", L"Moves the eye-tracked focus area up or down, in degrees, for a tracker that sits a little off. No cost."},
    {L"Vertical position", L"Moves the fixed focus area up or down, in degrees: towards the dash and mirrors, or the road. No cost."},
    {L"Crop the view",
     L"Cuts the edges a helmet or the lens hides before the game renders them. It saves GPU time and VRAM with the pixels cut; the "
     L"headset shows black there."},
    {L"Top", L"How much of the top of each eye’s view the game doesn’t render. Saves pixels in proportion."},
    {L"Bottom", L"How much of the bottom of each eye’s view the game doesn’t render. Saves pixels in proportion."},
    {L"Outer side", L"How much of each eye’s temple side the game doesn’t render. Saves pixels in proportion."},
    {L"Inner side", L"How much of each eye’s nose side the game doesn’t render; the other eye still sees most of it. Saves pixels in "
                    L"proportion."},
    {L"Show a helmet",
     L"Draws a helmet’s visor opening round the view, lit by the scene, in a layer of its own. It costs the headset a layer "
     L"to composite; nothing for the game."},
    {L"Show focus outline", L"Draws the focus area’s edge, to see where it is and how it follows your eyes. For setting up; "
                            L"turn it off to drive."},
    {L"Use Cheeky Foveated DLSS",
     L"Runs Cheeky Foveated DLSS inside the game, for the DLSS options below. Needs an NVIDIA RTX GPU, DLSS on in the game, and Cheeky’s "
     L"download. Applies at the next launch."},
    {L"Cheaper outer views",
     L"The outer views get Cheeky’s DLAA at a lower resolution instead of ACE’s DLSS: GPU time saved on the views you see "
     L"least."},
    {L"Outer DLAA scale", L"The resolution of that cheaper pass. Lower saves more GPU time and softens the side view."},
    {L"Focus supersampling", L"Renders the focus views above their size before DLSS: a sharper centre, for more GPU time."},
    {L"Frame generation",
     L"Cheeky’s DLSS frame generation adds frames between the game’s: smoother motion when the game can’t hold the refresh rate, for some "
     L"GPU time and latency. Turn OpenXR Toolkit’s Turbo mode off with it, and run no other frame generator."},
    {L"Center region", L"The part of the view frame generation makes at full quality (it follows your eyes when eye tracked). "
                       L"Larger looks better and costs more GPU time."},
    // Sections' reference text, by their titles (their footers keep a line).
    {L"Load order",
     L"Nearest the game first. Each launch loads these below the Sim-QVFR layer in this order; a layer off in Windows can "
     L"still be loaded for the game alone. Locked switches keep the mod and the game working."},
    {L"Describe the problem",
     L"Zips the logs and the game’s crash reports from those sessions (names and account IDs taken out) into the logs "
     L"folder, then opens a new issue on GitHub with what you wrote, your settings, the last session and your PC filled in. "
     L"Add the steps there, and drag in the zip (it shows below) and any screenshots."},
    {L"Busiest threads",
     L"Each thread is a share of ONE core. A thread near 100% can hold the frame rate back even when Windows shows the whole "
     L"CPU at 15%; a thread that spins by design looks busy whatever limits it."},
    {L"Capture and share",
     L"To report a problem, use Report an issue in the sidebar: it zips these logs and fills in the issue. Elsewhere, send a "
     L"screenshot of this page (or paste the summary) and the session’s log, with the latest cpu-profile file if you made one."},
    {L"CPU capture",
     L"At the next launch, the CPU capture hotkey (Ctrl+Alt+C) while driving records for 20 s where the game’s busiest "
     L"threads spend their time, into a cpu-profile file in the logs folder. The headset menu shows when it starts and ends; "
     L"the game may stutter slightly while it records."},
    {L"Frame capture",
     L"Saves the game’s own image and the composed output, as PNG files in the captures folder inside the logs folder. Each "
     L"press saves one more frame, to compare the views where something looks wrong. For games composed by the D3D11 "
     L"compositor (X-Plane 12, Assetto Corsa Competizione, Le Mans Ultimate)."},
    {L"Next launch without QVFR",
     L"Starts the game once with its own two views and QVFR off, still measuring, so this page can compare the same race with "
     L"and without QVFR. It turns itself off after that launch; the next one has QVFR again. The performance overlay works "
     L"in it, the settings menu does not."},
    {L"Presets for eye tracked foveation",
     L"Starting points that put the pixels in the centre: low outer views and a dense focus area. Eye tracked, the area "
     L"follows your eyes, so it can be small and very sharp. They set the resolution only and keep everything else."},
    {L"Presets for fixed foveation",
     L"Starting points that put the pixels in the centre: low outer views and a dense focus area. Fixed, your eyes move off "
     L"the centre, so the area is larger and a little less dense. They set the resolution only and keep everything else."},
    {L"Your profiles",
     L"A profile holds every setting of the picture: resolution, foveation and the focus overlay, and where the game has "
     L"them the view crop, the helmet and Foveated DLSS. Not the menu’s and overlay’s places, the hotkeys or the OpenXR "
     L"layers. Share copies one as a line of text to paste in a chat; Paste adds one from the clipboard. The headset menu’s "
     L"Profile line switches between them while you drive."},
    {L"Headset menu",
     L"A key’s first press also opens the menu. A wheel’s directions act only while the menu shows, so a switch the game "
     L"also uses doesn’t open it: use Open or close first."},
    {L"In the game",
     L"Keys work while the game window has focus; wheel, button box and controller buttons work whenever the game runs."},
    {L"Field of view crop",
     L"The outer view keeps its sharpness and the focus region its size. Changes reach a running game at once; with DLSS "
     L"on, or with less crop than at launch, the pixel count follows at the next launch."},
    {L"Helmet overlay",
     L"Built in, the visor opening follows the crop (Rim and Corners). Your own helmet: top.png, bottom.png and visor.png "
     L"with alpha in the helmet folder next to the layer; the log suggests the crop that matches them."},
    {L"Focus shape",
     L"Each corner moves toward the middle of the focus region, percent of its width (in) or height (down, up). Pull the top "
     L"corners in for a trapezoid: more of the dash sharp, less of the far field. The focus view still renders the whole "
     L"box, so the cost stays the same."},
    {L"Cheeky Foveated DLSS",
     L"No Cheeky loader goes in the game folder: Sim-QVFR loads Cheeky’s runtime into the game itself."},
    {L"Frame generation (experimental)",
     L"Outside the center region frames are warped with the game’s motion. Lower scales use less GPU time and memory. With "
     L"an eye-tracked focus (Foveation pane) the region follows your eyes too, through Cheeky’s OpenXR layer."},
    {L"Placement", L"The settings menu stays 0.8 m in front of your head, wherever you look."},
    {L"Use DLSS",
     L"Sim-QVFR evaluates NVIDIA’s DLSS on each view, with depth and motion it works out from the game’s own frames, "
     L"before the game’s post-processing. The game’s code isn’t changed or called. Needs an NVIDIA RTX graphics card. "
     L"Sim-QVFR includes NVIDIA’s nvngx_dlss.dll; Choose… takes another (a newer one from a game), Use included goes "
     L"back."},
    {L"Periphery preset",
     L"The DLSS model for the outer views. K is steadier at a higher cost; F, the older model, costs about half as much and "
     L"aliases more. With eye tracking you rarely look there, so F’s savings come cheap; with a fixed focus, K."},
    {L"Texture sharpening",
     L"DLSS rebuilds detail from frames drawn at a lower resolution, so textures are best sampled a step sharper while it "
     L"runs. Sim-QVFR does that through copies of the game’s shaders, in the focus area alone or in the periphery too. "
     L"Measured gains are modest (most in the periphery and on distant textures). Off at launch, nothing is installed for "
     L"it; turned on then, it starts at the next launch. Fine patterns may shimmer: then a step lower."},
    {L"Updates",
     L"Install downloads the release from GitHub, checks it against the SHA-256 GitHub gives, replaces the program files "
     L"and restarts the launcher; your settings, profiles and changed files stay, and the version you had is kept in the "
     L"update folder. Download opens the release’s page instead."},
    {L"Live", L"These settings reach the game while it runs, as soon as you change them. Without the game running, they "
              L"apply when it starts."},
    {L"Live when lowered",
     L"Lowering it reaches a running game at once, and so does raising it back up to what the game started with. Above "
     L"that, it takes effect at the next launch. With DLSS on in the game, the views keep their launch sizes until the "
     L"next launch."},
    {L"Focus region",
     L"Changes reach a running game at once. A focus area wider or taller than the game started with renders at a lower "
     L"density until the next launch, which sizes its view to fit."},
    {L"Next launch", L"These settings take effect the next time the game starts from the launcher. A running game keeps "
                     L"what it started with."},
    {L"Show the overlay",
     L"The performance overlay in the headset: frame rate, CPU and GPU time and what limits the frame. A little CPU time while "
     L"it shows."},
    // The Picture tab's cards (picture.cpp): what their rows' one line of help leaves out.
    {L"Shape",
     L"Box: a rectangle, rounded by Corner radius. Corners: each corner moved in on its own. Pull the top corners in for a "
     L"trapezoid: more of the dash sharp, at the same cost."},
    {L"Outline color", L"The outline’s colour in the headset. It shows while Show focus outline is on."},
    {L"Let the runtime compose",
     L"On a Pimax or Varjo runtime, the runtime puts the focus region on the display at its full resolution: sharper than "
     L"Sim-QVFR can compose it. On other runtimes (SteamVR) nothing changes. With the runtime composing, the resolution set "
     L"here applies and changes while you drive. The focus area shows inside the runtime’s own focus region (Pimax Play’s or "
     L"Varjo Base’s focus area), centred where the runtime puts it: keep that at least as large as the focus area here. Off "
     L"with frame generation (Cheeky’s or VectorXR’s), which needs one composed image."},
    {L"nvngx_dlss.dll",
     L"The DLSS Sim-QVFR uses: the one it includes (NVIDIA’s), or another you choose, a newer one from a game, say (DLSS 310 "
     L"or newer). Use included goes back. Applies at the next launch."},
    {L"Focus views",
     L"DLAA draws them at their size. Ultra quality, Quality, Balanced and Performance draw them at 77, 67, 58 or 50% each "
     L"way and upscale them: less GPU time, softer distant textures. DLSS has no ultra quality model of its own: 77% runs its "
     L"quality one."},
    {L"Focus preset",
     L"The DLSS preset for the sharp focus views. With Sim-QVFR’s DLSS, Default is NVIDIA’s choice for the mode: K, and M at "
     L"Performance, which costs more. With Cheeky, Game default keeps the game’s."},
    {L"Other cars’ motion",
     L"Gives DLSS each other car’s own motion, so cars passing, alongside or ahead stay clean instead of smearing. Turn it off "
     L"for more headroom: in a field of cars it costs about 0.5 ms of CPU and 0.2 to 0.5 ms of GPU a frame. Applies at the "
     L"next launch, with DLSS on at launch; with DLSS off it costs nothing."},
    {L"nvngx_dlssnr.dll",
     L"NVIDIA’s DLSS neural rendering runtime. Sim-QVFR includes none: it is NVIDIA’s, and comes only with games that ship "
     L"DLSS-NR. Choose the one in such a game’s folder. Applies at the next launch."},
    {L"Use DLSS-NR",
     L"NVIDIA’s DLSS neural rendering on the views: its model reworks lighting and materials from the picture, its depth and "
     L"its motion. Experimental, and heavy: about 2 ms of GPU time and 130 MB of video memory for every million pixels it "
     L"works on, per view (RTX 5070 Ti). It turns itself off if frames fall under 10 a second with it on."},
    {L"Intensity", L"How much of the model’s change you get: 0% is the picture as it came."},
    {L"Style", L"The look the model aims for."},
    {L"Preset",
     L"The model: A is the original, B a faster one in newer DLLs (310.9.16 on). Default is the DLL’s own choice. A change "
     L"rebuilds DLSS-NR: a short stutter."},
    {L"Local tone", L"The strength of the model’s local contrast and lighting changes."},
    {L"Local structure", L"The strength of the model’s changes to fine structure."},
    {L"Auto mask", L"Lets the model find what it treats differently (skin) by itself."},
    {L"Runs",
     L"After DLSS works on the finished, anti-aliased views. Before DLSS works on the views as drawn, before anti-aliasing "
     L"and upscaling: fewer pixels when the focus views are upscaled, and DLSS then smooths its result."},
    {L"Focus working size",
     L"What the model works at, a share of the focus view. Lower costs less; the picture keeps its own detail."},
    {L"Working size limit",
     L"The most one view’s model works on, in megapixels, whatever the sizes. Each megapixel costs about 2 ms of GPU time and "
     L"130 MB of video memory, per view."},
    {L"Edge blend", L"The band over which the processed area fades into what isn’t processed around it."},
    {L"Periphery too",
     L"Off, only the focus views are processed: where you look. On, the periphery too, at its own working size and coverage. "
     L"It adds about 2 ms of GPU time per eye at least, however small you make it."},
    {L"Periphery working size", L"As the focus working size, for the periphery."},
    {L"Periphery covered", L"How much of each eye’s view the model covers, around the focus area."},
    {L"Foveated DLSS", L"Off leaves the game’s own DLSS on every view."},
    {L"Outer preset", L"The DLSS preset of the outer views’ own pass (Cheaper outer views)."},
    {L"Fix motion-vector blur", L"Cheeky’s workaround for smearing with some presets. Adds GPU work."},
    {L"DLSS-NR", L"NVIDIA’s DLSS-NR (DLSS 5) on the focus views. Needs nvngx_dlssnr.dll next to the game."},
    {L"DLSS-NR passes", L"Each extra DLSS-NR pass costs about one more NR evaluation."},
    {L"Multiplier", L"Frames shown per frame the game renders: 2x adds one generated frame between each two of the game’s."},
    {L"Center FG scale", L"Frame generation’s resolution in the center region. Lower uses less GPU time and memory."},
    {L"Periphery FG scale", L"Frame generation’s resolution around the center region. Lower uses less GPU time and memory."},
    {L"Release game",
     L"When the game starts its next frame. Never: CPU and GPU overlap (most frames). Capture: after the GPU finishes. "
     L"Generated: after the generated frame."},
    {L"Show the center region", L"Outlines the region in the headset, to check that it follows your eyes."},
    {L"Rim", L"How far the helmet’s padding reaches into the view."},
    {L"Corners", L"Rounds the helmet’s visor opening."},
    {L"Lighting", L"How much the scene lights the helmet."},
    {L"Visor", L"Your own images only: closed, half or open. Ctrl+Alt+V in the game steps it."},
    {L"Depth", L"Your own images only: how far in front of your eyes, in cm. 0 is far away, with no depth."},
    {L"Detail", L"Your own images only: Detail (4096) is sharper, Performance (2048) costs less GPU time and memory."},
    {L"Horizontal", L"Degrees right of straight ahead. Negative is left. Drag it in Placement, or move it in the headset."},
    {L"Vertical", L"Degrees above eye level. Negative is below. Drag it in Placement, or move it in the headset."},
    {L"Size", L"How wide it is, in degrees of your view."},
    {L"Show the hotkeys under the menu",
     L"A line under the headset menu with the in-game hotkeys (overlay, visor, CPU capture) and their wheel buttons, as set "
     L"in Hotkeys. Also on the menu’s Overlay and menu page."},
    {L"Open or close", L"Opens the headset menu, or closes it."},
    {L"Up", L"Selects the line above in the headset menu."},
    {L"Down", L"Selects the line below in the headset menu."},
    {L"Left", L"Lowers the selected value in the headset menu."},
    {L"Right", L"Raises the selected value in the headset menu."},
    {L"Overlay", L"Turns the performance overlay on or off."},
    {L"Helmet visor", L"Steps the helmet’s visor: closed, half, open."},
    {L"Helmet images",
     L"Your own helmet: top.png, bottom.png and visor.png in the helmet folder next to the layer. Size and Height place them "
     L"(degrees of view, and above straight ahead)."},
};

const wchar_t* help_for(const std::wstring& label) {
    for (const Help& h : kHelp)
        if (label == h.label)
            return h.text;
    return nullptr;
}

// The ⓘ for `label` at (x, y) (a 13-point circle, vertically centred on a body line); nothing without help.
void help_icon(const std::wstring& label, float x, float y) {
    if (!help_for(label))
        return;
    const Rect r{x, y + (t::kBody.lineHeight - 14) / 2, 14, 14};
    int index = 0;
    while (label != kHelp[index].label)
        ++index;
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
    const std::wstring text = own ? app.tipText : kHelp[app.tip].text;
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
