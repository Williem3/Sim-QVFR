// Each setting's help: the kHelp table, the ⓘ next to a label and the tip it shows under the pointer.
#include <algorithm>
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
     L"compositor (X-Plane 12, Assetto Corsa Competizione)."},
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
    if (app.tip < 0 || app.tip >= static_cast<int>(std::size(kHelp)) || app.tipAnchor.w <= 0)
        return;
    const float w = 320, pad = t::kSpace4;
    const std::wstring text = kHelp[app.tip].text;
    const float th = app.canvas.measure_height(text, t::kCallout, w - 2 * pad);
    Rect card{app.tipAnchor.x - 12, app.tipAnchor.bottom() + 8, w, th + 2 * pad};
    card.x = std::clamp(card.x, 8.0f, app.canvas.width() - w - 8);
    if (card.bottom() > app.canvas.height() - 8)  // no room under it: above
        card.y = app.tipAnchor.y - 8 - card.h;
    app.canvas.fill({card.x, card.y + 4, card.w, card.h}, {0, app.dark ? 0.5f : 0.16f}, t::kRadiusGroup);
    app.canvas.fill(card, col(app.dark ? app.c->control_bg : app.c->group_bg), t::kRadiusGroup);
    app.canvas.stroke(card, col(app.dark ? app.c->control_hover : app.c->card_border), 1, t::kRadiusGroup);
    app.canvas.text(text, t::kCallout, {card.x + pad, card.y + pad, w - 2 * pad, th + 2}, col(app.c->label), Align::Leading, false,
                    true);
}

}  // namespace simqvfr::launcher::ui::win
