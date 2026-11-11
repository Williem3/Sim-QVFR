// The maths of the DLAA inputs (dlss_views.h): the projection jitter applied through the located FOV, and the
// reprojection that gives a static world's motion vectors from depth. Matrices are 4x4 row-major with row vectors
// (clip = v M), left-handed view space (+z forward), as LMU's (the first game). Header-only, for tests/lmu_dlss_test.cpp.
#pragma once

#include <array>
#include <cmath>

namespace simqvfr::dlss {

using M4 = std::array<double, 16>;

inline M4 identity() { return {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1}; }

inline M4 from_floats(const float* m) {
    M4 r;
    for (int i = 0; i < 16; ++i)
        r[i] = m[i];
    return r;
}

inline M4 mul(const M4& a, const M4& b) {
    M4 r{};
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j) {
            double s = 0;
            for (int k = 0; k < 4; ++k)
                s += a[i * 4 + k] * b[k * 4 + j];
            r[i * 4 + j] = s;
        }
    return r;
}

// Gauss-Jordan with partial pivoting. False: singular.
inline bool invert(const M4& m, M4& out) {
    double a[4][8];
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j) {
            a[i][j] = m[i * 4 + j];
            a[i][j + 4] = i == j ? 1 : 0;
        }
    for (int c = 0; c < 4; ++c) {
        int pivot = c;
        for (int r = c + 1; r < 4; ++r)
            if (std::fabs(a[r][c]) > std::fabs(a[pivot][c]))
                pivot = r;
        if (std::fabs(a[pivot][c]) < 1e-300)
            return false;
        if (pivot != c)
            for (int j = 0; j < 8; ++j)
                std::swap(a[c][j], a[pivot][j]);
        const double d = a[c][c];
        for (int j = 0; j < 8; ++j)
            a[c][j] /= d;
        for (int r = 0; r < 4; ++r) {
            if (r == c)
                continue;
            const double f = a[r][c];
            if (f != 0)
                for (int j = 0; j < 8; ++j)
                    a[r][j] -= f * a[c][j];
        }
    }
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j)
            out[i * 4 + j] = a[i][j + 4];
    return true;
}

// A rigid transform (a view matrix or a camera's world matrix): orthonormal rotation, last column 0, 0, 0, 1.
inline bool rigid(const M4& m, double tolerance = 1e-3) {
    if (std::fabs(m[3]) > tolerance || std::fabs(m[7]) > tolerance || std::fabs(m[11]) > tolerance ||
        std::fabs(m[15] - 1) > tolerance)
        return false;
    for (int i = 0; i < 3; ++i)
        for (int j = i; j < 3; ++j) {
            double dot = 0;
            for (int k = 0; k < 3; ++k)
                dot += m[i * 4 + k] * m[j * 4 + k];
            if (std::fabs(dot - (i == j ? 1 : 0)) > tolerance)
                return false;
        }
    return true;
}

// A perspective projection of LMU's shape (no rotation or skew, w = z).
inline bool projection(const M4& m) {
    return m[0] > 0.01 && m[5] > 0.01 && m[1] == 0 && m[2] == 0 && m[3] == 0 && m[4] == 0 && m[6] == 0 && m[7] == 0 &&
           m[11] == 1.0 && m[15] == 0 && std::isfinite(m[8]) && std::isfinite(m[9]);
}

inline double halton(unsigned index, unsigned base) {
    double f = 1, r = 0;
    while (index > 0) {
        f /= base;
        r += f * (index % base);
        index /= base;
    }
    return r;
}

// Frame n's jitter: how far the image moves, in pixels (x right, y down), each in [-0.5, 0.5). 16 phases unless told.
inline constexpr unsigned kJitterPhases = 16;
inline void jitter_pixels(unsigned n, double& x, double& y, unsigned phases = kJitterPhases) {
    const unsigned i = n % (phases ? phases : kJitterPhases) + 1;
    x = halton(i, 2) - 0.5;
    y = halton(i, 3) - 0.5;
}

// The phases for an image upscaled from `scale` of its size each way: NVIDIA's 8 x (output / render)^2, never fewer
// than DLAA's 16 (50%: 32).
inline unsigned jitter_phases(double scale) {
    if (!(scale > 0) || scale >= 1)
        return kJitterPhases;
    const double n = std::ceil(8.0 / (scale * scale) - 1e-9);
    return n > kJitterPhases ? static_cast<unsigned>(n) : kJitterPhases;
}

// The DLSS mode a render scale (percent of the output each way) is nearest: NVIDIA's ultra quality 77%, quality 67%,
// balanced 58%, performance 50%, ultra performance 33%; 100: DLAA (ngx_dlss.cpp picks NGX's mode the same way).
inline const char* focus_mode_name(int percent) {
    return percent >= 100 ? "DLAA"
           : percent >= 72 ? "ultra quality"
           : percent >= 62 ? "quality"
           : percent >= 54 ? "balanced"
           : percent >= 42 ? "performance"
                           : "ultra performance";
}

// NGX's render presets by letter: A is 1 ... O is 15 (nvsdk_ngx_defs.h NVSDK_NGX_DLSS_Hint_Render_Preset); 0 is NGX's
// default for the feature's mode. A setting's text: a letter, or the number; anything else, empty or out of range:
// `fallback`.
inline int preset_from_text(const wchar_t* text, int fallback) {
    if (!text)
        return fallback;
    while (*text == L' ' || *text == L'\t')
        ++text;
    if (!*text)
        return fallback;
    const wchar_t c = *text >= L'a' && *text <= L'z' ? static_cast<wchar_t>(*text - L'a' + L'A') : *text;
    if (c >= L'A' && c <= L'O' && (text[1] == 0 || text[1] == L' ' || text[1] == L'\t'))
        return c - L'A' + 1;
    int n = 0;
    for (; *text >= L'0' && *text <= L'9'; ++text)
        n = n * 10 + (*text - L'0');
    while (*text == L' ' || *text == L'\t')
        ++text;
    return *text || n > 15 ? fallback : n;
}

// "default" for 0, else the preset's letter ("F", "K").
inline const char* preset_name(int preset) {
    static const char* const names[] = {"default", "A", "B", "C", "D", "E", "F", "G", "H", "I", "J", "K", "L", "M", "N", "O"};
    return preset >= 0 && preset <= 15 ? names[preset] : "?";
}

// A side `output` pixels long rendered at `scale` of it for an upscale: rounded, at least 1, at most `output`.
inline unsigned render_size(unsigned output, double scale) {
    if (!(scale > 0) || scale >= 1)
        return output;
    const double r = std::floor(output * scale + 0.5);
    return r < 1 ? 1u : static_cast<unsigned>(r);
}

struct Tangents {
    double left = 0, right = 0, down = 0, up = 0;
};

// The clip-space shift (x += clipX w, y += clipY w) that moves a width x height image by (sx, sy) pixels (x right, y
// down).
inline void clip_shift_for(double sx, double sy, unsigned width, unsigned height, double& clipX, double& clipY) {
    clipX = width ? 2 * sx / width : 0;
    clipY = height ? -2 * sy / height : 0;
}

// The view's tangents moved so its off-centre projection (third row -(l + r) / (r - l), -(u + d) / (u - d)) gets that
// clip shift: both sides of an axis move together, the FOV's size stays.
inline void shift_tangents(Tangents& t, double clipX, double clipY) {
    const double dx = -clipX * (t.right - t.left) / 2, dy = -clipY * (t.up - t.down) / 2;
    t.left += dx;
    t.right += dx;
    t.down += dy;
    t.up += dy;
}

// The same projection with its depth the other way round: 1 at the near plane, 0 far away (z' = w - z). LMU's scene
// renders that way (depth test "greater"; docs/recon-lmu.md "DLSS step 1, run 3"), while the projection its VR code
// keeps per eye is the ordinary one.
inline M4 reversed_depth(const M4& p) {
    M4 r = p;
    for (int i = 0; i < 4; ++i)
        r[i * 4 + 2] = p[i * 4 + 3] - p[i * 4 + 2];
    return r;
}

// An eye's view matrix in the tracking space, from its OpenXR pose (orientation x, y, z, w and position; the eye's
// frame in the tracking space, right-handed, -z forward): tracking space -> the game's view space (left-handed, +z
// forward), row vectors. What is fixed to the player's seat (the cockpit, the car) stands still in the tracking space,
// so two frames' matrices give its motion on screen: the head's alone.
inline M4 head_view(const double q[4], const double t[3]) {
    const double x = q[0], y = q[1], z = q[2], w = q[3];
    const double r[3][3] = {{1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w)},
                            {2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w)},
                            {2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y)}};  // eye -> tracking
    const double s[3] = {1, 1, -1};  // the game's z is the eye's -z
    M4 m = identity();
    for (int j = 0; j < 3; ++j) {
        double tj = 0;
        for (int i = 0; i < 3; ++i) {
            m[i * 4 + j] = s[j] * r[i][j];
            tj += r[i][j] * t[i];
        }
        m[12 + j] = -s[j] * tj;
    }
    return m;
}

// The player's car as a box around the head in the tracking space (x right, y up, -z forward): how far it reaches to
// the left, right, down, up, front and back of the head, in metres. The car is fixed to the seat, the tracking space's
// forward is the car's, and the head sits within a few decimetres of one place in it.
struct CarBox {
    double left = 1.5, right = 1.5, down = 1.5, up = 0.6, front = 3.0, back = 3.0;
};

// Whether the point at NDC (nx, ny) with depth `depth` lies in that box: through `clipToTracking` (the inverse of the
// head's view x the projection as rendered), about the head's position. As the shaders test it (dlss_views.cpp).
inline bool in_car_box(const M4& clipToTracking, double nx, double ny, double depth, const double head[3], const CarBox& box) {
    const M4& m = clipToTracking;
    const double w = nx * m[3] + ny * m[7] + depth * m[11] + m[15];
    if (std::fabs(w) < 1e-9)
        return false;
    const double x = (nx * m[0] + ny * m[4] + depth * m[8] + m[12]) / w - head[0];
    const double y = (nx * m[1] + ny * m[5] + depth * m[9] + m[13]) / w - head[1];
    const double z = (nx * m[2] + ny * m[6] + depth * m[10] + m[14]) / w - head[2];
    return x > -box.left && x < box.right && y > -box.down && y < box.up && z > -box.front && z < box.back;
}

// clip' = clip S: x += cx w, y += cy w.
inline M4 clip_shift(double cx, double cy) {
    M4 m = identity();
    m[12] = cx;
    m[13] = cy;
    return m;
}

// clip' = clip M: x' = a x + b w, y' = c y + d w (a focus view from its eye; quad/offaxis_focus.h ClipRemap).
inline M4 clip_remap_matrix(double a, double b, double c, double d) {
    M4 m = identity();
    m[0] = a;
    m[12] = b;
    m[5] = c;
    m[13] = d;
    return m;
}

// From this frame's clip space as rendered (jitter included) to the frame before's without its jitter: for a point that
// stood still in the world. False: this frame's matrix can't be inverted.
inline bool reprojection(const M4& renderedNow, const M4& unjitteredBefore, M4& out) {
    M4 inverse;
    if (!invert(renderedNow, inverse))
        return false;
    out = mul(inverse, unjitteredBefore);
    return true;
}

// The motion vector at pixel centre (px, py) of a width x height view with depth `depth`: to where that point was in
// the frame before, in pixels, both positions without jitter (clipX, clipY: this frame's clip shift). As the shader
// computes it (dlss_views.cpp). False: behind the previous camera.
inline bool motion_vector(const M4& r, double clipX, double clipY, unsigned width, unsigned height, double px, double py,
                          double depth, double& mx, double& my) {
    const double nx = px / width * 2 - 1, ny = 1 - py / height * 2;
    const double x = nx * r[0] + ny * r[4] + depth * r[8] + r[12];
    const double y = nx * r[1] + ny * r[5] + depth * r[9] + r[13];
    const double w = nx * r[3] + ny * r[7] + depth * r[11] + r[15];
    if (w <= 1e-9) {
        mx = my = 0;
        return false;
    }
    mx = (x / w - (nx - clipX)) * 0.5 * width;
    my = (y / w - (ny - clipY)) * -0.5 * height;
    return true;
}

// The gate's colour verdict on one class of points (dlss_views.cpp kCheckShader), from the frame's mean difference from
// the one before where the points stood (`still`), where their vectors say they were (`along`) and a pixel off that
// (`off`). Fit: clearly better than standing; fit to the pixel: too little motion to beat standing, but clearly better
// than a pixel off; else no better than none, or worse. Detail finer than a pixel (far scenery at a small view size)
// scrambles colour between jittered frames whatever the vectors: there all three come out alike, and only the depth
// verdict below judges the vectors (measured: lmu_dlss_check's moving scene). Empty: nothing changed.
inline const char* gate_verdict(double still, double along, double off) {
    if (still < 1e-4)
        return "";
    if (along < 0.7 * still)
        return ": fit";
    if (along < 0.85 * off)
        return ": fit to the pixel";
    return along > 1.2 * still ? ": WORSE than none" : ": no better than none";
}

// The gate's depth verdict: the share of a class's points whose depth in the frame before, where their vectors say they
// were, is the depth the reprojection predicts (`along`), and whose depth there is this frame's where they stand
// (`still`). Fit: 85% or more agree (what doesn't was hidden in the frame before, or moved in the world: another car);
// lmu_dlss_check's moving scene, a 520x420 eye at 45 m/s through rows of posts: right vectors 88-98%, the view matrix
// inverted 61-64%. A little motion: standing agrees as well, so it shows little (a road driven along keeps each pixel's
// depth: only what stands across it tells). Worse than none: fewer agree than standing; else partly.
inline const char* gate_depth_verdict(double still, double along) {
    if (along >= 0.85)
        return still >= 0.85 ? ": fit (little motion)" : ": fit";
    return along < still ? ": WORSE than none" : ": partly";
}

// A focus box measured on an eye projection that carries a clip shift (x += cx w, y += cy w: DLAA's jitter) as it is
// without it, which is how the eye's DLAA output has it: the eye's tangents (left, right, down, up) move back by the
// shift's share of the span, the box's own tangents stay (they are the gaze's), its place in the eye's image (rect: x0,
// y0, x1, y1 from the top left, 0-1) moves by it.
template <class Box>
inline void unshift_box(Box& box, double cx, double cy) {
    const double dx = cx * (box.eyeTan[1] - box.eyeTan[0]) / 2, dy = cy * (box.eyeTan[3] - box.eyeTan[2]) / 2;
    box.eyeTan[0] = static_cast<float>(box.eyeTan[0] + dx);
    box.eyeTan[1] = static_cast<float>(box.eyeTan[1] + dx);
    box.eyeTan[2] = static_cast<float>(box.eyeTan[2] + dy);
    box.eyeTan[3] = static_cast<float>(box.eyeTan[3] + dy);
    box.rect[0] -= cx / 2;
    box.rect[2] -= cx / 2;
    box.rect[1] += cy / 2;
    box.rect[3] += cy / 2;
}

}  // namespace simqvfr::dlss
