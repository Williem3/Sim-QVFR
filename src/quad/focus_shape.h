// The focus area's shape ([quad] focus_shape and shape_points; live): a box (its corners rounded by
// corner_radius_percent), or a free-form outline of 3 to 10 points inside the box (a windscreen's, say). The area
// blended into the outer views changes; a game with the focus mask (quad/focus_mask.h: LMU, ACE) also skips rendering
// the focus views outside it, elsewhere they still render the whole box.
// Each point is in percent of the box from its centre, x right and y up (-50..50: the box's edges), so the outline
// moves with the box (its size, its offset, the gaze). The outline runs through the points in order and closes back to
// the first; points added between the two neighbours whose edge they lie nearest keep it from crossing itself.
// Shared by the layer, the launcher and the in-headset menu.
#pragma once

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <initializer_list>
#include <string>

#include <windows.h>

#include "../ini_put.h"

namespace simqvfr {

inline constexpr int kFocusPointsMax = 10;
inline constexpr int kFocusPointsMin = 3;
inline constexpr int kFocusPointReach = 50;  // percent of the box from its centre either way: its edges

struct FocusPoint {
    int x = 0;  // percent of the box right of its centre
    int y = 0;  // percent of the box up from its centre
    bool operator==(const FocusPoint&) const = default;
};

struct FocusShape {
    bool custom = false;  // [quad] focus_shape=points (else box)
    int count = 4;        // points in use (kFocusPointsMin..kFocusPointsMax)
    // [quad] shape_points, in outline order; the box's corners (clockwise from the top left) until moved.
    FocusPoint points[kFocusPointsMax] = {{-50, 50}, {50, 50}, {50, -50}, {-50, -50}};
    bool operator==(const FocusShape& o) const {
        if (custom != o.custom || count != o.count)
            return false;
        for (int i = 0; i < count && i < kFocusPointsMax; ++i)
            if (!(points[i] == o.points[i]))
                return false;
        return true;
    }
};

inline FocusPoint clamp_focus_point(FocusPoint p) {
    return {std::clamp(p.x, -kFocusPointReach, kFocusPointReach), std::clamp(p.y, -kFocusPointReach, kFocusPointReach)};
}

// The plain box's four corners (clockwise from the top left), the outline a fresh custom shape starts from.
inline void focus_shape_reset(FocusShape& s) {
    const FocusShape box;
    s.count = box.count;
    std::copy(std::begin(box.points), std::end(box.points), s.points);
}

// The outline is the plain box (its four corners in place): composed as the box, so the corner radius still applies.
inline bool focus_shape_is_box(const FocusShape& s) {
    if (s.count != 4)
        return false;
    const FocusShape box;
    for (int i = 0; i < 4; ++i)
        if (!(clamp_focus_point(s.points[i]) == box.points[i]))
            return false;
    return true;
}

// "x,y x,y ..." for [quad] shape_points.
inline std::wstring focus_points_text(const FocusShape& s) {
    std::wstring text;
    for (int i = 0; i < std::clamp(s.count, 0, kFocusPointsMax); ++i) {
        const FocusPoint p = clamp_focus_point(s.points[i]);
        text += (i ? L" " : L"") + std::to_wstring(p.x) + L"," + std::to_wstring(p.y);
    }
    return text;
}

// shape_points' text into `s` (at most kFocusPointsMax points; fewer than kFocusPointsMin: false, `s` unchanged).
inline bool parse_focus_points(const wchar_t* text, FocusShape& s) {
    FocusPoint read[kFocusPointsMax];
    int n = 0;
    const wchar_t* p = text;
    while (*p && n < kFocusPointsMax) {
        while (*p == L' ' || *p == L';' || *p == L'\t')
            ++p;
        if (!*p)
            break;
        int x = 0, y = 0, used = 0;
        if (swscanf_s(p, L"%d,%d%n", &x, &y, &used) != 2 || used <= 0)
            break;
        read[n++] = clamp_focus_point({x, y});
        p += used;
    }
    if (n < kFocusPointsMin)
        return false;
    s.count = n;
    std::copy(read, read + n, s.points);
    return true;
}

// Before shape_points, focus_shape=corners pulled the box's four corners in (shape_top_left_in, shape_top_left_down,
// ... shape_bottom_left_up, percent of the box): read as those four points.
inline void read_legacy_corners(const wchar_t* ini, FocusShape& s) {
    const auto inset = [ini](const wchar_t* key) {
        return std::clamp(static_cast<int>(GetPrivateProfileIntW(L"quad", key, 0, ini)), 0, 45);
    };
    s.count = 4;
    s.points[0] = {-50 + inset(L"shape_top_left_in"), 50 - inset(L"shape_top_left_down")};
    s.points[1] = {50 - inset(L"shape_top_right_in"), 50 - inset(L"shape_top_right_down")};
    s.points[2] = {50 - inset(L"shape_bottom_right_in"), -50 + inset(L"shape_bottom_right_up")};
    s.points[3] = {-50 + inset(L"shape_bottom_left_in"), -50 + inset(L"shape_bottom_left_up")};
}

inline FocusShape read_focus_shape(const wchar_t* ini) {
    FocusShape s;
    wchar_t text[16] = {};
    GetPrivateProfileStringW(L"quad", L"focus_shape", L"box", text, 16, ini);
    const bool legacy = _wcsicmp(text, L"corners") == 0;
    s.custom = legacy || _wcsicmp(text, L"points") == 0;
    wchar_t points[256] = {};
    GetPrivateProfileStringW(L"quad", L"shape_points", L"", points, 256, ini);
    if (!parse_focus_points(points, s) && legacy)
        read_legacy_corners(ini, s);
    return s;
}

inline bool write_focus_shape(const wchar_t* ini, const FocusShape& s) {
    bool ok = ini_put(ini, L"quad", L"focus_shape", s.custom ? L"points" : L"box");
    ok &= ini_put(ini, L"quad", L"shape_points", focus_points_text(s).c_str());
    return ok;
}

// The outline as the compositor's constants carry it (compose_shader.h inside_poly): five words, two points each (a
// point 14 bits: 7 across from the box's left, 7 down from its top, percent), the point count in word 0's top 4 bits.
// All 0: the box (not custom, or the outline is the box itself).
struct FocusShapeWords {
    uint32_t w[5] = {};
    bool any() const { return w[0] || w[1] || w[2] || w[3] || w[4]; }
    bool operator==(const FocusShapeWords&) const = default;
};

inline FocusShapeWords focus_shape_words(const FocusShape& s) {
    FocusShapeWords out;
    const int n = std::clamp(s.count, 0, kFocusPointsMax);
    if (!s.custom || n < kFocusPointsMin || focus_shape_is_box(s))
        return out;
    for (int i = 0; i < n; ++i) {
        const FocusPoint p = clamp_focus_point(s.points[i]);
        const uint32_t across = static_cast<uint32_t>(p.x + 50), down = static_cast<uint32_t>(50 - p.y);
        out.w[i / 2] |= (across | (down << 7)) << (14 * (i & 1));
    }
    out.w[0] |= static_cast<uint32_t>(n) << 28;
    return out;
}

// The words' points as shares of the box (0..1 across from the left, down from the top), and how many (0: the box).
inline int focus_shape_points(const FocusShapeWords& words, float (&xy)[2 * kFocusPointsMax]) {
    const int n = std::min(static_cast<int>(words.w[0] >> 28), kFocusPointsMax);
    for (int i = 0; i < kFocusPointsMax; ++i) {
        const uint32_t point = (words.w[i / 2] >> (14 * (i & 1))) & 0x3fff;
        xy[2 * i] = i < n ? static_cast<float>(point & 0x7f) / 100.0f : 0.0f;
        xy[2 * i + 1] = i < n ? static_cast<float>(point >> 7) / 100.0f : 0.0f;
    }
    return n >= kFocusPointsMin ? n : 0;
}

// The layer's copy of the words, written live by one thread and read by the compositors' (a sequence lock: a reader
// never sees half of an edit).
class SharedFocusShape {
public:
    void store(const FocusShapeWords& v) {
        seq_.fetch_add(1, std::memory_order_acq_rel);
        for (int i = 0; i < 5; ++i)
            w_[i].store(v.w[i], std::memory_order_relaxed);
        seq_.fetch_add(1, std::memory_order_release);
    }
    FocusShapeWords load() const {
        FocusShapeWords v;
        for (;;) {
            const uint32_t before = seq_.load(std::memory_order_acquire);
            if (before & 1)
                continue;
            for (int i = 0; i < 5; ++i)
                v.w[i] = w_[i].load(std::memory_order_relaxed);
            std::atomic_thread_fence(std::memory_order_acquire);
            if (seq_.load(std::memory_order_relaxed) == before)
                return v;
        }
    }

private:
    std::atomic<uint32_t> seq_{0};
    std::atomic<uint32_t> w_[5]{};
};

// A custom shape through `points` (at most kFocusPointsMax, percent of the box from its centre; tests, presets).
inline FocusShape focus_shape_of(std::initializer_list<FocusPoint> points) {
    FocusShape s;
    s.custom = true;
    s.count = 0;
    for (const FocusPoint& p : points)
        if (s.count < kFocusPointsMax)
            s.points[s.count++] = clamp_focus_point(p);
    return s;
}

// Adjust point's first choice (the headset menu, the launcher): the whole shape, its points moved together.
inline constexpr int kFocusWholeShape = -1;

// The middle of the outline's extent (percent of the box from its centre, as the points): where the whole shape is.
inline FocusPoint focus_shape_centre(const FocusShape& s) {
    const int n = std::clamp(s.count, 1, kFocusPointsMax);
    int x0 = kFocusPointReach, x1 = -kFocusPointReach, y0 = kFocusPointReach, y1 = -kFocusPointReach;
    for (int i = 0; i < n; ++i) {
        const FocusPoint p = clamp_focus_point(s.points[i]);
        x0 = std::min(x0, p.x);
        x1 = std::max(x1, p.x);
        y0 = std::min(y0, p.y);
        y1 = std::max(y1, p.y);
    }
    const auto mid = [](int a, int b) { return (a + b) / 2; };
    return {mid(x0, x1), mid(y0, y1)};
}

// Moves every point `dx` right and `dy` up together, as far as the box allows without changing the shape (a shape
// already at an edge moves no further that way). Whether it moved.
inline bool focus_shape_move(FocusShape& s, int dx, int dy) {
    const int n = std::clamp(s.count, 1, kFocusPointsMax);
    int x0 = kFocusPointReach, x1 = -kFocusPointReach, y0 = kFocusPointReach, y1 = -kFocusPointReach;
    for (int i = 0; i < n; ++i) {
        s.points[i] = clamp_focus_point(s.points[i]);
        x0 = std::min(x0, s.points[i].x);
        x1 = std::max(x1, s.points[i].x);
        y0 = std::min(y0, s.points[i].y);
        y1 = std::max(y1, s.points[i].y);
    }
    dx = std::clamp(dx, -kFocusPointReach - x0, kFocusPointReach - x1);
    dy = std::clamp(dy, -kFocusPointReach - y0, kFocusPointReach - y1);
    if (!dx && !dy)
        return false;
    for (int i = 0; i < n; ++i)
        s.points[i] = {s.points[i].x + dx, s.points[i].y + dy};
    return true;
}

// Moves the whole shape so its centre (focus_shape_centre) is at `x` across or `y` up (the other kept), as far as the
// box allows.
inline bool focus_shape_move_to(FocusShape& s, int x, int y) {
    const FocusPoint c = focus_shape_centre(s);
    return focus_shape_move(s, x - c.x, y - c.y);
}

// The point nearest (u, v) (percent of the box from its centre, as the points) within `reach`, else -1.
inline int focus_point_near(const FocusShape& s, double u, double v, double reach) {
    int best = -1;
    double bestD = reach * reach;
    for (int i = 0; i < std::clamp(s.count, 0, kFocusPointsMax); ++i) {
        const double dx = s.points[i].x - u, dy = s.points[i].y - v, d = dx * dx + dy * dy;
        if (d <= bestD) {
            best = i;
            bestD = d;
        }
    }
    return best;
}

// Adds a point at `p` between the two neighbours whose edge it lies nearest (so the outline doesn't cross itself);
// its index, or -1 with the outline full.
inline int focus_shape_add_point(FocusShape& s, FocusPoint p) {
    const int n = std::clamp(s.count, 0, kFocusPointsMax);
    if (n >= kFocusPointsMax)
        return -1;
    p = clamp_focus_point(p);
    int after = n - 1;
    double best = 1e18;
    for (int j = 0; j < n; ++j) {
        const FocusPoint a = s.points[j], b = s.points[(j + 1) % n];
        const double ex = b.x - a.x, ey = b.y - a.y, wx = p.x - a.x, wy = p.y - a.y;
        const double len = ex * ex + ey * ey;
        const double t = len > 0 ? std::clamp((wx * ex + wy * ey) / len, 0.0, 1.0) : 0.0;
        const double dx = wx - ex * t, dy = wy - ey * t, d = dx * dx + dy * dy;
        if (d < best) {
            best = d;
            after = j;
        }
    }
    for (int i = n; i > after + 1; --i)
        s.points[i] = s.points[i - 1];
    s.points[after + 1] = p;
    s.count = n + 1;
    return after + 1;
}

// Removes point `i` (not below kFocusPointsMin points): whether it did.
inline bool focus_shape_remove_point(FocusShape& s, int i) {
    const int n = std::clamp(s.count, 0, kFocusPointsMax);
    if (n <= kFocusPointsMin || i < 0 || i >= n)
        return false;
    for (int k = i; k + 1 < n; ++k)
        s.points[k] = s.points[k + 1];
    s.points[n - 1] = {};
    s.count = n - 1;
    return true;
}

// For the log: "box", or "points: -50,50 50,50 30,-50 -30,-50" (percent of the box from its centre, x right, y up).
inline std::wstring focus_shape_text(const FocusShape& s) {
    if (!s.custom)
        return L"box";
    return L"points: " + focus_points_text(s);
}

}  // namespace simqvfr
