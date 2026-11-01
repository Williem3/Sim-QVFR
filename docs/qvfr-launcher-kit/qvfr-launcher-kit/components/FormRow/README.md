# FormRow

One labeled setting inside a `FormSection`: label and description on the leading side, the control on the trailing side.

**Consumer provides:** `children` (the control), `label`, optional `description`, `labelId` (pass the same id to the control's `aria-labelledby`), `disabled`. Omit `label` for a full-width row (used for checkboxes).

**Use**
- Label in sentence case without a colon: Rendering method, Outer resolution.
- `disabled` greys the label and description; disable the control too.
