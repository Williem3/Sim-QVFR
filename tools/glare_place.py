"""Where a view's glare is in YEBIS's glare texture: checks the region LMU QVFR's glare share takes as the view's.

Usage:
    python glare_place.py --image <view.png> --glare <glare.dds> --region W H [--step 0.02]
--image: the view as LMU QVFR saves it at the frame capture hotkey (lmu-qvfr-frame*-eye0.png, -focus0.png ...).
--glare: that pass's glare texture from the same capture (lmu-qvfr-frame*-glare0.dds ...: eye passes YEBIS's own, focus
passes after the share, or their own with the share off). --region: the top-left W x H texels the share takes as the
view's (the capture's log line gives the whole command).

YEBIS's tonemap adds the glare to the scene, so around a light the view's brightness follows the glare. For every
top-left region of the texture (per axis in --step shares, then finer around the best) the region's luminance is
stretched over the view and correlated with the view's (both blurred to the glare's scale, log luminance): the best
region is where the view's glare is. Printed: the correlation for the region used, the best region, and the whole
texture (what a pass at YEBIS scale 1 would take with no regard to its own size), and how far the best is from the one
used, in view pixels at the view's far edge. A night scene with lights is needed: a glare with no structure can't be
placed.
"""
import argparse
import os
import sys

import numpy as np
from PIL import Image

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from dds_read import read_dds  # noqa: E402

LUM = np.array([0.2126, 0.7152, 0.0722])


def linear(rgb8):
    c = rgb8.astype(np.float64) / 255.0
    return np.where(c <= 0.04045, c / 12.92, ((c + 0.055) / 1.055) ** 2.4)


def resize(a, w, h, box=False):
    return np.asarray(
        Image.fromarray(a.astype(np.float32), mode="F").resize((w, h), Image.BOX if box else Image.BILINEAR),
        dtype=np.float64)


def blur(a, r):
    if r <= 0:
        return a
    p = np.pad(a, r + 1, mode="edge")
    c = p.cumsum(0).cumsum(1)
    n = 2 * r + 1
    s = c[n:, n:] - c[:-n, n:] - c[n:, :-n] + c[:-n, :-n]
    return s[: a.shape[0], : a.shape[1]] / (n * n)


def prepare(a, floor):
    a = np.log(np.maximum(a, 0) + floor)
    return (a - a.mean()) / (a.std() + 1e-12)


def correlation(view, glare, rw, rh, w, h, floor):
    g = prepare(blur(resize(glare[:rh, :rw], w, h), 1), floor)
    return float((g * view).mean())


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--image", required=True)
    ap.add_argument("--glare", required=True)
    ap.add_argument("--region", type=int, nargs=2, required=True, metavar=("W", "H"))
    ap.add_argument("--step", type=float, default=0.02, help="coarse search step, share of the texture per axis")
    a = ap.parse_args()

    img = np.asarray(Image.open(a.image).convert("RGB"))
    view_lum = linear(img) @ LUM
    tex, fmt = read_dds(a.glare)
    glare = tex[..., :3].astype(np.float64) @ LUM
    glare = np.where(np.isfinite(glare), glare, 0)
    gh, gw = glare.shape
    rw, rh = min(a.region[0], gw), min(a.region[1], gh)
    vh, vw = view_lum.shape
    # The view brought to about the glare's density over the region used (YEBIS's glare is 0.4 of a view's pixels).
    scale = max(rw / vw, rh / vh, 0.05)
    w, h = max(16, int(round(vw * scale))), max(8, int(round(vh * scale)))
    view_floor = max(float(np.percentile(view_lum, 50)), 1e-4)
    view = prepare(blur(resize(view_lum, w, h, box=True), 1), view_floor)
    region = glare[:rh, :rw]
    if region.max() <= 0 or np.percentile(region, 99) <= 1e-6:
        print(f"{a.glare}: the region used has no glare to place (max {region.max():.3g}); nothing measured")
        return
    floor = max(float(np.percentile(glare, 50)), 1e-6)

    print(f"view {vw}x{vh} ({a.image})")
    print(f"glare {gw}x{gh} format {fmt}, region used {rw}x{rh} ({rw / gw:.3f} x {rh / gh:.3f} of the texture)")
    used = correlation(view, glare, rw, rh, w, h, floor)
    whole = correlation(view, glare, gw, gh, w, h, floor)

    # Coarse grid over top-left regions, then a finer one around its best.
    def search(xs, ys):
        best = (-2.0, 0, 0)
        for x in xs:
            for y in ys:
                c = correlation(view, glare, x, y, w, h, floor)
                if c > best[0]:
                    best = (c, x, y)
        return best

    shares = np.arange(0.2, 1.0 + 1e-9, a.step)
    xs = sorted({max(8, min(gw, int(round(s * gw)))) for s in shares} | {rw})
    ys = sorted({max(8, min(gh, int(round(s * gh)))) for s in shares} | {rh})
    c, bx, by = search(xs, ys)
    dx, dy = max(2, int(a.step * gw)), max(2, int(a.step * gh))
    fine_x = range(max(8, bx - dx), min(gw, bx + dx) + 1, max(1, dx // 8))
    fine_y = range(max(8, by - dy), min(gh, by + dy) + 1, max(1, dy // 8))
    c, bx, by = max((c, bx, by), search(fine_x, fine_y))

    print(f"correlation (view vs glare region stretched over it, log luminance): region used {used:.3f}; "
          f"whole texture {whole:.3f}; best {bx}x{by} ({bx / gw:.3f} x {by / gh:.3f} of the texture) {c:.3f}")
    off_x = (bx / rw - 1) * vw
    off_y = (by / rh - 1) * vh
    print(f"best vs used: {bx - rw:+d} x {by - rh:+d} texels; a light at the view's far edge would sit "
          f"{off_x:+.1f} x {off_y:+.1f} view px off with the region used")
    tol_x, tol_y = max(2, round(0.01 * gw)), max(2, round(0.01 * gh))
    if abs(bx - rw) <= tol_x and abs(by - rh) <= tol_y and used >= c - 0.02:
        print("VERDICT: the view's glare is in the region used")
    else:
        print(f"VERDICT: the view's glare is NOT where the region used says: best {bx}x{by}, used {rw}x{rh}")


if __name__ == "__main__":
    main()
