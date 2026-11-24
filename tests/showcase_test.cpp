// Offline check of the showcase (quad/showcase.h): the A/B table's sums (each side, the settle after a switch, the 1%
// low, CPU and GPU), its rows' values and colours, the card, the Stock badge, and the headset menu's Showcase section
// (only for a game with it, [quad] stock written to the ini it is given, the A/B table turning the overlay on).
// SIMQVFR_AB_SAVE=<path.bmp> saves the card to look at.
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#include <windows.h>

#include "games/game.h"
#include "quad/settings_menu.h"
#include "quad/showcase.h"
#include "test_util.h"

using namespace simqvfr;

namespace {

std::vector<double> frames(int n, double ms) { return std::vector<double>(static_cast<size_t>(n), ms); }

const AbLine* line(const std::vector<AbLine>& lines, const wchar_t* label) {
    for (const AbLine& l : lines)
        if (l.label == label)
            return &l;
    return nullptr;
}

// The card's pixels as a bitmap over a dark grey (it is see-through).
void save_card(const wchar_t* path, const std::vector<uint32_t>& pixels) {
    FILE* f = _wfopen(path, L"wb");
    if (!f)
        return;
    const int w = kOsdWidth, h = kOsdHeight;
    BITMAPFILEHEADER file{0x4D42, static_cast<DWORD>(sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER) + w * h * 4), 0, 0,
                          sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER)};
    BITMAPINFOHEADER info{sizeof(BITMAPINFOHEADER), w, -h, 1, 32, BI_RGB};
    fwrite(&file, sizeof(file), 1, f);
    fwrite(&info, sizeof(info), 1, f);
    for (const uint32_t p : pixels) {
        const double a = (p >> 24) / 255.0;
        const auto over = [a](uint32_t c) { return static_cast<uint8_t>(c * a + 40 * (1 - a)); };
        const uint8_t bgra[4] = {over((p >> 16) & 0xff), over((p >> 8) & 0xff), over(p & 0xff), 255};
        fwrite(bgra, 4, 1, f);
    }
    fclose(f);
}

bool has(const std::vector<MenuItem>& items, MenuItem item) {
    for (const MenuItem i : items)
        if (i == item)
            return true;
    return false;
}

}  // namespace

int SIM_QVFR_TEST_MAIN() {
    {  // which games have it: the features are set by position (games/<id>/game.h), so a field added before it
       // moves it unless every game's list grows too (a rebase once put the trucks' flag in night cars' place)
        std::size_t count = 0;
        const games::GameInfo* const* all = games::all_games(count);
        std::string with;
        bool trucksPlain = true;
        for (std::size_t i = 0; i < count; ++i) {
            const std::string id = all[i]->id;
            if (all[i]->features.showcase)
                with += (with.empty() ? "" : ",") + id;
            if ((id == "ats" || id == "ets2") && (all[i]->features.carLighting || all[i]->features.focusCorridor ||
                                                 all[i]->features.peripheryTextureMatch))
                trucksPlain = false;
        }
        std::printf("games with the showcase: %s\n", with.c_str());
        check(with == "ace,lmu,ats,ets2" || (with.find("lmu") != std::string::npos && with.find("ace") != std::string::npos &&
                                             with.find("ats") != std::string::npos && with.find("ets2") != std::string::npos &&
                                             with.find("acc") == std::string::npos && with.find("xplane") == std::string::npos),
              "the showcase is LMU's, ACE's, ATS's and ETS2's");
        check(trucksPlain, "the trucks' showcase flag sits in its own place (no night cars, road focus or texture match)");
    }
    {  // the settle: the first window on a side starts it, windows within it are left out
        AbTable t;
        t.add(false, frames(45, 11.111), 6, 9, 0, 45);  // the first window after the reset: left out, settling from here
        check(!ab_stats(t.side(false)).valid && t.settling(0), "the first window after the reset is left out");
        t.add(false, frames(45, 11.111), 6, 9, 1000, 45);
        check(!ab_stats(t.side(false)).valid && t.settling(1000), "windows within 1.5 s of it are left out too");
        t.add(false, frames(45, 11.111), 6, 9, 1600, 45);
        const AbStats q = ab_stats(t.side(false));
        check(q.valid && close_to(q.fps, 90, 0.05) && close_to(q.cpuMs, 6, 1e-9) && close_to(q.gpuMs, 9, 1e-9) &&
                  !t.settling(1600),
              "after it: QVFR's side counts (90 fps, CPU 6 ms, GPU 9 ms)");
        t.add(true, frames(45, 13.0), 8, 12, 2100, 45);  // a switch: this window straddles it
        check(!ab_stats(t.side(true)).valid && t.settling(2100), "a switch to Stock: its first window is left out");
        t.add(true, frames(45, 13.0), 8, 12, 4000, 45);
        const AbStats s = ab_stats(t.side(true));
        check(s.valid && close_to(s.fps, 1000 / 13.0, 0.05) && close_to(s.cpuMs, 8, 1e-9) && close_to(s.gpuMs, 12, 1e-9),
              "Stock's side counts on its own");
        check(close_to(ab_stats(t.side(false)).seconds, 45 * 11.111 / 1000, 1e-6), "QVFR's side kept what it had");
        t.reset();
        check(!ab_stats(t.side(false)).valid && !ab_stats(t.side(true)).valid && t.settling(5000),
              "reset: both sides empty, settling again");
    }
    {  // menus and loading: left out, and the track showing again settles first
        AbTable t;
        t.add(false, frames(45, 11.111), 6, 9, 0, 45);
        t.add(false, frames(45, 11.111), 6, 9, 2000, 45);
        const uint64_t before = t.side(false).frames;
        t.add(false, frames(45, 30.0), 3, 2, 2500, 10);  // a loading screen: 10 of 45 frames drew the track
        check(t.side(false).frames == before && t.settling(2500), "a window mostly off the track is left out");
        t.add(false, frames(45, 11.111), 6, 9, 3000, 45);  // the track again: its first window starts the settle
        t.add(false, frames(45, 11.111), 6, 9, 4000, 45);
        check(t.side(false).frames == before && t.settling(4000), "back on the track: 1.5 s of settle first");
        t.add(false, frames(45, 11.111), 6, 9, 4600, 41);  // 41 of 45: enough
        check(t.side(false).frames == before + 45, "then it counts again (90% of a window on the track is enough)");
    }
    {  // the 1% low: 1000 / the 99th percentile frame time; CPU or GPU unknown left out
        AbTable t;
        t.add(false, frames(10, 11.1), -1, -1, 0, 10);
        std::vector<double> w = frames(990, 10.0);
        for (int i = 0; i < 10; ++i)
            w.push_back(25.0);  // the slowest 1%
        t.add(false, w, -1, 8, 2000, w.size());
        const AbStats q = ab_stats(t.side(false));
        check(close_to(q.low1Fps, 1000 / 10.05, 0.2), "990 frames at 10 ms, 10 at 25: the 1% low is the 10 ms frames' bin");
        w.push_back(25.0);  // now 11 of 1001 slower: past the 1%
        t.add(false, w, -1, 8, 3000, w.size());
        check(ab_stats(t.side(false)).low1Fps < 45, "more than 1% at 25 ms: the 1% low falls to them");
        check(ab_stats(t.side(false)).cpuMs < 0 && close_to(ab_stats(t.side(false)).gpuMs, 8, 1e-9),
              "a CPU time never known stays unknown; the GPU's averages over its frames");
    }
    {  // the rows: values, differences and colours
        AbStats s, q;
        s.valid = q.valid = true;
        s.fps = 72;
        q.fps = 90;
        s.low1Fps = 60;
        q.low1Fps = 82;
        s.cpuMs = 9.5;
        q.cpuMs = 8.25;
        s.gpuMs = 13.2;
        q.gpuMs = 8.8;
        AbPixels p{2.0 * 3000 * 3000, 2.0 * (1200.0 * 1200 + 900.0 * 900), 30, 45, 12};
        const std::vector<AbLine> lines = ab_lines(s, q, p);
        const AbLine* fps = line(lines, L"Average FPS");
        const AbLine* gpu = line(lines, L"GPU frame time");
        const AbLine* px = line(lines, L"Pixels rendered");
        const AbLine* centre = line(lines, L"Sharpness, centre");
        const AbLine* edge = line(lines, L"Sharpness, periphery");
        check(lines.size() == 7 && fps && gpu && px && centre && edge, "seven rows");
        check(fps && fps->stock == L"72.0" && fps->qvfr == L"90.0" && fps->delta == L"+18.0 (+25%)" && fps->tone == OsdTone::Good,
              "FPS: each side, the difference and its share, green when QVFR is ahead");
        check(gpu && gpu->stock == L"13.20 ms" && gpu->delta == L"−4.40 ms (−33%)" && gpu->tone == OsdTone::Good,
              "GPU time: lower is better, a minus sign");
        check(px && px->stock == L"18.0 MP" && px->qvfr == L"4.5 MP" && px->delta == L"−75%" && px->tone == OsdTone::Good,
              "pixels: millions for both eyes, the difference as a share");
        check(centre && centre->delta == L"+15 (+50%)" && centre->tone == OsdTone::Good && edge &&
                  edge->delta == L"−18 (−60%)" && edge->tone == OsdTone::Neutral,
              "sharpness: the centre's gain green, the periphery's loss the trade (not coloured)");
        s.fps = 90;  // a locked frame rate: equal
        check(line(ab_lines(s, q, p), L"Average FPS")->delta == L"0.0 (0%)" &&
                  line(ab_lines(s, q, p), L"Average FPS")->tone == OsdTone::Neutral,
              "equal frame rates: no sign, no colour");
        const std::vector<AbLine> empty = ab_lines(AbStats{}, q, AbPixels{});
        check(line(empty, L"Average FPS")->stock == L"—" && line(empty, L"Average FPS")->delta == L"—" &&
                  line(empty, L"Pixels rendered")->qvfr == L"—",
              "a side without frames, pixels unknown: dashes");
        // The card: drawn, see-through corners, opaque inside.
        const AbHeading heading{false, false, L"QVFR · MVR", 42, 61};
        const std::vector<uint32_t> card = draw_ab_panel(heading, lines);
        check(card.size() == static_cast<size_t>(kOsdWidth) * kOsdHeight && (card[0] >> 24) == 0 &&
                  (card[static_cast<size_t>(kOsdHeight / 2) * kOsdWidth + kOsdWidth / 2] >> 24) > 0xc0,
              "the card: kOsdWidth x kOsdHeight, its corners clear");
        if (wchar_t path[MAX_PATH] = {}; GetEnvironmentVariableW(L"SIMQVFR_AB_SAVE", path, MAX_PATH))
            save_card(path, card);
    }
    {  // the overlay's badge in Stock
        OsdInput in;
        in.multiView = OsdMultiView::Stock;
        in.dlss = true;
        const OsdRow b = osd_badge(in);
        check(b.value == L"Stock + DLSS" || b.value == L"Stock", "the badge says Stock");
        check(b.tone == OsdTone::Neutral, "grey: a choice, not a fault");
    }
    {  // the headset menu's Showcase section
        SettingsMenu menu;
        menu.page = 3;  // Picture, Foveation, Headset (no View section without its pages), Game
        menu.launch.cropPage = menu.launch.helmetPage = false;
        bool found = false;
        for (int p = 0; p < 6 && !found; ++p) {
            menu.page = p;
            found = menu_page(menu) == MenuPage::Headset;
        }
        check(found && !has(menu_page_items(menu), MenuItem::ShowcaseStock), "no Showcase section for a game without it");
        menu.launch.showcase = true;
        const std::vector<MenuItem> items = menu_page_items(menu);
        check(has(items, MenuItem::HeadShowcase) && has(items, MenuItem::ShowcaseStock) && has(items, MenuItem::AbTable),
              "the Headset page has it for a game with it");
        for (size_t i = 0; i < items.size(); ++i)
            if (items[i] == MenuItem::ShowcaseStock)
                menu.selected = static_cast<int>(i);
        check(menu_key(menu, MenuKey::Right) && menu.values.stock, "Right on Rendering: Stock");
        const std::vector<MenuLine> shown = menu_lines(menu);
        check(menu.selected < static_cast<int>(shown.size()) && shown[menu.selected].value == L"Stock", "the line says Stock");
        menu.launch.stockBlocked = L"the FOV crop is on";
        const std::vector<std::wstring> notes = menu_notes(menu);
        check(!notes.empty() && notes[0].find(L"the FOV crop is on") != std::wstring::npos, "a note says why Stock can't run");
        for (size_t i = 0; i < items.size(); ++i)
            if (items[i] == MenuItem::AbTable)
                menu.selected = static_cast<int>(i);
        menu.values.overlay.enabled = false;
        check(menu_key(menu, MenuKey::Right) && menu.values.overlay.abTable && menu.values.overlay.enabled,
              "the A/B table on turns the overlay on with it");
        // [quad] stock written only where asked, and [osd] ab_table with the overlay's place.
        const std::wstring ini = (std::filesystem::temp_directory_path() / L"simqvfr_showcase_test.ini").wstring();
        DeleteFileW(ini.c_str());
        check(write_menu_values(ini.c_str(), menu.values, false, false, false, false, false, false, false, true), "written");
        const MenuValues back = read_menu_values(ini.c_str());
        check(back.stock && back.overlay.abTable && back.overlay.enabled, "[quad] stock and [osd] ab_table read back");
        DeleteFileW(ini.c_str());
        check(write_menu_values(ini.c_str(), menu.values, false, false, false, false, false) &&
                  GetPrivateProfileIntW(L"quad", L"stock", -1, ini.c_str()) == -1,
              "without the showcase (the launcher's ini): [quad] stock not written");
        DeleteFileW(ini.c_str());
    }
    return test_result();
}
