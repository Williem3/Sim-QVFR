// Offline check of the in-headset settings menu: selection, steps and ranges, the items per method,
// the ini round trip and the launch-size note; and that the panel image draws text and highlight.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>

#include <windows.h>

#include "games/ace/game.h"
#include "games/acc/game.h"
#include "games/lmu/game.h"
#include "games/xplane/game.h"
#include "ini_put.h"
#include "lang.h"
#include "quad/corner_fit.h"
#include "quad/menu_panel.h"
#include "quad/settings_menu.h"
#include "test_util.h"

using namespace simqvfr;

namespace {

// `item`'s line on the current page (-1: not there).
int at(const SettingsMenu& m, MenuItem item) {
    const std::vector<MenuItem> items = menu_page_items(m);
    const auto it = std::find(items.begin(), items.end(), item);
    return it == items.end() ? -1 : static_cast<int>(it - items.begin());
}
bool has(const SettingsMenu& m, MenuItem item) { return at(m, item) >= 0; }
// Selects `item` on the current page; false when it isn't there.
bool pick(SettingsMenu& m, MenuItem item) {
    const int i = at(m, item);
    if (i < 0)
        return false;
    m.selected = i;
    return true;
}
// `item`'s line (an empty one when it isn't on the page).
MenuLine line_of(const SettingsMenu& m, MenuItem item) {
    const int i = at(m, item);
    return i < 0 ? MenuLine{} : menu_lines(m)[static_cast<size_t>(i)];
}
// Goes to `page`, the page switch selected; false when the menu has none.
bool go(SettingsMenu& m, MenuPage page) {
    const std::vector<MenuPage> pages = menu_pages(m);
    const auto it = std::find(pages.begin(), pages.end(), page);
    if (it == pages.end())
        return false;
    m.page = static_cast<int>(it - pages.begin());
    m.selected = 0;
    return true;
}
bool has_page(const SettingsMenu& m, MenuPage page) {
    const std::vector<MenuPage> pages = menu_pages(m);
    return std::find(pages.begin(), pages.end(), page) != pages.end();
}

// `sim_qvfr_tests settings_menu_test --render <lang folder> <code> <out folder>`: each page of the menu, drawn in that
// language, as page-<n>.bmp (32-bit, top-down), to look at a translation's fit without a game. Not part of the suite.
int render_pages(const char* folder, const char* code, const char* out) {
    lang::select(folder, std::wstring(code, code + std::strlen(code)));  // an ASCII code: de, pl, pt-BR
    SettingsMenu m;
    m.launch = {true, 50, 39, 39};
    std::filesystem::create_directories(out);
    for (int page = 0; page < static_cast<int>(menu_pages(m).size()); ++page) {
        m.page = page;
        m.selected = 1;
        const std::vector<MenuLine> lines = menu_lines(m);
        const std::vector<uint32_t> px = draw_menu_panel(menu_title(m), menu_resolution(m), menu_stats(m), lines,
                                                         menu_notes(m), L"", menu_subtitle(m), L"", L"", false,
                                                         menu_tabs(m), m.page, 1);
        BITMAPFILEHEADER file{};
        BITMAPINFOHEADER info{sizeof(info), kPanelWidth, -kPanelHeight, 1, 32, BI_RGB};
        file.bfType = 0x4d42;
        file.bfOffBits = sizeof(file) + sizeof(info);
        file.bfSize = file.bfOffBits + static_cast<DWORD>(px.size() * 4);
        std::vector<uint8_t> bgra(px.size() * 4);
        for (size_t i = 0; i < px.size(); ++i) {  // RGBA8 premultiplied, over black
            const uint32_t c = px[i];
            bgra[i * 4 + 0] = static_cast<uint8_t>(c >> 16);
            bgra[i * 4 + 1] = static_cast<uint8_t>(c >> 8);
            bgra[i * 4 + 2] = static_cast<uint8_t>(c);
            bgra[i * 4 + 3] = 255;
        }
        std::ofstream f(std::filesystem::path(out) / ("page-" + std::to_string(page) + ".bmp"), std::ios::binary);
        f.write(reinterpret_cast<const char*>(&file), sizeof(file));
        f.write(reinterpret_cast<const char*>(&info), sizeof(info));
        f.write(reinterpret_cast<const char*>(bgra.data()), static_cast<std::streamsize>(bgra.size()));
    }
    return 0;
}

}  // namespace

int SIM_QVFR_TEST_MAIN(int argc, char** argv) {
    if (argc >= 5 && std::strcmp(argv[1], "--render") == 0)
        return render_pages(argv[2], argv[3], argv[4]);
    SettingsMenu m;
    m.launch = {true, 50, 39, 39};
    check(menu_items(true).size() == 10 && menu_items(false).size() == 8 &&
              menu_items(false).front() == MenuItem::Width,
          "QVFR lists resolution; Two-loop starts at the focus width");
    // The tabs (the user's layout, 2026-11-16): Picture, Foveation, View, Headset, Game; their sections the launcher's cards.
    check(menu_pages(m) == std::vector<MenuPage>{MenuPage::Picture, MenuPage::Foveation, MenuPage::View, MenuPage::Headset,
                                                 MenuPage::Game} &&
              menu_tabs(m) == std::vector<std::wstring>{L"Picture", L"Foveation", L"View", L"Headset", L"Game"},
          "the pages: Picture, Foveation, View, Headset and Game");
    {
        const std::vector<MenuItem> items = menu_page_items(m);
        check(items == std::vector<MenuItem>{MenuItem::Page, MenuItem::HeadResolution, MenuItem::Periphery, MenuItem::Density,
                                             MenuItem::Optimize},
              "Picture: the page switch, then the Resolution card, Auto-fit last");
        const std::vector<MenuLine> lines = menu_lines(m);
        check(lines[1].header && lines[1].label == L"Resolution" && lines[1].value.empty() && !lines[2].header,
              "the sections' headings: the launcher's card names, no value");
        check(menu_title(m) == L"Headset menu" && menu_page_name(m) == L"Picture" && lines[0].label == L"Page" &&
                  lines[0].value == L"Picture  1 of 5",
              "the title is the panel's launcher name; the page switch says which page of how many");
        SettingsMenu f = m;
        go(f, MenuPage::Foveation);
        check(menu_page_items(f) == std::vector<MenuItem>{MenuItem::Page, MenuItem::HeadFoveation, MenuItem::EyeTracking,
                                                          MenuItem::Width, MenuItem::Height, MenuItem::Shape, MenuItem::Corners,
                                                          MenuItem::Feather, MenuItem::FocusOffset, MenuItem::FocusOffsetX, MenuItem::GazeDeadzone,
                                                          MenuItem::Outline} &&
                  menu_page_name(f) == L"Foveation" && line_of(f, MenuItem::HeadFoveation).label == L"Foveation",
              "Foveation: its own tab, the launcher's Foveation card in its order");
    }
    {
        SettingsMenu loop;
        loop.launch.qvfr = false;
        go(loop, MenuPage::Foveation);
        check(menu_page_items(loop).back() == MenuItem::Outline && menu_title(loop) == L"Headset menu · Two-loop (legacy)" &&
                  menu_pages(loop) == std::vector<MenuPage>{MenuPage::Picture, MenuPage::Foveation, MenuPage::Headset,
                                                            MenuPage::Game},
              "Two-loop: no View page; Foveation as in QVFR");
        go(loop, MenuPage::Picture);
        check(!has(loop, MenuItem::Optimize) && !has(loop, MenuItem::HeadResolution), "Two-loop: no Resolution card or Auto-fit");
    }

    // Up and Down skip the headings.
    menu_key(m, MenuKey::Down);
    check(m.selected == at(m, MenuItem::Periphery), "Down from the page switch skips the Resolution heading");
    menu_key(m, MenuKey::Up);
    check(m.selected == 0, "and Up from Periphery resolution skips it back to the page switch");
    menu_key(m, MenuKey::Up);
    check(m.selected == at(m, MenuItem::Optimize), "Up from the page switch wraps to the last line");
    menu_key(m, MenuKey::Down);
    check(m.selected == 0, "Down from the last wraps to the first");
    check(menu_section(m) == MenuItem::HeadResolution, "the section: the page's first for the page switch");
    {
        SettingsMenu f = m;
        go(f, MenuPage::Foveation);
        check(menu_section(f) == MenuItem::HeadFoveation && (pick(f, MenuItem::Feather), menu_section(f)) == MenuItem::HeadFoveation,
              "and the heading above a line");
    }
    {
        SettingsMenu o = m;
        pick(o, MenuItem::Optimize);
        const MenuValues before = o.values;
        check(!menu_key(o, MenuKey::Right) && o.optimizeRequest == 1 && o.values.periphery == before.periphery,
              "Optimize: Right asks the layer to run it, no value changes");
        check(!menu_key(o, MenuKey::Left) && o.optimizeRequest == -1, "Optimize: Left asks to undo");
        check(line_of(o, MenuItem::Optimize).label == L"Auto-fit to headset rate" &&
                  !line_of(o, MenuItem::Optimize).value.empty(),
              "Optimize's line before a run: how to use it");
        o.optimizeStatus = L"Running…";
        o.optimizeDetail = L"Measuring… keep driving";
        check(line_of(o, MenuItem::Optimize).value == L"Running…" && menu_subtitle(o) == L"Measuring… keep driving",
              "Optimize's status on its line, its detail under the title");
    }
    pick(m, MenuItem::Periphery);
    bool changed = menu_key(m, MenuKey::Left);
    check(changed && m.values.periphery == 45, "periphery steps by 5");
    for (int i = 0; i < 30; ++i)
        menu_key(m, MenuKey::Left);
    check(m.values.periphery == 10 && !menu_key(m, MenuKey::Left), "periphery stops at 10%, no change reported");
    go(m, MenuPage::Foveation);
    pick(m, MenuItem::Width);
    menu_key(m, MenuKey::Right);
    check(m.values.width == 36, "focus width steps by 1 (from the default 35)");
    pick(m, MenuItem::Outline);
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
    pick(run, MenuItem::Periphery);
    check(line_of(run, MenuItem::Periphery).capped && line_of(run, MenuItem::Periphery).value == L"30%  (10% now)" &&
              line_of(run, MenuItem::Density).capped && line_of(run, MenuItem::Density).value == L"100%  (84% x 71% now)" &&
              !line_of(run, MenuItem::Optimize).capped,
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

    // The SCS corner path (quad/corner_fit.h), 2026-11-19 tester: the Performance profile asked a 20% periphery, the path
    // drew 25% (its least) and the menu said 20%. Now the menu says what the path draws.
    {
        const CornerFit raised = corner_fit(20, 25, 25, 140);
        check(raised.periphery == 25 && raised.peripheryRaised && !raised.peripheryByRoom && !raised.densityCapped &&
                  std::lround(raised.density * 100) == 140,
              "corner fit: 20% asked is raised to 25%, the 25% box at 140% kept");
        const CornerFit room = corner_fit(80, 25, 25, 140);
        check(room.periphery == 65 && room.peripheryByRoom && !room.peripheryRaised,
              "corner fit: 80% asked is held to the 65% the focus region leaves");
        const CornerFit dense = corner_fit(40, 60, 60, 200);
        check(dense.densityCapped && std::lround(dense.density * 100) == 125 && dense.periphery == 25,
              "corner fit: a 60% box at 200% is held to 125% (room for the least corner)");
        check(corner_fit(40, 0, 0, 100).periphery == 40 && corner_fit(40, 0, 0, 100).focusX == 0,
              "corner fit: no focus view, the corner as asked");

        SettingsMenu scs;
        scs.launch = {true, 40, 35, 35, 6240, 6280};
        scs.launch.cornerFit = true;
        scs.values.periphery = 20;
        scs.values.density = 140;
        scs.values.width = 25;
        scs.values.height = 25;
        const MenuFit f = menu_fit(scs);
        check(f.peripheryCapped && f.peripheryRaised && std::lround(f.periphery) == 25 && !f.focusCapped &&
                  std::lround(f.densityX) == 140,
              "corner path: the menu has the periphery in effect (25%) and the density as asked");
        pick(scs, MenuItem::Periphery);
        check(line_of(scs, MenuItem::Periphery).capped && line_of(scs, MenuItem::Periphery).value == L"20%  (25% now)",
              "corner path: the periphery line shows 20% asked, 25% now");
        const std::vector<std::wstring> n = menu_notes(scs);
        check(n.size() == 1 && n[0] == L"Periphery below 25% isn't used in this game: 25% now",
              "corner path: the note says why");
        scs.values.periphery = 50;  // room for 65%: as asked, nothing capped
        check(!menu_fit(scs).peripheryCapped && std::lround(menu_fit(scs).periphery) == 50 && menu_notes(scs).empty(),
              "corner path: 50% fits beside the focus region");
        scs.values.periphery = 80;
        check(menu_fit(scs).peripheryByRoom && std::lround(menu_fit(scs).periphery) == 65,
              "corner path: 80% held to the room the focus region leaves, not the four views' launch rule");
    }

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
    {  // The focus mask: a game that masks (GameFeatures::focusMask) leaves the cut corners out of the pixels saved.
        SettingsMenu trap = stats;
        trap.values.shape = focus_shape_of({{-20, 50}, {20, 50}, {50, -50}, {-50, -50}});  // the top corners 30% in
        SettingsMenu masks = trap;
        masks.launch.focusMask = true;
        SettingsMenu maskOff = masks;
        maskOff.values.focusMask = false;
        SettingsMenu rounded = stats;
        rounded.launch.focusMask = true;
        rounded.values.corners = 100;
        const int plain = menu_stats(trap).savedPercent, masked = menu_stats(masks).savedPercent,
                  round = menu_stats(rounded).savedPercent;
        std::printf("     pixels saved: %d%% box, %d%% trapezoid masked, %d%% rounded masked\n", plain, masked, round);
        check(plain == st.savedPercent && menu_stats(maskOff).savedPercent == plain && masked > plain && round > plain &&
                  round < masked,
              "pixels saved: the trapezoid and the rounding count their masked corners where the game masks; nothing "
              "with focus_mask=0 or another game");
    }
    run.values.periphery = 10;
    run.values.density = 84;
    run.values.height = 35;  // 84% x 43% = 36.1%: still over 36%
    check(menu_notes(run).size() == 1 && menu_fit(run).focusCapped, "a value just over the launch still flags");
    run.values.width = 42;  // 84% x 42% = 35.3%, 84% x 35% = 29.4%: fits
    check(menu_notes(run).empty() && !line_of(run, MenuItem::Density).capped, "lowering the box makes it fit: no notes, no red");
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
    check(GetPrivateProfileIntW(L"quad", L"periphery_texture_match", -1, ini.c_str()) == -1 &&
              read_menu_values(ini.c_str()).textureMatch == 100,
          "periphery texture match: not written for a game without it; unset reads as 100%");
    v.textureMatch = 40;
    write_menu_values(ini.c_str(), v, true, false, false, false, false, false, false, true);
    check(GetPrivateProfileIntW(L"quad", L"periphery_texture_match", -1, ini.c_str()) == 40 &&
              read_menu_values(ini.c_str()).textureMatch == 40,
          "periphery texture match: written for a game with it ([quad] periphery_texture_match), and read back");
    v.textureMatch = 100;
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

    // Lines and the panel image: the launcher's names and values.
    SettingsMenu shown;
    pick(shown, MenuItem::Density);
    const std::vector<MenuLine> lines = menu_lines(shown);
    SettingsMenu shownFoveation = shown;
    go(shownFoveation, MenuPage::Foveation);
    check(lines[static_cast<size_t>(at(shown, MenuItem::Density))].selected && line_of(shown, MenuItem::Density).value == L"100%" &&
              line_of(shown, MenuItem::Periphery).label == L"Periphery resolution",
          "Picture's lines carry the launcher's labels and values, and the selection");
    shown = shownFoveation;
    check(line_of(shown, MenuItem::EyeTracking).label == L"Foveated mode" &&
              line_of(shown, MenuItem::EyeTracking).value == L"Eye tracked" &&
              line_of(shown, MenuItem::Shape).value == L"Rectangle" && line_of(shown, MenuItem::Corners).label == L"Corner radius" &&
              line_of(shown, MenuItem::FocusOffset).label == L"Gaze offset" && line_of(shown, MenuItem::FocusOffset).value == L"0°" &&
              line_of(shown, MenuItem::Outline).label == L"Show focus outline" && line_of(shown, MenuItem::Outline).value == L"Off",
          "Foveation's lines carry the launcher's labels and values");
    shown = SettingsMenu{};
    pick(shown, MenuItem::Density);

    // The vertical offset: each mode keeps its own, stepped by the one item, written to [quad].
    {
        SettingsMenu o;
        go(o, MenuPage::Foveation);
        pick(o, MenuItem::FocusOffset);
        o.values.eyeTracking = false;
        check(menu_key(o, MenuKey::Left) && menu_key(o, MenuKey::Left) && o.values.offset.fixed == -2 &&
                  o.values.offset.gaze == 0 && line_of(o, MenuItem::FocusOffset).value == L"2° down" &&
                  line_of(o, MenuItem::FocusOffset).label == L"Vertical position",
              "Fixed: the line is Vertical position (the launcher's), stepping the fixed offset");
        o.values.eyeTracking = true;
        check(menu_key(o, MenuKey::Right) && o.values.offset.gaze == 1 && o.values.offset.fixed == -2 &&
                  line_of(o, MenuItem::FocusOffset).value == L"1° up" && line_of(o, MenuItem::FocusOffset).label == L"Gaze offset",
              "Eye tracked: Gaze offset, leaving the fixed one");
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
        go(d, MenuPage::Foveation);
        pick(d, MenuItem::GazeDeadzone);
        check(line_of(d, MenuItem::GazeDeadzone).label == L"Gaze dead-zone" && line_of(d, MenuItem::GazeDeadzone).value == L"1.5°",
              "the dead-zone at 1.5° by default");
        check(menu_key(d, MenuKey::Right) && d.values.gazeDeadzone == 20 && line_of(d, MenuItem::GazeDeadzone).value == L"2.0°",
              "it steps by 0.5°");
        for (int i = 0; i < 10; ++i)
            menu_key(d, MenuKey::Left);
        check(d.values.gazeDeadzone == 0 && line_of(d, MenuItem::GazeDeadzone).value == L"Off (follows every sample)", "down to 0: off");
        d.values.eyeTracking = false;
        check(!has(d, MenuItem::GazeDeadzone), "Fixed: no dead-zone line, as the launcher hides it");
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

    // The Headset page (the launcher's Headset tab): the Headset menu, Performance overlay and Clock cards, their places
    // shown while they are on, stepped and written to [menu], [osd] and [clock] for the layer to move them.
    SettingsMenu moving;
    check(go(moving, MenuPage::Headset) && menu_page_name(moving) == L"Headset" && menu_subtitle(moving).empty() &&
              menu_page_items(moving) == std::vector<MenuItem>{MenuItem::Page, MenuItem::HeadMenu, MenuItem::MenuYaw,
                                                               MenuItem::MenuPitch, MenuItem::MenuWidth, MenuItem::HotkeyFooter,
                                                               MenuItem::HeadOverlay, MenuItem::Overlay, MenuItem::HeadClock,
                                                               MenuItem::Clock},
          "the Headset page: the menu's card, the overlay's and the clock's, their places hidden while they're off");
    check(line_of(moving, MenuItem::MenuYaw).label == L"Horizontal" && line_of(moving, MenuItem::MenuYaw).value == L"22° left" &&
              line_of(moving, MenuItem::MenuPitch).label == L"Vertical" && line_of(moving, MenuItem::MenuPitch).value == L"23° down" &&
              line_of(moving, MenuItem::MenuWidth).label == L"Size" && line_of(moving, MenuItem::MenuWidth).value == L"20°" &&
              line_of(moving, MenuItem::Overlay).label == L"Show the overlay" && line_of(moving, MenuItem::Overlay).value == L"Off" &&
              line_of(moving, MenuItem::Clock).label == L"Show the clock" && line_of(moving, MenuItem::Clock).value == L"Off" &&
              line_of(moving, MenuItem::HeadOverlay).label == L"Performance overlay" && line_of(moving, MenuItem::HeadClock).label == L"Clock" &&
              line_of(moving, MenuItem::HeadMenu).label == L"Headset menu",
          "the launcher's names: Horizontal, Vertical, Size under each card's heading");
    pick(moving, MenuItem::Overlay);
    menu_key(moving, MenuKey::Right);  // overlay on: its place shows
    check(line_of(moving, MenuItem::OverlayYaw).value == L"0°" && line_of(moving, MenuItem::OverlayPitch).value == L"25° down" &&
              line_of(moving, MenuItem::OverlayWidth).value == L"19°",
          "the overlay on: its place in degrees (the default: low ahead)");
    pick(moving, MenuItem::OverlayYaw);
    for (int i = 0; i < 3; ++i)
        menu_key(moving, MenuKey::Right);
    pick(moving, MenuItem::OverlayPitch);
    menu_key(moving, MenuKey::Left);
    pick(moving, MenuItem::Clock);
    menu_key(moving, MenuKey::Right);
    check(line_of(moving, MenuItem::ClockYaw).value == L"22° right" && line_of(moving, MenuItem::ClockPitch).value == L"23° down" &&
              line_of(moving, MenuItem::ClockWidth).label == L"Size" && line_of(moving, MenuItem::ClockWidth).value == L"8°",
          "the clock on: its place and size (the launcher's Size, new on the menu)");
    pick(moving, MenuItem::ClockYaw);
    menu_key(moving, MenuKey::Left);
    pick(moving, MenuItem::ClockPitch);
    menu_key(moving, MenuKey::Right);
    pick(moving, MenuItem::ClockWidth);
    menu_key(moving, MenuKey::Right);
    pick(moving, MenuItem::MenuYaw);
    menu_key(moving, MenuKey::Left);
    menu_key(moving, MenuKey::Left);
    pick(moving, MenuItem::MenuWidth);
    menu_key(moving, MenuKey::Right);
    const auto movingIni = (std::filesystem::temp_directory_path() / L"simqvfr_menu_moving.ini").wstring();
    write_menu_values(movingIni.c_str(), moving.values, true);
    const MenuPlacement moved = read_menu_placement(movingIni.c_str());
    const OsdPlacement overlayMoved = read_osd_placement(movingIni.c_str());
    const ClockPlacement clockMoved = read_clock_placement(movingIni.c_str());
    const MenuValues reread = read_menu_values(movingIni.c_str());
    DeleteFileW(movingIni.c_str());
    check(overlayMoved == OsdPlacement{true, 3, -26, 19} && reread.overlay == overlayMoved &&
              line_of(moving, MenuItem::OverlayYaw).value == L"3° right",
          "the overlay turns on and moves: right 3 degrees and down 1, written to [osd] and read back");
    check(clockMoved == ClockPlacement{true, 21, -22, 9} && reread.clock == clockMoved &&
              line_of(moving, MenuItem::Clock).value == L"On" && line_of(moving, MenuItem::ClockYaw).value == L"21° right",
          "the clock turns on, moves left 1 and up 1 and grows a degree, written to [clock] and read back");
    check(moved.yaw == -24 && moved.pitch == -23 && moved.width == 21 && reread.placement == moved &&
              line_of(moving, MenuItem::MenuYaw).value == L"24° left",
          "the menu moves itself: left 2 degrees and 1 degree wider, written to [menu] and read back");
    {  // Vision (a game with one eye off, LMU): the Picture page's last card.
        SettingsMenu e;
        e.launch.eyeOff = true;
        const std::vector<MenuItem> items = menu_page_items(e);
        check(items.size() >= 2 && items[items.size() - 2] == MenuItem::HeadVision && items.back() == MenuItem::EyeOff &&
                  line_of(e, MenuItem::HeadVision).label == L"Vision" && (go(e, MenuPage::Headset), !has(e, MenuItem::EyeOff)),
              "Eye off: under Vision, the Picture page's last card (not on Headset)");
    }
    const std::vector<uint32_t> pixels = draw_menu_panel(menu_title(shown), menu_resolution(shown), menu_stats(shown), lines,
                                                         {}, L"hint", L"", L"", L"", false, menu_tabs(shown), shown.page);
    bool alpha = !pixels.empty(), highlight = false, bright = false, red = false;
    const auto alphaAt = [&](int x, int y) { return pixels[static_cast<size_t>(y) * kPanelWidth + x] >> 24; };
    // Translucent inside, rounded corners clear (and black: premultiplied).
    alpha = alphaAt(kPanelWidth / 2, kPanelHeight / 2) == 0xe6 && alphaAt(40, 0) == 0xe6 && alphaAt(0, 0) == 0 &&
            alphaAt(kPanelWidth - 1, kPanelHeight - 1) == 0 && alphaAt(0, kPanelHeight - 1) == 0 && pixels[0] == 0;
    const uint32_t highlightRgb = panel_rgba(0x0a6e78, 0xe6) & 0xffffff;  // kHighlight, premultiplied
    for (uint32_t p : pixels) {
        highlight |= (p & 0xffffff) == highlightRgb;
        bright |= (p & 0xff) > 0xc0 && ((p >> 8) & 0xff) > 0xc0;
        red |= (p & 0xff) > 0xc0 && ((p >> 8) & 0xff) < 0x90;
    }
    check(pixels.size() == static_cast<size_t>(kPanelWidth) * kPanelHeight && alpha && highlight && bright && !red,
          "the panel draws its background, the tabs and the selected row and text; nothing red when everything fits");
    {  // The tab strip: the current page's tab filled with the highlight, under the title.
        int filled = 0;
        for (int x = 0; x < kPanelWidth; ++x)
            filled += (pixels[static_cast<size_t>(80 + 24) * kPanelWidth + x] & 0xffffff) == highlightRgb;
        check(filled > 60, "the tab strip marks the current page");
    }
    run.launch.nativeWidth = 5190;
    run.values = MenuValues{};
    run.values.periphery = 30;
    const std::vector<uint32_t> cappedPixels =
        draw_menu_panel(L"Picture", menu_resolution(run), menu_stats(run), menu_lines(run), menu_notes(run), L"hint");
    bool cappedRed = false;
    for (uint32_t p : cappedPixels)
        cappedRed |= (p & 0xff) > 0xc0 && ((p >> 8) & 0xff) < 0x90;
    check(cappedRed, "a capped value and its note draw in red");

    // The panel's place: straight ahead and a little low, 33 degrees (its first place); turned, it still faces the eye.
    const MenuPose first = menu_pose(MenuPlacement{0, -4, 33}, static_cast<float>(kPanelHeight) / kPanelWidth);
    check(std::fabs(first.width - 0.48f) < 0.01f && std::fabs(first.position[1] + 0.056f) < 0.005f &&
              std::fabs(first.position[2] + 0.8f) < 0.005f && std::fabs(first.position[0]) < 1e-6f,
          "menu placement: 33 degrees a little low is 0.48 m wide, 0.8 m ahead");
    const MenuPose lowLeft = menu_pose(MenuPlacement{}, static_cast<float>(kPanelHeight) / kPanelWidth);
    check(lowLeft.position[0] < 0 && lowLeft.position[1] < 0 && std::fabs(lowLeft.width - 0.28f) < 0.01f,
          "menu placement: the default is low on the left, 0.28 m wide");
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
    check(read.yaw == -kMenuYawMax && read.pitch == -7 && read.width == MenuPlacement{}.width,
          "menu placement: [menu] reads negative degrees, clamps them and defaults the rest");

    // Foveated DLSS (Cheeky running): its cards on the View page, with Frame generation's.
    {
        SettingsMenu c;
        c.launch = {true, 50, 39, 39};
        check(menu_pages(c).size() == 5 && menu_subtitle(c).empty(), "without Cheeky: five pages, nothing under the title");
        {
            // A game without the FOV crop, the helmet or live resolution (ACC without Cheeky): no View page, and a note
            // that resolution changes apply at the next launch.
            SettingsMenu g = c;
            g.launch.cropPage = g.launch.helmetPage = g.launch.liveResolution = false;
            bool next = false;
            for (const std::wstring& n : menu_notes(g))
                next |= n.find(L"next launch") != std::wstring::npos;
            check(menu_pages(g).size() == 4 && !has_page(g, MenuPage::View) && next,
                  "a game without crop, helmet or live resolution: no View page, resolution at the next launch");
        }
        {
            // X-Plane 12, its pages from its GameFeatures as the layer sets them (xr_layer.cpp): no FOV crop, helmet or
            // Foveated DLSS, and the layer never loads Cheeky there, so no View page.
            SettingsMenu g = c;
            const games::GameFeatures& f = xplane::kGame.features;
            g.launch.cropPage = f.fovCrop;
            g.launch.helmetPage = f.helmet;
            g.launch.fgPage = f.frameGeneration;
            g.cheeky.available = f.foveatedDlss && g.cheeky.available;
            check(!f.fovCrop && !f.helmet && !f.foveatedDlss &&
                      menu_pages(g) == std::vector<MenuPage>{MenuPage::Picture, MenuPage::Foveation, MenuPage::Headset,
                                                             MenuPage::Game},
                  "X-Plane 12: Picture, Foveation, Headset and Game (no View page)");
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
            {  // Periphery texture match: only a game with it (ACE), after Sharpening, 100% down to Off in tens
                SettingsMenu m = l;
                check(find(m, MenuItem::TextureMatch) == static_cast<std::ptrdiff_t>(menu_page_items(m).size()), "Periphery texture match: not without the game's feature");
                m.launch.textureMatch = true;
                check(find(m, MenuItem::TextureMatch) == find(m, MenuItem::Sharpening) + 1, "Periphery texture match: after Sharpening");
                m.selected = static_cast<int>(find(m, MenuItem::TextureMatch));
                check(menu_lines(m)[m.selected].label == L"Periphery texture match" && menu_lines(m)[m.selected].value == L"100%",
                      "Periphery texture match's line: 100% by default");
                check(!menu_key(m, MenuKey::Right) && menu_key(m, MenuKey::Left) && m.values.textureMatch == 90,
                      "Periphery texture match: no higher than 100%, Left steps 10%");
                for (int i = 0; i < 12; ++i)
                    menu_key(m, MenuKey::Left);
                check(m.values.textureMatch == 0 && menu_lines(m)[m.selected].value == L"Off", "Periphery texture match: down to Off");
            }
        }
        {
            // focus_layer=2's test line: only with launch.focusQuads, right after Gaze dead-zone, in steps of metres.
            SettingsMenu q = c;
            go(q, MenuPage::Foveation);
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
            go(n, MenuPage::Foveation);
            check(menu_subtitle(n).empty(), "runtime composition off: no note");
            check(has(n, MenuItem::FocusOffset) && has(n, MenuItem::FocusOffsetX),
                  "runtime composition off: the focus area's offsets are offered");
            n.launch.runtimeComposes = true;
            check(!has(n, MenuItem::FocusOffset) && !has(n, MenuItem::FocusOffsetX) && has(n, MenuItem::Feather) &&
                      has(n, MenuItem::Outline),
                  "runtime composing: no offsets (the runtime places the focus area), the feather and outline stay");
            check(menu_subtitle(n).find(L"inside its own focus region") != std::wstring::npos,
                  "runtime composing: Foveation notes the focus area shows inside the runtime's focus region");
            n.optimizeDetail = L"Measuring… keep driving";
            go(n, MenuPage::Picture);
            check(menu_subtitle(n) == L"Measuring… keep driving", "the optimizer's line: on the Resolution card");
        }
        c.cheeky = {true, L"0.6.3-standalone", CheekySettings{}};
        {
            // A game without Cheeky's frame generation (ACC): its Foveated DLSS card, no Frame generation card.
            SettingsMenu g = c;
            g.launch.fgPage = false;
            check(has(g, MenuItem::HeadCheeky) && !has(g, MenuItem::HeadFg) && menu_pages(g).size() == menu_pages(c).size(),
                  "a game without frame generation: the Foveated DLSS card, not the Frame generation one");
        }
        check(acc::kGame.features.foveatedDlss && !acc::kGame.features.frameGeneration &&
                  !ace::kGame.features.foveatedDlss && !ace::kGame.features.frameGeneration,
              "games: ACC has Foveated DLSS but not its frame generation yet; ACE neither (its API path has DLSS of its own)");
        check(menu_pages(c).size() == 5 && menu_page_items(c).front() == MenuItem::Page && menu_page_name(c) == L"Picture" &&
                  at(c, MenuItem::HeadCheeky) > at(c, MenuItem::Optimize),
              "with Cheeky: still five pages; Foveated DLSS on Picture, after the Resolution card");
        check(!menu_key(c, MenuKey::Right) && c.page == 1 && menu_page_name(c) == L"Foveation" && c.selected == 0 &&
                  (menu_key(c, MenuKey::Left), c.page == 0),
              "the switch turns to Foveation and back");
        check(pick(c, MenuItem::CheekyProcessing) && menu_section(c) == MenuItem::HeadCheeky &&
                  menu_subtitle(c) == L"CheekyFoveatedDLSS - v0.6.3" && line_of(c, MenuItem::HeadCheeky).label == L"Foveated DLSS",
              "on the Foveated DLSS card: Cheeky's credit line and version under the title");
        check(menu_key(c, MenuKey::Right) && !c.cheeky.values.processing, "Foveated DLSS turns off");
        pick(c, MenuItem::CheekyOuterScale);
        for (int i = 0; i < 30; ++i)
            menu_key(c, MenuKey::Left);
        check(c.cheeky.values.outerScale == kCheekyScaleMin, "outer DLAA scale stops at Cheeky's minimum");
        pick(c, MenuItem::CheekyOuterPreset);  // outer preset: E, J, K, L, M, never the game default
        bool neverDefault = true;
        for (int i = 0; i < 6; ++i) {
            menu_key(c, MenuKey::Right);
            neverDefault &= c.cheeky.values.outerPreset != 0;
        }
        check(neverDefault && c.cheeky.values.outerPreset == 10, "the outer preset cycles without the game default");
        pick(c, MenuItem::CheekyFocusPreset);  // focus preset: from the game default back to M
        menu_key(c, MenuKey::Left);
        check(c.cheeky.values.focusPreset == 13 && std::wstring(cheeky_preset_name(13)) == L"M",
              "the focus preset wraps from the game default to M");
        check(line_of(c, MenuItem::CheekyProcessing).value == L"Off" && line_of(c, MenuItem::CheekyOuterScale).value == L"20%" &&
                  line_of(c, MenuItem::CheekyFocusPreset).value == L"M" && menu_notes(c).empty() && !menu_resolution(c).shown,
              "the card's lines show Cheeky's values; no resolution or notes there");
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
        go(c, MenuPage::Picture);
        c.launch.dlssHeld = true;
        check(!menu_notes(c).empty() && menu_notes(c).front().find(L"DLSS") == 0,
              "with DLSS holding the views, Picture says resolution waits for the next launch");
        const std::vector<uint32_t> page = draw_menu_panel(menu_title(c), menu_resolution(c), menu_stats(c),
                                                           menu_lines(c), menu_notes(c), L"hint", L"credit");
        check(page.size() == static_cast<size_t>(kPanelWidth) * kPanelHeight, "the panel draws its lines and a subtitle");

        // The experimental focus-view options: supersampling, motion-vector fix, DLSS-NR.
        go(c, MenuPage::Picture);
        pick(c, MenuItem::CheekySupersampling);
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
        check(c.cheeky.values.motionFix && c.cheeky.values.nr && c.cheeky.values.nrPreset == 2 &&
                  c.cheeky.values.nrPasses == kCheekyNrPassesMax && line_of(c, MenuItem::CheekySupersampling).value == L"200%" &&
                  line_of(c, MenuItem::CheekyNrPreset).value == L"B (Faster)" && line_of(c, MenuItem::CheekyNrPasses).value == L"3" &&
                  line_of(c, MenuItem::CheekyNr).label == L"DLSS-NR",
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

        // The pages either way round from the page switch: Left from Picture wraps to Game, then Headset, View, Foveation.
        go(c, MenuPage::Picture);
        menu_key(c, MenuKey::Left);
        check(c.page == 4 && menu_page_name(c) == L"Game", "Left from the first page wraps to Game");
        menu_key(c, MenuKey::Left);
        check(c.page == 3 && menu_page_name(c) == L"Headset", "then Headset");
        menu_key(c, MenuKey::Left);
        check(c.page == 2 && menu_page_name(c) == L"View", "then View");
        menu_key(c, MenuKey::Left);
        check(c.page == 1 && menu_page_name(c) == L"Foveation", "then Foveation");
        go(c, MenuPage::Picture);
        check(has(c, MenuItem::HeadFg), "Frame generation's card on Picture, after Foveated DLSS");
        pick(c, MenuItem::FgOn);
        check(menu_subtitle(c) == L"CheekyFoveatedDLSS - v0.6.3" && menu_notes(c).empty(),
              "Frame generation: the credit line; no note while FG is off");
        menu_key(c, MenuKey::Right);  // FG on
        pick(c, MenuItem::FgMultiplier);
        for (int i = 0; i < 5; ++i)
            menu_key(c, MenuKey::Right);  // multiplier stops at 4x
        pick(c, MenuItem::FgScale);
        for (int i = 0; i < 5; ++i)
            menu_key(c, MenuKey::Left);  // centre FG scale stops at 25%
        check(c.cheeky.values.fg && c.cheeky.values.fgMultiplier == kCheekyFgMultiplierMax &&
                  c.cheeky.values.fgScale == 25 && line_of(c, MenuItem::FgMultiplier).value == L"4x" &&
                  line_of(c, MenuItem::FgScale).value == L"25%" && !menu_notes(c).empty() &&
                  menu_notes(c).front().find(L"Turbo") != std::wstring::npos,
              "FG turns on; its multiplier and scales stop at Cheeky's limits; the card warns about Turbo mode");
        const std::string fgSet = cheeky_set_lines(c.cheeky.values, true);
        check(fgSet.find("FgEnabled=1") != std::string::npos && fgSet.find("FgMultiplier=4") != std::string::npos &&
                  fgSet.find("FgReleasePoint=8") != std::string::npos && fgSet.find("FgWidth=0.30") != std::string::npos &&
                  fgSet.find("FgResolutionPercent=25") != std::string::npos && fgSet.find("FgCenterMethod=0") != std::string::npos,
              "Cheeky's set lines carry FG with the settings that work in ACE (DLSS-G, release after capture)");
        check(fgSet.find("CenterMode=0") != std::string::npos &&
                  cheeky_set_lines(c.cheeky.values, true, true).find("CenterMode=1") != std::string::npos,
              "the FG region follows the eyes (CenterMode 1) only when QVFR's focus does");
        // When Cheeky releases ACE: Capture (8) by default, then Generated (3); Never (5) the other way; the set lines carry it.
        pick(c, MenuItem::FgRelease);
        const bool captureFirst = line_of(c, MenuItem::FgRelease).label == L"Release game" &&
                                  line_of(c, MenuItem::FgRelease).value == L"Capture (8)" &&
                                  cheeky_set_lines(c.cheeky.values, true).find("FgReleasePoint=8") != std::string::npos;
        menu_key(c, MenuKey::Right);
        const bool generated = c.cheeky.values.fgRelease == 3 && line_of(c, MenuItem::FgRelease).value == L"Generated (3)" &&
                               !menu_key(c, MenuKey::Right);  // stops at the end
        menu_key(c, MenuKey::Left);
        menu_key(c, MenuKey::Left);
        check(captureFirst && generated && c.cheeky.values.fgRelease == 5 && line_of(c, MenuItem::FgRelease).value == L"Never (5)" &&
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
        c.cheeky.values.fgBorder = true;
        check(cheeky_set_lines(c.cheeky.values, true, true).find("FgShowBorder=1") != std::string::npos &&
                  menu_lines(c).back().label == L"Show the center region" && menu_lines(c).back().value == L"On",
              "the FG region outline is the page's last row (the launcher's Show the center region), reaching Cheeky");
        go(c, MenuPage::Game);
        menu_key(c, MenuKey::Right);
        check(c.page == 0 && menu_page_name(c) == L"Picture", "Right from Game wraps to Picture");
    }

    // The View & helmet card (QVFR): the crop off shows only its switch; on, four sides, each on its own, written to
    // [crop]; the outer views shrink with it in the fit and the pixels saved. Then the helmet under it.
    {
        SettingsMenu v;
        v.launch = {true, 50, 39, 39, 2000, 2000};
        v.values.width = v.values.height = 39;  // the launch's box
        go(v, MenuPage::View);
        check(menu_page_name(v) == L"View" &&
                  menu_page_items(v) == std::vector<MenuItem>{MenuItem::Page, MenuItem::HeadView, MenuItem::Crop, MenuItem::Helmet} &&
                  menu_resolution(v).shown && line_of(v, MenuItem::HeadView).label == L"View & helmet" &&
                  line_of(v, MenuItem::Helmet).label == L"Show a helmet",
              "View & helmet: the crop's and the helmet's switches (and the resolution: the crop changes it)");
        const int savedBefore = menu_stats(v).savedPercent;
        pick(v, MenuItem::Crop);
        check(menu_key(v, MenuKey::Right) && v.values.crop && menu_page_items(v).size() == 8, "the crop turns on: four sides");
        pick(v, MenuItem::CropTop);
        for (int i = 0; i < 50; ++i)
            menu_key(v, MenuKey::Right);
        pick(v, MenuItem::CropOuter);
        for (int i = 0; i < 10; ++i)
            menu_key(v, MenuKey::Right);
        check(v.values.cropTop == 40 && v.values.cropBottom == 0 && v.values.cropOuter == 10 && v.values.cropInner == 0,
              "the crop: the top stops at 40%; the other sides stay as they were");
        check(menu_stats(v).savedPercent > savedBefore && line_of(v, MenuItem::CropTop).value == L"40%" &&
                  menu_resolution(v).current == L"3360x780 (2.6 MP)",
              "the crop: fewer pixels; 900x600 outer views beside the 780x780 focus views");
        check(menu_notes(v).empty(), "the crop: no note when it only takes more off than at launch");
        v.launch.dlssHeld = true;
        check(menu_notes(v).front().find(L"DLSS is on") == 0, "the crop with DLSS: the savings wait for the next launch");
        const auto cropIni = (std::filesystem::temp_directory_path() / L"simqvfr_menu_crop.ini").wstring();
        DeleteFileW(cropIni.c_str());
        write_menu_values(cropIni.c_str(), v.values, true);
        const MenuValues back = read_menu_values(cropIni.c_str());
        DeleteFileW(cropIni.c_str());
        check(back.crop && back.cropTop == 40 && back.cropOuter == 10 && back.cropBottom == 0, "the crop: [crop] round-trips");
        // The helmet: on, its rim, corners and lighting stepped, written to [helmet].
        v.launch.dlssHeld = false;
        pick(v, MenuItem::Helmet);
        check(menu_key(v, MenuKey::Right) && v.values.helmet.on && has(v, MenuItem::HelmetRim) && has(v, MenuItem::HelmetCorners) &&
                  has(v, MenuItem::HelmetLight) && has(v, MenuItem::HelmetDebug) && line_of(v, MenuItem::Helmet).value == L"On" &&
                  menu_notes(v).empty() && line_of(v, MenuItem::HelmetRim).label == L"Rim" &&
                  line_of(v, MenuItem::HelmetLight).label == L"Lighting",
              "the helmet turns on: rim, corners, lighting and the light diagnostic, by the launcher's names");
        pick(v, MenuItem::HelmetRim);
        for (int i = 0; i < 30; ++i)
            menu_key(v, MenuKey::Right);
        pick(v, MenuItem::HelmetLight);
        menu_key(v, MenuKey::Left);
        pick(v, MenuItem::HelmetDebug);  // the light diagnostic
        menu_key(v, MenuKey::Right);
        write_menu_values(cropIni.c_str(), v.values, true);
        const HelmetSettings helmetBack = read_helmet_settings(cropIni.c_str());
        DeleteFileW(cropIni.c_str());
        check(helmetBack.on && helmetBack.rim == kHelmetRimMax && helmetBack.light == 60 && helmetBack.corners == 40 &&
                  helmetBack.debugLight,
              "Helmet: the rim stops at its maximum, lighting steps by 10, the light diagnostic; [helmet] round-trips");
        // With the helmet's own images: visor, size, height and depth instead of rim and corners.
        v.launch.helmetPieces = true;
        const int h = at(v, MenuItem::Helmet);
        check(at(v, MenuItem::Visor) == h + 1 && at(v, MenuItem::HelmetSize) == h + 2 && at(v, MenuItem::HelmetHeight) == h + 3 &&
                  at(v, MenuItem::HelmetDepth) == h + 4 && at(v, MenuItem::HelmetDetail) == h + 5 && !has(v, MenuItem::HelmetRim),
              "Helmet with images: visor, size, height, depth and detail rows");
        pick(v, MenuItem::HelmetDetail);
        const bool performance = line_of(v, MenuItem::HelmetDetail).value == L"Performance";
        check(performance && menu_key(v, MenuKey::Right) && v.values.helmet.detail &&
                  line_of(v, MenuItem::HelmetDetail).value == L"Detail" && line_of(v, MenuItem::HelmetDetail).label == L"Detail",
              "Helmet detail: Performance (2048) by default, Detail (4096) at a step");
        v.values.helmet.detail = false;
        pick(v, MenuItem::Visor);
        menu_key(v, MenuKey::Right);
        menu_key(v, MenuKey::Right);
        menu_key(v, MenuKey::Right);  // stops at open
        pick(v, MenuItem::HelmetHeight);
        menu_key(v, MenuKey::Left);
        check(v.values.helmet.visor == VisorPosition::Open && line_of(v, MenuItem::Visor).value == L"Open" &&
                  v.values.helmet.height == -1 && line_of(v, MenuItem::HelmetHeight).value == L"1° down",
              "Visor steps to Open and stops; height in degrees");
        // Depth: far, then 3 cm, 1 cm a step up close and 5 cm from 20 cm; back down to far.
        pick(v, MenuItem::HelmetDepth);
        const bool startsFar = line_of(v, MenuItem::HelmetDepth).value == L"Far";
        menu_key(v, MenuKey::Right);
        const bool nearest = v.values.helmet.depth == kHelmetDepthMin && line_of(v, MenuItem::HelmetDepth).value == L"3 cm";
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
        pick(v, MenuItem::Crop);
        v.launch.cropHeight = 50;  // launched with more off: less now is softer until the next launch
        check(!menu_notes(v).empty() && menu_notes(v).back().find(L"Less crop") == 0, "the crop: less than at launch is noted");
    }

    // The focus shape (both methods), on the Foveation card as in the launcher: Rectangle by default with its Corner
    // radius; Custom shape, three lines in the radius's place for any number of points (Adjust point, Up/Down,
    // Left/Right), a percent of the box a step, written to [quad] and read back.
    {
        SettingsMenu f;
        f.launch.qvfr = false;
        go(f, MenuPage::Foveation);
        pick(f, MenuItem::Shape);
        check(line_of(f, MenuItem::Shape).value == L"Rectangle" && has(f, MenuItem::Corners) && !has(f, MenuItem::ShapePoint) &&
                  !menu_notes(f).empty() && menu_notes(f).front().rfind(L"Custom shape", 0) == 0,
              "Shape: Rectangle with its corner radius, and on the line a note on what Custom shape does");
        check(menu_key(f, MenuKey::Right) && f.values.shape.custom && !has(f, MenuItem::Corners) &&
                  at(f, MenuItem::ShapePoint) == at(f, MenuItem::Shape) + 1 &&
                  at(f, MenuItem::ShapePointY) == at(f, MenuItem::Shape) + 2 &&
                  at(f, MenuItem::ShapePointX) == at(f, MenuItem::Shape) + 3 &&
                  line_of(f, MenuItem::Shape).value == L"Custom shape" &&
                  line_of(f, MenuItem::ShapePoint).label == L"Adjust point" &&
                  line_of(f, MenuItem::ShapePoint).value == L"Whole shape" &&
                  line_of(f, MenuItem::ShapePointY).value == L"0" && line_of(f, MenuItem::ShapePointX).value == L"0",
              "Custom shape: Adjust point (Whole shape first, its middle at the box's), its Up/Down and Left/Right under it");
        // Point 2 (the top right): 6 down and 25 in; Left wraps the picker to the last point.
        pick(f, MenuItem::ShapePoint);
        check(!menu_key(f, MenuKey::Right) && f.shapePoint == 0 && line_of(f, MenuItem::ShapePoint).value == L"Point 1 of 4" &&
                  line_of(f, MenuItem::ShapePointY).value == L"50" && line_of(f, MenuItem::ShapePointX).value == L"-50",
              "Adjust point: Right from Whole shape picks point 1 (the box's top left), no value changed");
        check(!menu_key(f, MenuKey::Right) && f.shapePoint == 1 && line_of(f, MenuItem::ShapePoint).value == L"Point 2 of 4",
              "Adjust point: Right picks the next point");
        pick(f, MenuItem::ShapePointY);
        for (int i = 0; i < 6; ++i)
            menu_key(f, MenuKey::Left);
        pick(f, MenuItem::ShapePointX);
        for (int i = 0; i < 25; ++i)
            menu_key(f, MenuKey::Left);
        check(f.values.shape.points[1] == FocusPoint{25, 44} && line_of(f, MenuItem::ShapePointY).value == L"44" &&
                  line_of(f, MenuItem::ShapePointX).value == L"25" && f.values.shape.points[0] == FocusPoint{-50, 50},
              "Up/Down, Left/Right: the picked point moves a percent a step; the others stay");
        pick(f, MenuItem::ShapePointY);
        f.values.shape.points[1].y = kFocusPointReach;
        check(!menu_key(f, MenuKey::Right) && f.values.shape.points[1].y == kFocusPointReach, "Up/Down: held at the box's edge");
        pick(f, MenuItem::ShapePoint);
        f.shapePoint = 0;
        check(!menu_key(f, MenuKey::Left) && f.shapePoint == kFocusWholeShape && !menu_key(f, MenuKey::Left) &&
                  f.shapePoint == 3,
              "Adjust point: Left from the first point to Whole shape, then wraps to the last");
        const auto shapeIni = (std::filesystem::temp_directory_path() / L"simqvfr_menu_shape.ini").wstring();
        DeleteFileW(shapeIni.c_str());
        write_menu_values(shapeIni.c_str(), f.values, false);
        const FocusShape back = read_focus_shape(shapeIni.c_str());
        wchar_t text[64] = {};
        GetPrivateProfileStringW(L"quad", L"shape_points", L"", text, 64, shapeIni.c_str());
        WritePrivateProfileStringW(L"quad", L"shape_points", L"-80,0 0,90 30,-10", shapeIni.c_str());
        const FocusShape clamped = read_focus_shape(shapeIni.c_str());
        DeleteFileW(shapeIni.c_str());
        check(back == f.values.shape && std::wstring(text) == L"-50,50 25,50 50,-50 -50,-50" && clamped.count == 3 &&
                  clamped.points[0] == FocusPoint{-50, 0} && clamped.points[1] == FocusPoint{0, 50},
              "Focus shape: [quad] focus_shape and shape_points round-trip; out of range clamps to the box");
        // The words the layer keeps: a box sends none, as does the outline that is the box itself.
        FocusShape box = f.values.shape;
        box.custom = false;
        FocusShape corners;
        corners.custom = true;
        check(focus_shape_words(f.values.shape).any() && !focus_shape_words(box).any() && !focus_shape_words(corners).any(),
              "Focus shape: the compositor gets the outline; a box (or the box's own corners) sends none");
        f.values.shape.custom = false;
        pick(f, MenuItem::Shape);
        check(menu_key(f, MenuKey::Left) && f.values.shape.custom && f.values.shape.points[1] == FocusPoint{25, 50},
              "Focus shape: back to Custom shape keeps the points set before");
    }
    // Whole shape: Up/Down and Left/Right move every point together, as far as the box allows, the shape kept.
    {
        SettingsMenu w;
        w.launch.qvfr = false;
        w.values.shape = focus_shape_of({{-30, 40}, {30, 40}, {40, -20}, {-40, -20}});  // a windscreen, 80 x 60
        go(w, MenuPage::Foveation);
        check(line_of(w, MenuItem::ShapePoint).value == L"Whole shape" && line_of(w, MenuItem::ShapePointY).value == L"10" &&
                  line_of(w, MenuItem::ShapePointX).value == L"0",
              "Whole shape: its middle 10 up of the box's");
        pick(w, MenuItem::ShapePointY);
        for (int i = 0; i < 20; ++i)
            menu_key(w, MenuKey::Left);
        pick(w, MenuItem::ShapePointX);
        for (int i = 0; i < 6; ++i)
            menu_key(w, MenuKey::Right);
        check(w.values.shape.points[0] == FocusPoint{-24, 20} && w.values.shape.points[2] == FocusPoint{46, -40} &&
                  line_of(w, MenuItem::ShapePointY).value == L"-10" && line_of(w, MenuItem::ShapePointX).value == L"6",
              "Whole shape: 20 down and 6 right, every point with it");
        for (int i = 0; i < 10; ++i)
            menu_key(w, MenuKey::Right);
        check(!menu_key(w, MenuKey::Right) && w.values.shape.points[2] == FocusPoint{50, -40} &&
                  w.values.shape.points[3] == FocusPoint{-30, -40},
              "Whole shape: stops at the box's edge without squashing the shape");
        FocusShape s = focus_shape_of({{-10, 10}, {10, 10}, {0, -10}});
        check(focus_shape_move_to(s, 30, -35) && focus_shape_centre(s) == FocusPoint{30, -35} &&
                  s.points[2] == FocusPoint{30, -45},
              "focus_shape_move_to: the middle where asked");
    }
    // Adding and removing points (the launcher's preview): between the neighbours of the nearest edge, at most ten, at
    // least three.
    {
        FocusShape s = focus_shape_of({{-50, 50}, {50, 50}, {50, -50}, {-50, -50}});
        check(focus_shape_add_point(s, {0, 52}) == 1 && s.count == 5 && s.points[1] == FocusPoint{0, 50} &&
                  s.points[2] == FocusPoint{50, 50},
              "a point added on the top edge goes between the top corners (clamped into the box)");
        check(focus_shape_add_point(s, {-48, 0}) == 5 && s.points[5] == FocusPoint{-48, 0}, "one by the left edge: last");
        while (s.count < kFocusPointsMax)
            focus_shape_add_point(s, {10, -10});
        check(focus_shape_add_point(s, {0, 0}) == -1 && s.count == kFocusPointsMax, "no more than ten points");
        check(focus_shape_remove_point(s, 1) && s.count == kFocusPointsMax - 1 && s.points[1] == FocusPoint{50, 50},
              "a point removed: the next takes its place");
        FocusShape three = focus_shape_of({{0, 50}, {50, -50}, {-50, -50}});
        check(!focus_shape_remove_point(three, 0) && three.count == 3, "never fewer than three");
        check(focus_point_near(three, 48, -47, 6) == 1 && focus_point_near(three, 0, 0, 6) == -1, "the point near a click");
    }
    // The horizontal offset: on the Foveation page under the vertical one, its label by the mode, right and left.
    {
        SettingsMenu h;
        h.launch.qvfr = false;
        go(h, MenuPage::Foveation);
        check(at(h, MenuItem::FocusOffsetX) == at(h, MenuItem::FocusOffset) + 1 &&
                  line_of(h, MenuItem::FocusOffsetX).label == L"Horizontal gaze offset" &&
                  line_of(h, MenuItem::FocusOffsetX).value == L"0°",
              "Horizontal gaze offset: under Gaze offset, 0 to start");
        pick(h, MenuItem::FocusOffsetX);
        menu_key(h, MenuKey::Right);
        menu_key(h, MenuKey::Right);
        check(h.values.offset.gazeX == 2 && h.values.offset.fixedX == 0 &&
                  line_of(h, MenuItem::FocusOffsetX).value == L"2° right",
              "Horizontal gaze offset: Right moves it right, the fixed one stays");
        h.values.eyeTracking = false;
        menu_key(h, MenuKey::Left);
        check(h.values.offset.fixedX == -1 && line_of(h, MenuItem::FocusOffsetX).label == L"Horizontal position" &&
                  line_of(h, MenuItem::FocusOffsetX).value == L"1° left",
              "Horizontal position: the fixed box's own, left");
        const auto offsetIni = (std::filesystem::temp_directory_path() / L"simqvfr_menu_offset_x.ini").wstring();
        DeleteFileW(offsetIni.c_str());
        write_menu_values(offsetIni.c_str(), h.values, false);
        const FocusOffset read = read_focus_offset(offsetIni.c_str());
        DeleteFileW(offsetIni.c_str());
        check(read.fixedX == -1 && read.gazeX == 2, "Horizontal offsets: written to [quad] and read back");
    }
    {  // The Profile line (profile/profiles.h): first on the Picture page, stepping through and asking the layer.
        SettingsMenu p;
        p.launch = {true, 50, 39, 39};
        check(menu_page_items(p)[1] != MenuItem::Profile, "Profile: no line without profiles");
        p.profiles = {L"Performance", L"Balanced", L"Quality", L"Night"};
        p.selected = 1;
        check(menu_page_items(p)[1] == MenuItem::Profile && menu_lines(p)[1].label == L"Profile" &&
                  menu_lines(p)[1].value == L"Your own" && menu_page_items(p)[2] == MenuItem::HeadResolution,
              "Profile: Picture's first line, before the Resolution card; none in use shows as your own");
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
    {  // The longest page scrolls: the page switch pinned, the list from menu_first_visible, three notes, the footer and
       // the hint, none over another.
        SettingsMenu full;
        full.launch = {true, 50, 39, 39, 4000, 3400};
        full.launch.sharpening = true;
        full.profiles = {L"Performance", L"Balanced", L"Quality"};
        full.values.shape.custom = true;  // Custom shape: three lines in the radius's place
        go(full, MenuPage::Foveation);
        const std::vector<std::wstring> notes = {L"note one", L"note two", L"note three"};
        const std::wstring footer = L"Overlay Ctrl+Alt+P  ·  Visor Ctrl+Alt+V";
        const int height = menu_list_height(true, notes.size(), true);
        std::vector<MenuLine> lines = menu_lines(full);
        int listed = 0;
        for (size_t i = 1; i < lines.size(); ++i)
            listed += lines[i].header ? kMenuHeaderHeight : kMenuLineHeight;
        check(listed > height && menu_first_visible(lines, 1, height) == 1, "a page longer than the panel starts at its top");
        full.selected = static_cast<int>(lines.size()) - 1;  // Show focus outline, the last
        lines = menu_lines(full);
        const int first = menu_first_visible(lines, 1, height);
        int shownHeight = 0;
        for (size_t i = static_cast<size_t>(first); i < lines.size(); ++i)
            shownHeight += lines[i].header ? kMenuHeaderHeight : kMenuLineHeight;
        check(first > 1 && shownHeight <= height && menu_first_visible(lines, first, height) == first,
              "selecting the last line scrolls just far enough to show it, and stays");
        full.selected = at(full, MenuItem::EyeTracking);
        lines = menu_lines(full);
        check(menu_first_visible(lines, first, height) == at(full, MenuItem::HeadFoveation),
              "back up to Foveated mode: its heading shows with it");
        const std::vector<uint32_t> page = draw_menu_panel(menu_title(full), menu_resolution(full), menu_stats(full),
                                                           menu_lines(full), notes, L"hint", L"", footer, L"", false,
                                                           menu_tabs(full), full.page, menu_first_visible(menu_lines(full), 1, height));
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
        check(lastNoteRow < kPanelHeight - 100 && footerText, "the panel: the list and three notes end above the footer, which shows");
        // SIMQVFR_MENU_SAVE=<path>.bmp: this page saved there, and LMU's Picture page (DLSS, DLSS-NR, Vision) beside it
        // as <path>-picture.bmp, to look at.
        const auto save = [](const std::wstring& path, const std::vector<uint32_t>& pixels) {
            if (FILE* f = _wfopen(path.c_str(), L"wb")) {
                const int w = kPanelWidth, h = kPanelHeight;
                BITMAPFILEHEADER file{0x4D42, static_cast<DWORD>(sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER) + w * h * 4), 0, 0,
                                      sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER)};
                BITMAPINFOHEADER info{sizeof(BITMAPINFOHEADER), w, -h, 1, 32, BI_RGB};
                fwrite(&file, sizeof(file), 1, f);
                fwrite(&info, sizeof(info), 1, f);
                for (const uint32_t p : pixels) {
                    const uint8_t bgra[4] = {static_cast<uint8_t>(p >> 16), static_cast<uint8_t>(p >> 8), static_cast<uint8_t>(p), 255};
                    fwrite(bgra, 4, 1, f);
                }
                fclose(f);
            }
        };
        if (wchar_t path[MAX_PATH] = {}; GetEnvironmentVariableW(L"SIMQVFR_MENU_SAVE", path, MAX_PATH)) {
            save(path, page);
            SettingsMenu lmu;
            lmu.launch = {true, 40, 40, 40, 5190, 4168, 36.95};
            lmu.launch.sharpening = lmu.launch.ownDlss = lmu.launch.ownNr = lmu.launch.eyeOff = true;
            lmu.profiles = {L"Performance", L"Balanced", L"Quality"};
            lmu.values.dlss.on = true;
            pick(lmu, MenuItem::DlssFocusMode);
            const std::vector<MenuLine> lmuLines = menu_lines(lmu);
            const MenuResolution lmuRes = menu_resolution(lmu);
            const std::vector<std::wstring> lmuNotes = menu_notes(lmu);
            const int lmuFirst = menu_first_visible(lmuLines, 1, menu_list_height(lmuRes.shown, lmuNotes.size(), true));
            std::wstring other = path;
            other.insert(other.size() - 4, L"-picture");
            save(other, draw_menu_panel(menu_title(lmu), lmuRes, menu_stats(lmu), lmuLines, lmuNotes, L"hint", menu_subtitle(lmu),
                                        footer, L"GPU 9.8 ms · CPU 7.1 ms · 1.3 ms left · 0% missed", false,
                                        menu_tabs(lmu), lmu.page, lmuFirst));
        }
    }
    {  // The hotkeys footer ([menu] hotkey_footer): on by default, on the Headset menu card, read and written.
        SettingsMenu f;
        f.launch = {true, 50, 39, 39};
        go(f, MenuPage::Headset);
        pick(f, MenuItem::HotkeyFooter);
        check(menu_page(f) == MenuPage::Headset && f.values.hotkeyFooter &&
                  line_of(f, MenuItem::HotkeyFooter).label == L"Show the hotkeys under the menu" &&
                  line_of(f, MenuItem::HotkeyFooter).value == L"On" && at(f, MenuItem::HotkeyFooter) < at(f, MenuItem::HeadOverlay),
              "Show the hotkeys under the menu: on by default, the Headset menu card's last line");
        check(menu_key(f, MenuKey::Right) && !f.values.hotkeyFooter && line_of(f, MenuItem::HotkeyFooter).value == L"Off",
              "the hotkeys footer: Right or Left turns it off and on");
        const std::wstring ini = temp_path(L"simqvfr-menu-footer.ini");
        DeleteFileW(ini.c_str());
        check(read_menu_values(ini.c_str()).hotkeyFooter && write_menu_values(ini.c_str(), f.values, true) &&
                  !read_menu_values(ini.c_str()).hotkeyFooter,
              "the hotkeys footer: unset is on; written and read back");
        DeleteFileW(ini.c_str());
    }
    {  // The main thread's core ([cpu] isolate_main_thread): every game, the Game page's CPU card, off by default.
        SettingsMenu f;
        f.launch = {true, 50, 39, 39};
        check(menu_pages(f).back() == MenuPage::Game && go(f, MenuPage::Game) && menu_page_name(f) == L"Game" &&
                  menu_page_items(f) == std::vector<MenuItem>{MenuItem::Page, MenuItem::HeadCpu, MenuItem::CpuIsolateMain} &&
                  line_of(f, MenuItem::HeadCpu).label == L"CPU",
              "Game: the last page, its CPU card");
        pick(f, MenuItem::CpuIsolateMain);
        check(!f.values.cpuIsolateMain && line_of(f, MenuItem::CpuIsolateMain).label == L"Give the main thread a core of its own" &&
                  line_of(f, MenuItem::CpuIsolateMain).value == L"Off",
              "the main thread's core: the launcher's name, off by default");
        check(menu_key(f, MenuKey::Right) && f.values.cpuIsolateMain && line_of(f, MenuItem::CpuIsolateMain).value == L"On",
              "a step gives it its own core");
        const std::wstring ini = temp_path(L"simqvfr-menu-cpu.ini");
        DeleteFileW(ini.c_str());
        check(!read_menu_values(ini.c_str()).cpuIsolateMain && write_menu_values(ini.c_str(), f.values, false) &&
                  read_menu_values(ini.c_str()).cpuIsolateMain,
              "the main thread's core: unset is off; written (Two-loop too) and read back");
        DeleteFileW(ini.c_str());
        SettingsMenu headset;
        go(headset, MenuPage::Headset);
        check(!has(headset, MenuItem::CpuIsolateMain), "not on the Headset page");
    }

    // Only what changed is written (ini_put.h): WritePrivateProfileString rewrites the whole ini for every key, even
    // one that already holds the value, and a step changes one key of the ~50 the menu writes.
    {
        const std::wstring ini = temp_path(L"simqvfr-menu-only-changes.ini");
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
        const size_t at = expected.find("focus_percent=35");
        if (at != std::string::npos)
            expected.replace(at, 16, "focus_percent=33");
        check(at != std::string::npos && bytes() == expected, "menu step: only the changed key's line is rewritten");
        DeleteFileW(ini.c_str());

        // On the documented ini (34 KB): 20 steps took ~7 s when every key was rewritten (0.37 s a step).
        const std::wstring documented = temp_path(L"simqvfr-menu-documented.ini");
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

    // Sim-QVFR's own DLSS (engine/dlss_settings.h): its card on the View page only where the launch can run it, its
    // choices stepped, and [dlss] in the ini, read from an older file's [experiment] lmu_dlss_* keys while [dlss] lacks
    // its own.
    {
        SettingsMenu dm;
        dm.launch = {true, 50, 39, 39};
        const auto has_dlss = [](SettingsMenu s) { return go(s, MenuPage::Picture) && has(s, MenuItem::DlssOn); };
        check(!has_dlss(dm), "DLSS card: none without a launch that can run it");
        dm.launch.ownDlss = true;
        check(has_dlss(dm), "DLSS card: shown for the launch, on the Picture page");
        dm.launch.qvfr = false;
        check(!has_dlss(dm), "DLSS card: none in Two-loop");
        dm.launch.qvfr = true;
        go(dm, MenuPage::Picture);
        check(at(dm, MenuItem::HeadDlss) == at(dm, MenuItem::Optimize) + 1 && at(dm, MenuItem::DlssOn) == at(dm, MenuItem::HeadDlss) + 1 &&
                  menu_page_items(dm).back() == MenuItem::DlssOn && line_of(dm, MenuItem::DlssOn).label == L"Use DLSS" &&
                  line_of(dm, MenuItem::HeadDlss).label == L"DLSS",
              "DLSS card: off, only its switch (the launcher's Use DLSS), after the Resolution card");
        pick(dm, MenuItem::DlssOn);
        check(menu_key(dm, MenuKey::Right) && dm.values.dlss.on && has(dm, MenuItem::DlssFocusMode) &&
                  has(dm, MenuItem::DlssFocusPreset) && has(dm, MenuItem::DlssEyePreset) && has(dm, MenuItem::DlssSharpening) &&
                  line_of(dm, MenuItem::DlssOn).value == L"On",
              "DLSS card: on, then the focus views' mode and preset, the eyes' preset and texture sharpening");
        check(menu_subtitle(dm).find(L"NVIDIA DLSS") == 0, "DLSS card: what runs it, under the title");
        pick(dm, MenuItem::DlssFocusMode);  // Focus views: DLAA, then smaller renders; the ends hold
        check(!menu_key(dm, MenuKey::Left) && dm.values.dlss.focusScale == 100 && line_of(dm, MenuItem::DlssFocusMode).value == L"DLAA",
              "DLSS card: Left from DLAA stays at DLAA");
        for (int i = 0; i < 4; ++i)
            menu_key(dm, MenuKey::Right);
        check(dm.values.dlss.focusScale == 50 && line_of(dm, MenuItem::DlssFocusMode).value == L"Performance (50%)" &&
                  !menu_key(dm, MenuKey::Right),
              "DLSS card: Right steps to Performance (50%), the last");
        pick(dm, MenuItem::DlssFocusPreset);
        check(line_of(dm, MenuItem::DlssFocusPreset).value == L"Default (M)",
              "DLSS card: the focus preset's default named as NGX's for Performance");
        check(menu_key(dm, MenuKey::Right) && dm.values.dlss.focusPreset == 11 && line_of(dm, MenuItem::DlssFocusPreset).value == L"K",
              "DLSS card: the focus preset steps to K");
        pick(dm, MenuItem::DlssEyePreset);
        check(dm.values.dlss.eyePreset == 11 && menu_key(dm, MenuKey::Right) && dm.values.dlss.eyePreset == 6 &&
                  line_of(dm, MenuItem::DlssEyePreset).value == L"F (cheaper)",
              "DLSS card: the eyes' preset K by default, F a step on");
        pick(dm, MenuItem::DlssSharpening);  // texture sharpening: off by default, then the focus views, then the eyes too
        check(dm.values.dlss.textureSharpening == 0 && line_of(dm, MenuItem::DlssSharpening).value == L"Off" &&
                  !menu_key(dm, MenuKey::Left),
              "DLSS card: texture sharpening off by default, Left from Off stays there");
        check(menu_key(dm, MenuKey::Right) && dm.values.dlss.textureSharpening == 1 &&
                  line_of(dm, MenuItem::DlssSharpening).value == L"Focus only" && menu_key(dm, MenuKey::Right) &&
                  dm.values.dlss.textureSharpening == 2 && line_of(dm, MenuItem::DlssSharpening).value == L"Focus and periphery" &&
                  !menu_key(dm, MenuKey::Right),
              "DLSS card: texture sharpening steps to Focus only, then Focus and periphery, the last");
        const auto has_note = [&](const wchar_t* start) {
            for (const std::wstring& n : menu_notes(dm))
                if (n.rfind(start, 0) == 0)
                    return true;
            return false;
        };
        check(has_note(L"Texture sharpening was off at launch"),
              "DLSS card: texture sharpening turned on with it off at launch: the note says the next launch");
        dm.launch.sharpeningReady = true;
        check(!has_note(L"Texture sharpening was off at launch"), "DLSS card: on at launch: no such note (its settings live)");
        {  // AMD FSR, an experiment (MenuLaunch::fsr): the Upscaler row after the switch; NGX's presets go under FSR
            SettingsMenu fm = dm;
            fm.launch.fsr = true;
            check(at(fm, MenuItem::DlssUpscaler) == at(fm, MenuItem::DlssOn) + 1 &&
                      line_of(fm, MenuItem::DlssUpscaler).value == L"NVIDIA DLSS",
                  "DLSS card with FSR allowed: the Upscaler row, NVIDIA DLSS by default");
            pick(fm, MenuItem::DlssUpscaler);
            fm.values.dlss.focusScale = 100;
            menu_key(fm, MenuKey::Right);
            check(fm.values.dlss.upscaler == 1 && line_of(fm, MenuItem::DlssUpscaler).value == L"AMD FSR" &&
                      has(fm, MenuItem::DlssFocusMode) && has(fm, MenuItem::DlssSharpening) && !has(fm, MenuItem::DlssFocusPreset) &&
                      !has(fm, MenuItem::DlssEyePreset) && line_of(fm, MenuItem::DlssFocusMode).value == L"Native AA" &&
                      line_of(fm, MenuItem::HeadDlss).label == L"FSR",
                  "Right takes AMD FSR: the focus views (DLAA named Native AA) and texture sharpening; the card is FSR's");
            check(menu_key(fm, MenuKey::Left) && fm.values.dlss.upscaler == 0 && has(fm, MenuItem::DlssFocusPreset),
                  "back to NVIDIA DLSS, its presets again");
            check(!has(dm, MenuItem::DlssUpscaler), "DLSS card without the experiment: no Upscaler row");
        }
        {  // A PC without an NVIDIA RTX card: AMD FSR runs the views; the card is FSR's, no presets, no Upscaler row
            SettingsMenu am = dm;
            am.launch.fsrPc = true;
            am.launch.fsr = true;  // the RTX test's key changes nothing here
            am.values.dlss.focusScale = 100;
            check(line_of(am, MenuItem::HeadDlss).label == L"FSR" && !has(am, MenuItem::DlssUpscaler) && !has(am, MenuItem::DlssFocusPreset) &&
                      at(am, MenuItem::DlssFocusMode) == at(am, MenuItem::DlssOn) + 1 &&
                      line_of(am, MenuItem::DlssOn).label == L"Use FSR" && line_of(am, MenuItem::DlssFocusMode).value == L"Native AA",
                  "FSR without an RTX card: the card FSR's, its switch Use FSR, Native AA, no presets or Upscaler row");
        }

        const std::wstring ini = temp_path(L"simqvfr-menu-dlss.ini");
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
        check(fresh.upscaler == 0 && old.upscaler == 0, "[dlss]: upscaler NVIDIA DLSS unless the file says fsr");
        mine.upscaler = 1;
        check(write_dlss_settings(ini.c_str(), mine) && read_dlss_settings(ini.c_str()) == mine,
              "[dlss]: written and read back (the other cars' motion off, AMD FSR too), ahead of the older keys");
        wchar_t text0[16] = {};
        GetPrivateProfileStringW(L"dlss", L"upscaler", L"", text0, 16, ini.c_str());
        check(std::wstring(text0) == L"fsr", "[dlss]: the upscaler written by name");
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

    // DLSS-NR on those views (engine/nr_settings.h): its card only where the launch can run it (the player's
    // nvngx_dlssnr.dll there), its switches and sliders stepped within their ranges, [dlss_nr] read with the menu's values.
    {
        SettingsMenu nm;
        nm.launch = {true, 50, 39, 39};
        nm.launch.ownDlss = true;
        const auto has_nr = [](SettingsMenu s) { return go(s, MenuPage::Picture) && has(s, MenuItem::NrOn); };
        check(!has_nr(nm), "DLSS-NR card: none without the player's DLL at launch");
        nm.launch.ownNr = true;
        check(has_nr(nm), "DLSS-NR card: shown for the launch");
        nm.launch.ownDlss = false;
        check(!has_nr(nm), "DLSS-NR card: none without Sim-QVFR's own DLSS");
        nm.launch.ownDlss = true;
        go(nm, MenuPage::Picture);
        check(at(nm, MenuItem::HeadNr) > at(nm, MenuItem::DlssOn) && menu_page_items(nm).back() == MenuItem::NrOn &&
                  line_of(nm, MenuItem::NrOn).label == L"Use DLSS-NR" && line_of(nm, MenuItem::HeadNr).label == L"DLSS-NR",
              "DLSS-NR card: after the DLSS card, off, only its switch (the launcher's Use DLSS-NR)");
        pick(nm, MenuItem::NrOn);
        check(menu_key(nm, MenuKey::Right) && nm.values.nr.on && has(nm, MenuItem::NrIntensity) && has(nm, MenuItem::NrPeriphery) &&
                  !has(nm, MenuItem::NrPeripheryScale) && line_of(nm, MenuItem::NrOn).value == L"On",
              "DLSS-NR card: on, then the model's settings, the order, the focus working size, the edge blend, the periphery");
        check(menu_subtitle(nm).find(L"neural rendering") != std::wstring::npos, "DLSS-NR card: what it is, under the title");
        pick(nm, MenuItem::NrIntensity);
        check(!menu_key(nm, MenuKey::Right) && nm.values.nr.intensity == 100 && menu_key(nm, MenuKey::Left) &&
                  nm.values.nr.intensity == 95 && line_of(nm, MenuItem::NrIntensity).value == L"95%",
              "DLSS-NR card: intensity stops at 100%, steps down by 5");
        pick(nm, MenuItem::NrStyle);
        check(line_of(nm, MenuItem::NrStyle).value == L"Standard" && menu_key(nm, MenuKey::Right) && menu_key(nm, MenuKey::Right) &&
                  line_of(nm, MenuItem::NrStyle).value == L"Cinematic" && !menu_key(nm, MenuKey::Right),
              "DLSS-NR card: style steps Standard, Natural, Cinematic, the last");
        pick(nm, MenuItem::NrPreset);
        check(line_of(nm, MenuItem::NrPreset).value == L"Default" && menu_key(nm, MenuKey::Right) && nm.values.nr.preset == 1 &&
                  line_of(nm, MenuItem::NrPreset).value == L"A (original)",
              "DLSS-NR card: preset steps from the runtime's default to A");
        pick(nm, MenuItem::NrLocalTone);
        for (int i = 0; i < 12; ++i)
            menu_key(nm, MenuKey::Right);
        check(nm.values.nr.localTone == 200 && line_of(nm, MenuItem::NrLocalTone).value == L"200%",
              "DLSS-NR card: local tone stops at 200%");
        pick(nm, MenuItem::NrOrder);
        check(line_of(nm, MenuItem::NrOrder).value == L"After DLSS" && menu_key(nm, MenuKey::Right) && nm.values.nr.beforeDlss &&
                  line_of(nm, MenuItem::NrOrder).value == L"Before DLSS",
              "DLSS-NR card: it runs after DLSS by default, before it a step on");
        pick(nm, MenuItem::NrFocusScale);
        for (int i = 0; i < 20; ++i)
            menu_key(nm, MenuKey::Left);
        check(nm.values.nr.focusScale == kNrScaleMin, "DLSS-NR card: the focus working size stops at its minimum");
        pick(nm, MenuItem::NrLimit);
        check(line_of(nm, MenuItem::NrLimit).value == L"1.0 MP a view" && menu_key(nm, MenuKey::Right) &&
                  nm.values.nr.workingLimit == 11 && line_of(nm, MenuItem::NrLimit).value == L"1.1 MP a view",
              "DLSS-NR card: the working size limit is 1.0 megapixel a view by default, a tenth a step");
        pick(nm, MenuItem::NrPeriphery);
        check(!nm.values.nr.periphery && menu_key(nm, MenuKey::Right) && nm.values.nr.periphery &&
                  menu_page_items(nm).back() == MenuItem::NrPeripheryCoverage,
              "DLSS-NR card: the periphery off by default; on, its working size and coverage follow");
        pick(nm, MenuItem::NrPeripheryCoverage);
        check(line_of(nm, MenuItem::NrPeripheryCoverage).value == L"100%" && menu_key(nm, MenuKey::Left) &&
                  nm.values.nr.peripheryCoverage == 90,
              "DLSS-NR card: the periphery covered steps down from all of it");
        const auto has_note = [&](const wchar_t* start) {
            for (const std::wstring& n : menu_notes(nm))
                if (n.rfind(start, 0) == 0)
                    return true;
            return false;
        };
        check(has_note(L"Runs with DLSS on"), "DLSS-NR card: with DLSS off the note says it runs with DLSS on");
        nm.values.dlss.on = true;
        check(!has_note(L"Runs with DLSS on"), "DLSS-NR card: with DLSS on, no such note");

        const std::wstring ini = temp_path(L"simqvfr-menu-nr.ini");
        DeleteFileW(ini.c_str());
        check(write_nr_settings(ini.c_str(), nm.values.nr) && read_menu_values(ini.c_str()).nr == nm.values.nr,
              "[dlss_nr]: the menu's values read what the page wrote");
        DeleteFileW(ini.c_str());
    }
    {  // Road focus ([quad] focus_corridor): the Foveation page's switch for a game with it, fixed foveation only
        SettingsMenu r;
        r.launch = {true, 50, 39, 39};
        r.values.eyeTracking = false;
        go(r, MenuPage::Foveation);
        check(!has(r, MenuItem::FocusCorridor), "road focus: not on the Foveation page of a game without it");
        r.launch.focusCorridor = true;
        check(has(r, MenuItem::FocusCorridor) && line_of(r, MenuItem::FocusCorridor).value == L"Off" &&
                  !has(r, MenuItem::FocusCorridorPreset) && !MenuValues{}.focusCorridor,
              "road focus: off by default (the player opts in), its lines hidden");
        r.values.focusCorridor = true;
        check(has(r, MenuItem::FocusCorridor) && at(r, MenuItem::FocusCorridor) + 1 == at(r, MenuItem::FocusCorridorPreset) &&
                  at(r, MenuItem::FocusCorridorPreset) + 1 == at(r, MenuItem::FocusCorridorWidth) &&
                  line_of(r, MenuItem::FocusCorridorPreset).value == L"Preset A" &&
                  at(r, MenuItem::FocusCorridorWidth) + 1 == at(r, MenuItem::FocusCorridorHeight) &&
                  at(r, MenuItem::FocusCorridorHeight) + 1 == at(r, MenuItem::FocusCorridorSpread) &&
                  at(r, MenuItem::FocusCorridorSpread) + 1 == at(r, MenuItem::FocusCorridorBlend) &&
                  line_of(r, MenuItem::FocusCorridorSpread).label == L"Road widening" &&
                  line_of(r, MenuItem::FocusCorridorSpread).value == L"15°" &&
                  at(r, MenuItem::FocusCorridorBlend) + 1 == at(r, MenuItem::FocusCorridorShow) &&
                  at(r, MenuItem::FocusCorridorShow) + 1 == at(r, MenuItem::Outline) &&
                  line_of(r, MenuItem::FocusCorridorBlend).value == L"1.0 m" &&
                  line_of(r, MenuItem::FocusCorridorShow).value == L"Off" &&
                  line_of(r, MenuItem::FocusCorridorHeight).label == L"Road height" &&
                  line_of(r, MenuItem::FocusCorridorHeight).value == L"3.5 m" &&
                  line_of(r, MenuItem::FocusCorridor).label == L"Road focus" &&
                  line_of(r, MenuItem::FocusCorridor).value == L"On" &&
                  line_of(r, MenuItem::FocusCorridorWidth).label == L"Road width" &&
                  line_of(r, MenuItem::FocusCorridorWidth).value == L"10 m",
              "road focus on: its preset (A), width (10 m) and height (3.5 m) under it, before the outline");
        r.launch.ownDlss = true;
        r.values.dlss.on = true;
        check(at(r, MenuItem::FocusCorridorShow) + 1 == at(r, MenuItem::Outline),
              "road focus: no DLSS band line, with Sim-QVFR's DLSS on too");
        pick(r, MenuItem::FocusCorridorBlend);
        check(menu_key(r, MenuKey::Left) && r.values.focusCorridorBlend == 9 &&
                  line_of(r, MenuItem::FocusCorridorBlend).value == L"0.9 m",
              "road edge blend: a tenth of a metre a step");
        pick(r, MenuItem::FocusCorridorSpread);
        for (int i = 0; i < 40; ++i)
            menu_key(r, MenuKey::Right);
        check(r.values.focusCorridorSpread == 25 && !menu_key(r, MenuKey::Right) &&
                  line_of(r, MenuItem::FocusCorridorSpread).value == L"25°",
              "road widening: up to 25 degrees, a degree a step");
        for (int i = 0; i < 40; ++i)
            menu_key(r, MenuKey::Left);
        check(r.values.focusCorridorSpread == 10, "road widening: down to 10 (below it corners fall out)");
        r.values.focusCorridorSpread = 12;
        pick(r, MenuItem::FocusCorridorShow);
        check(menu_key(r, MenuKey::Right) && r.values.focusCorridorShow, "show road focus area: switches on");
        r.values.focusCorridorBlend = kFocusCorridorBlend.def;
        r.values.focusCorridorShow = false;
        r.values.eyeTracking = true;
        check(!has(r, MenuItem::FocusCorridor) && !has(r, MenuItem::FocusCorridorWidth),
              "road focus: hidden while the focus follows the eyes");
        r.values.eyeTracking = false;
        pick(r, MenuItem::FocusCorridorWidth);
        for (int i = 0; i < 20; ++i)
            menu_key(r, MenuKey::Right);
        check(r.values.focusCorridorWidth == 20 && !menu_key(r, MenuKey::Right), "road width: up to 20 m, a metre a step");
        for (int i = 0; i < 20; ++i)
            menu_key(r, MenuKey::Left);
        check(r.values.focusCorridorWidth == 5, "road width: down to 5 m");
        pick(r, MenuItem::FocusCorridorHeight);
        menu_key(r, MenuKey::Right);
        check(r.values.focusCorridorHeight == 40 && line_of(r, MenuItem::FocusCorridorHeight).value == L"4.0 m",
              "road height: half a metre a step");
        for (int i = 0; i < 20; ++i)
            menu_key(r, MenuKey::Left);
        check(r.values.focusCorridorHeight == 20, "road height: down to 2 m");
        pick(r, MenuItem::FocusCorridor);
        check(menu_key(r, MenuKey::Right) && !r.values.focusCorridor && !has(r, MenuItem::FocusCorridorWidth) &&
                  !has(r, MenuItem::FocusCorridorHeight) && !has(r, MenuItem::FocusCorridorPreset),
              "road focus: off, its preset, width and height hidden");
        {  // On: the cockpit's shape and size, the player's kept; off: theirs back.
            const FocusShape own = focus_shape_of({{-50, 40}, {50, 40}, {40, -50}, {-40, -50}});
            r.values.shape = own;
            r.values.width = 44;
            r.values.height = 38;
            pick(r, MenuItem::FocusCorridor);
            menu_key(r, MenuKey::Right);
            check(r.values.focusCorridor && r.values.shape == road_focus_cockpit_shape() &&
                      r.values.width == kRoadFocusCockpitWidth && r.values.height == kRoadFocusCockpitHeight &&
                      r.values.roadSaved.have && r.values.roadSaved.shape == own && r.values.roadSaved.width == 44,
                  "road focus on: the cockpit shape at its size, the player's own kept");
            const std::wstring savedIni = temp_path(L"simqvfr-menu-road-saved.ini");
            DeleteFileW(savedIni.c_str());
            write_menu_values(savedIni.c_str(), r.values, true, false, false, false, false, false, true);
            check(read_menu_values(savedIni.c_str()).roadSaved == r.values.roadSaved,
                  "road focus: the player's own shape and size kept in the ini");
            DeleteFileW(savedIni.c_str());
            pick(r, MenuItem::FocusCorridor);
            menu_key(r, MenuKey::Right);
            check(!r.values.focusCorridor && r.values.shape == own && r.values.width == 44 && r.values.height == 38 &&
                      !r.values.roadSaved.have,
                  "road focus off: the player's own shape and size back");
            {  // Preset B: picked while on, set at once (smaller, 8 degrees down); off, the player's own place back
                r.values.offset.fixed = 3;
                pick(r, MenuItem::FocusCorridor);
                menu_key(r, MenuKey::Right);  // on, preset A
                pick(r, MenuItem::FocusCorridorPreset);
                check(menu_key(r, MenuKey::Right) && r.values.focusCorridorPreset == 1 && r.values.width == 46 &&
                          r.values.height == 43 && r.values.offset.fixed == -8 &&
                          line_of(r, MenuItem::FocusCorridorPreset).value == L"Preset B",
                      "road focus preset B: picked while on, its size and place set at once");
                check(!menu_key(r, MenuKey::Right), "road focus preset: B the last");
                pick(r, MenuItem::FocusCorridor);
                menu_key(r, MenuKey::Right);  // off
                check(r.values.shape == own && r.values.width == 44 && r.values.height == 38 && r.values.offset.fixed == 3,
                      "road focus off after preset B: the player's own shape, size and place back");
                r.values.focusCorridorPreset = 0;
                r.values.offset.fixed = 0;
            }
            r.launch.runtimeFocusWidth = 46;  // native quad views: the runtime's focus region
            r.launch.runtimeFocusHeight = 52;
            pick(r, MenuItem::FocusCorridor);
            menu_key(r, MenuKey::Right);
            check(r.values.width == 46 && r.values.height == 52 && r.values.shape == road_focus_cockpit_shape(),
                  "road focus on under native quad views: the cockpit shape no larger than the runtime's focus region");
            pick(r, MenuItem::FocusCorridor);
            menu_key(r, MenuKey::Right);
        }
        {  // a preset set before the runtime's focus region was known, capped once it is (road_focus_capped)
            FocusShape shape;
            int width = kFocusPercent.def, height = kFocusHeightPercent.def;
            FocusOffset offset;
            road_focus_apply(0, shape, width, height, offset);  // no region known: preset A at its own size
            check(width == kRoadFocusCockpitWidth && height == kRoadFocusCockpitHeight,
                  "road focus cap: preset A uncapped with no region known");
            check(!road_focus_capped(0, shape, width, height, 0, 0) && width == kRoadFocusCockpitWidth,
                  "road focus cap: nothing while the region is unknown");
            check(road_focus_capped(0, shape, width, height, 46, 52) && width == 46 && height == 52,
                  "road focus cap: preset A capped to the runtime's region");
            check(!road_focus_capped(0, shape, width, height, 46, 52), "road focus cap: once (already inside)");
            int own = 72, ownH = 60;  // the player's own size on the preset's shape: theirs
            check(!road_focus_capped(0, shape, own, ownH, 46, 52) && own == 72,
                  "road focus cap: a size the player changed is left alone");
            FocusShape rect;  // the player's own shape at preset A's size: theirs too
            int rw = kRoadFocusCockpitWidth, rh = kRoadFocusCockpitHeight;
            check(!road_focus_capped(0, rect, rw, rh, 46, 52), "road focus cap: another shape is left alone");
            road_focus_apply(1, shape, width, height, offset);  // preset B (46 x 43) fits a 46 x 52 region
            check(!road_focus_capped(1, shape, width, height, 46, 52) && width == 46 && height == 43,
                  "road focus cap: preset B already inside the region");
            // Through an ini, as the layer reads it: the preset's shape reads back equal.
            const std::wstring capIni = temp_path(L"simqvfr-menu-corridor-cap.ini");
            DeleteFileW(capIni.c_str());
            MenuValues capValues;
            road_focus_apply(0, capValues.shape, capValues.width, capValues.height, capValues.offset);
            capValues.focusCorridor = true;
            write_menu_values(capIni.c_str(), capValues, true, false, false, false, false, false, true);
            int iw = ini_int(capIni.c_str(), kFocusPercent), ih = ini_int(capIni.c_str(), kFocusHeightPercent, iw);
            check(road_focus_capped(ini_int(capIni.c_str(), kFocusCorridorPreset), read_focus_shape(capIni.c_str()), iw,
                                    ih, 46, 52) &&
                      iw == 46 && ih == 52,
                  "road focus cap: the preset read back from an ini is capped");
            DeleteFileW(capIni.c_str());
        }
        const std::wstring ini = temp_path(L"simqvfr-menu-corridor.ini");
        DeleteFileW(ini.c_str());
        write_menu_values(ini.c_str(), r.values, true, false, false, false, false, false, false);
        check(GetPrivateProfileIntW(L"quad", L"focus_corridor", -1, ini.c_str()) == -1,
              "road focus: not written for a game without it");
        write_menu_values(ini.c_str(), r.values, true, false, false, false, false, false, true);
        const MenuValues back = read_menu_values(ini.c_str());
        check(!back.focusCorridor && back.focusCorridorWidth == 5 && back.focusCorridorHeight == 20 &&
                  back.focusCorridorSpread == 12 &&
                  GetPrivateProfileIntW(L"quad", L"focus_corridor", -1, ini.c_str()) == 0,
              "road focus: written for a game with it, and read back");
        DeleteFileW(ini.c_str());
    }

    return test_result();
}
