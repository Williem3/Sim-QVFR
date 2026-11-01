# SegmentedControl

Choose one of two to five visible options; used for the focus outline color.

**Consumer provides:** `options` (`{value, label, swatch?}[]`, `swatch` a CSS color such as `var(--overlay-teal)`), `value` + `onChange(value)` (or `defaultValue`), `aria-label`, optional `disabled`.

**Use**
- Outline color: Teal, Magenta, Green, each with its swatch. The word is always shown; the swatch never stands alone.
- Disabled while Show focus outline is off.
- Keyboard: Tab moves to the selected segment; arrow keys move the selection.

**Don't**
- Don't use it for actions; segments select, they don't run commands.
