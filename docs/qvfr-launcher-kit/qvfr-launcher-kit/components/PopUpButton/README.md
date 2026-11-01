# PopUpButton

Pick one option from a short list; used for Rendering method and Foveated mode.

**Consumer provides:** `options` (`{value, label, disabled?}[]`), `value` + `onChange(value)` (or `defaultValue`), an accessible name via `aria-labelledby` (the row label's id) or `aria-label`, optional `width`, `disabled`.

**Use**
- Up to about seven options. Option labels are the user-facing names: Quad View Foveated Rendering, Two-loop Foveated Interlaced, Eye tracked, Fixed.
- Put it in the trailing side of a `FormRow`; the row label names the setting.
- Default `min-width` is 180px; give short lists `width={140}`.

**Don't**
- Don't use it for three visible choices that benefit from comparison side by side (outline color uses `SegmentedControl`).
