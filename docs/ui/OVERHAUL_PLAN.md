# Launcher UI overhaul plan

How the 13 current pages map onto the new "Hub + game rail" layout. The visual target is the "QVFR Launcher Redesign" boards (Overview, light and dark). The rules are in the overhaul brief. Every control referred to here is listed in `SETTINGS_INVENTORY.md`.

Constraints (brief §0):
- The presentation changes; config keys, file formats, profiles, the layer, injection and launch logic do not.
- Hidden values are kept.
- One commit per phase, each phase built and checked before the next.

## 1. Where each page goes

| Current page | New tab › card | Notes |
|---|---|---|
| Game tab strip | Rail | One block per shown game, built from `games::all_games()`. The selected game is the yellow block with a notch. |
| Sidebar (13 rows) | Top bar tabs | Overview, Picture, Headset, Game. |
| Action bar: status | Top bar › status | Same states and order (inventory §20). Click goes where it does today. |
| Action bar: Restore Defaults (+ Undo) | Each card's ⋯ › Reset this card; Profile menu › Restore defaults for <game>… | See open question 7. |
| Action bar: Launch | Top bar › Launch (red, play triangle, names the game) | Always visible. |
| Launch checks (landing today) | Game › Launch checks; the top bar status opens it; Overview shows an attention banner | Overview becomes the landing view (open question 4). |
| Profiles | Overview › Profile segmented control; Top bar › Profile menu (+ Manage profiles… sheet) | |
| Resolution | Picture › Resolution; Picture › Rendering (runtime composition); Picture › View & helmet (crop, helmet) | |
| Foveation | Picture › Foveation; Picture › Rendering (method, Two-loop only) | |
| Focus overlay | Picture › Foveation (merged) | The page goes. |
| Preview column | Picture › Preview | A card of its own. |
| DLSS (Cheeky: ACE, ACC) | Picture › Rendering (on/off); Picture › Foveated DLSS; Picture › Frame generation (ACE) | Cards appear when switched on. |
| DLSS (own: LMU) | Picture › Rendering (on/off); Picture › DLSS; Picture › DLSS-NR | Cards appear when switched on. |
| Headset menu | Headset › Settings menu; Headset › Performance overlay; Headset › Placement | |
| Hotkeys | Headset › Hotkeys (compact table) | |
| Games | Game › Games in the rail; Game › <Game> options (anti-cheat) | Colour wheel moves into a "Custom…" popover. |
| OpenXR layers | Game › OpenXR layers | |
| Diagnostics | Game › Diagnostics; Game › Get help (copy, logs); Overview › Last session | |
| Report an issue | Report sheet, opened from Game › Get help and from the launch status link | Open question 6. |
| About | Rail bottom (version, Ko-fi); About sheet (updates, notices) | Open question 5. |

## 2. The new layout

### Rail (left, about 120 px; abbreviations only under 1024 px)

- Logo ■●▲ at the top.
- One block per shown game: abbreviation (Jost 900) and full name (Inter, rail-muted token). The selected game is a yellow block with ink text and a notch pointing at the content. Click, Up/Down and Ctrl+Tab switch games, as today (`select_game` swaps the per-game settings, unchanged).
- Bottom: a red-circle heart with "Support on Ko-fi" (opens the Ko-fi page), then the version with "BETA". The version opens the About sheet and carries a dot when an update is available.

### Top bar

`[□ OVERVIEW] [■ PICTURE] [● HEADSET] [▲ GAME]  …  ☾  🔍  Profile: Custom ▾  ● Ready  [▶ LAUNCH LMU]`

- **Tabs:** shape and colour together. The selected tab gets a 2 px underline in its colour. Arrow keys move between tabs. The Picture tab shows a dot with a word when the eye tracker is missing.
- **☾ / ☀:** cycles System → Light → Dark. The setting is persisted (open question 2). `SIM_QVFR_LAUNCHER_THEME` still overrides it, for screenshots.
- **🔍 Find a setting (Ctrl+K):**
  - A popover with a field and results. The search covers labels, ⓘ help text and section names, taken from one table the cards also use, so it can't drift from them.
  - Enter switches to the tab, scrolls the card into view and highlights the row for 1.5 s.
  - A setting hidden by its parent shows as "Rim — turn on Show a helmet to see it". Choosing it highlights the parent switch. It never changes a value on its own.
- **Profile menu:**
  - Presets of the current set and saved profiles, with a check on the one in use.
  - When the one in use has changed since: "Update <name>" (saved profiles only) and "Revert to <name>".
  - Save current settings…, Paste a shared profile, Manage profiles… (a sheet with Use, Share and Delete per saved profile), and Restore defaults for <game>….
  - The menu's last line shows the message line from today.
- **Status:** dot + word, same texts and priority as today. It is clickable where today's badge is.
- **Launch:** red, play triangle, "LAUNCH <SHORT NAME>". It becomes "<game> is running" (secondary style) while the game runs. Return still launches.

### Overview tab (landing view)

- **Left panel (fixed width about 300 px):**
  - Yellow header "▲ SELECTED GAME / <NAME>".
  - Profile segmented control: Performance / Balanced / Quality / Custom. Custom is selected when no preset is in use or matches; clicking Custom opens the Profile menu.
  - Summary line, e.g. "Eye tracked · Outer 20% · Focus 150%".
  - Last session: date and length; three big numbers (fps against the target refresh rate, 90th-percentile slow-frame ms, pixels saved); a 5-session bar chart with a dashed target line. A text summary of the chart is exposed for screen readers.
  - Empty state when the game has no sessions: "No sessions yet. Launch <game> from here and this fills in while you drive."
- **Right side:** summary cards in three groups, each with a shape + colour header and a rule. Each card shows values in plain words, opens its tab and card on click or Enter/Space, and shows warnings as chips.
  - **■ PICTURE:**
    - Resolution: "Outer 20% · Focus 150%" and the outer view's px.
    - Foveation: "Eye tracked · 25 × 25%" and Outline on/off; chip "Eye tracker not detected".
    - View & helmet: "Crop off · Helmet off" (only for games with them).
    - Rendering: runtime composition, DLSS / Foveated DLSS on/off, and the multi-view line from the last session (status only).
  - **● HEADSET:**
    - Menu & overlay: "Menu 22° left, 22° down" and Overlay on/off.
    - Hotkeys: "Ctrl+Alt+arrows" and "4 wheel buttons set"; chip on a clash.
  - **▲ GAME:**
    - Diagnostics: "5 sessions recorded"; chip "Below 90 fps".
    - Games: "4 of 4 in the rail" and Anti-cheat on/off for LMU.
    - OpenXR layers: "None below Sim-QVFR" and the layer's ms per frame.
    - Launch checks: shown first in the group only when something needs attention.
    - Ko-fi tile: dark, quiet, always present: "Made by one sim racer, in spare time. ♥ Support on Ko-fi".

### Picture tab

Cards in reading order. The grid places each card in the shortest column, so no column sits empty while another overflows. "More" is an in-card expander for the rarely used rows.

1. **Resolution** (badge "Live when lowered" / "Next launch" in Two-loop):
   - Outer and Inner sliders with px under the track. Inner is hidden in Two-loop with one note.
   - Sharpening; Object detail (X-Plane).
   - ⓘ: the recommended-resolution footer.
2. **Foveation** (Live):
   - Mode segmented (Eye tracked / Fixed) and the eye-tracker chip.
   - Width and Height on one row.
   - Shape segmented (Box / Corners): Box shows Corner radius; Corners shows the 8 corner insets as a 2 × 4 grid laid out like the corners.
   - Feather.
   - Gaze offset (eye tracked) or Vertical position (fixed).
   - Gaze dead-zone (eye tracked only).
   - Show focus outline, with the colour swatches when on.
3. **Preview:** the left-eye preview (as today, all inputs), the legend, and readouts (headset, outer, focus, centre PPD, pixels per frame with meter, % saved, VRAM estimate with meter).
4. **View & helmet** (Live; only for games with crop or helmet):
   - Crop the view; when on, Top/Bottom and Outer/Inner in a 2 × 2 grid.
   - Show a helmet; when on, Rim, Corners and Lighting, plus "More: your own images" (Visor, Size, Height, Depth, Detail).
   - When off, the parent row reads "Off · rim, corners and lighting hidden".
5. **Rendering** (shown when it has rows):
   - Two-loop only: Method + legacy note.
   - LMU: Let the runtime compose + last-launch line, and the frame generation note only when frame generation is on.
   - DLSS switch (LMU) or Use Cheeky Foveated DLSS (ACE, ACC), with the reason when it can't be used ("Needs an NVIDIA RTX graphics card…").
   - Multi-view status line (LMU, last session).
6. **DLSS** (LMU, when on): DLL version (Choose…, Use included); Focus views, Focus preset, Periphery preset, Texture sharpening; More: Other cars' motion.
7. **DLSS-NR** (LMU, when DLSS is on): Use DLSS-NR or its reason; DLL Choose…; Intensity, Style, Preset, Focus working size, Periphery too (+ its two sliders); More: Local tone, Local structure, Auto mask, Runs, Working size limit, Edge blend.
8. **Foveated DLSS** (ACE, ACC, when on): credit with version; Runtime found/not found + Choose…; OpenXR layer status; last launch; Processing, Cheaper outer views (+ scale, preset), Focus preset; More (experimental): Focus supersampling, Fix motion-vector blur, DLSS-NR (+ preset, passes); Frame generation switch (ACE); licence footer.
9. **Frame generation** (ACE, when on): Multiplier, Center region, Center and Periphery FG scale, Show the center region; More: Release game.

In the default state (DLSS, Cheeky, crop, helmet and outline off) cards 1–5 fit 1440 × 900 without scrolling.

### Headset tab

1. **Settings menu** (Live): Horizontal, Vertical and Size as three compact sliders; Show the hotkeys under the menu; ⓘ "Opening the menu".
2. **Performance overlay** (Live): Show the overlay; when on, Horizontal, Vertical and Size; ⓘ Ctrl+Alt+P.
3. **Placement:** the 100° × 80° drag preview (menu and overlay; keyboard as today) and the readouts in two short columns.
4. **Hotkeys** (Live): a table with Action, Keyboard and Wheel columns, 9 rows (Helmet visor only for helmet games), in two groups (Headset menu, In the game). Clashes show a warning icon with the word "Clash" and details in a tooltip. Wheels found is the footer. ⋯ › Reset this card replaces Reset hotkeys.

### Game tab

1. **Launch checks:** the verdict first ("Ready to launch" / "2 need attention"), then the checks that need attention with their fixes, then "Ready" collapsed to a count with "Show all".
2. **Diagnostics:**
   - Verdict: fps against target (refresh), slow frames (p90) against budget, and what limits the frame.
   - Compared with previous / with or without QVFR.
   - Recent sessions with what changed.
   - More: late frames, CPU and GPU time, cores, busiest threads, layer time, layers below, VRAM.
   - Capture tools: CPU capture, Frame capture, Next launch without QVFR.
3. **Get help:** likely causes (top two, "More"), Report an issue… (Report sheet), Export logs, Open logs folder, Copy summary.
4. **Games in the rail:** one row per game with a show/hide switch and the "Not found" tag. Colour as preset swatches: Default plus 6 presets, and "Custom…" opening a popover with the wheel, Brightness and a preview.
5. **<Game> options** (LMU): Run with anti-cheat; the risk warning shows only when on.
6. **OpenXR layers:**
   - With the launcher / Windows segmented.
   - The list with switches and moves; Reset (⋯) or Revert / Apply.
   - Messages, and a compact load-order chain.

### Sheets

- **Report sheet:** Before you report, Kind of problem, What happened?, Report an issue, the drag tile, Export logs. The native edit control stays (`report_edit`).
- **About sheet:** version, Ko-fi, updates (switch, status, Install / Download / Check now), the disclaimer, the NVIDIA notice and the Cheeky licence.
- **Manage profiles sheet** and **Reset <card>? sheet** (new). All other sheets stay as they are.

## 3. Progressive disclosure

Rules from brief §5, applied to the dependencies in inventory §19:
- A hidden child keeps its value: hiding only skips drawing and the focus order. `Settings` and the ini are untouched.
- The parent row shows a one-line summary when it hides children.
- Children appear in place with a 150 ms height animation. With "Show animations" off in Windows (`SPI_GETCLIENTAREAANIMATION`) they appear at once.

Corrections to the brief's table, found in Phase 1:
- **Feather is not a focus-outline option.** It is the focus region's blend (`[quad] feather_percent`) and stays visible. Only Outline colour depends on the outline.
- **Gaze offset is not hidden in Fixed mode.** The same slot becomes Vertical position (`[quad] fixed_offset_y`), a different key. Only Gaze dead-zone hides.
- **Cheeky's options are not gated by "Use Cheeky Foveated DLSS" today**: they are editable while it is off. Hiding them when off is new behaviour, but values are kept, so nothing is lost.
- **The performance overlay has no content options** in the launcher, only its place and size.
- **There is no multi-view option** on `development`. The Rendering card shows a status line only.
- **"Off with frame generation"** is about any frame generation (Cheeky's or VectorXR's). LMU has no Cheeky, so the note uses the last session's `session_frame_generation`.

## 4. How it will be built

The launcher keeps its own Direct2D UI; there is no framework to swap in. New pieces, all in `src/launcher/ui/`:

- **Tokens:**
  - A new `docs/ui/tokens.json` with the brief's tokens (light and dark), Jost and Inter type styles, the 4 px radius and the switch, slider and rail sizes.
  - `tools/gen_launcher_theme.py` reads it into `theme_tokens.h` alongside the old names until the last old page is gone, then the old ones go.
  - Every colour is used by token name.
  - The old kit (`docs/qvfr-launcher-kit`) is marked superseded where its rules conflict ("disable rather than hide", system font, Restore Defaults in the action bar). Open question 9.
- **Fonts:**
  - Jost and Inter (SIL OFL 1.1) embedded as `RCDATA` in `launcher.rc` and loaded with DirectWrite's in-memory font loader (`IDWriteFactory5`, Windows 10 1703+). No install, nothing written to disk. Segoe UI is the fallback if loading fails.
  - The fonts and their OFL texts go under `third_party/`, the notices into `THIRD_PARTY_NOTICES.md`, and `package.ps1` copies the OFL texts into the package's `licenses` folder.
  - Built binaries get the Defender scan as usual.
- **Shell:**
  - `rail.cpp` and `topbar.cpp` replace the tab strip, sidebar and action bar.
  - `Pane` becomes `Tab` (Overview, Picture, Headset, Game).
  - During Phase 3 the old pane functions are mounted inside the new tabs unchanged, so everything keeps working while cards are built.
- **Cards:** `card.cpp` draws a card:
  - Header: shape, uppercase Jost title, 2 px rule in the section colour, Live / Next launch badge, ⋯ menu.
  - Rows reuse today's `Row` (label, help, control).
  - Disclosure state lives per row.
  - A card declares the setting fields it owns. "Reset this card" copies those fields from `Settings{}`, with the same exclusions as `restored_defaults` (hotkeys reset only from their own card; the DLSS-NR folder kept), then calls `changed()` as every edit does.
- **Grid:** `card_grid.cpp` gives 3 columns at ≥ 1400 px, 2 at 1024–1399 px and 1 below 1024 px; minimum card width about 300 px; shortest-column placement; cards size to their content. Each tab scrolls only if its cards don't fit.
- **Controls:**
  - Restyled, not rewritten. Switch 44 × 24 with a 6 px radius and a square knob; off track `switch-off`; on `blue`.
  - Slider thumbs are rounded squares.
  - Segmented controls for 2–4 choices.
  - 4 px radii; no shadows or gradients (the popup menu's drop shadow goes too).
- **Search:** one table of searchable settings (label, help, tab, card, row id, parent row), shared with the cards.
- **Accessibility:**
  - The launcher has no UI Automation tree today: a screen reader sees one blank window. To meet §6 (names, roles, on/off state, selected tab and game), a UIA fragment provider will be built from each frame's `hits`, with a name and role per control.
  - Focus rings show only after keyboard use (as today).
  - Windows text scaling (`TextScaleFactor`) multiplies the type sizes; rows grow rather than clip.
  - Open question 10.
- **Copy:**
  - Sentence-case labels and one line of help; longer text goes behind ⓘ or "More".
  - Paths and internal names come out of visible text: DLL folders, the Cheeky runtime path, registry hive names, `XR_APILAYER_…` names, `sim-qvfr.ini`. They are replaced by "Found / Not found" plus "Show in Explorer", or friendly layer names with the raw name in a tooltip.
  - Errors get a fix action.

### Contrast check of the brief's tokens (WCAG 2.2)

Every text pair passes 4.5:1 in both themes, including:
- white on the red Launch button (5.02);
- white on blue (6.78 light, 5.04 dark);
- ink on yellow (10.89);
- green and warning text on their tints (5.15 / 6.85 light, 7.76 / 8.28 dark).

Switch tracks pass 3:1 (off 3.32 / 3.31; on-blue 6.78 / 3.35).

Three gaps to handle in Phase 2:
1. **Yellow on light surfaces is 1.6:1.** The ▲ Game marks and rules on light cards are always paired with the word, so they are decorative. Yellow is never used alone for a state on a light surface; the selected game's yellow block sits on the dark rail (11:1).
2. **`line` is 1.3:1** in both themes: fine for dividers, not for control edges. Text fields (search, report) take `switch-off` for their border (3.3:1).
3. **`text-muted` on the rail fails in light mode (2.78:1).** The rail is dark in both themes, so it needs its own `rail-text-muted` (#A3A6AE: 7.3:1 on the light rail).

## 5. Phases and checks

Each phase is one commit on this branch (based on `development`), built with `build.ps1`, with the suite run (`sim_qvfr_tests.exe`). The UI is checked on a dev build with PrintWindow captures and SendMessage input (no desktop control, and never the user's own launcher process).

1. **Inventory and plan** (this commit). Stop for review.
2. **Tokens, fonts, light/dark override**, layout unchanged. Screenshots of all 13 current pages in both themes. Check: the config round trip. A new test loads a sample ini plus profiles, saves through `save_settings`, and compares every key byte for byte.
3. **Shell:** rail, top bar, tabs, Launch, status, Profile menu, search (over the old pages), theme toggle. The old panes are mounted in the new tabs. Check: every inventory row is reachable, Ctrl+Tab, Return, Ctrl+Z.
4. **Picture tab:** cards, preview, disclosure rules 1–22, Reset this card. Check: every Picture row saves and reloads; each dependency hides and shows again with its value unchanged; 1440 × 900 without scroll in the default state.
5. **Headset tab:** cards, placement, hotkey table. Same checks.
6. **Game tab:** launch checks, diagnostics redesign, get help + Report sheet, games and colour popover, LMU options, layers. Same checks; the layer Apply path (administrator prompt) is checked by hand.
7. **Overview hub and Ko-fi placements**, About sheet. Check: summaries match the settings; the empty state; profile switching from the segmented control and the headset menu.
8. **Polish:** responsive pass at 1024, 1280, 1440 and 1920; accessibility pass (keyboard-only, Narrator, 125 % and 150 % text, reduced motion); copy pass; `docs/ui/CHANGES.md`; screenshots of every tab in both themes at 1440 and 1024 for the PR.

## 6. Decisions (review, 2026-11-12)

The user answered "use your defaults" to the twelve open questions:

1. **Per-game colours** stay as the game's marker in the rail (a 4 px bar on each block, and the selected block's notch). They no longer recolour Launch, selection or sliders. The setting stays in Game › Games in the rail, with presets and Custom….
2. **Theme override** is persisted in a new key, `[launcher] ui_theme = system | light | dark`. It is outside profiles and per-game copies, and no existing key changes.
3. **Rail abbreviations** come from a UI-side table (`xplane` → "XP12"), with `shortName` as the fallback. The game table is unchanged.
4. **Landing view** is Overview. Launch checks lives in Game › Launch checks; the top bar status and an Overview banner flag problems.
5. **About and updates** are in an About sheet, opened from the version in the rail. An available update shows as a dot on the version and in the status.
6. **Report an issue** is a sheet, opened from Game › Get help and from the launch status link.
7. **Restore Defaults:** each card has "Reset this card" (with confirmation), and the Profile menu has "Restore defaults for <game>…" with today's Undo and Ctrl+Z.
8. **Fonts:** Jost and Inter are downloaded from their official OFL repositories and bundled.
9. **Old design kit** stays as history, with a note that `docs/ui` supersedes it.
10. **Screen readers:** the UI Automation provider is built in Phase 8.
11. **Pixels saved** in Last session uses that session's own settings.
12. **LMU resolution badge** keeps today's tag.

## 7. Progress

- **Phase 1** (2dd8ac4): this plan and the inventory.
- **Phase 2** (e4ae4a2): tokens, Jost and Inter, the restyled controls, the theme button (`[launcher] ui_theme`), the round-trip test.
- **Phase 3** (the shell):
  - The rail: the games from `games::all_games()`, the selected one yellow with its notch, each game's colour as a 4 px marker, Ko-fi and the version (it opens About; a dot when an update is out). Under 1024 px it shows abbreviations only.
  - The top bar: the tabs with their shapes, the theme button, Find a setting, the profile menu, the status (a dot and a few words, the whole sentence as its tip, clicked where the action bar's badge went), and Launch.
  - The profile menu: the summary line, the presets and saved profiles, Update and Revert when the one in use has changed, Save, Paste, Manage profiles…, and Restore defaults for <game> (its Undo stays in the status and on Ctrl+Z).
  - Find a setting (Ctrl+K): its index is what every page draws, so it can't drift from the pages; Return shows the page, scrolls to the row and highlights it.
  - Narrow windows drop "Profile:", then tighten the tabs, then shorten the status, then Launch's padding: all four tabs fit at the 880 px minimum.
  - The window opens on Overview. Until the tabs have their cards, the old panes are each tab's pages (a strip under the top bar): Overview: Profiles, About; Picture: Resolution, Foveation, Focus overlay, DLSS; Headset: Headset menu, Hotkeys; Game: Launch checks, Diagnostics, Report an issue, Games, OpenXR layers.
