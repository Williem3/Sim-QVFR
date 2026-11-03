// Offline check of the in-headset settings menu: selection, steps and ranges, the items per method,
// the ini round trip and the launch-size note; and that the panel image draws text and highlight.
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <string>

#include <windows.h>

#include "quad/menu_panel.h"
#include "quad/settings_menu.h"

using namespace aceqv;

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
    check(menu_items(true).size() == 9 && menu_items(false).size() == 7 &&
              menu_items(false).front() == MenuItem::Width,
          "QVFR lists resolution; Two-loop starts at the focus width");
    check(menu_page_items(m).size() == 11 && menu_page_items(m).front() == MenuItem::Page &&
              menu_page_items(m).back() == MenuItem::Optimize && menu_pages(m).size() == 4,
          "the first page: the page switch, the method's items, then Optimize; four pages without Cheeky (QVFR: View crop, Helmet)");
    {
        SettingsMenu loop;
        loop.launch.qvfr = false;
        check(menu_page_items(loop).back() == MenuItem::FocusOffset, "Two-loop: no Optimize");
    }

    menu_key(m, MenuKey::Up);
    check(m.selected == 10, "Up from the first item wraps to the last (Optimize)");
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
    const std::wstring ini = std::wstring(temp) + L"ace-quadview-menu-test.ini";
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
    const std::wstring loopIni = std::wstring(temp) + L"ace-quadview-menu-test-loop.ini";
    DeleteFileW(loopIni.c_str());
    write_menu_values(loopIni.c_str(), v, false);
    check(GetPrivateProfileIntW(L"experiment", L"periphery_scale", -1, loopIni.c_str()) == -1 &&
              GetPrivateProfileIntW(L"experiment", L"focus_percent", -1, loopIni.c_str()) == 30,
          "Two-loop writes foveation but no resolution");
    DeleteFileW(ini.c_str());
    DeleteFileW(loopIni.c_str());

    // Lines and the panel image.
    SettingsMenu shown;
    shown.selected = 2;
    const std::vector<MenuLine> lines = menu_lines(shown);
    check(lines.size() == 11 && lines[0].label == L"Page" && lines[0].value == L"QVFR" && lines[2].selected &&
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
        const std::wstring offsetIni = std::wstring(temp) + L"ace-quadview-menu-test-offset.ini";
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

    // The Overlay and menu page: the overlay and both panels' places, stepped and written to [osd] and
    // [menu] for the layer to move them.
    SettingsMenu moving;
    moving.page = 3;
    check(menu_title(moving) == L"Overlay and menu" && menu_subtitle(moving).empty() && menu_page_items(moving).size() == 8,
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
    const auto movingIni = (std::filesystem::temp_directory_path() / L"aceqv_menu_moving.ini").wstring();
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
    const auto placementIni = (std::filesystem::temp_directory_path() / L"aceqv_menu_placement.ini").wstring();
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
        check(menu_pages(c).size() == 4 && menu_subtitle(c).empty(), "without Cheeky: no Cheeky pages");
        c.cheeky = {true, L"0.6.3-standalone", CheekySettings{}};
        check(menu_pages(c).size() == 6 && menu_page_items(c).size() == 11 && menu_page_items(c).front() == MenuItem::Page &&
                  menu_title(c) == L"QVFR settings",
              "with Cheeky: six pages, the first the QVFR items under the page switch");
        check(!menu_key(c, MenuKey::Right) && c.page == 1 && menu_title(c) == L"View crop", "then the View crop page");
        check(!menu_key(c, MenuKey::Right) && c.page == 2 && menu_title(c) == L"Helmet", "then the Helmet page");
        check(!menu_key(c, MenuKey::Right) && c.page == 3 && c.selected == 0 && menu_page_items(c).size() == 11 &&
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
        const auto cheekyIni = (std::filesystem::temp_directory_path() / L"aceqv_menu_cheeky.ini").wstring();
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
        c.page = 3;
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
        const auto nrIni = (std::filesystem::temp_directory_path() / L"aceqv_menu_cheeky_nr.ini").wstring();
        DeleteFileW(nrIni.c_str());
        write_cheeky_settings(nrIni.c_str(), c.cheeky.values, false);
        const CheekySettings nrBack = read_cheeky_settings(nrIni.c_str());
        DeleteFileW(nrIni.c_str());
        check(nrBack == c.cheeky.values, "the experimental keys round-trip through [cheeky]");

        // Frame generation: the third page, reached either way round from the page switch.
        c.page = 0;
        c.selected = 0;
        menu_key(c, MenuKey::Left);
        check(c.page == 5 && menu_title(c) == L"Overlay and menu", "Left from the first page wraps to Overlay and menu");
        menu_key(c, MenuKey::Left);
        check(c.page == 4 && menu_title(c) == L"Frame generation" && menu_page_items(c).size() == 8 &&
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
        const bool captureFirst = menu_lines(c)[6].label == L"Release ACE" && menu_lines(c)[6].value == L"Capture (8)" &&
                                  cheeky_set_lines(c.cheeky.values, true).find("FgReleasePoint=8") != std::string::npos;
        menu_key(c, MenuKey::Right);
        const bool generated = c.cheeky.values.fgRelease == 3 && menu_lines(c)[6].value == L"Generated (3)" &&
                               !menu_key(c, MenuKey::Right);  // stops at the end
        menu_key(c, MenuKey::Left);
        menu_key(c, MenuKey::Left);
        check(captureFirst && generated && c.cheeky.values.fgRelease == 5 && menu_lines(c)[6].value == L"Never (5)" &&
                  cheeky_set_lines(c.cheeky.values, true).find("FgReleasePoint=5") != std::string::npos,
              "Release ACE: Capture (8), Generated (3), Never (5); Cheeky's FgReleasePoint follows");
        const auto releaseIni = (std::filesystem::temp_directory_path() / L"aceqv_menu_release.ini").wstring();
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
        v.page = 1;
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
        const auto cropIni = (std::filesystem::temp_directory_path() / L"aceqv_menu_crop.ini").wstring();
        DeleteFileW(cropIni.c_str());
        write_menu_values(cropIni.c_str(), v.values, true);
        const MenuValues back = read_menu_values(cropIni.c_str());
        DeleteFileW(cropIni.c_str());
        check(back.crop && back.cropTop == 40 && back.cropOuter == 10 && back.cropBottom == 0, "View crop: [crop] round-trips");
        // The Helmet page: on, its rim, corners and lighting stepped, written to [helmet].
        v.page = 2;
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
        v.page = 1;  // back on View crop
        v.launch.dlssHeld = false;
        v.launch.cropHeight = 50;  // launched with more off: less now is softer until the next launch
        check(!menu_notes(v).empty() && menu_notes(v).back().find(L"Less crop") == 0, "View crop: less than at launch is noted");
    }

    std::printf("RESULT: %s\n", failures == 0 ? "pass" : "FAIL");
    return failures == 0 ? 0 : 1;
}
