# Button

Push button for commands; the large primary variant is reserved for **Launch Assetto Corsa EVO**.

**Consumer provides:** `children` (the command name), `onClick`, optional `variant` (`secondary` default, `primary`, `destructive`, `plain`), `size` (`regular` | `large`), `disabled`.

**Use**
- Exactly one `primary` button per window: the default action, bound to Return.
- Name buttons with a verb in title case: Launch Assetto Corsa EVO, Restore Defaults, Open Log Folder.
- `plain` for low-weight links inside panes (open a folder, view the log).

**Don't**
- Don't use `size="large"` for anything except the launch action.
- Don't put an ellipsis on a button unless it opens another window or sheet.
- Don't color a secondary button; `destructive` only for actions that discard data.
