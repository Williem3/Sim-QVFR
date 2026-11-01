# FoveationPreview

Live diagram of one eye's view: the outer view, the focus region, its pixel density, and the overlay options.

**Consumer provides:** `outer` and `inner` (`{width, height}` px, computed from the percent settings with `QVFR.viewPixels`), `focusSize` (`{width, height}` %), `method` (`qvfr` | `tfi`), `mode` (`eye` | `fixed`), `showOutline`, `outlineColor` (`teal` | `magenta` | `green`), `edgeBlend`, `feather`, optional `gaze` (`{x, y}` 0 to 1), `caption`.

**How it reads**
- Grid cell size shows pixel density. Focus density = (inner width ÷ focus share) ÷ outer width.
- Two-loop Foveated Interlaced adds interlace lines to the outer view.
- Eye tracked shows a gaze marker and offsets the focus region; Fixed centers it.

**Use**
- Keep it beside the form so every change is visible right away.
- It's a schematic. Never label it as a capture from the headset.
