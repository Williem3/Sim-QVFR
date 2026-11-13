> **Superseded.** The launcher now follows `docs/ui` (the "Bauhaus × Flat" overhaul: `docs/ui/OVERHAUL_PLAN.md`, tokens in `docs/ui/tokens.json`). This kit is kept as history; where its rules differ (system font, disable rather than hide, the action bar, Restore Defaults), `docs/ui` wins.

A Windows launcher that starts Assetto Corsa EVO with foveated rendering, built in the macOS design language: one calm settings window, grouped forms, standard controls, and a live preview of what each setting does.

## Principles

- **Deference.** The window is a quiet tool in front of a game. Use neutral surfaces (`window-bg`, `group-bg`) and spend color only on the default action, selection and state.
- **Clarity.** Every setting has a plain label, the right standard control, and a description when the effect isn't obvious. The preview shows the result before launch.
- **Settings apply as they change.** No Save or Cancel. A footer says when a change needs a relaunch: "Applies at next launch."
- **Keyboard first.** Everything is reachable with Tab and arrow keys. Return triggers Launch. Ctrl+, (⌘, on Mac) opens the window when the game is running.

## The settings

| Setting | Control | Default | Range | Pane |
| --- | --- | --- | --- | --- |
| Rendering method | `PopUpButton` | Quad View Foveated Rendering | Quad View Foveated Rendering, Two-loop Foveated Interlaced | Rendering |
| Foveated mode | `PopUpButton` | Eye tracked | Eye tracked, Fixed | Rendering |
| Outer resolution | `PercentSlider` | 50% | 20–100% of recommended | Resolution |
| Inner resolution | `PercentSlider` | 100% | 50–200% focus density | Resolution |
| Foveated width | `PercentSlider` | 39% | 10–100% of the outer view | Foveation |
| Foveated height | `PercentSlider` | 39% | 10–100% of the outer view | Foveation |
| Show focus outline | `Checkbox` | Off | | Focus overlay |
| Outline color | `SegmentedControl` | Teal | Teal, Magenta, Green | Focus overlay |
| Focus edge blend | `Checkbox` | On | | Focus overlay |
| Feather | `Checkbox` | Off | | Focus overlay |

All sizes are percentages. Pixel sizes are derived from the headset's recommended per-eye resolution, read from the OpenXR runtime:

- Outer view px = recommended × outer resolution %.
- Focus view px = recommended × inner resolution % × foveated size %.

With the example recommended resolution of 2584 × 2160, the defaults give a 1292 × 1080 outer view and a 1008 × 842 focus view: a 4600 × 1080 render target at 4.49 M pixels per frame. Your focus density 128% run is inner resolution 128%.

Rules:

- Outer and inner resolution are independent. Never link them.
- Every slider shows its percent; resolution sliders also show the resulting pixel size under the track.
- Outline color stays visible and is disabled while Show focus outline is off.
- If Eye tracked is selected and no tracker is found, keep the choice, render Fixed, and show a `warning` StatusBadge plus a sidebar attention glyph on Foveation.
- Recompute the readouts on every change: render target = 2 × (outer width + inner width) by the taller height; pixels per frame = 2 × (outer area + inner area).

## Content

- **Voice:** direct and calm, second person ("The focus region follows your gaze."). No exclamation marks, no emoji.
- **Capitalization:** title case for buttons, menu items and the window title (Launch Assetto Corsa EVO, Restore Defaults). Sentence case for row labels, checkboxes, section titles, descriptions and footers (Show focus outline, Per-eye resolution).
- **Names:** spell out the methods in the UI: Quad View Foveated Rendering, Two-loop Foveated Interlaced. QVFR and TFI are fine in logs and file names, not in labels.
- **Numbers:** dimensions use the multiplication sign with spaces, `1292 × 1080`. Units follow the value: `px`, `%`, `M` for megapixels. Use tabular figures (`value` style) wherever numbers line up.
- **Errors** say what to enter: "Enter a value from 256 to 8192." **Status** says what is true: "Eye tracker not detected."

## Color

- Text: `label` on any surface; `label-secondary` for descriptions, footers and units; `label-tertiary` only for disabled controls.
- Surfaces: `window-bg` for the content area, `sidebar-bg` for the sidebar, `group-bg` for section boxes, `titlebar-bg` for the title and action bars.
- Accent: `accent-fill` with `on-accent` for the Launch button, checked checkboxes and the selected sidebar row; `accent` for accent text and sidebar icons. Nothing else is blue.
- Status: `success`, `warning`, `danger` on their `-bg` tints, always with a glyph and a word.
- Overlay swatches: `overlay-teal`, `overlay-magenta`, `overlay-green` represent the in-headset outline colors in the UI. The launcher passes the color name to the runtime; the runtime owns the exact headset values.
- Preview: `preview-canvas`, `preview-periphery`, `preview-focus`, `preview-label` stay dark in both themes.
- Every text pair meets 4.5:1 and every control border, focus ring and glyph meets 3:1, in Light and Dark.

## Type

- System font everywhere (`--font-system`): SF Pro on macOS, Segoe UI Variable on Windows. No web fonts.
- `body` (13/16) is the default for labels and controls. `headline` for section titles, `title2` for the pane title, `callout` for descriptions, `subheadline` for footers, hints and badges, `footnote` only inside the preview.
- Readouts use `value` in `--font-mono` with tabular figures.

## Layout

- Window 960 × 600 by default, minimum 820 × 540, corners `radius-window`, the only `shadow-window`.
- Three columns: `sidebar-width` (188px) sidebar, flexible form column, `preview-width` (300px) preview column. Below 900px wide, drop the preview column under the form.
- Content margins `space-5`, section gap `space-6`, row padding `space-2` × `space-3`, rows at least `row-min-height`.
- Regular controls are `control-height` (22px). Only Launch is `control-height-large`.
- The action bar is pinned to the bottom: StatusBadge on the leading side, Restore Defaults and Launch on the trailing side.
- Radii: `radius-check` checkboxes, `radius-control` controls and sidebar rows, `radius-group` sections and the preview, `radius-full` badges and swatches.

## States

- Focus: a solid 3px `focus-ring` outline, offset 1px, on every interactive element.
- Disabled: `label-tertiary` text, `separator` border, no shadow. Disable rather than hide.
- Pressed: brightness 92%. No hover color changes, following macOS.
- Sliders: `accent-fill` for the filled part of the track, `segment-track` for the rest, a `control-bg` knob with a `control-border` edge. Disabled sliders fill with `label-tertiary`.

## Iconography

- Sidebar glyphs are simple 16px outline icons with a 1.5px stroke and round caps, drawn for this kit in the spirit of SF Symbols: `layers` (Rendering), `display` (Resolution), `eye` (Foveation), `target` (Focus overlay), `info` (About). They tint with `accent`, or `on-accent` when selected.
- Substitution note: SF Symbols can't ship in a Windows app, so these glyphs stand in for them. Replace them with your own icon set if you have one; keep the 16px grid and stroke weight.
- No emoji. No product or headset logos.

## Platform

- The game runs on Windows, so set `WindowChrome` `controls="windows"` in shipping builds. Everything else stays the same on every platform.
- Opaque `sidebar-bg` stands in for macOS vibrancy, which Windows can't reproduce reliably.
