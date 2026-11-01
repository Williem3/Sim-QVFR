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
    check(menu_items(true).size() == 11 && menu_items(false).size() == 9 &&
              menu_items(false).front() == MenuItem::Width,
          "QVFR lists resolution; Two-loop starts at the focus width");

    menu_key(m, MenuKey::Up);
    check(m.selected == 10, "Up from the first item wraps to the last");
    menu_key(m, MenuKey::Down);
    check(m.selected == 0, "Down from the last wraps to the first");
    bool changed = menu_key(m, MenuKey::Left);
    check(changed && m.values.periphery == 45, "periphery steps by 5");
    for (int i = 0; i < 30; ++i)
        menu_key(m, MenuKey::Left);
    check(m.values.periphery == 10 && !menu_key(m, MenuKey::Left), "periphery stops at 10%, no change reported");
    m.selected = 2;
    menu_key(m, MenuKey::Right);
    check(m.values.width == 40, "focus width steps by 1");
    m.selected = 6;
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
    check(runLines[0].capped && runLines[0].value == L"30%  (10% now)" && runLines[1].capped &&
              runLines[1].value == L"100%  (84% x 71% now)" && !runLines[2].capped,
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
    check(menu_notes(run).empty() && !menu_lines(run)[1].capped, "lowering the box makes it fit: no notes, no red");
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
    check(write_menu_values(ini.c_str(), v, true), "the menu's keys are written");
    const MenuValues back = read_menu_values(ini.c_str());
    check(back.periphery == 35 && back.density == 110 && back.width == 30 && back.height == 25 && back.feather == 12 &&
              back.corners == 60 && back.outline && !back.eyeTracking,
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
    shown.selected = 1;
    const std::vector<MenuLine> lines = menu_lines(shown);
    check(lines.size() == 11 && lines[1].selected && lines[1].label == L"Focus density" && lines[1].value == L"100%" &&
              lines[7].value == L"On" && lines[8].value == L"0°" && lines[9].value == L"4° down" &&
              lines[10].value == L"33°",
          "lines carry labels, values and the selection; the panel's place in degrees");

    // The panel's place from the menu itself: stepped, shown and written to [menu] for the layer to move it.
    SettingsMenu moving;
    moving.selected = 8;  // Menu left / right
    menu_key(moving, MenuKey::Left);
    menu_key(moving, MenuKey::Left);
    moving.selected = 10;  // Menu size
    menu_key(moving, MenuKey::Right);
    const auto movingIni = (std::filesystem::temp_directory_path() / L"aceqv_menu_moving.ini").wstring();
    write_menu_values(movingIni.c_str(), moving.values, true);
    const MenuPlacement moved = read_menu_placement(movingIni.c_str());
    const MenuValues reread = read_menu_values(movingIni.c_str());
    DeleteFileW(movingIni.c_str());
    check(moved.yaw == -2 && moved.pitch == -4 && moved.width == 34 && reread.placement == moved &&
              menu_lines(moving)[8].value == L"2° left",
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

    std::printf("RESULT: %s\n", failures == 0 ? "pass" : "FAIL");
    return failures == 0 ? 0 : 1;
}
