// Offline check of the performance overlay (quad/osd.h): frame rate, pacing and headroom from frame
// times, which processor limits the frame, the rows' states, the [osd] round trip, and that the card
// draws with rounded, see-through corners.
#include <cstdio>
#include <filesystem>
#include <string>

#include <windows.h>

#include "quad/osd.h"

using namespace aceqv;

namespace {

int failures = 0;

void check(bool ok, const char* what) {
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    failures += ok ? 0 : 1;
}

bool close_to(double a, double b, double tolerance = 0.05) { return a > b - tolerance && a < b + tolerance; }

OsdInput steady(double intervalMs, double cpuMs, double gpuMs, double periodMs = 11.111) {
    OsdInput in;
    in.periodMs = periodMs;
    in.gpuMs = gpuMs;
    for (int i = 0; i < 45; ++i)
        in.frames.push_back({intervalMs, cpuMs});
    return in;
}

const OsdRow* row(const std::vector<OsdRow>& rows, const wchar_t* label) {
    for (const OsdRow& r : rows)
        if (r.label == label)
            return &r;
    return nullptr;
}

// A card's pixels as a bitmap to look at: RGBA over a dark grey (the card is see-through).
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

}  // namespace

int main() {
    {
        const OsdSummary s = osd_summary(steady(11.111, 5, 7));
        check(close_to(s.fps, 90, 0.1) && close_to(s.budgetMs, 11.111) && s.latePercent == 0 && close_to(s.jitterMs, 0),
              "a steady 90 Hz frame: 90 fps, on budget, smooth");
        check(close_to(s.cpuHeadroom, 55, 0.1) && close_to(s.gpuHeadroom, 37, 0.1) && s.bound == OsdBound::None,
              "headroom is the budget left; neither processor limits");
    }
    {
        const OsdSummary s = osd_summary(steady(11.111, 4, 10.5));
        check(s.bound == OsdBound::Gpu, "GPU time near the budget with the CPU well under: GPU bound");
    }
    {
        const OsdSummary s = osd_summary(steady(13, 10.5, 11));
        check(s.bound == OsdBound::Cpu, "GPU time close to the CPU time (it waited for ACE): CPU bound");
        check(s.cpuHeadroom > 0 && s.cpuHeadroom < 10, "CPU headroom is small");
    }
    {
        OsdInput in = steady(11.111, 4, 10.5);
        in.gpuBusyPercent = 60;
        check(osd_summary(in).bound == OsdBound::None, "the GPU's busy share, when known, overrules the timestamps");
    }
    {
        OsdInput in = steady(22.222, 6, 15);
        in.fg = OsdFg::On;
        in.fgMultiplier = 2;
        const OsdSummary s = osd_summary(in);
        check(close_to(s.budgetMs, 22.222) && s.latePercent == 0 && s.bound == OsdBound::None,
              "frame generation 2x: ACE's budget is two refresh periods");
    }
    {
        OsdInput in = steady(11.111, 5, 7);
        for (int i = 0; i < 5; ++i)
            in.frames[i * 9].intervalMs = 22.222;
        const OsdSummary s = osd_summary(in);
        const std::vector<OsdRow> rows = osd_rows(in, s);
        const OsdRow* pacing = row(rows, L"Pacing");
        check(close_to(s.latePercent, 100.0 * 5 / 45, 0.1) && pacing && pacing->tone == OsdTone::Bad &&
                  pacing->value.find(L"late") != std::wstring::npos,
              "skipped frames show as late in the pacing row");
    }
    {
        const OsdSummary s = osd_summary(steady(14, 3, 6));
        check(s.bound == OsdBound::Cpu, "frames over budget with an idle GPU: limited by the CPU");
    }
    {
        const std::string json =
            R"({"version":"0.6.3","system_load": {"valid": true, "gpu_valid": true, "cpu_total_percent": 24.1, )"
            R"("gpu_busy_percent": 78.0024}, "fg": {"layer_present": true, "active": true, "state": "Generating frames", )"
            R"("automatic": {"cpu_ms": 0, "gpu_ms": 0}, "performance": {"center_ms": 1.07, "total_ms": 1.72236}, )"
            R"("cadence": {"valid": true, "game_fps": 86.1, "unique_fps": 90.1143, "refresh_hz": 90.0}}})";
        const OsdCheeky c = osd_cheeky(json);
        check(c.valid && c.active && c.state == L"Generating frames" && close_to(c.outputFps, 90.1143, 0.001) &&
                  close_to(c.fgMs, 1.72236, 0.0001) && close_to(c.gpuBusyPercent, 78.0024, 0.0001) &&
                  close_to(c.refreshHz, 90, 0.001),
              "Cheeky's snapshot: FG active, its state, output fps, FG time, GPU busy and the headset's refresh");
        check(!osd_cheeky("{}").valid && !osd_cheeky("").valid, "no snapshot: nothing from Cheeky");
    }
    {
        const OsdSummary s = osd_summary(steady(11.111, -1, -1));
        check(s.cpuMs < 0 && s.bound == OsdBound::Unknown, "no CPU or GPU time: the bound is unknown");
    }
    {
        OsdInput in = steady(11.111, 5, 7);
        in.vramUsed = 11ull << 30;
        in.vramBudget = 12ull << 30;
        in.gaze = OsdGaze::Tracking;
        in.fg = OsdFg::Starting;
        in.fgState = L"Waiting for guides";
        in.composeMs = 0.4;
        const std::vector<OsdRow> rows = osd_rows(in, osd_summary(in));
        const OsdRow* vram = row(rows, L"VRAM");
        const OsdRow* fg = row(rows, L"Frame generation");
        const OsdRow* gaze = row(rows, L"Eye tracking");
        const OsdRow* gpuParts = row(rows, L"GPU ms: ACE/FG/layer/rest");
        check(rows.size() == 9 && row(rows, L"Busiest ACE thread") && row(rows, L"Render thread") &&
                  row(rows, L"Frame p90 · 1% low"),
              "nine cells: three rows of three");
        const std::vector<OsdTile> tiles = osd_tiles(in, osd_summary(in));
        check(tiles.size() == 4 && tiles[0].value == L"90" && tiles[0].unit == L"fps" && tiles[0].detail == L"of 90 Hz" &&
                  tiles[1].value == L"11.1" && tiles[1].tone == OsdTone::Good && tiles[2].label == L"CPU" &&
                  tiles[2].detail == L"55% headroom" && tiles[3].value == L"7.0" && tiles[3].detail == L"37% headroom",
              "four tiles: frame rate, frame time, CPU and GPU with their headroom");
        check(vram && vram->value == L"11.0 / 12.0 GB" && vram->tone == OsdTone::Warn && close_to(vram->meter, 11.0 / 12),
              "VRAM: used of the budget, with a meter, amber above 85%");
        check(fg && fg->pill && fg->value == L"Waiting for guides" && fg->tone == OsdTone::Warn,
              "frame generation not yet generating shows Cheeky's state");
        check(gaze && gaze->pill && gaze->value == L"Tracking" && gaze->tone == OsdTone::Good, "eye tracking badge");
        check(gpuParts && gpuParts->value == L"7.0 / — / 0.4 / —", "without the whole GPU's load: ACE's queue and the layer");
        in.fg = OsdFg::On;
        in.fgMultiplier = 2;
        in.fgOutputFps = 180;
        const std::vector<OsdTile> fgTiles = osd_tiles(in, osd_summary(in));
        check(fgTiles[0].value == L"180" && fgTiles[0].detail.find(L"ACE 90") == 0,
              "with frame generation the frame rate tile shows what the headset shows, and ACE's own");
    }
    // The whole GPU and ACE's threads (the layer's samplers): 2x frame generation at 90 Hz (a 22.2 ms budget).
    const auto measured = [](double intervalMs, double busyMs, double workMs, double waitMs, double gpuBusy,
                             double busiest) {
        OsdInput in;
        in.periodMs = 11.111;
        in.fg = OsdFg::On;
        in.fgMultiplier = 2;
        in.fgMs = 1.1;
        in.gpuMs = 19;
        in.composeMs = 0.4;
        in.gpuBusyPercent = gpuBusy;
        in.cpuBusiestPercent = busiest;
        for (int i = 0; i < 100; ++i)
            in.frames.push_back({intervalMs, busyMs, workMs, waitMs});
        return in;
    };
    {
        const OsdInput in = measured(22.222, 9, 12.5, 9.7, 98, 60);
        const OsdSummary s = osd_summary(in);
        check(s.gpuWhole && close_to(s.gpuMs, 0.98 * 22.222, 0.01) && close_to(s.gpuRestMs, 0.98 * 22.222 - 19 - 1.1 - 0.4, 0.01) &&
                  close_to(s.gpuFgMs, 1.1) && s.bound == OsdBound::Gpu && s.gpuHeadroom < 3,
              "the whole GPU 98% busy: its time per frame, the rest beyond ACE, FG and the layer; GPU bound, ~2% left");
        const std::vector<OsdRow> rows = osd_rows(in, s);
        check(row(rows, L"GPU ms: ACE/FG/layer/rest")->value == L"19.0 / 1.1 / 0.4 / 1.3" &&
                  row(rows, L"Limited by")->value == L"GPU" && osd_tiles(in, s)[3].label == L"GPU (all work)",
              "the GPU row shows its parts; the tile says it is all the GPU's work");
        check(close_to(s.cpuMs, 12.5) && close_to(s.cpuBusyMs, 9) && close_to(s.cpuBlockedMs, 3.5) &&
                  close_to(s.cpuThreadMs, 0.6 * 22.222, 0.01) && close_to(s.waitMs, 9.7) &&
                  row(rows, L"Render thread")->value == L"9.0 busy · 3.5 blocked",
              "the render thread: 12.5 ms of work, 9 running and 3.5 blocked; the busiest thread 60% of the frame");
        check(close_to(s.cpuHeadroom, (22.222 - 0.6 * 22.222) / 22.222 * 100, 0.1),
              "CPU headroom from the busier of the render thread's work and the busiest thread");
    }
    {
        // Cheeky's layer gives ACE the doubled period (22.2 ms): with the headset's refresh known, the budget is
        // that refresh times the multiplier, not doubled again (the 44.4 ms that showed false headroom).
        OsdInput doubled = measured(22.4, 9, 12.6, 9.7, 71, 60);
        doubled.periodMs = 22.222;
        doubled.refreshMs = 11.111;
        const OsdSummary d = osd_summary(doubled);
        check(close_to(d.budgetMs, 22.222) && close_to(d.refreshHz, 90, 0.01) &&
                  close_to(d.gpuHeadroom, (22.222 - 0.71 * 22.4) / 22.222 * 100, 0.1),
              "frame generation through Cheeky's layer: the budget is 22.2 ms (not 44.4), the refresh 90 Hz");
        OsdInput plain = doubled;
        plain.periodMs = 11.111;  // a runtime that gives ACE the refresh period itself
        check(close_to(osd_summary(plain).budgetMs, 22.222), "the refresh period given as it is: multiplied once");
        // ACE held in xrWaitFrame yet 3% past its slot (43 fps of 45), neither processor full: pacing.
        OsdInput slip = doubled;
        for (OsdFrame& f : slip.frames)
            f.intervalMs = 22.9;
        check(osd_summary(slip).bound == OsdBound::Pacing, "held and 3% past the budget: pacing (under the 5% late mark)");
    }
    {
        check(osd_summary(measured(26, 9, 25.5, 0.3, 80, 60)).bound == OsdBound::Cpu,
              "over budget, barely held in xrWaitFrame, the GPU not full: CPU bound");
        check(osd_summary(measured(22.222, 9, 12, 10, 70, 97)).bound == OsdBound::Cpu,
              "a thread at a full core: CPU bound, whatever the render thread shows");
        const OsdInput paced = measured(26, 9, 12, 13, 80, 60);
        check(osd_summary(paced).bound == OsdBound::Pacing && osd_rows(paced, osd_summary(paced))[1].value == L"Pacing",
              "over budget, held in xrWaitFrame, neither processor full: pacing");
        check(osd_summary(measured(22.222, 9, 12, 10, 70, 60)).bound == OsdBound::None, "on budget with both to spare: neither");
    }
    {
        OsdInput in = measured(22.222, 9, 12, 10, 70, 60);
        for (int i = 0; i < 10; ++i)
            in.frames[i * 10].intervalMs = 23.5;  // 6% over
        in.frames[50].intervalMs = 40;
        const OsdSummary s = osd_summary(in);
        check(close_to(s.latePercent, 10) && close_to(s.frameP90Ms, 23.5) && close_to(s.low1Fps, 1000 / 40.0, 0.01),
              "frames 6% over count as late; p90 23.5 ms; the 1% low from the slowest frame");
    }
    {
        const std::wstring ini = (std::filesystem::temp_directory_path() / L"aceqv_osd_test.ini").wstring();
        DeleteFileW(ini.c_str());
        check(read_osd_placement(ini.c_str()) == OsdPlacement{}, "no [osd]: off, above the centre, 26 degrees");
        const OsdPlacement p{true, -20, -8, 22};
        check(write_osd_placement(ini.c_str(), p) && read_osd_placement(ini.c_str()) == p,
              "[osd] round trip, negative degrees included");
        WritePrivateProfileStringW(L"osd", L"width", L"99", ini.c_str());
        check(read_osd_placement(ini.c_str()).width == kOsdWidthMax, "the width is clamped");
        WritePrivateProfileStringW(L"osd", L"yaw", L"-90", ini.c_str());
        check(read_osd_placement(ini.c_str()).yaw == -kOsdYawMax, "and so is the yaw");
        DeleteFileW(ini.c_str());
    }
    // ACEQV_OSD_SAVE=<file.bmp>: the card with every cell filled, as a bitmap to look at.
    if (wchar_t path[MAX_PATH] = {}; GetEnvironmentVariableW(L"ACEQV_OSD_SAVE", path, MAX_PATH)) {
        const OsdInput in = measured(24.6, 9.1, 12.5, 10.9, 97, 64);
        OsdInput full = in;
        full.vramUsed = 13ull << 30;
        full.vramBudget = 15ull << 30;
        full.gaze = OsdGaze::Tracking;
        full.fgOutputFps = 81;
        full.cpuBusiestName = L"GameThread";
        const OsdSummary s = osd_summary(full);
        std::vector<double> history(150, 24.6);
        const std::vector<uint32_t> pixels = draw_osd_panel(s, history, osd_tiles(full, s), osd_rows(full, s));
        save_card(path, pixels);
    }
    {
        OsdInput in = steady(11.111, 5, 7);
        const OsdSummary s = osd_summary(in);
        std::vector<double> history(120, 11.1);
        history[60] = 30;
        const std::vector<uint32_t> pixels = draw_osd_panel(s, history, osd_tiles(in, s), osd_rows(in, s));
        const auto alpha = [&](int x, int y) { return pixels[static_cast<size_t>(y) * kOsdWidth + x] >> 24; };
        bool text = false;
        for (int y = 30; y < 100 && !text; ++y)
            for (int x = 36; x < 200; ++x)
                text |= (pixels[static_cast<size_t>(y) * kOsdWidth + x] & 0xff) > 0xc0;  // the white frame rate
        check(pixels.size() == static_cast<size_t>(kOsdWidth) * kOsdHeight && alpha(0, 0) == 0 &&
                  alpha(kOsdWidth - 1, kOsdHeight - 1) == 0 && alpha(kOsdWidth / 2, kOsdHeight / 2) > 0xc0 && text,
              "the card: rounded transparent corners, translucent body, the frame rate drawn");
    }
    {
        const std::vector<uint32_t> pixels =
            draw_notice_panel(L"ace-quadview is off", {L"This Assetto Corsa EVO build isn’t one ace-quadview supports, so the "
                                                       L"game runs as normal, without foveated rendering.",
                                                       L"Look for an ace-quadview release for the new build."});
        bool yellow = false, white = false;
        for (int y = 40; y < 104; ++y)  // the title: yellow (red and green high, blue low)
            for (int x = 64; x < 400; ++x) {
                const uint32_t p = pixels[static_cast<size_t>(y) * kOsdWidth + x];
                yellow |= (p & 0xff) > 0xc0 && ((p >> 8) & 0xff) > 0xb0 && ((p >> 16) & 0xff) < 0x60;
            }
        for (int y = 132; y < 220; ++y)  // the first paragraph: white
            for (int x = 64; x < 600; ++x)
                white |= (pixels[static_cast<size_t>(y) * kOsdWidth + x] & 0xffffff) == 0xf7f5f5;
        check(pixels.size() == static_cast<size_t>(kOsdWidth) * kOsdHeight && (pixels[0] >> 24) == 0 && yellow && white,
              "the notice card: a yellow title, the first paragraph in white, rounded corners");
        if (wchar_t path[MAX_PATH] = {}; GetEnvironmentVariableW(L"ACEQV_NOTICE_SAVE", path, MAX_PATH))
            save_card(path, pixels);
    }
    std::printf(failures ? "%d FAILED\n" : "all passed\n", failures);
    return failures ? 1 : 0;
}
