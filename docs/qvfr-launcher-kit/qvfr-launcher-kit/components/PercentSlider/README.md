# PercentSlider

Horizontal slider with a live percent readout and an optional detail line, such as the resulting pixel size.

**Consumer provides:** `value` + `onChange(percent)` (or `defaultValue`), an accessible name via `aria-labelledby` (the row label's id) or `aria-label`, optional `min`, `max`, `step` (default 0 to 100, step 1), `detail` (text, or a function of the value), `width` (default 240px), `disabled`.

**Ranges in the launcher**
- Outer resolution: 20–100% of the headset's recommended per-eye resolution. Detail shows the outer view in px.
- Inner resolution: 50–200% pixel density inside the focus region. Detail shows the focus view in px.
- Foveated width and Foveated height: 10–100% of the outer view, one slider each.

**Use**
- Put it on the trailing side of a `FormRow`; the row label names the setting.
- The readout always shows the whole-number percent. The detail line updates as the knob moves.
- Keyboard: arrow keys move by `step`, Page Up/Down by 10%, Home/End to the ends (native range input).
- Screen readers hear the percent plus the detail text ("50 percent, 1292 × 1080 px").

**Don't**
- Don't link the outer and inner sliders. They are independent by design.
- Don't use it for values that aren't percentages; use a number field instead.
