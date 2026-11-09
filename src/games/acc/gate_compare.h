// The comparisons of ACC's copies and focus gates (gates.h), on 8-bit BGRA images as read back (sRGB-encoded). Pure,
// for tests/acc_gates_test.cpp.
#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <vector>

namespace acc {

struct GateImage {  // BGRA, `width` pixels a row
    const std::uint8_t* px = nullptr;
    int width = 0, height = 0;
    const std::uint8_t* at(int x, int y) const { return px + (static_cast<std::size_t>(y) * width + x) * 4; }
};
struct GateRect {
    int x = 0, y = 0, w = 0, h = 0;
};

inline bool inside(const GateImage& img, const GateRect& r) {
    return r.w > 0 && r.h > 0 && r.x >= 0 && r.y >= 0 && r.x + r.w <= img.width && r.y + r.h <= img.height;
}

// sRGB-encoded 8-bit to linear.
inline double srgb_linear(std::uint8_t v) {
    static const std::array<double, 256> table = [] {
        std::array<double, 256> t{};
        for (int i = 0; i < 256; ++i) {
            const double c = i / 255.0;
            t[i] = c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4);
        }
        return t;
    }();
    return table[v];
}
// Linear luminance of a BGRA pixel.
inline double linear_luminance(const std::uint8_t* p) {
    return 0.0722 * srgb_linear(p[0]) + 0.7152 * srgb_linear(p[1]) + 0.2126 * srgb_linear(p[2]);
}
// Its sRGB-encoded luminance (0-255), for matching.
inline int encoded_luminance(const std::uint8_t* p) { return (18 * p[0] + 183 * p[1] + 54 * p[2] + 128) >> 8; }

// ---- Copies: view b against view a at the same size and camera ----
struct CopiesResult {
    bool valid = false;
    int dx = 0, dy = 0;  // b's best shift against a (0, 0: where it should be)
    std::uint64_t pixels = 0, differing = 0;
    std::uint64_t over1 = 0, over16 = 0;  // differing by more than one step (dither aside), and by more than 16
    // Content: 8x8-pixel block means (largest channel). The game's dithers follow each pixel's place in the target,
    // and views 2-3 sit elsewhere in it than their eyes, so single pixels never all match; an object, a shadow or a
    // shading change that differs survives the averaging.
    std::uint64_t blocks = 0, blocksOver8 = 0;
    double blockMax = 0;
    int maxDiff = 0;
    double meanDiff = 0;          // over the differing pixels, the largest channel's
    long long signedSum[3] = {};  // b - a summed, R G B: one sign everywhere is an exposure shift
    GateRect differingBox;        // the differing pixels' bounds, in a's own coordinates
    // b / a mean linear luminance by a's brightness (8 bands of its encoded luminance, 32 wide), and the pixels in each.
    double bandRatio[8] = {};
    std::uint64_t bandCount[8] = {};
    double share(std::uint64_t n) const { return pixels ? 100.0 * static_cast<double>(n) / static_cast<double>(pixels) : 0; }
    double percent() const { return share(differing); }
    // The largest brightness gain or loss over the bands holding at least 1% of the pixels (0: none).
    double worst_band() const {
        double worst = 0;
        for (int i = 0; i < 8; ++i)
            if (bandCount[i] * 100 >= pixels && std::fabs(bandRatio[i] - 1) > std::fabs(worst))
                worst = bandRatio[i] - 1;
        return worst;
    }
    double block_share() const {
        return blocks ? 100.0 * static_cast<double>(blocksOver8) / static_cast<double>(blocks) : 0;
    }
    // The gate's tolerance: in place, at most 0.1% of the 8x8 blocks off by more than 8 steps (content), and no
    // brightness band brighter or darker by more than 1% (exposure: ACC's were 6-11% before the share).
    bool within() const { return valid && dx == 0 && dy == 0 && block_share() <= 0.1 && std::fabs(worst_band()) <= 0.01; }
};

// b shifted by up to `search` pixels each way is matched first (the mean absolute difference on a sparse grid), so a
// view placed a pixel or two off says so instead of differing everywhere; then every pixel at that shift.
inline CopiesResult compare_copies(const GateImage& img, const GateRect& a, const GateRect& b, int search = 3) {
    CopiesResult r;
    if (!inside(img, a) || !inside(img, b) || a.w != b.w || a.h != b.h)
        return r;
    const int m = search;
    if (a.w <= 4 * m || a.h <= 4 * m)
        return r;
    double best = 1e300;
    for (int dy = -m; dy <= m; ++dy)
        for (int dx = -m; dx <= m; ++dx) {
            std::uint64_t sum = 0, n = 0;
            for (int y = m; y < a.h - m; y += 7)
                for (int x = m; x < a.w - m; x += 7) {
                    sum += static_cast<std::uint64_t>(std::abs(encoded_luminance(img.at(a.x + x, a.y + y)) -
                                                               encoded_luminance(img.at(b.x + x + dx, b.y + y + dy))));
                    ++n;
                }
            const double mean = n ? static_cast<double>(sum) / static_cast<double>(n) : 0;
            // Ties go to the smaller shift (0, 0 first among equals).
            if (mean < best - 1e-9 || (std::fabs(mean - best) <= 1e-9 && std::abs(dx) + std::abs(dy) < std::abs(r.dx) + std::abs(r.dy))) {
                best = mean;
                r.dx = dx;
                r.dy = dy;
            }
        }
    int x0 = a.w, y0 = a.h, x1 = -1, y1 = -1;
    double bandA[8] = {}, bandB[8] = {};
    std::uint64_t sumDiff = 0;
    for (int y = 0; y < a.h; ++y) {
        const int by = y + r.dy;
        if (by < 0 || by >= b.h)
            continue;
        for (int x = 0; x < a.w; ++x) {
            const int bx = x + r.dx;
            if (bx < 0 || bx >= b.w)
                continue;
            const std::uint8_t* pa = img.at(a.x + x, a.y + y);
            const std::uint8_t* pb = img.at(b.x + bx, b.y + by);
            int d = 0;
            for (int c = 0; c < 3; ++c) {
                const int s = static_cast<int>(pb[c]) - static_cast<int>(pa[c]);
                r.signedSum[2 - c] += s;  // BGRA in, R G B out
                d = (std::max)(d, std::abs(s));
            }
            ++r.pixels;
            const int band = (std::min)(7, encoded_luminance(pa) / 32);
            bandA[band] += linear_luminance(pa);
            bandB[band] += linear_luminance(pb);
            ++r.bandCount[band];
            if (!d)
                continue;
            ++r.differing;
            r.over1 += d > 1 ? 1 : 0;
            r.over16 += d > 16 ? 1 : 0;
            sumDiff += static_cast<std::uint64_t>(d);
            r.maxDiff = (std::max)(r.maxDiff, d);
            x0 = (std::min)(x0, x);
            y0 = (std::min)(y0, y);
            x1 = (std::max)(x1, x);
            y1 = (std::max)(y1, y);
        }
    }
    // 8x8 block means at the shift found.
    constexpr int kBlock = 8;
    for (int y0b = 0; y0b + kBlock <= a.h; y0b += kBlock)
        for (int x0b = 0; x0b + kBlock <= a.w; x0b += kBlock) {
            const int bx0 = x0b + r.dx, by0 = y0b + r.dy;
            if (bx0 < 0 || by0 < 0 || bx0 + kBlock > b.w || by0 + kBlock > b.h)
                continue;
            int sa[3] = {}, sb[3] = {};
            for (int y = 0; y < kBlock; ++y)
                for (int x = 0; x < kBlock; ++x) {
                    const std::uint8_t* pa = img.at(a.x + x0b + x, a.y + y0b + y);
                    const std::uint8_t* pb = img.at(b.x + bx0 + x, b.y + by0 + y);
                    for (int c = 0; c < 3; ++c) {
                        sa[c] += pa[c];
                        sb[c] += pb[c];
                    }
                }
            double d = 0;
            for (int c = 0; c < 3; ++c)
                d = (std::max)(d, std::fabs(static_cast<double>(sb[c] - sa[c])) / (kBlock * kBlock));
            ++r.blocks;
            r.blocksOver8 += d > 8 ? 1 : 0;
            r.blockMax = (std::max)(r.blockMax, d);
        }
    r.meanDiff = r.differing ? static_cast<double>(sumDiff) / static_cast<double>(r.differing) : 0;
    if (r.differing)
        r.differingBox = {x0, y0, x1 - x0 + 1, y1 - y0 + 1};
    for (int i = 0; i < 8; ++i)
        r.bandRatio[i] = bandA[i] > 0 ? bandB[i] / bandA[i] : 0;
    r.valid = true;
    return r;
}

// ---- Focus: a focus view against its box's crop of its eye ----
// `box`: the focus view's place in its eye's view (x0, y0, x1, y1 as fractions of the eye's rect, rows down), from the
// frame's record (an off-axis focus view: the same camera, so a rectangle). Every focus pixel is put into the eye pixel
// its centre falls in (the eye pixels the box covers whole), so the focus view at the eye's density sits on the eye's
// own grid. Brightness: linear luminance in blocks of 4 x 4 eye pixels, focus / eye, classed by the detail the focus
// view resolves inside each eye pixel (the spread of its pixels there, as a share of their mean). The focus view's
// denser pixels resolve fine texture and thin highlights the eye averages away, and the tonemapper's toe lifts such
// detail in dark areas (ACC's first focus gate, 2026-11-08: flat 0.999, finest detail 1.2-1.3; catalogue row 5), so
// the verdict reads the flat blocks: there both see the same thing.
struct FocusResult {
    bool valid = false;
    double overall = 0;                   // focus / eye mean linear luminance over the blocks compared
    double median = 0, p10 = 0, p90 = 0;  // over all blocks' focus / eye
    int blocks = 0;
    double flat = 0;  // median over the flat blocks (detail < 0.15)
    int flatBlocks = 0;
    double detailed = 0;  // median over the blocks with detail >= 0.3
    int detailedBlocks = 0;
    double edge = 0, middle = 0;  // flat blocks within a tenth of the box's border, and the rest
    double band[3] = {};          // flat blocks by the eye's brightness: dark (< 0.03 linear), mid, bright (> 0.25)
    int bandBlocks[3] = {};
    int dx = 0, dy = 0;             // the offset (eye pixels) at which the focus view, at the eye's density, best matches
    double sad = 0, sadAtZero = 0;  // mean absolute encoded-luminance difference there and at 0, 0
    int boxW = 0, boxH = 0;         // the eye pixels the box covers whole
    // Light/shadow flips: the largest region (eye pixels, after erosion) where the focus view is more than twice as dark,
    // or as bright, as its eye, both smooth there; and where (x0, y0, x1, y1 in the box's eye pixels).
    int darkerCore = 0, brighterCore = 0;
    int darkerAt[4] = {}, brighterAt[4] = {};
    static constexpr int kFlipLimit = 20;
    // The gate's tolerance: flat blocks 1.00 +- 0.02, their edge ring within 0.03 of the middle, aligned within an eye
    // pixel, enough flat blocks to tell, and no light/shadow flip of kFlipLimit eye pixels or more.
    bool within() const {
        return valid && flatBlocks >= 40 && std::fabs(flat - 1) <= 0.02 && std::fabs(edge - middle) <= 0.03 &&
               std::abs(dx) <= 1 && std::abs(dy) <= 1 && darkerCore < kFlipLimit && brighterCore < kFlipLimit;
    }
};

// The largest 8-connected region of `mask` (w x h) after a 3x3 erosion: its size and bounds (x0, y0, x1, y1).
inline int largest_core(const std::vector<std::uint8_t>& mask, int w, int h, int (&at)[4]) {
    std::vector<std::uint8_t> core(mask.size(), 0);
    for (int y = 1; y + 1 < h; ++y)
        for (int x = 1; x + 1 < w; ++x) {
            bool all = true;
            for (int dy = -1; dy <= 1 && all; ++dy)
                for (int dx = -1; dx <= 1 && all; ++dx)
                    all = mask[static_cast<std::size_t>(y + dy) * w + x + dx] != 0;
            core[static_cast<std::size_t>(y) * w + x] = all ? 1 : 0;
        }
    int best = 0;
    std::vector<int> stack;
    for (int start = 0; start < w * h; ++start) {
        if (!core[static_cast<std::size_t>(start)])
            continue;
        int size = 0, x0 = w, y0 = h, x1 = -1, y1 = -1;
        stack.assign(1, start);
        core[static_cast<std::size_t>(start)] = 0;
        while (!stack.empty()) {
            const int p = stack.back();
            stack.pop_back();
            const int x = p % w, y = p / w;
            ++size;
            x0 = (std::min)(x0, x);
            y0 = (std::min)(y0, y);
            x1 = (std::max)(x1, x);
            y1 = (std::max)(y1, y);
            for (int dy = -1; dy <= 1; ++dy)
                for (int dx = -1; dx <= 1; ++dx) {
                    const int xx = x + dx, yy = y + dy;
                    if (xx < 0 || yy < 0 || xx >= w || yy >= h)
                        continue;
                    const std::size_t q = static_cast<std::size_t>(yy) * w + xx;
                    if (core[q]) {
                        core[q] = 0;
                        stack.push_back(static_cast<int>(q));
                    }
                }
        }
        if (size > best) {
            best = size;
            at[0] = x0;
            at[1] = y0;
            at[2] = x1;
            at[3] = y1;
        }
    }
    return best;
}

inline double median_of(std::vector<double> v, double q = 0.5) {
    if (v.empty())
        return 0;
    std::sort(v.begin(), v.end());
    return v[static_cast<std::size_t>(q * static_cast<double>(v.size() - 1) + 0.5)];
}

inline FocusResult compare_focus(const GateImage& img, const GateRect& eye, const GateRect& focus, const double (&box)[4],
                                 int search = 4) {
    FocusResult r;
    if (!inside(img, eye) || !inside(img, focus) || !(box[2] > box[0]) || !(box[3] > box[1]))
        return r;
    const double ex0 = box[0] * eye.w, ey0 = box[1] * eye.h, ex1 = box[2] * eye.w, ey1 = box[3] * eye.h;
    const int ix0 = static_cast<int>(std::ceil(ex0)), iy0 = static_cast<int>(std::ceil(ey0));
    const int ix1 = static_cast<int>(std::floor(ex1)), iy1 = static_cast<int>(std::floor(ey1));
    const int W = ix1 - ix0, H = iy1 - iy0;
    r.boxW = W;
    r.boxH = H;
    if (W < 8 || H < 8 || ix0 < 0 || iy0 < 0 || ix1 > eye.w || iy1 > eye.h)
        return r;
    // The focus view on the eye's grid: per eye pixel the sum, sum of squares and count of the linear luminance of the
    // focus pixels whose centres fall in it, and the sum of their encoded luminance (for the alignment).
    const std::size_t n = static_cast<std::size_t>(W) * H;
    std::vector<double> sum(n, 0.0), sum2(n, 0.0), enc(n, 0.0);
    std::vector<int> count(n, 0);
    const double sx = (ex1 - ex0) / focus.w, sy = (ey1 - ey0) / focus.h;
    for (int y = 0; y < focus.h; ++y) {
        const int j = static_cast<int>(std::floor(ey0 + (y + 0.5) * sy)) - iy0;
        if (j < 0 || j >= H)
            continue;
        for (int x = 0; x < focus.w; ++x) {
            const int i = static_cast<int>(std::floor(ex0 + (x + 0.5) * sx)) - ix0;
            if (i < 0 || i >= W)
                continue;
            const std::uint8_t* p = img.at(focus.x + x, focus.y + y);
            const double l = linear_luminance(p);
            const std::size_t k = static_cast<std::size_t>(j) * W + i;
            sum[k] += l;
            sum2[k] += l * l;
            enc[k] += encoded_luminance(p);
            ++count[k];
        }
    }
    // Light/shadow flips, per eye pixel (FocusResult::darkerCore): the focus view more than twice as dark or as bright
    // as the eye, the brighter of the two lit; the focus pixels inside the eye pixel close to their own mean, and the eye
    // pixel close to its neighbours along at least one axis (a band or an area, not a texture's speckle).
    {
        std::vector<double> ev(n);
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x)
                ev[static_cast<std::size_t>(y) * W + x] = linear_luminance(img.at(eye.x + ix0 + x, eye.y + iy0 + y));
        std::vector<std::uint8_t> darker(n, 0), brighter(n, 0);
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x) {
                const std::size_t k = static_cast<std::size_t>(y) * W + x;
                if (!count[k])
                    continue;
                const double d = sum[k] / count[k], e = ev[k];
                if ((std::max)(d, e) <= 0.03)
                    continue;
                const double sdev = std::sqrt((std::max)(0.0, sum2[k] / count[k] - d * d));
                if (sdev / (std::max)(d, 0.01) >= 0.35)
                    continue;
                const auto at = [&](int xx, int yy) {
                    xx = std::clamp(xx, 0, W - 1);
                    yy = std::clamp(yy, 0, H - 1);
                    return ev[static_cast<std::size_t>(yy) * W + xx];
                };
                const double ref = (std::max)(e, 0.01);
                const double horiz = (std::max)(std::fabs(at(x - 1, y) - e), std::fabs(at(x + 1, y) - e)) / ref;
                const double vert = (std::max)(std::fabs(at(x, y - 1) - e), std::fabs(at(x, y + 1) - e)) / ref;
                if ((std::min)(horiz, vert) >= 0.25)
                    continue;
                darker[k] = d < 0.5 * e ? 1 : 0;
                brighter[k] = d > 2.0 * e ? 1 : 0;
            }
        r.darkerCore = largest_core(darker, W, H, r.darkerAt);
        r.brighterCore = largest_core(brighter, W, H, r.brighterAt);
    }
    // Blocks of 4 x 4 eye pixels.
    constexpr int kB = 4;
    std::vector<double> all, flat, detailed, edge, middle, bands[3];
    double sumE = 0, sumF = 0;
    for (int by = 0; by + kB <= H; by += kB)
        for (int bx = 0; bx + kB <= W; bx += kB) {
            double le = 0, lf = 0, spread = 0;
            int m = 0;
            for (int y = by; y < by + kB; ++y)
                for (int x = bx; x < bx + kB; ++x) {
                    const std::size_t k = static_cast<std::size_t>(y) * W + x;
                    if (!count[k])
                        continue;
                    const double mean = sum[k] / count[k];
                    const double var = (std::max)(0.0, sum2[k] / count[k] - mean * mean);
                    lf += mean;
                    le += linear_luminance(img.at(eye.x + ix0 + x, eye.y + iy0 + y));
                    spread += mean > 1e-4 ? std::sqrt(var) / mean : 0;
                    ++m;
                }
            if (m < kB * kB)
                continue;
            le /= m;
            lf /= m;
            spread /= m;
            sumE += le;
            sumF += lf;
            if (le <= 0.003)
                continue;
            const double ratio = lf / le;
            all.push_back(ratio);
            if (spread >= 0.3)
                detailed.push_back(ratio);
            if (spread >= 0.15)
                continue;
            flat.push_back(ratio);
            const bool ring = bx < W / 10 || by < H / 10 || bx + kB > W - W / 10 || by + kB > H - H / 10;
            (ring ? edge : middle).push_back(ratio);
            bands[le < 0.03 ? 0 : le > 0.25 ? 2 : 1].push_back(ratio);
        }
    r.blocks = static_cast<int>(all.size());
    r.overall = sumE > 0 ? sumF / sumE : 0;
    r.median = median_of(all);
    r.p10 = median_of(all, 0.1);
    r.p90 = median_of(all, 0.9);
    r.flat = median_of(flat);
    r.flatBlocks = static_cast<int>(flat.size());
    r.detailed = median_of(detailed);
    r.detailedBlocks = static_cast<int>(detailed.size());
    r.edge = median_of(edge);
    r.middle = median_of(middle);
    for (int b = 0; b < 3; ++b) {
        r.band[b] = median_of(bands[b]);
        r.bandBlocks[b] = static_cast<int>(bands[b].size());
    }
    // Alignment on the eye's grid: the focus view's mean encoded luminance per eye pixel against the eye's, shifted.
    if (W > 4 * search && H > 4 * search) {
        double best = 1e300;
        for (int dy = -search; dy <= search; ++dy)
            for (int dx = -search; dx <= search; ++dx) {
                double total = 0;
                std::uint64_t m = 0;
                for (int y = search; y < H - search; ++y)
                    for (int x = search; x < W - search; ++x) {
                        const std::size_t k = static_cast<std::size_t>(y) * W + x;
                        if (!count[k])
                            continue;
                        const int ex = ix0 + x + dx, ey = iy0 + y + dy;
                        if (ex < 0 || ey < 0 || ex >= eye.w || ey >= eye.h)
                            continue;
                        total += std::fabs(enc[k] / count[k] - encoded_luminance(img.at(eye.x + ex, eye.y + ey)));
                        ++m;
                    }
                const double mean = m ? total / static_cast<double>(m) : 1e300;
                if (dx == 0 && dy == 0)
                    r.sadAtZero = mean;
                if (mean < best - 1e-9 ||
                    (std::fabs(mean - best) <= 1e-9 && std::abs(dx) + std::abs(dy) < std::abs(r.dx) + std::abs(r.dy))) {
                    best = mean;
                    r.dx = dx;
                    r.dy = dy;
                }
            }
        r.sad = best;
    }
    r.valid = r.blocks > 0;
    return r;
}

// The box (x0, y0, x1, y1 as fractions of the eye's rect, rows down) of a focus view with tangents `focus` in an eye
// with tangents `eye` (both left, right, up, down; the same camera). False: an empty or inverted span.
inline bool box_in_eye(const float (&eye)[4], const float (&focus)[4], double (&box)[4]) {
    const double w = static_cast<double>(eye[1]) - eye[0], h = static_cast<double>(eye[2]) - eye[3];
    if (!(w > 0) || !(h > 0))
        return false;
    box[0] = (focus[0] - eye[0]) / w;
    box[2] = (focus[1] - eye[0]) / w;
    box[1] = (eye[2] - focus[2]) / h;
    box[3] = (eye[2] - focus[3]) / h;
    return box[2] > box[0] && box[3] > box[1];
}

}  // namespace acc
