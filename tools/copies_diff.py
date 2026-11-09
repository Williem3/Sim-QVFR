"""Where a copies gate's copy differs from its eye: two PNGs of one size (the gate's eye and copy images).

    python tools/copies_diff.py <eye.png> <copy.png> [--out <diff.png>] [--grid 8]

Prints the difference's histogram (largest channel, 0-255), the share of pixels above a few thresholds, the mean
difference per grid cell (where it sits), the signed copy - eye by channel, and whether the large differences cluster
(connected blobs) or are scattered (noise). Writes an amplified difference map (x8, large differences red) for looking at.
Run with plain `python` (Pillow and numpy from the user site-packages).
"""
import argparse
import sys

import numpy as np
from PIL import Image


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("eye")
    ap.add_argument("copy")
    ap.add_argument("--out")
    ap.add_argument("--grid", type=int, default=8)
    a = ap.parse_args()
    e = np.asarray(Image.open(a.eye).convert("RGB")).astype(np.int16)
    c = np.asarray(Image.open(a.copy).convert("RGB")).astype(np.int16)
    if e.shape != c.shape:
        sys.exit(f"sizes differ: {e.shape} vs {c.shape}")
    d = c - e
    m = np.abs(d).max(axis=2)
    h, w = m.shape
    n = m.size
    print(f"{a.copy} vs {a.eye}: {w}x{h}")
    for t in (1, 2, 4, 8, 16, 32, 64):
        print(f"  > {t:3d}: {100.0 * (m > t).sum() / n:7.3f}% of pixels")
    hist = np.bincount(m.ravel(), minlength=256)
    print("  histogram 0..8:", " ".join(f"{v}:{hist[v]}" for v in range(9)))
    print(f"  signed copy - eye, mean per pixel: R {d[..., 0].mean():+.4f} G {d[..., 1].mean():+.4f} B {d[..., 2].mean():+.4f}")
    g = a.grid
    print(f"  mean difference per cell ({g}x{g}, rows top to bottom):")
    for gy in range(g):
        row = []
        for gx in range(g):
            cell = m[gy * h // g:(gy + 1) * h // g, gx * w // g:(gx + 1) * w // g]
            row.append(f"{cell.mean():5.2f}")
        print("   ", " ".join(row))
    print(f"  share of pixels > 16 per cell (%):")
    for gy in range(g):
        row = []
        for gx in range(g):
            cell = m[gy * h // g:(gy + 1) * h // g, gx * w // g:(gx + 1) * w // g]
            row.append(f"{100.0 * (cell > 16).mean():5.1f}")
        print("   ", " ".join(row))
    # Large differences: clustered or scattered? Fraction of >16 pixels whose 3x3 neighbourhood has another >16 pixel.
    big = m > 16
    if big.any():
        p = np.pad(big, 1)
        neigh = sum(p[1 + dy:h + 1 + dy, 1 + dx:w + 1 + dx] for dy in (-1, 0, 1) for dx in (-1, 0, 1) if dy or dx)
        clustered = (big & (neigh > 0)).sum() / big.sum()
        ys, xs = np.nonzero(big)
        print(f"  > 16: {big.sum()} pixels, {100 * clustered:.1f}% with a > 16 neighbour; bounds x {xs.min()}-{xs.max()} "
              f"y {ys.min()}-{ys.max()}")
    # Brightness of the eye where the copy differs, against everywhere (noise in dark areas, or edges?)
    lum = e.mean(axis=2)
    print(f"  eye brightness: everywhere {lum.mean():.1f}; where > 1 {lum[m > 1].mean() if (m > 1).any() else 0:.1f}; "
          f"where > 16 {lum[m > 16].mean() if (m > 16).any() else 0:.1f}")
    # Edges: the eye's gradient where it differs against everywhere.
    gx = np.abs(np.diff(lum, axis=1, prepend=lum[:, :1]))
    gy = np.abs(np.diff(lum, axis=0, prepend=lum[:1, :]))
    grad = np.maximum(gx, gy)
    print(f"  eye gradient: everywhere {grad.mean():.2f}; where > 1 {grad[m > 1].mean() if (m > 1).any() else 0:.2f}; "
          f"where > 16 {grad[m > 16].mean() if (m > 16).any() else 0:.2f}")
    if a.out:
        vis = np.clip(m.astype(np.int32) * 8, 0, 255).astype(np.uint8)
        rgb = np.stack([vis, vis, vis], axis=2)
        rgb[m > 16] = (255, 0, 0)
        Image.fromarray(rgb).save(a.out)
        print(f"  map: {a.out}")


if __name__ == "__main__":
    main()
