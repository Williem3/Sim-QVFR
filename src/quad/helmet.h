// The helmet overlay ([helmet] in sim-qvfr.ini, QVFR): the layer draws a helmet round the view,
// lit by the frame, shared by the layer (which composes it, live), the launcher's Resolution pane and
// the in-headset menu's View crop page.
//
// Built in: a visor opening (the FOV crop's rectangle, fov_crop.h, with rounded corners) whose padded
// lip reaches `rim` into the rendered view with a soft, out-of-focus edge (beyond the crop is black: the
// scene image keeps the FOV ACE rendered).
//
// From images (any of top_image, bottom_image, visor_image found): three PNGs with alpha, each one picture
// of the whole face from inside the helmet, fixed to the head: they cover `size` degrees across, centred
// `height` degrees above straight ahead, and each eye looks through them in its own direction. Drawn over
// the scene in order visor, bottom (chin), top (brow), so the visor slides up behind the brow: Closed as
// drawn, Half halfway, Open by `visor_travel` of the image's height. The pieces' edges extend outward
// (the visor's only sideways). `depth` places them that far in front of the eyes: each eye looks at
// them from its own position, so they have stereo depth (0: far, kHelmetFarM away).
//
// The layer draws the images into their own head-locked quad layer, which the runtime composites over
// the scene after any frame generation (so the scene image keeps the FOV ACE rendered, matching its depth
// and motion vectors); helmet_cover sizes the quad. Only the bands the pieces draw on are rendered and
// submitted (helmet_bands): the clear middle of the view costs nothing. `detail` picks its resolution.
// A visor image with no transparency is glass: the scene is filtered by
// its colour (its brightest part clear), so a dark strip tints the view like sunglasses.
//
// Either way it is lit by the frame: the rendered image's light along each side of the eye, averaged
// over a few frames.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include <windows.h>

namespace simqvfr {

enum class VisorPosition { Closed, Half, Open };
inline constexpr const wchar_t* kVisorPositionNames[] = {L"Closed", L"Half", L"Open"};

struct HelmetSettings {
    bool on = false;      // enabled
    int rim = 6;          // rim (built in): how far the lip reaches into the rendered view, percent of the eye's height
    int corners = 40;     // corners (built in): the opening's corner radius, percent of half its shorter side
    int light = 70;       // light: how much the frame lights it, percent (0: as drawn / a fixed dim grey)
    // The images, relative to the layer's folder; a missing file is no piece.
    std::wstring topImage = L"helmet\\top.png", bottomImage = L"helmet\\bottom.png", visorImage = L"helmet\\visor.png";
    int size = 110;       // size: the images' width, degrees of view
    int height = 0;       // height: their centre, degrees above straight ahead (negative: below)
    int depth = 0;        // depth: how far in front of the eyes, centimetres (0: far, kHelmetFarM)
    VisorPosition visor = VisorPosition::Closed;  // visor: closed, half or open
    int visorTravel = 40;  // visor_travel: how far Open slides the visor up, percent of the image's height
    int visorBlend = 0;    // visor_blend: auto, tint (the scene filtered by its colour, like glass) or alpha (as drawn)
    bool debugLight = false;  // debug_light: shows the frame's light itself where the helmet is (to check it follows the scene)
    bool detail = false;  // detail: its layer at kHelmetDetailSide a side (sharper) instead of kHelmetPerformanceSide (cheaper)
    bool operator==(const HelmetSettings&) const = default;
};

inline constexpr int kHelmetRimMax = 20, kHelmetCornersMax = 100, kHelmetLightMax = 100, kHelmetLightStep = 10;
inline constexpr int kHelmetSizeMin = 40, kHelmetSizeMax = 160, kHelmetHeightMax = 30, kHelmetTravelMax = 100;
inline constexpr uint32_t kHelmetPerformanceSide = 2048, kHelmetDetailSide = 4096;
inline uint32_t helmet_layer_side(const HelmetSettings& h) { return h.detail ? kHelmetDetailSide : kHelmetPerformanceSide; }
inline constexpr const wchar_t* kHelmetDetailNames[] = {L"Performance", L"Detail"};
inline constexpr int kHelmetDepthMin = 3, kHelmetDepthMax = 150;  // centimetres; 0 is far (kHelmetFarM)
inline int helmet_depth(int cm) { return cm <= 0 ? 0 : std::clamp(cm, kHelmetDepthMin, kHelmetDepthMax); }
inline constexpr double kHelmetFarM = 10;  // depth 0: the quad's distance (no parallax to speak of)
// The quad's distance in metres.
inline double helmet_distance(const HelmetSettings& h) {
    const int cm = helmet_depth(h.depth);
    return cm ? cm / 100.0 : kHelmetFarM;
}

// The half extents (tangents of the images' plane) a quad `distance` away must cover so that both eyes
// see it to the edges of their uncropped views: for each eye, `rotation` (row-major) takes a direction in
// its frame into the images', `offset` is where it sits from the eyes' middle in the images' frame (m) and
// `view` its uncropped tangents (left, right, up, down). A point at plane tangent t is seen by an eye in
// direction t - offset / distance; kept within kHelmetCoverMax.
inline constexpr double kHelmetCoverMax = 8;
inline void helmet_cover(const float (&rotation)[2][9], const float (&offset)[2][3], const float (&view)[2][4],
                         double distance, double& x, double& y) {
    x = y = 0;
    for (int e = 0; e < 2; ++e)
        for (int corner = 0; corner < 4; ++corner) {
            const double d[3] = {view[e][corner & 1 ? 1 : 0], view[e][corner & 2 ? 3 : 2], -1};
            double r[3];
            for (int i = 0; i < 3; ++i)
                r[i] = rotation[e][i * 3] * d[0] + rotation[e][i * 3 + 1] * d[1] + rotation[e][i * 3 + 2] * d[2];
            if (r[2] > -0.05)  // at or past 87 degrees: as far as the quad goes
                r[2] = -0.05;
            x = std::max(x, std::abs(r[0] / -r[2] + offset[e][0] / distance));
            y = std::max(y, std::abs(r[1] / -r[2] + offset[e][1] / distance));
        }
    x = std::min(kHelmetCoverMax, x * 1.05);  // a margin for the head's motion between frames
    y = std::min(kHelmetCoverMax, y * 1.05);
}

inline HelmetSettings read_helmet_settings(const wchar_t* ini) {
    const auto number = [ini](const wchar_t* key, int fallback) {
        wchar_t text[16] = {};  // as a string: GetPrivateProfileInt reads negative values as 0
        GetPrivateProfileStringW(L"helmet", key, L"", text, 16, ini);
        return text[0] ? _wtoi(text) : fallback;
    };
    const auto path = [ini](const wchar_t* key, const std::wstring& fallback) {
        wchar_t text[MAX_PATH] = {};
        const DWORD n = GetPrivateProfileStringW(L"helmet", key, L"\x01", text, MAX_PATH, ini);
        return n == 1 && text[0] == L'\x01' ? fallback : std::wstring(text);  // unset: the default
    };
    HelmetSettings h;
    h.on = number(L"enabled", 0) != 0;
    h.rim = std::clamp(number(L"rim", h.rim), 0, kHelmetRimMax);
    h.corners = std::clamp(number(L"corners", h.corners), 0, kHelmetCornersMax);
    h.light = std::clamp(number(L"light", h.light), 0, kHelmetLightMax);
    h.topImage = path(L"top_image", h.topImage);
    h.bottomImage = path(L"bottom_image", h.bottomImage);
    h.visorImage = path(L"visor_image", h.visorImage);
    h.size = std::clamp(number(L"size", h.size), kHelmetSizeMin, kHelmetSizeMax);
    h.height = std::clamp(number(L"height", h.height), -kHelmetHeightMax, kHelmetHeightMax);
    h.depth = helmet_depth(number(L"depth", h.depth));
    wchar_t visor[16] = {};
    GetPrivateProfileStringW(L"helmet", L"visor", L"closed", visor, 16, ini);
    h.visor = _wcsicmp(visor, L"open") == 0   ? VisorPosition::Open
              : _wcsicmp(visor, L"half") == 0 ? VisorPosition::Half
                                              : VisorPosition::Closed;
    h.visorTravel = std::clamp(number(L"visor_travel", h.visorTravel), 0, kHelmetTravelMax);
    wchar_t blend[16] = {};
    GetPrivateProfileStringW(L"helmet", L"visor_blend", L"auto", blend, 16, ini);
    h.visorBlend = _wcsicmp(blend, L"tint") == 0 ? 1 : _wcsicmp(blend, L"alpha") == 0 ? 2 : 0;
    h.debugLight = number(L"debug_light", 0) != 0;
    h.detail = number(L"detail", 0) != 0;
    return h;
}

// The keys the launcher and the menu change (the image paths and visor_travel are set in the ini by hand).
inline bool write_helmet_settings(const wchar_t* ini, const HelmetSettings& h) {
    const auto write = [ini](const wchar_t* key, int value) {
        return WritePrivateProfileStringW(L"helmet", key, std::to_wstring(value).c_str(), ini) != 0;
    };
    bool ok = write(L"enabled", h.on ? 1 : 0);
    ok &= write(L"rim", std::clamp(h.rim, 0, kHelmetRimMax));
    ok &= write(L"corners", std::clamp(h.corners, 0, kHelmetCornersMax));
    ok &= write(L"light", std::clamp(h.light, 0, kHelmetLightMax));
    ok &= write(L"size", std::clamp(h.size, kHelmetSizeMin, kHelmetSizeMax));
    ok &= write(L"height", std::clamp(h.height, -kHelmetHeightMax, kHelmetHeightMax));
    ok &= write(L"depth", helmet_depth(h.depth));
    const wchar_t* const visor[] = {L"closed", L"half", L"open"};
    ok &= WritePrivateProfileStringW(L"helmet", L"visor", visor[static_cast<int>(h.visor)], ini) != 0;
    ok &= write(L"debug_light", h.debugLight ? 1 : 0);
    ok &= write(L"detail", h.detail ? 1 : 0);
    return ok;
}

// The overlay's geometry for one eye, in that eye's tangents, as the compositor's shader takes it:
// the opening (left, right, up, down: what ACE rendered), the rim's width and the corner radius.
struct HelmetShape {
    float opening[4];
    float rim, radius;
};
inline HelmetShape helmet_shape(const HelmetSettings& h, const float (&full)[4], const float (&opening)[4]) {
    HelmetShape s;
    std::copy(opening, opening + 4, s.opening);
    s.rim = (full[2] - full[3]) * std::clamp(h.rim, 0, kHelmetRimMax) / 100.0f;
    const float halfShort = 0.5f * std::min(opening[1] - opening[0], opening[2] - opening[3]);
    s.radius = std::max(0.0f, halfShort) * std::clamp(h.corners, 0, kHelmetCornersMax) / 100.0f;
    return s;
}

inline constexpr double kHelmetDegrees = 3.14159265358979323846 / 180;

// The images' half extents as tangents (across, and up/down for an image `aspect` high per wide).
inline void helmet_tan_half(const HelmetSettings& h, double aspect, double& x, double& y) {
    x = std::tan(std::clamp(h.size, kHelmetSizeMin, kHelmetSizeMax) * kHelmetDegrees / 2);
    y = x * aspect;
}

// Depth: where an eye `offset` metres from the eyes' middle (x right, y up, in the images' frame) sees the
// images, as a shift of their tangents (0, 0 when infinitely far): a point at tangent t on their plane
// `depth` in front is seen by that eye in direction t - offset / depth.
inline void helmet_parallax(const HelmetSettings& h, const float (&offset)[3], float (&shift)[2]) {
    const int cm = helmet_depth(h.depth);
    shift[0] = cm ? offset[0] / (cm / 100.0f) : 0.0f;
    shift[1] = cm ? offset[1] / (cm / 100.0f) : 0.0f;
}

// The pieces' rows: kHelmetRows bins down the images' height, each true where any piece draws (the
// compositor's helmet_rows). helmet_bands turns them into the bands of the quad to draw and submit, as
// (top, bottom) in the quad's v (0 its top, 1 its bottom; the quad spans tangents +-coverY of the images'
// plane, the images +-imageY): the quad's whole height, or the parts above and below the largest clear run
// of rows, which is left out when it spans at least kHelmetBandMin of the quad. The images' edge rows
// extend beyond them (clamped), so a clear top or bottom row leaves that end of the quad out too.
inline constexpr int kHelmetRows = 256;
inline constexpr double kHelmetBandMin = 0.08;
struct HelmetBand {
    float top = 0, bottom = 1;
};
inline std::vector<HelmetBand> helmet_bands(const std::vector<bool>& rows, double imageY, double coverY) {
    const int n = static_cast<int>(rows.size());
    std::vector<HelmetBand> whole = {{0, 1}};
    if (n == 0 || imageY <= 0 || coverY <= 0)
        return whole;
    // A row's neighbours count as drawn too (filtering reaches one texel past an edge).
    std::vector<bool> drawn(n);
    for (int i = 0; i < n; ++i)
        drawn[i] = rows[i] || (i > 0 && rows[i - 1]) || (i + 1 < n && rows[i + 1]);
    int bestStart = -1, bestEnd = -1;
    for (int i = 0; i < n;) {
        if (drawn[i]) {
            ++i;
            continue;
        }
        int j = i;
        while (j < n && !drawn[j])
            ++j;
        if (j - i > bestEnd - bestStart) {
            bestStart = i;
            bestEnd = j;
        }
        i = j;
    }
    if (bestStart < 0)
        return whole;
    // The images' v to the quad's: tangent (0.5 - v) * 2 * imageY on the plane.
    const auto quad_v = [&](double v) {
        return static_cast<float>(std::clamp(0.5 - (0.5 - v) * imageY / coverY, 0.0, 1.0));
    };
    const float clearTop = bestStart == 0 ? 0.0f : quad_v(static_cast<double>(bestStart) / n);
    const float clearBottom = bestEnd == n ? 1.0f : quad_v(static_cast<double>(bestEnd) / n);
    if (clearBottom - clearTop < kHelmetBandMin)
        return whole;
    std::vector<HelmetBand> bands;
    if (clearTop > 0)
        bands.push_back({0, clearTop});
    if (clearBottom < 1)
        bands.push_back({clearBottom, 1});
    return bands;
}

// The visor's slide, in image heights, at `position` (0 closed, 0.5 half, 1 open).
inline double visor_offset(const HelmetSettings& h, double position) {
    return position * std::clamp(h.visorTravel, 0, kHelmetTravelMax) / 100.0;
}
inline double visor_target(const HelmetSettings& h) { return static_cast<int>(h.visor) * 0.5; }

// The crop top and bottom (percent) that meet the images' opening straight ahead, for an eye whose
// uncropped view spans tangents `up` to `down`: `topEdge` is where the top piece stops being opaque down
// its middle, `bottomEdge` where the bottom piece starts (image heights from the top, < 0: no piece).
inline void helmet_crop_hint(const HelmetSettings& h, double aspect, double topEdge, double bottomEdge, double up,
                             double down, int& top, int& bottom) {
    double tx, ty;
    helmet_tan_half(h, aspect, tx, ty);
    const auto eye_tan = [&](double v) {
        return std::tan(std::atan((0.5 - v) * 2 * ty) + h.height * kHelmetDegrees);
    };
    const double span = up - down;
    top = topEdge < 0 || span <= 0 ? 0 : static_cast<int>(std::lround(std::clamp((up - eye_tan(topEdge)) / span, 0.0, 1.0) * 100));
    bottom = bottomEdge < 0 || span <= 0 ? 0
                                         : static_cast<int>(std::lround(std::clamp((eye_tan(bottomEdge) - down) / span, 0.0, 1.0) * 100));
}

}  // namespace simqvfr
