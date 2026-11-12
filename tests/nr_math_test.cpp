// Offline check of DLSS-NR's arithmetic and settings (engine/nr_math.h, engine/nr_settings.h): the part of a view the
// model covers, the size it works at, the focus view's place from its remap, the edge blend's weights, the curve the
// HDR scene goes through and back, and [dlss_nr] in the ini. No Direct3D and no NVIDIA runtime: the hardware side is
// tests/nr_views_check.cpp.
#include <cmath>
#include <cstdio>
#include <string>

#include <windows.h>

#include "engine/nr_math.h"
#include "engine/nr_settings.h"

using namespace simqvfr;

namespace {

int failures = 0;

void check(bool ok, const char* what) {
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    failures += ok ? 0 : 1;
}

}  // namespace

int SIM_QVFR_TEST_MAIN() {
    // The covered part of an eye's view.
    {
        const nr::Region all = nr::covered(2000, 1800, 0.7, 0.4, 100);
        check(all == nr::Region{0, 0, 2000, 1800}, "covered: 100% is the whole view");
        const nr::Region half = nr::covered(2000, 1800, 0.5, 0.5, 50);
        check(half == nr::Region{500, 450, 1000, 900}, "covered: 50% about the centre is the middle half each way");
        const nr::Region off = nr::covered(2000, 1800, 0.7, 0.4, 50);
        check(off.width == 1000 && off.height == 900 && off.x == 900 && off.y == 270,
              "covered: about a focus view at (0.7, 0.4) the part is centred there");
        const nr::Region edge = nr::covered(2000, 1800, 0.95, 0.02, 50);
        check(edge.x == 1000 && edge.y == 0 && edge.x + edge.width == 2000, "covered: a part past the view's edge is moved inside");
        const nr::Region odd = nr::covered(1999, 1801, 0.5, 0.5, 33);
        check(odd.width % 2 == 0 && odd.height % 2 == 0 && odd.x + odd.width <= 1999 && odd.y + odd.height <= 1801,
              "covered: even sizes, inside the view");
        const nr::Region tiny = nr::covered(200, 100, 0.5, 0.5, 20);
        check(tiny.width == nr::kMinWorking && tiny.height == nr::kMinWorking, "covered: never under the smallest working size");
        const nr::Region nan = nr::covered(2000, 1800, std::nan(""), 0.5, 50);
        check(nan.x == 500, "covered: a centre that is no number is the view's centre");
        // A gaze that moves a pixel moves the part a pixel: its size is steady (one feature, its history kept).
        const nr::Region a = nr::covered(2000, 1800, 0.5000, 0.5, 60), b = nr::covered(2000, 1800, 0.5005, 0.5, 60);
        check(a.width == b.width && a.height == b.height && b.x == a.x + 1, "covered: the size holds as the centre moves");
    }
    // The working size.
    check(nr::working(1600, 100) == 1600 && nr::working(1600, 50) == 800 && nr::working(1600, 25) == 400,
          "working: the percent of the region each way");
    check(nr::working(200, 25) == nr::kMinWorking && nr::working(40, 50) == 40 && nr::working(1600, 400) == 1600,
          "working: no smaller than the runtime's least, no larger than the region");
    // The limit on what a view's model works on.
    {
        unsigned w = 2595, h = 2084;
        check(nr::limit_working(w, h, 1.0) && w * h <= 1000000 && w * h > 990000 && std::fabs(w / double(h) - 2595.0 / 2084) < 0.01,
              "limit: a 5.4 megapixel view comes under 1.0, its shape kept");
        w = 900, h = 900;
        check(!nr::limit_working(w, h, 1.0) && w == 900 && h == 900, "limit: a view under it is left alone");
        w = 4000, h = 100;
        check(nr::limit_working(w, h, 0.3) && h >= nr::kMinWorking, "limit: no side under the runtime's least");
    }
    // The focus view's place from its remap (x' = a x + b w): the eye's clip (-b/a, -d/c) is its centre.
    {
        const float centred[4] = {4, 0, 3, 0};
        double u = 0, v = 0;
        check(nr::focus_centre(centred, u, v) && std::fabs(u - 0.5) < 1e-9 && std::fabs(v - 0.5) < 1e-9,
              "focus centre: a remap without a shift is the view's centre");
        const float right[4] = {4, -1.2f, 3, 0.6f};  // centred on clip (0.3, -0.2): right of and below the middle
        check(nr::focus_centre(right, u, v) && std::fabs(u - 0.65) < 1e-6 && std::fabs(v - 0.6) < 1e-6,
              "focus centre: a shifted remap gives the focus view's place, v down");
        const float none[4] = {0, 0, 0, 0};
        check(!nr::focus_centre(none, u, v) && u == 0.5 && v == 0.5, "focus centre: no remap, the view's centre");
    }
    // The edge blend.
    check(nr::edge_weight(0, 0.1, true, true) == 0 && nr::edge_weight(1, 0.1, true, true) == 0 &&
              nr::edge_weight(0.5, 0.1, true, true) == 1 && nr::edge_weight(0.1, 0.1, true, true) == 1,
          "edge blend: nothing of the model at a fading edge, all of it from the band on");
    check(std::fabs(nr::edge_weight(0.05, 0.1, true, true) - 0.5) < 1e-9, "edge blend: half way through the band, half");
    check(nr::edge_weight(0, 0.1, false, true) == 1 && nr::edge_weight(0, 0, true, true) == 1,
          "edge blend: no fade at an edge not marked, none with no band");
    // The curve: every value comes back, the hue is kept, the model's range is 0..1.
    {
        bool back = true, inRange = true, hue = true;
        for (const bool gamma : {false, true})
            for (const double exposure : {0.25, 1.0, 4.0})
                for (const double scale : {0.001, 0.2, 1.0, 6.0, 80.0}) {
                    const double in[3] = {1.0 * scale, 0.5 * scale, 0.1 * scale};
                    double e[3], out[3];
                    nr::encode(in, exposure, gamma, e);
                    nr::decode(e, exposure, gamma, out);
                    for (int i = 0; i < 3; ++i) {
                        back = back && std::fabs(out[i] - in[i]) < 1e-6 * (1 + in[i]);
                        inRange = inRange && e[i] >= 0 && e[i] < 1;
                    }
                    if (!gamma)
                        hue = hue && std::fabs(e[1] / e[0] - 0.5) < 1e-9 && std::fabs(e[2] / e[0] - 0.1) < 1e-9;
                }
        check(back, "curve: in and out again gives the value back, from 0.001 to 80");
        check(inRange, "curve: what the model gets is within 0..1");
        check(hue, "curve: the channels keep their proportions (the hue)");
        const double negative[3] = {-1, 0.5, 0.2};
        double e[3];
        nr::encode(negative, 1, true, e);
        check(e[0] == 0, "curve: a negative value goes in as 0");
        const double white[3] = {1, 1, 1};
        double out[3];
        nr::decode(white, 1, false, out);
        check(std::isfinite(out[0]) && out[0] > 1000, "curve: the model's 1.0 comes back very bright, not endless");
    }
    // The model's change as a gain on the scene.
    check(nr::gain(0.4, 0.4, 3) == 1 && nr::gain(0, 0, 3) == 1, "gain: what the model gave back unchanged leaves the scene as it came");
    check(std::fabs(nr::gain(0.9, 0.95, 3) - 0.96 / 0.91) < 1e-12 && nr::gain(0.9, 0.95, 3) < 1.06,
          "gain: a small change near white stays a small change (the curve's inverse made it 2x)");
    check(nr::gain(0.01, 0.9, 3) == 3 && nr::gain(0.9, 0.0, 3) == 1.0 / 3, "gain: held to the most either way");
    check(nr::gain(0.002, 0.001, 3) > 0.9, "gain: the darkest values barely swing it (no crushed blacks from their noise)");
    // [dlss_nr].
    {
        wchar_t temp[MAX_PATH];
        GetTempPathW(MAX_PATH, temp);
        const std::wstring ini = std::wstring(temp) + L"simqvfr-nr-settings.ini";
        DeleteFileW(ini.c_str());
        const NrSettings fresh = read_nr_settings(ini.c_str());
        check(!fresh.on && fresh.folder.empty() && !fresh.periphery && !fresh.beforeDlss && fresh.intensity == 100 && fresh.style == 0 &&
                  fresh.preset == 0 && fresh.localTone == 100 && fresh.localStructure == 100 && !fresh.autoMask &&
                  fresh.focusScale == 100 && fresh.peripheryScale == 50 && fresh.peripheryCoverage == 100 && fresh.edgeBlend == 10 &&
                  fresh.workingLimit == 10 &&
                  fresh.exposure == 100 && fresh.inputGamma && fresh.autoExposure && fresh.maxGain == 300 && fresh.saveFrames == 0,
              "[dlss_nr]: an empty file reads off, the focus views only, after DLSS, the model's own strengths");
        NrSettings s;
        s.on = true;
        s.folder = L"C:\\Games\\Some Game";
        s.preset = 2, s.style = 1, s.intensity = 65, s.localTone = 140, s.localStructure = 30;
        s.autoMask = true, s.periphery = true;
        s.focusScale = 75, s.peripheryScale = 40, s.peripheryCoverage = 60, s.edgeBlend = 14;
        s.workingLimit = 25;
        s.beforeDlss = true, s.exposure = 150, s.inputGamma = false;
        s.autoExposure = false, s.maxGain = 450;
        check(write_nr_settings(ini.c_str(), s) && read_nr_settings(ini.c_str()) == s, "[dlss_nr]: every setting written reads back");
        WritePrivateProfileStringW(L"dlss_nr", L"intensity", L"250", ini.c_str());
        WritePrivateProfileStringW(L"dlss_nr", L"local_tone", L"-5", ini.c_str());
        WritePrivateProfileStringW(L"dlss_nr", L"focus_working", L"3", ini.c_str());
        WritePrivateProfileStringW(L"dlss_nr", L"periphery_coverage", L"500", ini.c_str());
        WritePrivateProfileStringW(L"dlss_nr", L"edge_blend", L"90", ini.c_str());
        WritePrivateProfileStringW(L"dlss_nr", L"style", L"7", ini.c_str());
        WritePrivateProfileStringW(L"dlss_nr", L"preset", L"9", ini.c_str());
        const NrSettings held = read_nr_settings(ini.c_str());
        check(held.intensity == 100 && held.localTone == 0 && held.focusScale == kNrScaleMin && held.peripheryCoverage == 100 &&
                  held.edgeBlend == kNrEdgeMax && held.style == 0 && held.preset == 0,
              "[dlss_nr]: values out of range are held to it, an unknown style or preset is the default");
        DeleteFileW(ini.c_str());
    }
    std::printf(failures ? "\n%d check(s) FAILED\n" : "\nall checks passed\n", failures);
    return failures ? 1 : 0;
}
