# Sidebar

Pane navigation in the leading column of the launcher window.

**Consumer provides:** `items` (`{id, label, icon, attention?}[]`), `selected`, `onSelect(id)`, optional `heading`, `aria-label`.

**Icons:** `layers`, `display`, `eye`, `target`, `info`.

**Use**
- Panes: Rendering, Resolution, Foveation, Focus overlay, About. Order follows the setup flow.
- `attention` adds a warning glyph with an accessible name, e.g. when eye tracking is selected but no tracker is found.
- The selected row uses `sidebar-selection` with `on-accent` text.

**Don't**
- Don't put settings controls in the sidebar.
