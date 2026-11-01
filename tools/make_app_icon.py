"""Builds the launcher's Windows icon (src/launcher/res/app.ico) from assets/app-icon.png.

Usage: python tools/make_app_icon.py [source.png] [out.ico]
The artwork is cropped to its opaque edges (a square around them, so the icon fills its frame the
way Windows icons do), then resampled to every size Windows asks for, up to 256 px.
"""
import os
import sys

from PIL import Image

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(ROOT, 'assets', 'app-icon.png')
OUT = os.path.join(ROOT, 'src', 'launcher', 'res', 'app.ico')
SIZES = [16, 20, 24, 32, 40, 48, 64, 96, 128, 256]


def main(src=SRC, out=OUT):
    art = Image.open(src).convert('RGBA')
    left, top, right, bottom = art.getchannel('A').getbbox()
    side = max(right - left, bottom - top)
    cx, cy = (left + right) / 2, (top + bottom) / 2
    box = (round(cx - side / 2), round(cy - side / 2), round(cx + side / 2), round(cy + side / 2))
    square = Image.new('RGBA', (side, side), (0, 0, 0, 0))
    square.paste(art.crop(box), (0, 0))
    os.makedirs(os.path.dirname(out), exist_ok=True)
    square.save(out, format='ICO', sizes=[(s, s) for s in SIZES])  # Pillow resamples each size from the full art
    print('wrote', out, 'from', src, 'crop', box)


if __name__ == '__main__':
    main(*sys.argv[1:])
