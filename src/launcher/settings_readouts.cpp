#include "settings.h"

#include "../games/game.h"
#include "../quad/focus_mask.h"
#include "../quad/views4_layout.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cwchar>
#include <string>

// Display maths: the session summary, the pixel readouts, the VRAM estimate and the slider steps.
namespace simqvfr::launcher {

std::wstring session_summary_text(const RuntimeInfo& r) {
    const SessionInfo& s = r.session;
    if (!s.known())
        return L"Sim-QVFR: no session measured yet.";
    const auto f = [](const wchar_t* format, double v) {  // at whatever length it comes to
        const int n = _scwprintf(format, v);
        if (n <= 0)
            return std::wstring();
        std::wstring text(static_cast<size_t>(n), L'\0');
        swprintf_s(text.data(), static_cast<size_t>(n) + 1, format, v);
        return text;
    };
    std::wstring t = L"Sim-QVFR session " + s.time + L" · " + s.mode;
    if (s.refreshHz > 0)
        t += f(L" · %.0f Hz", s.refreshHz);
    t += s.frameGeneration ? L" · frame generation on\n" : L" · frame generation off\n";
    t += f(L"Frame rate: %.1f fps", s.fps);
    if (s.frameP50 > 0)
        t += f(L"; frame time %.2f ms median", s.frameP50) + f(L", %.2f ms 90th percentile", s.frameP90);
    if (s.latePercent >= 0)
        t += f(L"; %.1f%% of frames late", s.latePercent);
    t += L"\n";
    if (s.halfRatePercent > 0)
        t += f(L"Runtime at half rate: %.1f%% of the session's frames\n", s.halfRatePercent);
    if (s.cpuMs >= 0)
        t += f(L"CPU frame time (render thread): %.2f ms\n", s.cpuMs);
    if (s.gpuMs >= 0)
        t += f(L"GPU frame time (whole GPU): %.2f ms", s.gpuMs) + f(L", %.0f%% busy\n", s.gpuBusy);
    if (!s.limited.empty())
        t += L"Limited by: " + s.limited + L"\n";
    if (s.cores >= 0)
        t += f(L"CPU in use: %.1f cores in all\n", s.cores);
    if (!s.threads.empty()) {
        t += L"Busiest threads (share of one core):";
        for (size_t i = 0; i < s.threads.size(); ++i)
            t += (i ? L"," : L"") + std::wstring(L" ") + s.threads[i].first + f(L" %.0f%%", s.threads[i].second);
        t += L"\n";
    }
    if (s.gazeMs >= 0)
        t += f(L"Eye tracking lookup: %.3f ms (own thread)\n", s.gazeMs);
    if (s.layerMs >= 0)
        t += f(L"Layer's own render-thread time: %.2f ms per frame\n", s.layerMs);
    if (r.vramUsedMb > 0)
        t += f(L"VRAM: %.1f GB", r.vramUsedMb / 1024) + f(L" of %.1f GB\n", r.vramBudgetMb / 1024);
    t += L"Layers below: " + (s.layersBelow.empty() ? std::wstring(L"none") : s.layersBelow) + L"\n";
    if (!s.log.empty())
        t += L"Log: " + s.log + L"\n";
    if (!s.cpuProfile.empty())
        t += L"CPU capture: " + s.cpuProfile + L"\n";
    return t;
}

Readouts readouts(const Settings& s, const RuntimeInfo& runtime) {
    const games::GameInfo& game = games::launcher_game();
    return readouts(s, runtime, game.features.focusMask, game.qvfrImage, game.features.multiView);
}

// The image the layer offers the game per eye, in native eye widths and heights, and whether two of them share one
// double-wide image (layer/session.cpp layerEnumerateViewConfigurationViews, the same Views4Layout).
struct OfferedEye {
    double width = 0, height = 0;
    bool doubleWide = true;
};

static OfferedEye offered_eye(const Settings& s, games::QvfrImage image, bool multiView) {
    using games::QvfrImage;
    Views4Layout l = views4_layout(s.outer / 100.0, s.inner / 100.0 * s.width / 100.0, s.inner / 100.0 * s.height / 100.0,
                                   s.crop_width(), s.crop_height());
    switch (image) {
    case QvfrImage::EyeColumns: return {l.column_eye_width(), l.column_eye_height(), true};
    case QvfrImage::OneViewSize: return {l.layer_width(), 2 * l.layer_height(), true};
    case QvfrImage::SeparateViews:
        l.secondViewRoom = l.secondViewBeside = multiView;  // multi_view on (lmu_mvr_beside's default)
        return {l.layer_width(), l.layer_height(), false};
    default: return {l.eye_width(), l.eye_height(), true};
    }
}

Readouts readouts(const Settings& s, const RuntimeInfo& runtime, bool gameMasksFocus, games::QvfrImage image,
                  bool multiView) {
    Readouts r;
    const auto scaled = [](int base, double fraction) { return static_cast<int>(std::lround(base * fraction)); };
    const auto area = [](Size v) { return static_cast<double>(v.width) * v.height; };
    // QVFR's FOV crop: the outer views keep their density over the kept share of the eye.
    r.outer = {scaled(runtime.width, s.outer / 100.0 * s.crop_width()), scaled(runtime.height, s.outer / 100.0 * s.crop_height())};
    // The kit's QVFR.viewPixels: focus px = recommended x inner % x foveated size %.
    r.focus = {scaled(runtime.width, s.inner / 100.0 * s.width / 100.0),
               scaled(runtime.height, s.inner / 100.0 * s.height / 100.0)};
    r.target = {2 * (r.outer.width + r.focus.width), std::max(r.outer.height, r.focus.height)};
    r.focusDensity = s.inner / 100.0;
    // The eye image the layer offers must fit the runtime's maximum: half its width each in a double-wide target (ACE,
    // ACC, X-Plane), all of it where each view is an image of its own (LMU). Measured as the layer does:
    // recommended x the layout's scale, truncated.
    const OfferedEye eye = offered_eye(s, image, multiView);
    const int eyeWidth = static_cast<int>(runtime.width * eye.width);
    const int eyeHeight = static_cast<int>(runtime.height * eye.height);
    const bool doubleWide = eye.doubleWide;
    const int maxEyeWidth = runtime.maxWidth > 0 ? std::max(64, runtime.maxWidth / (doubleWide ? 2 : 1)) : 0;
    const int maxEyeHeight = runtime.maxHeight > 0 ? std::max(64, runtime.maxHeight) : 0;
    const double sx = maxEyeWidth > 0 && eyeWidth > maxEyeWidth ? static_cast<double>(maxEyeWidth) / eyeWidth : 1.0;
    const double sy = maxEyeHeight > 0 && eyeHeight > maxEyeHeight ? static_cast<double>(maxEyeHeight) / eyeHeight : 1.0;
    r.overRuntimeLimit = sx < 1.0 || sy < 1.0;
    if (r.overRuntimeLimit) {
        // The layer clamps the offered eye to the maximum and every view keeps its share of it, so all
        // of them shrink by the same factor on that axis.
        const auto shrink = [&](Size v) { return Size{scaled(v.width, sx), scaled(v.height, sy)}; };
        r.outer = shrink(r.outer);
        r.focus = shrink(r.focus);
        r.target = shrink(r.target);
    }
    // Two outer and two focus views, less what the focus mask skips in the focus views (outside the focus shape, in a
    // game that masks them).
    if (gameMasksFocus && s.focusMask && r.focus.width > 0 && r.focus.height > 0)
        r.focusMasked = focus_mask_share(focus_area_of(s.shape, s.corner), static_cast<unsigned>(r.focus.width),
                                         static_cast<unsigned>(r.focus.height),
                                         static_cast<unsigned>(std::max(s.focusMaskMargin, 0)));
    r.pixels = 2.0 * (area(r.outer) + area(r.focus) * (1 - r.focusMasked));
    r.runtime = {runtime.width, runtime.height};
    r.runtimePixels = 2.0 * area(r.runtime);
    r.saved = r.runtimePixels > 0 ? 1.0 - r.pixels / r.runtimePixels : 0;
    r.centerPpd = runtime.centerPpd * r.focusDensity * sx;  // a runtime-maximum clamp lowers the density too
    return r;
}

VramEstimate vram_estimate(const Settings&, const Readouts& r, const RuntimeInfo& runtime) {
    VramEstimate e;
    const double launchArea = static_cast<double>(runtime.vramTarget.width) * runtime.vramTarget.height;
    if (runtime.vramUsedMb <= 0 || launchArea <= 0)
        return e;
    const double area = static_cast<double>(r.target.width) * r.target.height;
    const double scaled = std::min(runtime.vramScaledMb, runtime.vramUsedMb);
    e.mb = runtime.vramUsedMb - scaled + scaled * area / launchArea;
    e.budgetMb = runtime.vramBudgetMb;
    return e;
}

int slider_value_at(Range range, double share, int step) {
    step = std::max(step, 1);
    const double offset = std::clamp(share, 0.0, 1.0) * (range.max - range.min);
    return std::min(range.min + static_cast<int>(std::lround(offset / step)) * step, range.max);
}

}  // namespace simqvfr::launcher
