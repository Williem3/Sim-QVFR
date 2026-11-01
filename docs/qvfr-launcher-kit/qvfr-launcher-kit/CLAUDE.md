# QVFR Launcher: build guide for Claude Code

This folder is the UI/UX kit for a Windows launcher that starts Assetto Corsa EVO with foveated rendering. Build the real app from it.

## Read first

1. `README.md`: design rules, the settings table, content and layout rules. This is the spec.
2. `tokens.json`: every color (light and dark), type style, spacing, radius and size. `tokens.css` is the same values as CSS variables.
3. `components/<Name>/README.md`: what each component does and what it needs.
4. `components/index.d.ts`: props and the `LauncherSettings` shape.
5. `preview.html`: open it in a browser to see the target. It runs the reference React components in `components/bundle.js`.

## Rules

- Match the reference visually: sizes, spacing, colors and copy come from the tokens and READMEs. Don't invent new values.
- Use tokens by name. If the stack can't read CSS variables, generate its theme from `tokens.json` instead of copying hex values by hand.
- Support light and dark, following the Windows app theme.
- Keep every label and message exactly as written in the READMEs (title case for buttons, sentence case for labels).
- Outer and inner resolution are independent. Never link them.
- Every size setting is a slider with a percent readout; resolution sliders also show the resulting px under the track.
- Outline color stays visible but disabled while Show focus outline is off.
- Settings save as they change (no Save button). Resolution changes apply at next launch.
- Ship Windows caption buttons (`WindowChrome controls="windows"`).
- Keyboard: Tab order follows the layout, arrow keys work in sliders and the segmented control, Return triggers Launch.

## Settings model

All sizes are percentages. `outer` and `inner` are single sliders; foveated size has one slider for width and one for height.

```json
{
  "method": "qvfr",
  "mode": "eye",
  "outer": 50,
  "inner": 100,
  "size": { "width": 39, "height": 39 },
  "outline": false,
  "outlineColor": "teal",
  "blend": true,
  "feather": false
}
```

Ranges: outer 20–100, inner 50–200, size 10–100 (each axis).

Pixel sizes come from the headset's recommended per-eye resolution (OpenXR `recommendedImageRectWidth/Height`):

- Outer view px = recommended × outer %.
- Focus view px = recommended × inner % × size %.
- Runtime resolution = recommended per eye, as the runtime reports it before the layer changes it (any OpenXR runtime).
- Pixels per frame = 2 × (outer area + focus area), shown against the runtime's 2 × recommended area. Pixels saved = 1 − pixels per frame / runtime pixels.

`components/bundle.js` exports `QVFR.viewPixels(settings, recommended)` and `QVFR.ranges` as the reference implementation.

## To decide with the user before coding

- Stack (for example WPF/WinUI 3 in C#, or Tauri/Electron reusing `components/bundle.js`).
- Where settings are stored and how the launcher hands them to the QVFR / foveated rendering runtime (config file path, format, key names).
- How the game is started (Steam URI, executable path, launch arguments).
- How to detect whether an eye tracker is present.
