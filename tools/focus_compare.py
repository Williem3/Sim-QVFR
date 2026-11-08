"""A focus view against its eye's crop: alignment, brightness and structure (the QVFR focus gate).

Usage: python focus_compare.py <eye.png> <focus.png> <x0> <y0> <x1> <y1> [--out <prefix>]
x0..y1: the focus box in the eye image as fractions of width and height (y down), as the layer logs them.
1. Alignment: the eye's crop, scaled to the focus image, is phase-correlated with the focus image at low resolution;
   the offset says how far the focus view sits from where the projection maths puts it (0 = right).
2. Brightness: linear luminance, box-filtered, focus / eye inside the box (its border left out): median and spread,
   by brightness band and over a 6 x 4 grid. A gain shows as a flat ratio; vignette or AO as a pattern.
3. Structure: the focus image brought down to the eye's density and back up, against the eye's crop: what one has and
   the other lacks (LOD, culling, missing objects) shows in the difference map, not fine detail.
Writes <prefix>-side.png (eye crop | focus) and <prefix>-diff.png (structure differences, brighter = larger).
"""
import argparse

import numpy as np
from PIL import Image


def linear(rgb):
    c = rgb / 255.0
    return np.where(c <= 0.04045, c / 12.92, ((c + 0.055) / 1.055) ** 2.4)


def luminance(img):
    lin = linear(np.asarray(img.convert("RGB")).astype(np.float64))
    return lin @ np.array([0.2126, 0.7152, 0.0722])


def box(a, r):
    p = np.pad(a, r + 1, mode="edge")
    c = p.cumsum(0).cumsum(1)
    n = 2 * r + 1
    s = c[n:, n:] - c[:-n, n:] - c[n:, :-n] + c[:-n, :-n]
    return s[: a.shape[0], : a.shape[1]] / (n * n)


def phase_offset(a, b):
    """Shift (dy, dx) that best aligns b to a (phase correlation), in pixels of these images."""
    fa, fb = np.fft.fft2(a - a.mean()), np.fft.fft2(b - b.mean())
    r = fa * np.conj(fb)
    r /= np.abs(r) + 1e-12
    c = np.fft.ifft2(r).real
    dy, dx = np.unravel_index(np.argmax(c), c.shape)
    h, w = a.shape
    return (dy - h if dy > h // 2 else dy), (dx - w if dx > w // 2 else dx), c.max()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("eye")
    ap.add_argument("focus")
    ap.add_argument("rect", nargs=4, type=float)
    ap.add_argument("--out", default=None)
    a = ap.parse_args()
    eye_img, focus_img = Image.open(a.eye), Image.open(a.focus)
    ew, eh = eye_img.size
    fw, fh = focus_img.size
    x0, y0, x1, y1 = a.rect
    crop_box = (x0 * ew, y0 * eh, x1 * ew, y1 * eh)
    crop = eye_img.convert("RGB").transform((fw, fh), Image.EXTENT, crop_box, Image.BILINEAR)
    print(f"eye {ew}x{eh}, focus {fw}x{fh}; box {crop_box[2] - crop_box[0]:.0f}x{crop_box[3] - crop_box[1]:.0f} eye pixels "
          f"-> density x{fw / (crop_box[2] - crop_box[0]):.2f}")

    # 1. Alignment at 1/8 size.
    small = (max(64, fw // 8), max(64, fh // 8))
    le_s = luminance(crop.resize(small, Image.BOX))
    lf_s = luminance(focus_img.resize(small, Image.BOX))
    dy, dx, peak = phase_offset(le_s, lf_s)
    sx, sy = fw / small[0], fh / small[1]
    print(f"alignment: focus vs where the maths puts it: dx {dx * sx:+.0f} dy {dy * sy:+.0f} focus pixels "
          f"({dx * sx / fw * 100:+.2f}% / {dy * sy / fh * 100:+.2f}% of the box; peak {peak:.3f})")

    # 2. Brightness.
    le, lf = luminance(crop), luminance(focus_img)
    r = max(4, fw // 128)
    be, bf = box(le, r), box(lf, r)
    m = np.zeros_like(be, bool)
    m[fh // 10: fh * 9 // 10, fw // 10: fw * 9 // 10] = True
    k = m & (be > 0.002)
    ratio = bf[k] / be[k]
    print(f"brightness focus / eye: median {np.median(ratio):.3f}, p10 {np.percentile(ratio, 10):.3f}, "
          f"p90 {np.percentile(ratio, 90):.3f}; summed over the box {bf[m].sum() / max(be[m].sum(), 1e-9):.3f}")
    for lo, hi in ((0.002, 0.01), (0.01, 0.03), (0.03, 0.1), (0.1, 0.3), (0.3, 10)):
        q = k & (be >= lo) & (be < hi)
        if q.sum() > 1000:
            print(f"   eye luminance {lo:.3f}-{hi:.3f}: {q.mean() * 100:5.1f}% of the box, ratio {np.median(bf[q] / be[q]):.3f}")
    print("   ratio over the box, 6 x 4 (rows top to bottom):")
    for gy in range(4):
        row = []
        for gx in range(6):
            ya, yb = fh // 10 + (fh * 8 // 10) * gy // 4, fh // 10 + (fh * 8 // 10) * (gy + 1) // 4
            xa, xb = fw // 10 + (fw * 8 // 10) * gx // 6, fw // 10 + (fw * 8 // 10) * (gx + 1) // 6
            c, e = bf[ya:yb, xa:xb], be[ya:yb, xa:xb]
            q = e > 0.002
            row.append(f"{np.median(c[q] / e[q]):.2f}" if q.sum() > 50 else "  - ")
        print("   " + " ".join(row))

    # 3. Structure at the eye's density.
    cw, ch = max(1, int(round(crop_box[2] - crop_box[0]))), max(1, int(round(crop_box[3] - crop_box[1])))
    focus_at_eye = focus_img.convert("RGB").resize((cw, ch), Image.BOX).resize((fw, fh), Image.BILINEAR)
    lfe = luminance(focus_at_eye)
    d = np.abs(box(lfe, 2) - box(le, 2)) / (box(le, 2) + box(lfe, 2) + 0.01)
    big = (d > 0.25) & m
    print(f"structure: {big.mean() * 100:.2f}% of the box differs by more than 25% at the eye's density "
          f"(objects, LOD or culling one view has and the other lacks)")
    if big.any():
        ys, xs = np.nonzero(big)
        cells = {}
        for y, x in zip(ys // (fh // 8), xs // (fw // 8)):
            cells[(y, x)] = cells.get((y, x), 0) + 1
        top = sorted(cells.items(), key=lambda kv: -kv[1])[:6]
        print("   most in cells (row, column of 8 x 8):", ", ".join(f"{c} {n}" for c, n in top))

    if a.out:
        side = Image.new("RGB", (fw * 2, fh))
        side.paste(crop, (0, 0))
        side.paste(focus_img.convert("RGB"), (fw, 0))
        side.thumbnail((2400, 2400))
        side.save(a.out + "-side.png")
        Image.fromarray((np.clip(d * 2, 0, 1) * 255).astype(np.uint8)).resize(
            (min(fw, 1600), int(fh * min(fw, 1600) / fw))).save(a.out + "-diff.png")
        print(f"wrote {a.out}-side.png and {a.out}-diff.png")


if __name__ == "__main__":
    main()
