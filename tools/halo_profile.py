"""A light's halo (bloom/glare) in a focus view against its eye's, at the same angles: does it stop at the box edge?

Usage:
    python halo_profile.py --atlas <atlas.png> --eye 0 --box x0 y0 x1 y1 [--light x y] [--out <prefix>]
    python halo_profile.py --eye-image <eye.png> --focus-image <focus.png> --box x0 y0 x1 y1 [...]
--atlas: LMU QVFR's four views as the layer saves them at the frame capture hotkey (lmu-qvfr-frame*-atlas.png: eyes
in the top row, eye e at x = e W, its focus view below). --box: where the focus view sits in its eye's image, as
fractions of width and height (y down), as the log line of that capture gives it. --light: the light's centre in the
eye image (pixels); by default the brightest spot of the focus view (after a small blur).

Both images go to eye pixels around the light: the eye's own pixels inside the box (what the periphery would show
there) and the focus view averaged down to the eye's density. Per ring of radius r (eye pixels) it prints the mean
linear luminance and yellowness ((R + G) / 2 - B, linear) of each, and their ratio focus / eye. A glare that YEBIS sizes
in each view's own pixels shows as a focus/eye ratio well under 1 between the light's core and the box edge (the focus
view's halo is a box-share of the eye's in angle). With the glare shared, the ratio is ~1 at every radius.
Summary lines: the halo's reach in each (radius where the excess over the ring at the box edge falls to 50% and 10% of
its value at the core's edge, eye pixels), and the median ratio over the halo rings.
--out writes <prefix>-halo.png: the eye's crop of the box | the focus view at the eye's density, the light marked.
"""
import argparse

import numpy as np
from PIL import Image, ImageDraw


def linear(rgb8):
    c = rgb8.astype(np.float64) / 255.0
    return np.where(c <= 0.04045, c / 12.92, ((c + 0.055) / 1.055) ** 2.4)


def measures(lin):
    lum = lin @ np.array([0.2126, 0.7152, 0.0722])
    yellow = (lin[..., 0] + lin[..., 1]) / 2 - lin[..., 2]
    return lum, yellow


def blur(a, r):
    if r <= 0:
        return a
    p = np.pad(a, r + 1, mode="edge")
    c = p.cumsum(0).cumsum(1)
    n = 2 * r + 1
    s = c[n:, n:] - c[:-n, n:] - c[n:, :-n] + c[:-n, :-n]
    return s[: a.shape[0], : a.shape[1]] / (n * n)


def rings(values, radius, step, count):
    """Mean of `values` per ring [k step, (k + 1) step) of `radius`; NaN where a ring has no pixels."""
    k = np.floor(radius / step).astype(int).ravel()
    v = values.ravel()
    ok = (k >= 0) & (k < count)
    sums = np.bincount(k[ok], weights=v[ok], minlength=count)
    n = np.bincount(k[ok], minlength=count)
    with np.errstate(invalid="ignore", divide="ignore"):
        return np.where(n > 0, sums / np.maximum(n, 1), np.nan), n


def reach(profile, step, start, stop):
    """Radius (eye pixels) where the excess over the profile's floor in rings [start, stop] falls to 50% and 10% of
    its value at ring `start`."""
    floor = np.nanmin(profile[start:stop + 1])
    base = profile[start] - floor
    out = []
    for frac in (0.5, 0.1):
        r = None
        for k in range(start, stop + 1):
            if np.isfinite(profile[k]) and profile[k] - floor <= frac * base:
                r = int(k * step)
                break
        out.append(r)
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--atlas")
    ap.add_argument("--eye", type=int, default=0)
    ap.add_argument("--eye-image")
    ap.add_argument("--focus-image")
    ap.add_argument("--box", type=float, nargs=4, required=True, metavar=("X0", "Y0", "X1", "Y1"))
    ap.add_argument("--light", type=float, nargs=2, metavar=("X", "Y"))
    ap.add_argument("--step", type=float, default=4, help="ring width, eye pixels")
    ap.add_argument("--out")
    a = ap.parse_args()

    if a.atlas:
        atlas = np.asarray(Image.open(a.atlas).convert("RGB"))
        h, w = atlas.shape[0] // 2, atlas.shape[1] // 2
        eye8 = atlas[:h, a.eye * w:(a.eye + 1) * w]
        focus8 = atlas[h:, a.eye * w:(a.eye + 1) * w]
    else:
        eye8 = np.asarray(Image.open(a.eye_image).convert("RGB"))
        focus8 = np.asarray(Image.open(a.focus_image).convert("RGB"))
    eh, ew = eye8.shape[:2]
    fh, fw = focus8.shape[:2]
    x0, y0, x1, y1 = a.box
    bx0, by0, bw, bh = x0 * ew, y0 * eh, (x1 - x0) * ew, (y1 - y0) * eh
    sx, sy = fw / bw, fh / bh  # focus pixels per eye pixel, per axis
    print(f"eye {ew}x{eh}, focus {fw}x{fh}; box {bw:.1f}x{bh:.1f} eye pixels at {bx0:.1f},{by0:.1f}; "
          f"focus magnification x {sx:.3f}, y {sy:.3f}")

    # The box's eye pixels, and the focus view averaged down to them (box filter: every focus pixel counts once).
    ix0, iy0 = int(round(bx0)), int(round(by0))
    iw, ih = int(round(bw)), int(round(bh))
    eye_box8 = eye8[iy0:iy0 + ih, ix0:ix0 + iw]
    focus_e8 = np.asarray(Image.fromarray(focus8).resize((eye_box8.shape[1], eye_box8.shape[0]), Image.BOX))
    eye_lum, eye_yel = measures(linear(eye_box8))
    foc_lum, foc_yel = measures(linear(focus_e8))

    if a.light:
        lx, ly = a.light[0] - ix0, a.light[1] - iy0
    else:
        lum_full = measures(linear(focus8))[0]
        b = blur(lum_full, max(1, int(fw / 300)))
        fy_, fx_ = np.unravel_index(np.argmax(b), b.shape)
        lx, ly = (fx_ + 0.5) / sx, (fy_ + 0.5) / sy
    print(f"light at {lx + ix0:.1f},{ly + iy0:.1f} in the eye image ({lx:.1f},{ly:.1f} in the box)")

    yy, xx = np.mgrid[0:eye_box8.shape[0], 0:eye_box8.shape[1]]
    radius = np.hypot(xx + 0.5 - lx, yy + 0.5 - ly)
    # Rings up to the farthest box corner; only whole box pixels (both images cover exactly these).
    count = int(np.ceil(radius.max() / a.step))
    e_l, n = rings(eye_lum, radius, a.step, count)
    f_l, _ = rings(foc_lum, radius, a.step, count)
    e_y, _ = rings(eye_yel, radius, a.step, count)
    f_y, _ = rings(foc_yel, radius, a.step, count)

    # The core: rings where the eye's luminance is still above half its peak ring (the light itself, sharper in the
    # focus view by design); the halo starts after it. The edge: the box's nearest edge from the light.
    peak = np.nanmax(e_l[:3])
    core = 0
    while core + 1 < count and np.isfinite(e_l[core]) and e_l[core] > 0.5 * peak:
        core += 1
    # The halo rings: from the core out while the box still holds at least a quarter of each ring.
    edge_r = min(lx, ly, eye_box8.shape[1] - lx, eye_box8.shape[0] - ly)
    edge_k = core + 1
    while edge_k + 1 < count and n[edge_k + 1] >= 0.25 * 2 * np.pi * (edge_k + 1.5) * a.step * a.step:
        edge_k += 1
    edge_k = min(edge_k, count - 1)
    print(f"core: {core * a.step:.0f} eye px; nearest box edge {edge_r:.0f} eye px from the light; halo rings to "
          f"{edge_k * a.step:.0f} eye px (a quarter of each ring or more inside the box)\n")
    print("  r(eye px)  pixels   eye lum   focus lum  ratio    eye yel   focus yel  ratio")
    for k in range(count):
        if n[k] == 0:
            continue
        if k > edge_k * 2 and k % 4:
            continue
        rl = f_l[k] / e_l[k] if e_l[k] > 0 else np.nan
        ry = f_y[k] / e_y[k] if e_y[k] > 1e-6 else np.nan
        print(f"  {k * a.step:8.0f}  {n[k]:6d}  {e_l[k]:9.5f}  {f_l[k]:9.5f}  {rl:6.3f}  {e_y[k]:9.5f}  {f_y[k]:9.5f}  {ry:6.3f}")

    halo = slice(core, edge_k + 1)
    with np.errstate(invalid="ignore", divide="ignore"):
        ratio_l = np.nanmedian((f_l / e_l)[halo])
        ratio_y = np.nanmedian((f_y / np.where(e_y > 1e-6, e_y, np.nan))[halo])
    print()
    for name, e, f in (("luminance", e_l, f_l), ("yellowness", e_y, f_y)):
        re, rf = reach(e, a.step, core, edge_k), reach(f, a.step, core, edge_k)
        print(f"halo reach ({name}, eye px from the light where the excess over the halo rings' floor is 50% / 10% "
              f"of its value at the core's edge): eye {re[0]} / {re[1]}, focus {rf[0]} / {rf[1]}")
    print(f"halo rings {core * a.step:.0f}-{edge_k * a.step:.0f} eye px: median focus/eye luminance {ratio_l:.3f}, "
          f"yellowness {ratio_y:.3f} (1 = the same halo; a box-sized glare is well under 1)")

    if a.out:
        scale = max(1, int(round(600 / max(eye_box8.shape))))
        left = Image.fromarray(eye_box8).resize((eye_box8.shape[1] * scale, eye_box8.shape[0] * scale), Image.NEAREST)
        right = Image.fromarray(focus_e8).resize(left.size, Image.NEAREST)
        side = Image.new("RGB", (left.width * 2 + 8, left.height))
        side.paste(left, (0, 0))
        side.paste(right, (left.width + 8, 0))
        d = ImageDraw.Draw(side)
        for ox in (0, left.width + 8):
            cx, cy = ox + lx * scale, ly * scale
            for r in (core * a.step, edge_r):
                d.ellipse([cx - r * scale, cy - r * scale, cx + r * scale, cy + r * scale], outline=(0, 255, 255))
        side.save(a.out + "-halo.png")
        print(f"wrote {a.out}-halo.png")


if __name__ == "__main__":
    main()
