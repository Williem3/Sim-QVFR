// Offline check of the in-headset settings menu: selection, steps and ranges, the items per method,
// the ini round trip and the launch-size note; and that the panel image draws text and highlight.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <string>

#include <windows.h>

#include "games/ace/game.h"
#include "games/acc/game.h"
#include "games/lmu/game.h"
#include "games/xplane/game.h"
#include "ini_put.h"
#include "quad/menu_panel.h"
#include "quad/settings_menu.h"

using namespace simqvfr;

namespace {

int failures = 0;

void check(bool ok, const char* what) {
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    failures += ok ? 0 : 1;
}

}  // namespace

int main() {
    SettingsMenu m;
    m.launch = {true, 50, 39, 39};
    check(menu_items(true).size() == 10 && menu_items(false).size() == 8 &&
              menu_items(false).front() == MenuItem::Width,
          "QVFR lists resolution; Two-loop starts at the focus width");
    check(menu_page_items(m).size() == 12 && menu_page_items(m).front() == MenuItem::Page &&
              menu_page_items(m).back() == MenuItem::Optimize && menu_pages(m).size() == 5,
          "the first page: the page switch, the method's items, then Optimize; five pages without Cheeky (Focus shape; QVFR: View crop, Helmet)");
    {
        SettingsMenu loop;
        loop.launch.qvfr = false;
        check(menu_page_items(loop).back() == MenuItem::GazeDeadzone, "Two-loop: no Optimize");
    }

    menu_key(m, MenuKey::Up);
    check(m.selected == 11, "Up from the first item wraps to the last (Optimize)");
    {
        SettingsMenu o = m;
        const MenuValues before = o.values;
        check(!menu_key(o, MenuKey::Right) && o.optimizeRequest == 1 && o.values.periphery == before.periphery,
              "Optimize: Right asks the layer to run it, no value changes");
        check(!menu_key(o, MenuKey::Left) && o.optimizeRequest == -1, "Optimize: Left asks to undo");
        check(menu_lines(o).back().label == L"Optimize for frame rate" && !menu_lines(o).back().value.empty(),
              "Optimize's line before a run: how to use it");
        o.optimizeStatus = L"Running…";
        o.optimizeDetail = L"Measuring… keep driving";
        check(menu_lines(o).back().value == L"Running…" && menu_subtitle(o) == L"Measuring… keep driving",
              "Optimize's status on its line, its detail under the title");
    }
    menu_key(m, MenuKey::Down);
    check(m.selected == 0, "Down from the last wraps to the first");
    m.selected = 1;  // periphery
    bool changed = menu_key(m, MenuKey::Left);
    check(changed && m.values.periphery == 45, "periphery steps by 5");
    for (int i = 0; i < 30; ++i)
        menu_key(m, MenuKey::Left);
    check(m.values.periphery == 10 && !menu_key(m, MenuKey::Left), "periphery stops at 10%, no change reported");
    m.selected = 3;
    menu_key(m, MenuKey::Right);
    check(m.values.width == 40, "focus width steps by 1");
    m.selected = 7;
    check(menu_key(m, MenuKey::Right) && m.values.outline && menu_key(m, MenuKey::Left) && !m.values.outline,
          "outline toggles either way");

    // Launch-size note: periphery or focus pixels above the launch.
    SettingsMenu over;
    over.launch = {true, 50, 39, 39};
    check(!menu_over_launch(over), "the default values fit a launch at the defaults");
    over.values.density = 120;
    check(menu_over_launch(over), "more density than the launch is flagged");
    over.launch.qvfr = false;
    check(!menu_over_launch(over), "Two-loop never flags (no live resolution)");

    // The 14:19 run: launched at periphery 10%, density 120% over 30% x 25% (focus views 36% x 30% of
    // a 5190x4168 eye); then periphery 30% and density 100% over 43% x 42%.
    SettingsMenu run;
    run.launch = {true, 10, 36, 30, 5190, 4168};
    run.values.periphery = 30;
    run.values.density = 100;
    run.values.width = 43;
    run.values.height = 42;
    const MenuFit fit = menu_fit(run);
    check(fit.peripheryCapped && std::lround(fit.periphery) == 10 && fit.focusCapped && std::lround(fit.densityX) == 84 &&
              std::lround(fit.densityY) == 71,
          "the run's values: periphery held at 10%, density 84% x 71% in effect (as the layer logged)");
    run.selected = 1;
    const std::vector<MenuLine> runLines = menu_lines(run);
    check(runLines[1].capped && runLines[1].value == L"30%  (10% now)" && runLines[2].capped &&
              runLines[2].value == L"100%  (84% x 71% now)" && !runLines[3].capped,
          "capped lines show the asked value and what is in effect");
    const std::vector<std::wstring> notes = menu_notes(run);
    check(notes.size() == 2 &&
              notes[0] == L"Focus area needs 2232x1751 px, 1868x1250 fits: lower Focus density, width or height" &&
              notes[1] == L"Periphery needs 1557x1250 px, the focus area leaves 519x417: lower Focus density or size to make room",
          "the notes say why, in pixels, and what to lower");
    const MenuResolution res = menu_resolution(run);
    check(res.shown && res.capped && res.max == L"4774x1250 (6.0 MP)" && res.current == L"4774x1250 (6.0 MP)" &&
              res.asked == L"7578x1751 (13.3 MP)",
          "the render target: max, now and asked");

    // The second test: inner lowered to 80% over a 30% x 25% box (focus 24% x 20%), then periphery 30%.
    SettingsMenu traded;
    traded.launch = run.launch;
    traded.values.periphery = 30;
    traded.values.density = 80;
    traded.values.width = 30;
    traded.values.height = 25;
    const MenuFit tradedFit = menu_fit(traded);
    const std::vector<std::wstring> tradedNotes = menu_notes(traded);
    check(tradedFit.peripheryCapped && tradedFit.peripheryByRoom && std::lround(tradedFit.periphery) == 22 &&
              !tradedFit.focusCapped && tradedNotes.size() == 1 &&
              tradedNotes[0] == L"Periphery needs 1557x1250 px, the focus area leaves 1142x917: lower Focus density or size to make room",
          "lowering the focus area makes room: periphery 22% in effect (above the launch 10%), the note says how much");
    traded.values.density = 50;  // focus 15% x 12.5%: room for 31%, the target's height allows 30%
    check(!menu_over_launch(traded) && menu_notes(traded).empty() && std::lround(menu_fit(traded).periphery) == 30,
          "with inner at 50% the asked periphery 30% fits");
    check(menu_resolution(traded).current == L"4672x1250 (5.8 MP)" && !menu_resolution(traded).capped,
          "and the target now is under the max");

    // Center PPD and pixels saved, from what is in effect. 45.32 PPD at 100% density.
    SettingsMenu stats = run;
    stats.launch.nativePpd = 45.32;
    stats.values.periphery = 30;
    stats.values.density = 100;
    stats.values.width = 43;
    stats.values.height = 42;  // the capped run: density 84% in effect, views 10% and 36% x 30%
    const MenuStats st = menu_stats(stats);
    check(st.centerPpd == 38 && st.savedPercent == 88,
          "Center PPD follows the density in effect (45.32 x 84% = 38); 88% of the native pixels saved");
    SettingsMenu loop;
    loop.launch = {false, 100, 100, 100, 5190, 4168, 45.32, 50};
    loop.values.width = 39;
    const MenuStats loopStats = menu_stats(loop);
    check(loopStats.centerPpd == 58 && loopStats.savedPercent == 50,
          "Two-loop: density eye_scale / width (128%: 58 PPD); two frames at 50% save 50%");
    SettingsMenu unknownPpd = stats;
    unknownPpd.launch.nativePpd = 0;
    check(menu_stats(unknownPpd).centerPpd == 0, "no PPD before ACE has rendered");
    run.values.periphery = 10;
    run.values.density = 84;
    run.values.height = 35;  // 84% x 43% = 36.1%: still over 36%
    check(menu_notes(run).size() == 1 && menu_fit(run).focusCapped, "a value just over the launch still flags");
    run.values.width = 42;  // 84% x 42% = 35.3%, 84% x 35% = 29.4%: fits
    check(menu_notes(run).empty() && !menu_lines(run)[2].capped, "lowering the box makes it fit: no notes, no red");
    run.launch.nativeWidth = 0;
    run.values.density = 150;
    check(menu_notes(run).size() == 1 && menu_notes(run)[0].find(L"px") == std::wstring::npos,
          "without the native size the note has no pixels");

    // The ini: written keys read back; Two-loop writes no resolution.
    wchar_t temp[MAX_PATH] = {};
    GetTempPathW(MAX_PATH, temp);
    const std::wstring ini = std::wstring(temp) + L"sim-qvfr-menu-test.ini";
    DeleteFileW(ini.c_str());
    WritePrivateProfileStringW(L"experiment", L"eye_scale", L"40", ini.c_str());
    WritePrivateProfileStringW(L"experiment", L"focus_percent", L"31", ini.c_str());
    const MenuValues fromEyeScale = read_menu_values(ini.c_str());
    check(fromEyeScale.periphery == 40 && fromEyeScale.density == 129 && fromEyeScale.height == 31,
          "unset periphery and density follow eye_scale, as the launcher reads them");
    MenuValues v;
    v.periphery = 35;
    v.density = 110;
    v.width = 30;
    v.height = 25;
    v.feather = 12;
    v.corners = 60;
    v.outline = true;
    v.eyeTracking = false;
    v.overlay = {true, -12, 20, 30};
    check(write_menu_values(ini.c_str(), v, true), "the menu's keys are written");
    const MenuValues back = read_menu_values(ini.c_str());
    check(back.periphery == 35 && back.density == 110 && back.width == 30 && back.height == 25 && back.feather == 12 &&
              back.corners == 60 && back.outline && !back.eyeTracking && back.overlay == v.overlay,
          "and read back");
    const std::wstring loopIni = std::wstring(temp) + L"sim-qvfr-menu-test-loop.ini";
    DeleteFileW(loopIni.c_str());
    write_menu_values(loopIni.c_str(), v, false);
    check(GetPrivateProfileIntW(L"experiment", L"periphery_scale", -1, loopIni.c_str()) == -1 &&
              GetPrivateProfileIntW(L"experiment", L"focus_percent", -1, loopIni.c_str()) == 30,
          "Two-loop writes foveation but no resolution");
    check(GetPrivateProfileIntW(L"experiment", L"object_detail", -1, ini.c_str()) == -1,
          "object detail isn't written for a game without it");
    v.objectDetail = 40;
    write_menu_values(ini.c_str(), v, true, true);
    check(read_menu_values(ini.c_str()).objectDetail == 40, "object detail is written for a game with it, and read back");
    check(GetPrivateProfileIntW(L"quad", L"sharpen_percent", -1, ini.c_str()) == -1 && read_menu_values(ini.c_str()).sharpen == 0,
          "sharpening: not written for a game without it; unset reads as off");
    v.sharpen = 35;
    write_menu_values(ini.c_str(), v, true, false, false, true);
    check(GetPrivateProfileIntW(L"quad", L"sharpen_percent", -1, ini.c_str()) == 35 && read_menu_values(ini.c_str()).sharpen == 35,
          "sharpening: written for a game with it ([quad] sharpen_percent), and read back");
    v.sharpen = 0;
    check(GetPrivateProfileIntW(L"experiment", L"focus_quad_distance", -1, ini.c_str()) == -1 &&
              read_menu_values(ini.c_str()).focusQuadStep == kFocusQuadSteps - 1,
          "focus quad distance: not written without focus_layer=2; unset reads as 100 m");
    v.focusQuadStep = focus_quad_step(5);
    write_menu_values(ini.c_str(), v, true, false, true);
    check(GetPrivateProfileIntW(L"experiment", L"focus_quad_distance", -1, ini.c_str()) == 5 &&
              focus_quad_metres(read_menu_values(ini.c_str()).focusQuadStep) == 5,
          "focus quad distance: written in metres with focus_layer=2, and read back");
    DeleteFileW(ini.c_str());
    DeleteFileW(loopIni.c_str());

    // Lines and the panel image.
    SettingsMenu shown;
    shown.selected = 2;
    const std::vector<MenuLine> lines = menu_lines(shown);
    check(lines.size() == 12 && lines[0].label == L"Page" && lines[0].value == L"QVFR" && lines[2].selected &&
              lines[2].label == L"Focus density" && lines[2].value == L"100%" && lines[8].value == L"On" &&
              lines[9].label == L"Focus up / down" && lines[9].value == L"0°",
          "lines carry labels, values and the selection");

    // The vertical offset: each mode keeps its own, stepped by the one item, written to [quad].
    {
        SettingsMenu o;
        o.selected = 9;
        o.values.eyeTracking = false;
        check(menu_key(o, MenuKey::Left) && menu_key(o, MenuKey::Left) && o.values.offset.fixed == -2 &&
                  o.values.offset.gaze == 0 && menu_lines(o)[9].value == L"2° down",
              "Focus up / down steps the fixed offset with eye tracking off");
        o.values.eyeTracking = true;
        check(menu_key(o, MenuKey::Right) && o.values.offset.gaze == 1 && o.values.offset.fixed == -2 &&
                  menu_lines(o)[9].value == L"1° up",
              "and the gaze offset with it on, leaving the fixed one");
        o.values.offset.gaze = kFocusOffsetMax;
        check(!menu_key(o, MenuKey::Right) && o.values.offset.gaze == kFocusOffsetMax, "clamped at the most");
        const std::wstring offsetIni = std::wstring(temp) + L"sim-qvfr-menu-test-offset.ini";
        DeleteFileW(offsetIni.c_str());
        check(write_menu_values(offsetIni.c_str(), o.values, true), "offsets written");
        const FocusOffset read = read_menu_values(offsetIni.c_str()).offset;
        check(read.fixed == -2 && read.gaze == kFocusOffsetMax, "negative and positive offsets read back");
        WritePrivateProfileStringW(L"quad", L"fixed_offset_y", L"-99", offsetIni.c_str());
        check(read_focus_offset(offsetIni.c_str()).fixed == -kFocusOffsetMax, "out-of-range offsets clamp");
        DeleteFileW(offsetIni.c_str());
        check(std::abs(offset_tan_y(0, 0)) < 1e-9 && std::abs(offset_tan_y(0, 45) - 1) < 1e-9 &&
                  std::abs(offset_tan_y(1, -45)) < 1e-9 && std::abs(offset_tan_y(std::tan(0.5), 10) - std::tan(0.5 + 3.14159265358979 / 18)) < 1e-9,
              "offset_tan_y turns the tangent by the angle");
    }

    // The gaze dead-zone: tenths of a degree, 0.5 a step, written to [quad] gaze_deadzone as degrees.
    {
        SettingsMenu d;
        d.selected = 10;
        check(menu_lines(d)[10].label == L"Gaze dead-zone" && menu_lines(d)[10].value == L"1.5°", "the dead-zone at 1.5° by default");
        check(menu_key(d, MenuKey::Right) && d.values.gazeDeadzone == 20 && menu_lines(d)[10].value == L"2.0°", "it steps by 0.5°");
        for (int i = 0; i < 10; ++i)
            menu_key(d, MenuKey::Left);
        check(d.values.gazeDeadzone == 0 && menu_lines(d)[10].value == L"Off (follows every sample)", "down to 0: off");
        d.values.eyeTracking = false;
        check(menu_lines(d)[10].value == L"Eye tracking off", "and says so without eye tracking");
        const std::wstring deadzoneIni = std::wstring(temp) + L"sim-qvfr-menu-test-deadzone.ini";
        DeleteFileW(deadzoneIni.c_str());
        check(read_gaze_deadzone(deadzoneIni.c_str()) == kGazeDeadzoneDefault, "unset reads as the default");
        d.values.gazeDeadzone = 25;
        write_menu_values(deadzoneIni.c_str(), d.values, true);
        wchar_t text[16] = {};
        GetPrivateProfileStringW(L"quad", L"gaze_deadzone", L"", text, 16, deadzoneIni.c_str());
        check(std::wstring(text) == L"2.5" && read_menu_values(deadzoneIni.c_str()).gazeDeadzone == 25,
              "written as degrees (2.5) and read back");
        WritePrivateProfileStringW(L"quad", L"gaze_deadzone", L"9", deadzoneIni.c_str());
        check(read_gaze_deadzone(deadzoneIni.c_str()) == kGazeDeadzoneMax, "out-of-range clamps to 5°");
        DeleteFileW(deadzoneIni.c_str());
    }

    // The Overlay and menu page: the overlay and both panels' places, stepped and written to [osd] and
    // [menu] for the layer to move them, then the hotkeys footer.
    SettingsMenu moving;
    moving.page = 4;
    check(menu_title(moving) == L"Overlay and menu" && menu_subtitle(moving).empty() && menu_page_items(moving).size() == 9,
          "the Overlay and menu page");
    const std::vector<MenuLine> placeLines = menu_lines(moving);
    check(placeLines[1].label == L"Performance overlay" && placeLines[1].value == L"Off" && placeLines[2].value == L"0°" &&
              placeLines[3].value == L"18° up" && placeLines[4].value == L"26°" && placeLines[5].value == L"0°" &&
              placeLines[6].value == L"4° down" && placeLines[7].value == L"33°",
          "the overlay's and the menu's places in degrees");
    moving.selected = 1;
    menu_key(moving, MenuKey::Right);  // overlay on
    moving.selected = 2;
    for (int i = 0; i < 3; ++i)
        menu_key(moving, MenuKey::Right);
    moving.selected = 3;
    menu_key(moving, MenuKey::Left);
    moving.selected = 5;  // Menu left / right
    menu_key(moving, MenuKey::Left);
    menu_key(moving, MenuKey::Left);
    moving.selected = 7;  // Menu size
    menu_key(moving, MenuKey::Right);
    const auto movingIni = (std::filesystem::temp_directory_path() / L"simqvfr_menu_moving.ini").wstring();
    write_menu_values(movingIni.c_str(), moving.values, true);
    const MenuPlacement moved = read_menu_placement(movingIni.c_str());
    const OsdPlacement overlayMoved = read_osd_placement(movingIni.c_str());
    const MenuValues reread = read_menu_values(movingIni.c_str());
    DeleteFileW(movingIni.c_str());
    check(overlayMoved == OsdPlacement{true, 3, 17, 26} && reread.overlay == overlayMoved &&
              menu_lines(moving)[2].value == L"3° right",
          "the overlay turns on and moves: right 3 degrees and down 1, written to [osd] and read back");
    check(moved.yaw == -2 && moved.pitch == -4 && moved.width == 34 && reread.placement == moved &&
              menu_lines(moving)[5].value == L"2° left",
          "the menu moves itself: left 2 degrees and 1 degree wider, written to [menu] and read back");
    const std::vector<uint32_t> pixels =
        draw_menu_panel(L"QVFR settings", menu_resolution(shown), menu_stats(shown), lines, {}, L"hint");
    bool alpha = !pixels.empty(), highlight = false, bright = false, red = false;
    for (uint32_t p : pixels) {
        alpha &= (p >> 24) == 0xe6;
        highlight |= (p & 0xffffff) == 0x786e0a;  // kHighlight 0x0a6e78 as RGBA bytes (R in the low byte)
        bright |= (p & 0xff) > 0xc0 && ((p >> 8) & 0xff) > 0xc0;
        red |= (p & 0xff) > 0xc0 && ((p >> 8) & 0xff) < 0x90;
    }
    check(pixels.size() == static_cast<size_t>(kPanelWidth) * kPanelHeight && alpha && highlight && bright && !red,
          "the panel draws its background, the selected row and text; nothing red when everything fits");
    run.launch.nativeWidth = 5190;
    run.values = MenuValues{};
    run.values.periphery = 30;
    const std::vector<uint32_t> cappedPixels =
        draw_menu_panel(L"QVFR settings", menu_resolution(run), menu_stats(run), menu_lines(run), menu_notes(run), L"hint");
    bool cappedRed = false;
    for (uint32_t p : cappedPixels)
        cappedRed |= (p & 0xff) > 0xc0 && ((p >> 8) & 0xff) < 0x90;
    check(cappedRed, "a capped value and its note draw in red");

    // The panel's place: the defaults keep its first pose; turned, it still faces the eye.
    const MenuPose first = menu_pose(MenuPlacement{}, static_cast<float>(kPanelHeight) / kPanelWidth);
    check(std::fabs(first.width - 0.48f) < 0.01f && std::fabs(first.position[1] + 0.056f) < 0.005f &&
              std::fabs(first.position[2] + 0.8f) < 0.005f && std::fabs(first.position[0]) < 1e-6f,
          "menu placement: the defaults are the panel's first place (0.48 m wide, a little low, 0.8 m ahead)");
    const MenuPose turned = menu_pose(MenuPlacement{30, 20, 40}, 0.75f);
    // The quad's +Z, rotated by its orientation, points back along -position.
    const float qx = turned.orientation[0], qy = turned.orientation[1], qz = turned.orientation[2], qw = turned.orientation[3];
    const float zx = 2 * (qx * qz + qw * qy), zy = 2 * (qy * qz - qw * qx), zz = 1 - 2 * (qx * qx + qy * qy);
    check(turned.position[0] > 0 && turned.position[1] > 0 && std::fabs(zx + turned.position[0] / kMenuDistance) < 1e-4f &&
              std::fabs(zy + turned.position[1] / kMenuDistance) < 1e-4f &&
              std::fabs(zz + turned.position[2] / kMenuDistance) < 1e-4f,
          "menu placement: right of and above ahead, the panel faces the eye");
    const auto placementIni = (std::filesystem::temp_directory_path() / L"simqvfr_menu_placement.ini").wstring();
    WritePrivateProfileStringW(L"menu", L"yaw", L"-50", placementIni.c_str());
    WritePrivateProfileStringW(L"menu", L"pitch", L"-7", placementIni.c_str());
    const MenuPlacement read = read_menu_placement(placementIni.c_str());
    DeleteFileW(placementIni.c_str());
    check(read.yaw == -kMenuYawMax && read.pitch == -7 && read.width == 33,
          "menu placement: [menu] reads negative degrees, clamps them and defaults the rest");

    // Foveated DLSS (Cheeky running): a page switch first, then the method's items or Cheeky's.
    {
        SettingsMenu c;
        c.launch = {true, 50, 39, 39};
        check(menu_pages(c).size() == 5 && menu_subtitle(c).empty(), "without Cheeky: no Cheeky pages");
        {
            // A game without the FOV crop, the helmet or live resolution (ACC): no View crop or Helmet page, and a note
            // that resolution changes apply at the next launch.
            SettingsMenu g = c;
            g.launch.cropPage = g.launch.helmetPage = g.launch.liveResolution = false;
            bool crop = false, helmet = false;
            for (const MenuPage p : menu_pages(g)) {
                crop |= p == MenuPage::Crop;
                helmet |= p == MenuPage::Helmet;
            }
            bool next = false;
            for (const std::wstring& n : menu_notes(g))
                next |= n.find(L"next launch") != std::wstring::npos;
            check(menu_pages(g).size() == 3 && !crop && !helmet && next,
                  "a game without crop, helmet or live resolution: their pages left out, resolution at the next launch");
        }
        {
            // X-Plane 12, its pages from its GameFeatures as the layer sets them (xr_layer.cpp): no FOV crop, helmet or
            // Foveated DLSS, and the layer never loads Cheeky there, so no Foveated DLSS or Frame generation page.
            SettingsMenu g = c;
            const games::GameFeatures& f = xplane::kGame.features;
            g.launch.cropPage = f.fovCrop;
            g.launch.helmetPage = f.helmet;
            g.launch.fgPage = f.frameGeneration;
            g.cheeky.available = f.foveatedDlss && g.cheeky.available;
            check(!f.fovCrop && !f.helmet && !f.foveatedDlss &&
                      menu_pages(g) == std::vector<MenuPage>{MenuPage::Settings, MenuPage::Shape, MenuPage::Headset},
                  "X-Plane 12: Settings, Shape and Headset only (no crop, helmet or Foveated DLSS pages)");
        }
        {
            // Object detail: only a game whose GameFeatures has it (X-Plane), as the layer sets launch.objectDetail.
            const auto has_detail = [](const SettingsMenu& s) {
                const std::vector<MenuItem> items = menu_page_items(s);
                return std::find(items.begin(), items.end(), MenuItem::ObjectDetail) != items.end();
            };
            check(xplane::kGame.features.objectDetail && !ace::kGame.features.objectDetail &&
                      !acc::kGame.features.objectDetail,
                  "games: only X-Plane has Object detail");
            SettingsMenu other = c;
            check(!other.launch.objectDetail && !has_detail(other), "Object detail: off by default, so no other game shows it");
            for (const games::GameInfo* game : {&ace::kGame, &acc::kGame}) {
                SettingsMenu g = c;
                g.launch.objectDetail = game->features.objectDetail;
                check(!has_detail(g), "Object detail: not on ACE's or ACC's settings page");
            }
            SettingsMenu x = c;
            x.launch.objectDetail = xplane::kGame.features.objectDetail;
            const std::vector<MenuItem> items = menu_page_items(x);
            const auto density = std::find(items.begin(), items.end(), MenuItem::Density);
            check(density != items.end() && density + 1 != items.end() && density[1] == MenuItem::ObjectDetail &&
                      items.size() == menu_page_items(other).size() + 1,
                  "X-Plane: Object detail on the settings page, right after Focus density");
            x.selected = static_cast<int>(density - items.begin()) + 1;
            check(menu_lines(x)[x.selected].label == L"Object detail" && menu_lines(x)[x.selected].value == L"100%",
                  "Object detail's line: 100% by default");
            check(!menu_key(x, MenuKey::Right) && x.values.objectDetail == 100, "Object detail: Right at 100% stays there");
            check(menu_key(x, MenuKey::Left) && x.values.objectDetail == 90, "Object detail: Left steps 10%");
            for (int i = 0; i < 12; ++i)
                menu_key(x, MenuKey::Left);
            check(x.values.objectDetail == 0, "Object detail: down to 0%, no lower");
            SettingsMenu loop = x;
            loop.launch.qvfr = false;
            check(!has_detail(loop), "Object detail: QVFR only (not in Two-loop)");
        }
        {
            // Sharpening: a game whose GameFeatures has it (all four), after Focus density (and Object detail).
            const auto find = [](const SettingsMenu& s, MenuItem item) {
                const std::vector<MenuItem> items = menu_page_items(s);
                return std::find(items.begin(), items.end(), item) - items.begin();
            };
            const auto shown = [&](const SettingsMenu& s) {
                return find(s, MenuItem::Sharpening) < static_cast<std::ptrdiff_t>(menu_page_items(s).size());
            };
            check(lmu::kGame.features.sharpening && ace::kGame.features.sharpening && acc::kGame.features.sharpening &&
                      xplane::kGame.features.sharpening,
                  "games: all four have Sharpening");
            SettingsMenu other = c;
            check(!other.launch.sharpening && !shown(other), "Sharpening: off by default, so a game without it doesn't show it");
            SettingsMenu l = c;
            l.launch.sharpening = lmu::kGame.features.sharpening;
            check(shown(l) && find(l, MenuItem::Sharpening) == find(l, MenuItem::Density) + 1 &&
                      menu_page_items(l).size() == menu_page_items(other).size() + 1,
                  "LMU: Sharpening on the settings page, right after Focus density");
            SettingsMenu both = l;
            both.launch.objectDetail = true;
            check(find(both, MenuItem::Sharpening) == find(both, MenuItem::ObjectDetail) + 1,
                  "Sharpening: after Object detail when a game has both");
            l.selected = static_cast<int>(find(l, MenuItem::Sharpening));
            check(menu_lines(l)[l.selected].label == L"Sharpening" && menu_lines(l)[l.selected].value == L"Off",
                  "Sharpening's line: Off by default");
            check(!menu_key(l, MenuKey::Left) && l.values.sharpen == 0, "Sharpening: Left at Off stays there");
            check(menu_key(l, MenuKey::Right) && l.values.sharpen == 5 && menu_lines(l)[l.selected].value == L"5%",
                  "Sharpening: Right steps 5%, shown in percent");
            for (int i = 0; i < 30; ++i)
                menu_key(l, MenuKey::Right);
            check(l.values.sharpen == 100, "Sharpening: up to 100%, no higher");
            SettingsMenu loop = l;
            loop.launch.qvfr = false;
            check(!shown(loop), "Sharpening: QVFR only (not in Two-loop)");
        }
        {
            // focus_layer=2's test line: only with launch.focusQuads, right after Gaze dead-zone, in steps of metres.
            SettingsMenu q = c;
            const std::vector<MenuItem> plain = menu_page_items(q);
            check(std::find(plain.begin(), plain.end(), MenuItem::FocusQuadDistance) == plain.end(),
                  "Focus quad distance: not shown without focus_layer=2");
            q.launch.focusQuads = true;
            const std::vector<MenuItem> items = menu_page_items(q);
            const auto dz = std::find(items.begin(), items.end(), MenuItem::GazeDeadzone);
            check(dz != items.end() && dz + 1 != items.end() && dz[1] == MenuItem::FocusQuadDistance,
                  "Focus quad distance: right after Gaze dead-zone with focus_layer=2");
            q.selected = static_cast<int>(dz - items.begin()) + 1;
            check(menu_lines(q)[q.selected].label == L"Focus quad distance" && menu_lines(q)[q.selected].value == L"100 m",
                  "Focus quad distance's line: 100 m by default");
            check(menu_key(q, MenuKey::Left) && menu_lines(q)[q.selected].value == L"50 m",
                  "Focus quad distance: Left steps down to 50 m");
            for (int i = 0; i < 20; ++i)
                menu_key(q, MenuKey::Left);
            check(menu_lines(q)[q.selected].value == L"1 m" && !menu_key(q, MenuKey::Left),
                  "Focus quad distance: down to 1 m, and no further");
        }
        {
            // The runtime composing the views (Pimax, Varjo): the settings page says the focus area shows inside the
            // runtime's focus region, unless the optimizer has something to say there.
            SettingsMenu n = c;
            check(menu_subtitle(n).empty(), "runtime composition off: no note");
            n.launch.runtimeComposes = true;
            check(menu_subtitle(n).find(L"inside its own focus region") != std::wstring::npos,
                  "runtime composing: the settings page notes the focus area shows inside the runtime's focus region");
            n.optimizeDetail = L"Measuring… keep driving";
            check(menu_subtitle(n) == L"Measuring… keep driving", "runtime composing: the optimizer's line comes first");
        }
        c.cheeky = {true, L"0.6.3-standalone", CheekySettings{}};
        {
            // A game without Cheeky's frame generation (ACC): its Foveated DLSS page, no Frame generation page.
            SettingsMenu g = c;
            g.launch.fgPage = false;
            bool dlss = false, fg = false;
            for (const MenuPage p : menu_pages(g)) {
                dlss |= p == MenuPage::Dlss;
                fg |= p == MenuPage::Fg;
            }
            check(dlss && !fg && menu_pages(g).size() == menu_pages(c).size() - 1,
                  "a game without frame generation: the Foveated DLSS page, not the Frame generation one");
        }
        check(acc::kGame.features.foveatedDlss && !acc::kGame.features.frameGeneration &&
                  ace::kGame.features.frameGeneration,
              "games: ACC has Foveated DLSS but not its frame generation yet; ACE has both");
        check(menu_pages(c).size() == 7 && menu_page_items(c).size() == 12 && menu_page_items(c).front() == MenuItem::Page &&
                  menu_title(c) == L"QVFR settings",
              "with Cheeky: seven pages, the first the QVFR items under the page switch");
        check(!menu_key(c, MenuKey::Right) && c.page == 1 && menu_title(c) == L"Focus shape", "then the Focus shape page");
        check(!menu_key(c, MenuKey::Right) && c.page == 2 && menu_title(c) == L"View crop", "then the View crop page");
        check(!menu_key(c, MenuKey::Right) && c.page == 3 && menu_title(c) == L"Helmet", "then the Helmet page");
        check(!menu_key(c, MenuKey::Right) && c.page == 4 && c.selected == 0 && menu_page_items(c).size() == 11 &&
                  menu_title(c) == L"Foveated DLSS" && menu_subtitle(c) == L"CheekyFoveatedDLSS - v0.6.3",
              "the switch turns to the Foveated DLSS page, titled, with Cheeky's credit line and version");
        menu_key(c, MenuKey::Down);  // Foveated DLSS on/off
        check(menu_key(c, MenuKey::Right) && !c.cheeky.values.processing, "Foveated DLSS turns off");
        menu_key(c, MenuKey::Down);
        menu_key(c, MenuKey::Down);  // outer scale
        for (int i = 0; i < 30; ++i)
            menu_key(c, MenuKey::Left);
        check(c.cheeky.values.outerScale == kCheekyScaleMin, "outer DLAA scale stops at Cheeky's minimum");
        menu_key(c, MenuKey::Down);  // outer preset: E, J, K, L, M, never the game default
        bool neverDefault = true;
        for (int i = 0; i < 6; ++i) {
            menu_key(c, MenuKey::Right);
            neverDefault &= c.cheeky.values.outerPreset != 0;
        }
        check(neverDefault && c.cheeky.values.outerPreset == 10, "the outer preset cycles without the game default");
        menu_key(c, MenuKey::Down);  // focus preset: from the game default back to M
        menu_key(c, MenuKey::Left);
        check(c.cheeky.values.focusPreset == 13 && std::wstring(cheeky_preset_name(13)) == L"M",
              "the focus preset wraps from the game default to M");
        const std::vector<MenuLine> lines = menu_lines(c);
        check(lines.size() == 11 && lines[0].value == L"Foveated DLSS" && lines[1].value == L"Off" &&
                  lines[3].value == L"20%" && lines[5].value == L"M" && menu_notes(c).empty() &&
                  !menu_resolution(c).shown,
              "the page's lines show Cheeky's values; no resolution or notes there");
        const std::string set = cheeky_set_lines(c.cheeky.values, true);
        check(set.find("Enabled=0\n") == 0 && set.find("PeripheralDlaaScale=0.20") != std::string::npos &&
                  set.find("CenterPreset=13") != std::string::npos && set.find("QuadView4Role=4") != std::string::npos &&
                  cheeky_set_lines(c.cheeky.values, false).find("Quad") == std::string::npos,
              "Cheeky's set lines carry the values, and the quad roles only for QVFR");
        const auto cheekyIni = (std::filesystem::temp_directory_path() / L"simqvfr_menu_cheeky.ini").wstring();
        DeleteFileW(cheekyIni.c_str());
        write_cheeky_settings(cheekyIni.c_str(), c.cheeky.values, false);
        const CheekySettings back = read_cheeky_settings(cheekyIni.c_str());
        DeleteFileW(cheekyIni.c_str());
        check(back == c.cheeky.values && !back.load, "[cheeky]'s live keys round-trip; enabled is the launcher's");
        c.page = 0;
        c.launch.dlssHeld = true;
        check(!menu_notes(c).empty() && menu_notes(c).front().find(L"DLSS") == 0,
              "with DLSS holding the views, page 0 says resolution waits for the next launch");
        const std::vector<uint32_t> page = draw_menu_panel(menu_title(c), menu_resolution(c), menu_stats(c),
                                                           menu_lines(c), menu_notes(c), L"hint", L"credit");
        check(page.size() == static_cast<size_t>(kPanelWidth) * kPanelHeight, "the panel draws 12 lines and a subtitle");

        // The experimental focus-view options: supersampling, motion-vector fix, DLSS-NR.
        c.page = 4;
        c.selected = 6;  // focus supersampling
        for (int i = 0; i < 30; ++i)
            menu_key(c, MenuKey::Right);
        check(c.cheeky.values.supersampling == kCheekySupersamplingMax, "focus supersampling stops at 200%");
        menu_key(c, MenuKey::Down);
        menu_key(c, MenuKey::Right);
        menu_key(c, MenuKey::Down);
        menu_key(c, MenuKey::Right);
        menu_key(c, MenuKey::Down);
        menu_key(c, MenuKey::Left);  // NR preset: from Default back to B
        menu_key(c, MenuKey::Down);
        for (int i = 0; i < 5; ++i)
            menu_key(c, MenuKey::Right);
        const std::vector<MenuLine> more = menu_lines(c);
        check(c.cheeky.values.motionFix && c.cheeky.values.nr && c.cheeky.values.nrPreset == 2 &&
                  c.cheeky.values.nrPasses == kCheekyNrPassesMax && more[6].value == L"200%" &&
                  more[9].value == L"B (Faster)" && more[10].value == L"3",
              "motion-vector fix and DLSS-NR turn on; the NR preset wraps; NR passes stop at 3");
        const std::string nrSet = cheeky_set_lines(c.cheeky.values, true);
        check(nrSet.find("CenterSupersampling=2.00") != std::string::npos && nrSet.find("FixMotionBlur=1") != std::string::npos &&
                  nrSet.find("NrEnabled=1") != std::string::npos && nrSet.find("NrPreset=2") != std::string::npos &&
                  nrSet.find("NrPasses=3") != std::string::npos,
              "Cheeky's set lines carry supersampling, the motion fix and DLSS-NR");
        {
            CheekySettings off = c.cheeky.values;
            off.nr = false;
            check(cheeky_set_lines(c.cheeky.values, true, false, true).find("D3D11D3D12Transport=1") != std::string::npos &&
                      cheeky_set_lines(off, true, false, true).find("D3D11D3D12Transport=0") != std::string::npos &&
                      nrSet.find("D3D11D3D12Transport") == std::string::npos,
                  "a D3D11 game: Cheeky's DX12 transport on only with DLSS-NR; a D3D12 game never sets it");
        }
        const auto nrIni = (std::filesystem::temp_directory_path() / L"simqvfr_menu_cheeky_nr.ini").wstring();
        DeleteFileW(nrIni.c_str());
        write_cheeky_settings(nrIni.c_str(), c.cheeky.values, false);
        const CheekySettings nrBack = read_cheeky_settings(nrIni.c_str());
        DeleteFileW(nrIni.c_str());
        check(nrBack == c.cheeky.values, "the experimental keys round-trip through [cheeky]");

        // Frame generation: the third page, reached either way round from the page switch.
        c.page = 0;
        c.selected = 0;
        menu_key(c, MenuKey::Left);
        check(c.page == 6 && menu_title(c) == L"Overlay and menu", "Left from the first page wraps to Overlay and menu");
        menu_key(c, MenuKey::Left);
        check(c.page == 5 && menu_title(c) == L"Frame generation" && menu_page_items(c).size() == 8 &&
                  menu_subtitle(c) == L"CheekyFoveatedDLSS - v0.6.3" && menu_notes(c).empty(),
              "then Frame generation, with the credit line; no note while FG is off");
        menu_key(c, MenuKey::Down);
        menu_key(c, MenuKey::Right);  // FG on
        menu_key(c, MenuKey::Down);
        for (int i = 0; i < 5; ++i)
            menu_key(c, MenuKey::Right);  // multiplier stops at 4x
        menu_key(c, MenuKey::Down);
        menu_key(c, MenuKey::Down);
        for (int i = 0; i < 5; ++i)
            menu_key(c, MenuKey::Left);  // centre FG scale stops at 25%
        const std::vector<MenuLine> fgLines = menu_lines(c);
        check(c.cheeky.values.fg && c.cheeky.values.fgMultiplier == kCheekyFgMultiplierMax &&
                  c.cheeky.values.fgScale == 25 && fgLines[2].value == L"4x" && fgLines[4].value == L"25%" &&
                  !menu_notes(c).empty() && menu_notes(c).front().find(L"Turbo") != std::wstring::npos,
              "FG turns on; its multiplier and scales stop at Cheeky's limits; the page warns about Turbo mode");
        const std::string fgSet = cheeky_set_lines(c.cheeky.values, true);
        check(fgSet.find("FgEnabled=1") != std::string::npos && fgSet.find("FgMultiplier=4") != std::string::npos &&
                  fgSet.find("FgReleasePoint=8") != std::string::npos && fgSet.find("FgWidth=0.30") != std::string::npos &&
                  fgSet.find("FgResolutionPercent=25") != std::string::npos && fgSet.find("FgCenterMethod=0") != std::string::npos,
              "Cheeky's set lines carry FG with the settings that work in ACE (DLSS-G, release after capture)");
        check(fgSet.find("CenterMode=0") != std::string::npos &&
                  cheeky_set_lines(c.cheeky.values, true, true).find("CenterMode=1") != std::string::npos,
              "the FG region follows the eyes (CenterMode 1) only when QVFR's focus does");
        // When Cheeky releases ACE: Capture (8) by default, then Generated (3); Never (5) the other way; the set lines carry it.
        c.selected = 6;
        const bool captureFirst = menu_lines(c)[6].label == L"Release game" && menu_lines(c)[6].value == L"Capture (8)" &&
                                  cheeky_set_lines(c.cheeky.values, true).find("FgReleasePoint=8") != std::string::npos;
        menu_key(c, MenuKey::Right);
        const bool generated = c.cheeky.values.fgRelease == 3 && menu_lines(c)[6].value == L"Generated (3)" &&
                               !menu_key(c, MenuKey::Right);  // stops at the end
        menu_key(c, MenuKey::Left);
        menu_key(c, MenuKey::Left);
        check(captureFirst && generated && c.cheeky.values.fgRelease == 5 && menu_lines(c)[6].value == L"Never (5)" &&
                  cheeky_set_lines(c.cheeky.values, true).find("FgReleasePoint=5") != std::string::npos,
              "Release game: Capture (8), Generated (3), Never (5); Cheeky's FgReleasePoint follows");
        const auto releaseIni = (std::filesystem::temp_directory_path() / L"simqvfr_menu_release.ini").wstring();
        DeleteFileW(releaseIni.c_str());
        write_cheeky_settings(releaseIni.c_str(), c.cheeky.values, false);
        const bool roundTrip = read_cheeky_settings(releaseIni.c_str()).fgRelease == 5;
        WritePrivateProfileStringW(L"cheeky", L"fg_release", L"4", releaseIni.c_str());
        const bool unknownIsCapture = read_cheeky_settings(releaseIni.c_str()).fgRelease == 8;
        DeleteFileW(releaseIni.c_str());
        check(roundTrip && unknownIsCapture, "fg_release round-trips through [cheeky]; a value not offered is 8");
        c.cheeky.values.fgRelease = 8;
        c.selected = 4;  // back on Center FG scale, where the steps above left it
        c.cheeky.values.fgBorder = true;
        check(cheeky_set_lines(c.cheeky.values, true, true).find("FgShowBorder=1") != std::string::npos &&
                  menu_lines(c).back().label == L"Show FG region" && menu_lines(c).back().value == L"On",
              "the FG region outline is the page's last row and reaches Cheeky as FgShowBorder");
        menu_key(c, MenuKey::Up);
        menu_key(c, MenuKey::Up);
        menu_key(c, MenuKey::Up);
        menu_key(c, MenuKey::Up);  // back on the page switch
        menu_key(c, MenuKey::Right);
        menu_key(c, MenuKey::Right);
        check(c.page == 0 && menu_title(c) == L"QVFR settings", "Right from Frame generation, then Overlay and menu, wraps to the first page");
    }

    // The View crop page (QVFR): off shows only the switch; on, four sides, each on its own, written to
    // [crop]; the outer views shrink with it in the fit and the pixels saved.
    {
        SettingsMenu v;
        v.launch = {true, 50, 39, 39, 2000, 2000};
        v.page = 2;
        check(menu_title(v) == L"View crop" && menu_page_items(v).size() == 2 && menu_resolution(v).shown,
              "View crop: off, only the switch (and the resolution)");
        const int savedBefore = menu_stats(v).savedPercent;
        v.selected = 1;
        check(menu_key(v, MenuKey::Right) && v.values.crop && menu_page_items(v).size() == 6, "View crop turns on: four sides");
        v.selected = 2;  // top
        for (int i = 0; i < 50; ++i)
            menu_key(v, MenuKey::Right);
        v.selected = 4;  // outer side
        for (int i = 0; i < 10; ++i)
            menu_key(v, MenuKey::Right);
        check(v.values.cropTop == 40 && v.values.cropBottom == 0 && v.values.cropOuter == 10 && v.values.cropInner == 0,
              "View crop: the top stops at 40%; the other sides stay as they were");
        check(menu_stats(v).savedPercent > savedBefore && menu_lines(v)[2].value == L"40%" &&
                  menu_resolution(v).current == L"3360x780 (2.6 MP)",
              "View crop: fewer pixels; 900x600 outer views beside the 780x780 focus views");
        check(menu_notes(v).empty(),
              "View crop: no note when it only takes more off than at launch");
        v.launch.dlssHeld = true;
        check(menu_notes(v).front().find(L"DLSS is on") == 0, "View crop with DLSS: the savings wait for the next launch");
        const auto cropIni = (std::filesystem::temp_directory_path() / L"simqvfr_menu_crop.ini").wstring();
        DeleteFileW(cropIni.c_str());
        write_menu_values(cropIni.c_str(), v.values, true);
        const MenuValues back = read_menu_values(cropIni.c_str());
        DeleteFileW(cropIni.c_str());
        check(back.crop && back.cropTop == 40 && back.cropOuter == 10 && back.cropBottom == 0, "View crop: [crop] round-trips");
        // The Helmet page: on, its rim, corners and lighting stepped, written to [helmet].
        v.page = 3;
        v.selected = 1;
        check(menu_title(v) == L"Helmet" && menu_key(v, MenuKey::Right) && v.values.helmet.on &&
                  menu_page_items(v).size() == 6 && menu_lines(v)[1].value == L"On" && menu_notes(v).empty(),
              "Helmet overlay turns on: rim, corners, lighting and the light diagnostic");
        v.selected = 2;
        for (int i = 0; i < 30; ++i)
            menu_key(v, MenuKey::Right);
        v.selected = 4;
        menu_key(v, MenuKey::Left);
        v.selected = 5;  // the light diagnostic
        menu_key(v, MenuKey::Right);
        write_menu_values(cropIni.c_str(), v.values, true);
        const HelmetSettings helmetBack = read_helmet_settings(cropIni.c_str());
        DeleteFileW(cropIni.c_str());
        check(helmetBack.on && helmetBack.rim == kHelmetRimMax && helmetBack.light == 60 && helmetBack.corners == 40 &&
                  helmetBack.debugLight,
              "Helmet: the rim stops at its maximum, lighting steps by 10, the light diagnostic; [helmet] round-trips");
        // With the helmet's own images: visor, size, height and depth instead of rim and corners.
        v.launch.helmetPieces = true;
        const std::vector<MenuItem> pieceItems = menu_page_items(v);
        check(pieceItems.size() == 9 && pieceItems[2] == MenuItem::Visor && pieceItems[3] == MenuItem::HelmetSize &&
                  pieceItems[4] == MenuItem::HelmetHeight && pieceItems[5] == MenuItem::HelmetDepth &&
                  pieceItems[6] == MenuItem::HelmetDetail,
              "Helmet with images: visor, size, height, depth and detail rows");
        v.selected = 6;
        const bool performance = menu_lines(v)[6].value == L"Performance";
        check(performance && menu_key(v, MenuKey::Right) && v.values.helmet.detail && menu_lines(v)[6].value == L"Detail",
              "Helmet detail: Performance (2048) by default, Detail (4096) at a step");
        v.values.helmet.detail = false;
        v.selected = 2;
        menu_key(v, MenuKey::Right);
        menu_key(v, MenuKey::Right);
        menu_key(v, MenuKey::Right);  // stops at open
        v.selected = 4;
        menu_key(v, MenuKey::Left);
        check(v.values.helmet.visor == VisorPosition::Open && menu_lines(v)[2].value == L"Open" &&
                  v.values.helmet.height == -1 && menu_lines(v)[4].value == L"1° down",
              "Visor steps to Open and stops; height in degrees");
        // Depth: far, then 3 cm, 1 cm a step up close and 5 cm from 20 cm; back down to far.
        v.selected = 5;
        const bool startsFar = menu_lines(v)[5].value == L"Far";
        menu_key(v, MenuKey::Right);
        const bool nearest = v.values.helmet.depth == kHelmetDepthMin && menu_lines(v)[5].value == L"3 cm";
        for (int i = 0; i < 17; ++i)
            menu_key(v, MenuKey::Right);
        const bool twenty = v.values.helmet.depth == 20;
        menu_key(v, MenuKey::Right);
        const bool coarse = v.values.helmet.depth == 25;
        for (int i = 0; i < 60; ++i)
            menu_key(v, MenuKey::Left);
        check(startsFar && nearest && twenty && coarse && v.values.helmet.depth == 0,
              "Helmet depth: Far, then 3 cm, 1 cm steps to 20 cm, then 5 cm; down again to Far");
        v.launch.helmetPieces = false;
        v.page = 2;  // back on View crop
        v.launch.dlssHeld = false;
        v.launch.cropHeight = 50;  // launched with more off: less now is softer until the next launch
        check(!menu_notes(v).empty() && menu_notes(v).back().find(L"Less crop") == 0, "View crop: less than at launch is noted");
    }

    // The Focus shape page (both methods): a box by default, only the switch; corners, its eight insets,
    // stepped by 5 up to kFocusCornerMax, written to [quad] and read back.
    {
        SettingsMenu f;
        f.launch.qvfr = false;
        f.page = 1;
        check(menu_title(f) == L"Focus shape" && menu_page_items(f).size() == 2 && menu_lines(f)[1].value == L"Box" &&
                  !menu_notes(f).empty(),
              "Focus shape: a box, only the switch, and a note on what Corners does");
        f.selected = 1;
        check(menu_key(f, MenuKey::Right) && f.values.shape.corners && menu_page_items(f).size() == 10 &&
                  menu_lines(f)[1].value == L"Corners" && menu_lines(f)[2].label == L"Top left in",
              "Focus shape: Corners lists each corner's insets, top left first");
        f.selected = 2;  // top left in
        for (int i = 0; i < 20; ++i)
            menu_key(f, MenuKey::Right);
        f.selected = 4;  // top right in
        for (int i = 0; i < 4; ++i)
            menu_key(f, MenuKey::Right);
        f.selected = 9;  // bottom right up
        menu_key(f, MenuKey::Right);
        const FocusShape& sh = f.values.shape;
        check(sh.in[static_cast<int>(FocusCorner::TopLeft)] == kFocusCornerMax &&
                  sh.in[static_cast<int>(FocusCorner::TopRight)] == 20 &&
                  sh.vertical[static_cast<int>(FocusCorner::BottomRight)] == 5 &&
                  sh.in[static_cast<int>(FocusCorner::BottomLeft)] == 0 && menu_lines(f)[4].value == L"20%",
              "Focus shape: corners step by 5 and stop at 45%; the others stay");
        f.page = 0;
        const std::vector<MenuLine> first = menu_lines(f);
        check(first[4].label == L"Corners" && first[4].value == L"Corner shape",
              "the settings page's corner radius says the corner shape replaces it");
        const auto shapeIni = (std::filesystem::temp_directory_path() / L"simqvfr_menu_shape.ini").wstring();
        DeleteFileW(shapeIni.c_str());
        write_menu_values(shapeIni.c_str(), f.values, false);
        const FocusShape back = read_focus_shape(shapeIni.c_str());
        wchar_t text[16] = {};
        GetPrivateProfileStringW(L"quad", L"shape_top_right_in", L"", text, 16, shapeIni.c_str());
        WritePrivateProfileStringW(L"quad", L"shape_bottom_left_up", L"80", shapeIni.c_str());
        const FocusShape clamped = read_focus_shape(shapeIni.c_str());
        DeleteFileW(shapeIni.c_str());
        check(back == sh && std::wstring(text) == L"20" &&
                  clamped.vertical[static_cast<int>(FocusCorner::BottomLeft)] == kFocusCornerMax,
              "Focus shape: [quad] focus_shape and its corners round-trip; out of range clamps to 45%");
        // The bits the layer keeps and the compositor's shares.
        float inset[2 * kFocusCorners];
        focus_shape_insets(focus_shape_bits(sh), inset);
        FocusShape box = sh;
        box.corners = false;
        check(inset[0] == 0.45f && inset[1] == 0 && inset[2] == 0.2f && inset[5] == 0.05f && focus_shape_bits(box) == 0,
              "Focus shape: the compositor gets shares of the box; a box sends none");
        f.values.shape.corners = false;
        f.page = 1;
        f.selected = 1;
        check(menu_key(f, MenuKey::Left) && f.values.shape.corners && f.values.shape.in[0] == kFocusCornerMax,
              "Focus shape: back to Corners keeps the corners set before");
    }
    {  // The Profile line (profile/profiles.h): first on the settings page, stepping through and asking the layer.
        SettingsMenu p;
        p.launch = {true, 50, 39, 39};
        check(menu_page_items(p)[1] != MenuItem::Profile, "Profile: no line without profiles");
        p.profiles = {L"Performance", L"Balanced", L"Quality", L"Night"};
        p.selected = 1;
        check(menu_page_items(p)[1] == MenuItem::Profile && menu_lines(p)[1].label == L"Profile" &&
                  menu_lines(p)[1].value == L"Your own",
              "Profile: the settings page's first line; none in use shows as your own");
        const MenuValues before = p.values;
        check(!menu_key(p, MenuKey::Right) && p.profile == 0 && p.profileRequest == 0 && p.values.periphery == before.periphery &&
                  menu_lines(p)[1].value == L"Performance",
              "Profile: Right from none takes the first, asks the layer to use it, changes no value itself");
        p.profileRequest = -1;
        check(!menu_key(p, MenuKey::Left) && p.profile == 3 && p.profileRequest == 3, "Profile: Left wraps to the last");
        check(!menu_key(p, MenuKey::Right) && p.profile == 0, "Profile: Right wraps to the first");
        p.profileChanged = true;
        check(menu_lines(p)[1].value == L"Performance (changed)", "Profile: changed since it was used");
        SettingsMenu none = p;
        none.profile = -1;
        check(!menu_key(none, MenuKey::Left) && none.profile == 3, "Profile: Left from none takes the last");
    }
    {  // The fullest page fits: 13 lines (the Profile line too), three notes, the footer and the hint, none over another.
        SettingsMenu full;
        full.launch = {true, 50, 39, 39, 4000, 3400};
        full.profiles = {L"Performance", L"Balanced", L"Quality"};
        const std::vector<MenuLine> lines = menu_lines(full);
        const std::vector<std::wstring> notes = {L"note one", L"note two", L"note three"};
        const std::vector<uint32_t> page = draw_menu_panel(menu_title(full), menu_resolution(full), menu_stats(full), lines, notes,
                                                           L"hint", L"", L"Overlay Ctrl+Alt+P  ·  Visor Ctrl+Alt+V");
        // Rows with text: any pixel brighter than the background. The notes end above the footer's band, which has text.
        const auto row_has_text = [&page](int y) {
            for (int x = 36; x < kPanelWidth - 36; ++x)
                if ((page[static_cast<size_t>(y) * kPanelWidth + x] & 0xff) > 0x60)
                    return true;
            return false;
        };
        int lastNoteRow = 0;
        for (int y = 0; y < kPanelHeight - 100; ++y)
            if (row_has_text(y))
                lastNoteRow = y;
        bool footerText = false;
        for (int y = kPanelHeight - 96; y < kPanelHeight - 56; ++y)
            footerText |= row_has_text(y);
        check(lines.size() == 13 && lastNoteRow < kPanelHeight - 100 && footerText,
              "the panel: 13 lines and three notes end above the footer, which shows");
        if (wchar_t path[MAX_PATH] = {}; GetEnvironmentVariableW(L"SIMQVFR_MENU_SAVE", path, MAX_PATH)) {
            if (FILE* f = _wfopen(path, L"wb")) {
                const int w = kPanelWidth, h = kPanelHeight;
                BITMAPFILEHEADER file{0x4D42, static_cast<DWORD>(sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER) + w * h * 4), 0, 0,
                                      sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER)};
                BITMAPINFOHEADER info{sizeof(BITMAPINFOHEADER), w, -h, 1, 32, BI_RGB};
                fwrite(&file, sizeof(file), 1, f);
                fwrite(&info, sizeof(info), 1, f);
                for (const uint32_t p : page) {
                    const uint8_t bgra[4] = {static_cast<uint8_t>(p >> 16), static_cast<uint8_t>(p >> 8), static_cast<uint8_t>(p), 255};
                    fwrite(bgra, 4, 1, f);
                }
                fclose(f);
            }
        }
    }
    {  // The hotkeys footer ([menu] hotkey_footer): on by default, the Overlay and menu page's last line, read and written.
        SettingsMenu f;
        f.launch = {true, 50, 39, 39};
        f.page = static_cast<int>(menu_pages(f).size()) - 1;  // Overlay and menu, the last page
        const std::vector<MenuItem> items = menu_page_items(f);
        f.selected = static_cast<int>(items.size()) - 1;
        check(menu_page(f) == MenuPage::Headset && items.back() == MenuItem::HotkeyFooter && f.values.hotkeyFooter &&
                  menu_lines(f).back().label == L"Hotkeys footer" && menu_lines(f).back().value == L"On",
              "Hotkeys footer: on by default, the Overlay and menu page's last line");
        check(menu_key(f, MenuKey::Right) && !f.values.hotkeyFooter && menu_lines(f).back().value == L"Off",
              "Hotkeys footer: Right or Left turns it off and on");
        wchar_t temp[MAX_PATH];
        GetTempPathW(MAX_PATH, temp);
        const std::wstring ini = std::wstring(temp) + L"simqvfr-menu-footer.ini";
        DeleteFileW(ini.c_str());
        check(read_menu_values(ini.c_str()).hotkeyFooter && write_menu_values(ini.c_str(), f.values, true) &&
                  !read_menu_values(ini.c_str()).hotkeyFooter,
              "Hotkeys footer: unset is on; written and read back");
        DeleteFileW(ini.c_str());
    }

    // Only what changed is written (ini_put.h): WritePrivateProfileString rewrites the whole ini for every key, even
    // one that already holds the value, and a step changes one key of the ~50 the menu writes.
    {
        wchar_t temp[MAX_PATH];
        GetTempPathW(MAX_PATH, temp);
        const std::wstring ini = std::wstring(temp) + L"simqvfr-menu-only-changes.ini";
        const auto bytes = [&ini] {
            std::string text;
            if (FILE* f = _wfopen(ini.c_str(), L"rb")) {
                char buffer[4096];
                for (size_t n; (n = std::fread(buffer, 1, sizeof(buffer), f)) > 0;)
                    text.append(buffer, n);
                std::fclose(f);
            }
            return text;
        };
        const auto put_text = [&ini](const std::string& text) {
            if (FILE* f = _wfopen(ini.c_str(), L"wb")) {
                std::fwrite(text.data(), 1, text.size(), f);
                std::fclose(f);
            }
        };
        // Spaces round the value: a rewrite by Windows would make the line "focus_percent=31".
        const std::string original = "; notes\r\n[experiment]\r\nfocus_percent = 31\r\n";
        put_text(original);
        check(ini_put(ini.c_str(), L"experiment", L"focus_percent", 31) && bytes() == original,
              "ini_put: the value already there: the file is left as it is");
        check(ini_put(ini.c_str(), L"experiment", L"focus_percent", 32) &&
                  GetPrivateProfileIntW(L"experiment", L"focus_percent", 0, ini.c_str()) == 32,
              "ini_put: a different value is written");
        check(ini_put(ini.c_str(), L"quad", L"feather_percent", 0) &&
                  GetPrivateProfileIntW(L"quad", L"feather_percent", 7, ini.c_str()) == 0,
              "ini_put: a missing key is written");
        const std::wstring longValue(200, L'x');
        check(ini_put(ini.c_str(), L"x", L"long", longValue.c_str()) && ini_put(ini.c_str(), L"x", L"long", longValue.c_str()),
              "ini_put: a value longer than its read buffer is written each time");

        // A step's write: every value again, one of them changed. Only that key's line changes. A fresh file: a
        // rewrite keeps a key's own spacing ("focus_percent =32" above).
        DeleteFileW(ini.c_str());
        MenuValues v = read_menu_values(ini.c_str());
        write_menu_values(ini.c_str(), v, true);
        const std::string before = bytes();
        check(write_menu_values(ini.c_str(), v, true) && bytes() == before, "menu step: the same values again leave the ini as it is");
        v.width = 33;
        write_menu_values(ini.c_str(), v, true);
        std::string expected = before;
        const size_t at = expected.find("focus_percent=39");
        if (at != std::string::npos)
            expected.replace(at, 16, "focus_percent=33");
        check(at != std::string::npos && bytes() == expected, "menu step: only the changed key's line is rewritten");
        DeleteFileW(ini.c_str());

        // On the documented ini (34 KB): 20 steps took ~7 s when every key was rewritten (0.37 s a step).
        const std::wstring documented = std::wstring(temp) + L"simqvfr-menu-documented.ini";
        const bool copied = CopyFileW(L"" SIM_QVFR_DOCUMENTED_INI, documented.c_str(), FALSE) != 0;
        MenuValues d = read_menu_values(documented.c_str());
        const auto start = std::chrono::steady_clock::now();
        for (int i = 0; i < 20; ++i) {
            d.width = 50 + i;
            write_menu_values(documented.c_str(), d, true);
        }
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        std::printf("     20 menu steps on the documented ini: %.0f ms (%.1f ms a step)\n", ms, ms / 20);
        check(copied && read_menu_values(documented.c_str()).width == 69 && ms < 3000,
              "menu step: on the documented ini, 20 steps well under the 7 s a full rewrite took");
        DeleteFileW(documented.c_str());
    }

    // Sim-QVFR's own DLSS (engine/dlss_settings.h): its page only where the launch can run it, its choices stepped,
    // and [dlss] in the ini, read from an older file's [experiment] lmu_dlss_* keys while [dlss] lacks its own.
    {
        SettingsMenu dm;
        dm.launch = {true, 50, 39, 39};
        const auto has_page = [](const SettingsMenu& s) {
            const std::vector<MenuPage> pages = menu_pages(s);
            return std::find(pages.begin(), pages.end(), MenuPage::OwnDlss) != pages.end();
        };
        check(!has_page(dm), "DLSS page: none without a launch that can run it");
        dm.launch.ownDlss = true;
        check(has_page(dm) && menu_pages(dm).back() == MenuPage::Headset, "DLSS page: shown for the launch, before Overlay and menu");
        dm.launch.qvfr = false;
        check(!has_page(dm), "DLSS page: none in Two-loop");
        dm.launch.qvfr = true;
        const std::vector<MenuPage> pages = menu_pages(dm);
        dm.page = static_cast<int>(std::find(pages.begin(), pages.end(), MenuPage::OwnDlss) - pages.begin());
        check(menu_title(dm) == L"DLSS" && menu_page_items(dm).size() == 2 && menu_page_items(dm)[1] == MenuItem::DlssOn,
              "DLSS page: off, only its switch");
        dm.selected = 1;
        check(menu_key(dm, MenuKey::Right) && dm.values.dlss.on && menu_page_items(dm).size() == 6 &&
                  menu_lines(dm)[1].value == L"On",
              "DLSS page: on, then the focus views' mode and preset, the eyes' preset and texture sharpening");
        dm.selected = 2;  // Focus views: DLAA, then smaller renders; the ends hold
        check(!menu_key(dm, MenuKey::Left) && dm.values.dlss.focusScale == 100 && menu_lines(dm)[2].value == L"DLAA",
              "DLSS page: Left from DLAA stays at DLAA");
        for (int i = 0; i < 4; ++i)
            menu_key(dm, MenuKey::Right);
        check(dm.values.dlss.focusScale == 50 && menu_lines(dm)[2].value == L"Performance (50%)" &&
                  !menu_key(dm, MenuKey::Right),
              "DLSS page: Right steps to Performance (50%), the last");
        dm.selected = 3;
        check(menu_lines(dm)[3].value == L"Default (M)", "DLSS page: the focus preset's default named as NGX's for Performance");
        check(menu_key(dm, MenuKey::Right) && dm.values.dlss.focusPreset == 11 && menu_lines(dm)[3].value == L"K",
              "DLSS page: the focus preset steps to K");
        dm.selected = 4;
        check(dm.values.dlss.eyePreset == 11 && menu_key(dm, MenuKey::Right) && dm.values.dlss.eyePreset == 6 &&
                  menu_lines(dm)[4].value == L"F (cheaper)",
              "DLSS page: the eyes' preset K by default, F a step on");
        dm.selected = 5;  // texture sharpening: off by default, then the focus views, then the eyes too; the ends hold
        check(dm.values.dlss.textureSharpening == 0 && menu_lines(dm)[5].value == L"Off" && !menu_key(dm, MenuKey::Left),
              "DLSS page: texture sharpening off by default, Left from Off stays there");
        check(menu_key(dm, MenuKey::Right) && dm.values.dlss.textureSharpening == 1 && menu_lines(dm)[5].value == L"Focus only" &&
                  menu_key(dm, MenuKey::Right) && dm.values.dlss.textureSharpening == 2 &&
                  menu_lines(dm)[5].value == L"Focus and periphery" && !menu_key(dm, MenuKey::Right),
              "DLSS page: texture sharpening steps to Focus only, then Focus and periphery, the last");
        const auto has_note = [&](const wchar_t* start) {
            for (const std::wstring& n : menu_notes(dm))
                if (n.rfind(start, 0) == 0)
                    return true;
            return false;
        };
        check(has_note(L"Texture sharpening was off at launch"),
              "DLSS page: texture sharpening turned on with it off at launch: the note says the next launch");
        dm.launch.sharpeningReady = true;
        check(!has_note(L"Texture sharpening was off at launch"), "DLSS page: on at launch: no such note (its settings live)");

        wchar_t temp[MAX_PATH];
        GetTempPathW(MAX_PATH, temp);
        const std::wstring ini = std::wstring(temp) + L"simqvfr-menu-dlss.ini";
        DeleteFileW(ini.c_str());
        DlssSettings fresh = read_dlss_settings(ini.c_str());
        check(!fresh.on && fresh.folder.empty() && fresh.focusScale == 100 && fresh.focusPreset == 0 && fresh.eyePreset == 11 &&
                  fresh.textureSharpening == 0 && fresh.carVectors,
              "[dlss]: an empty file reads off, DLAA, NGX's default, K, no texture sharpening, the other cars' motion on");
        // An older file: [experiment] lmu_dlss=2, its folder, 50%, preset 0, the eyes on F.
        WritePrivateProfileStringW(L"experiment", L"lmu_dlss", L"2", ini.c_str());
        WritePrivateProfileStringW(L"experiment", L"lmu_dlss_folder", L"K:\\Games\\X", ini.c_str());
        WritePrivateProfileStringW(L"experiment", L"lmu_dlss_focus_scale", L"50", ini.c_str());
        WritePrivateProfileStringW(L"experiment", L"lmu_dlss_preset", L"0", ini.c_str());
        WritePrivateProfileStringW(L"experiment", L"lmu_dlss_eye_preset", L"F", ini.c_str());
        const DlssSettings old = read_dlss_settings(ini.c_str());
        check(old.on && old.folder == L"K:\\Games\\X" && old.focusScale == 50 && old.focusPreset == 0 && old.eyePreset == 6,
              "[dlss]: an older file's [experiment] lmu_dlss_* keys read in its place");
        DlssSettings mine = old;
        mine.on = false;
        mine.focusScale = 67;
        mine.focusPreset = 13;
        mine.eyePreset = 11;
        mine.textureSharpening = 2;
        mine.carVectors = false;
        check(write_dlss_settings(ini.c_str(), mine) && read_dlss_settings(ini.c_str()) == mine,
              "[dlss]: written and read back (the other cars' motion off too), ahead of the older keys");
        wchar_t text[16] = {};
        GetPrivateProfileStringW(L"dlss", L"focus_preset", L"", text, 16, ini.c_str());
        const std::wstring focusText = text;
        GetPrivateProfileStringW(L"dlss", L"eye_preset", L"", text, 16, ini.c_str());
        check(focusText == L"M" && std::wstring(text) == L"K", "[dlss]: presets written as their letters");
        WritePrivateProfileStringW(L"dlss", L"focus_scale", L"60", ini.c_str());
        WritePrivateProfileStringW(L"dlss", L"focus_preset", L"J", ini.c_str());
        WritePrivateProfileStringW(L"dlss", L"texture_sharpening", L"7", ini.c_str());
        const DlssSettings odd = read_dlss_settings(ini.c_str());
        check(odd.focusScale == 58 && odd.focusPreset == 0 && odd.textureSharpening == 2,
              "[dlss]: a scale between modes snaps to the nearest; a preset not offered reads as NGX's default; a "
              "sharpening past the last reads as the last");
        check(read_menu_values(ini.c_str()).dlss == odd, "[dlss]: the menu's values read it");
        DeleteFileW(ini.c_str());
    }

    std::printf("RESULT: %s\n", failures == 0 ? "pass" : "FAIL");
    return failures == 0 ? 0 : 1;
}
