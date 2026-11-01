# LauncherWindow

The complete launcher, composed from the kit: sidebar panes, grouped settings, live preview, render-target readouts and the action bar.

**Consumer provides:** optional `recommendedResolution` (the headset's per-eye `{width, height}` from the OpenXR runtime; the example is 2584 × 2160), `defaultSettings` (merged over `QVFR.defaults`), `defaultPane`, `eyeTrackingAvailable`, `onChange(settings)`, `onLaunch(settings)`, `width`, `height`, `controls`, `version`.

**Settings object**
`{method, mode, outer, inner, size, outline, outlineColor, blend, feather}`. `outer` and `inner` are percents; `size` is `{width, height}` in percent.

**Use**
- Outer view px = recommended × outer %. Focus view px = recommended × inner % × foveated size %. `QVFR.viewPixels(settings, recommended)` does both.
- Runtime resolution = recommended per eye, as the runtime reports it before the layer changes it. Pixels per frame = 2 × (outer area + focus area), shown against the runtime's 2 × recommended area. Pixels saved = 1 − pixels per frame / runtime pixels.
- Launch is the default button. Settings save as they change; resolution changes apply at next launch.
