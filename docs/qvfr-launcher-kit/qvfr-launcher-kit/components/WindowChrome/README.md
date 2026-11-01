# WindowChrome

The launcher window frame: title bar with window controls and a body that fills the rest.

**Consumer provides:** `title`, `children`, optional `width`, `height`, `toolbar`, `controls` (`mac` default, or `windows`).

**Use**
- Assetto Corsa EVO runs on Windows. Ship `controls="windows"` in the Windows build so caption buttons sit where users expect; keep the macOS look everywhere else.
- Default window size 960 × 600; minimum 820 × 540.

**Don't**
- Don't add a second title or logo inside the body; the title bar carries the app name.
