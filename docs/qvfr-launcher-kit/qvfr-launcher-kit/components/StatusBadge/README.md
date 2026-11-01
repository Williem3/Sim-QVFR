# StatusBadge

Small pill that reports launcher state with a glyph, a word and a tone.

**Consumer provides:** `children` (short status text), `tone` (`success`, `warning`, `danger`, `neutral`), optional `live` to announce changes to screen readers.

**Use**
- Ready (success), Eye tracker not detected (warning), OpenXR runtime not found (danger), Starting Assetto Corsa EVO… (neutral).
- Lives at the leading end of the action bar. One badge at a time.

**Don't**
- Don't rely on color: every tone has its own glyph, and the text says what happened.
