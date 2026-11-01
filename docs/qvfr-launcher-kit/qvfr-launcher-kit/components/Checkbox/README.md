# Checkbox

On/off setting with a sentence-case label and an optional one-line description.

**Consumer provides:** `label`, `checked` + `onChange(checked)` (or `defaultChecked`), optional `description`, `disabled`.

**Use**
- Show focus outline, Focus edge blend and Feather are each a Checkbox inside a full-width `FormRow`.
- The description says what turning it on does, in one plain sentence ending with a period.
- A control that depends on a checkbox (Outline color) goes in the next row and is disabled while the box is off. Don't hide it.

**Don't**
- Don't phrase labels as negatives ("Disable feather").
- Don't use a checkbox to pick between two modes; use `PopUpButton`.
