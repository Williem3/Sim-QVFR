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
- **Phase 4** (the Picture tab):
  - One page of cards (`picture.cpp`) replaces Resolution, Foveation, Focus overlay and DLSS: Resolution, Foveation, Preview, View & helmet, Rendering, then DLSS and DLSS-NR (LMU) or Foveated DLSS and Frame generation (ACE, ACC), each only where the game has it, and the DLSS cards only once their switch is on.
  - `card.cpp`: the card (section shape and colour, title, Live / Next launch tag, ⋯ menu with Reset this card…, the 2 px rule) and its compact rows (two-line sliders, pairs side by side, switches, segmented choices, pop-ups, a More), in 3, 2 or 1 columns by window width, each card in the shortest column.
  - Disclosure (§3): what a switch or choice hides isn't drawn and keeps its value. The switch says what it hides ("Off · rim, corners and lighting hidden"), and its rows slide open for 160 ms when it turns on (not with Windows' animations off). Find a setting still lists hidden settings ("shows with Show a helmet") and highlights what shows them, or opens the More they are in.
  - The rows' longer explanations (costs, cautions) moved into their ⓘ, so nothing was dropped.
  - Checked: 1440 × 900 shows the default Picture tab without scrolling; the crop hides and shows with its values kept; a segmented choice saves; Reset this card asks, then resets only its own settings.
  - The Rendering card has no multi-view line: runtime.ini's session mode says only QVFR, Two-loop or Off.
- **Phase 5** (the Headset tab):
  - One page of cards (`headset.cpp`) replaces Headset menu and Hotkeys: Settings menu (place and size, the hotkeys under it), Performance overlay (its place and size hidden while it is off), Placement (the drag preview and its readouts) and Hotkeys.
  - Hotkeys is a table: action, keyboard, wheel; a clash shows a warning mark on both actions with what clashes in its tip; the wheels found are its note. Its card menu's reset replaces Reset hotkeys (the Ctrl+Alt keys, no wheel buttons).
  - Each hotkey button is its own Tab stop (buttons take an option: only the focused one shows the ring), and the placement panels are in the Tab order again (arrows move them).
  - Checked: 1440 × 900 shows the Headset tab without scrolling; a key caught and saved; the card's reset; the overlay's place kept while it is off; the menu dragged in Placement; Tab to a hotkey and Return to catch.
- **Phase 6** (the Game tab):
  - One page of cards (`pane_games.cpp` `game_page`) replaces Launch checks, Diagnostics, Report an issue, Games and OpenXR layers: Launch checks, Diagnostics, Get help, OpenXR layers, Games in the rail and, for LMU, LMU options. The tabs now have no page strip but Overview's.
  - Launch checks: the verdict as a chip, what needs attention with its fix, the ready checks behind a More. The status, a failed launch and "Show Checks" scroll to the card and highlight it.
  - Diagnostics: the verdict rows (frame rate against the headset's, slow frames against the frame's budget, what limits it), Previous session and With / Without QVFR, Recent sessions, the rest behind More: details, and the three capture switches. A long value (a comparison) takes its own line.
  - Get help: the last session's likely causes (two, the rest in a More), Report an issue…, Export logs, Copy summary, Open logs folder. Report an issue is a panel over the window (`panels.cpp`), also opened by the launch status after a crash; Escape or its ✕ closes it, and its description box is the same native edit.
  - Games in the rail: a row per game (its colour, Not found, its switch; the last one locked), then the colour of the game picked: its own colour, 6 presets apart from the games' own and the sections' colours, and Custom…, a panel with the wheel, Brightness and how it looks on the rail.
  - LMU options: Run with anti-cheat; the risk chip and note show only while it is on, the whole text is in its ⓘ.
  - OpenXR layers: the profile as a segmented choice (With the launcher / Windows’ list), the list with switches and moves, Revert / Apply, the message, and the chain a frame goes through. Windows' order (forgetting the launcher's profile) is the card's Reset this card…; a known layer's registered name is no longer under its name.
  - Keyboard: the cards' controls in drawn order, a layer list as one stop (arrows, Space, Ctrl+arrows), each check's fix, game switch and swatch its own stop; a panel's own controls only while it is open.
  - Checked: 1440 × 900 shows the Game tab without scrolling, with no session and with two measured sessions (Previous session with the setting changed between); Report an issue opens over the page with its description box and closes on Escape; a preset saves `[launcher] color_ace` and its own colour removes it; the colour panel; a layer's switch saves `on_layers` and the card's reset clears it (asked first); Windows' list shown (nothing applied); LMU's anti-cheat switch saves, its warning only while on; Tab stays inside an open panel.
- **Phase 7** (the Overview hub):
  - `overview.cpp`: the selected game's panel and the summary cards. The Overview is the landing view, and every tab is now one page: the page strip is gone.
  - The panel: the yellow header with the game's name, the profile as Performance / Balanced / Quality / Custom (a preset is used at once, as Use in Manage profiles; Custom, or a changed preset, is selected otherwise and opens the profile menu), the summary line (a saved profile in use under it), and the last session: fps against the headset's refresh, slow-frame ms, pixels saved with that session's own settings, and the last 5 sessions as bars with the target as a dashed line (its tip says the numbers in words). With no session for the game, the empty state.
  - The summary cards, grouped under Picture, Headset and Game with their shapes and rules: Resolution, Foveation (Outline on/off; Eye tracker not detected), View & helmet, Rendering, Menu & overlay, Hotkeys (the menu keys, the wheel buttons set; Clash), Launch checks (only when something needs attention), Diagnostics (Below N fps), Games, OpenXR layers. A click, or Return on the focused card, opens its tab and highlights its card.
  - Ko-fi: the dark tile among the Game cards, the rail's heart, and About's button.
  - Manage profiles (the profile menu) and About (the version in the rail, the update dot) are panels. Find a setting finds their labels too, and opens the panel.
  - Checked: 1440 × 900 shows the Overview without scrolling, with no session and with two (light and dark); Balanced from the segments writes its keys and `[profiles] current`; Custom opens the profile menu; Manage profiles from the menu; a summary card and Return on a focused one open the card; the version opens About; "updates" in Find a setting opens About. The Ko-fi tile was not clicked (it opens the browser), and 1024 px and narrower scroll (Phase 8).
- **Phase 8** (polish):
  - Responsive: the card grids fit as many columns of at least 360 px as the width allows, up to four (3 at 1440, 2 at 1024 and 1280, 4 at 1920); every tab was captured at 1024, 1280, 1440 and 1920. 1440 × 900 shows every tab without scrolling in its default state; 1024 reflows to two columns and scrolls.
  - Screen readers (`accessibility.cpp`, UI Automation): what the last frame drew is the element tree, in reading order (the rail, the top bar, then the page; a panel, a sheet or an open menu alone). Controls have names (their own, with their row's label), kinds and states: switches toggle, sliders have their value and range, segments, tabs and games are selection items, pop-ups have their value, buttons invoke, menu items choose. Text is read too (`reader_text`): card titles and group names as headings, rows' values, notes, chips, readouts, the session chart's summary. A focus change raises the focus event, and the focused control's change its property event. Actions are posted to the window and done as a click or the keys would. Checked with Windows' UI Automation client: the Overview, Picture and Game trees, a switch toggled, a slider set, a segment and a tab chosen (each saved), the profile menu's items, Manage profiles, a card's reset sheet and Cancel.
  - Contrast (WCAG 2.2) re-checked from `theme_tokens.h` for every pair the new pages draw: all text pairs pass 4.5:1 and all control edges 3:1 in both themes, but the yellow Game rule and triangle on light surfaces (decorative, always with the word; see §4).
  - Target size: the rail's version is 24 px high (the smallest target).
  - Copy: no registry names or developer paths in the text (Windows' layers show a manifest's path as the switch's tip); help text points at the cards, not the old panes.
  - Fixed on the way: the arrow keys on the colour panel's Brightness now recolour the game, as dragging does (`set_slider`, shared with the screen reader).
  - `CHANGES.md`: what moved where, for the release notes.
  - Not done: Windows' "Make text bigger" setting (text scaling apart from display scaling) is not applied; display scaling at 125 % and 150 % is (the layout is in DIPs).
- **Review fixes** (after the user's first run of the redesign):
  - A card was measured with its Mores closed (they are remembered by the card's title, which the measuring pass didn't set), so an open More drew past the card's bottom: Launch checks' ready list, the helmet's own images, DLSS's advanced rows, Frame generation's timing. Cards are now measured under their title.
  - The page stayed scrolled when the window grew (no scroll left to undo it): paint brings the scroll back in range.
  - Each feature's card holds its switch, with its options opening under it: DLSS (LMU) holds Use DLSS, Foveated DLSS (ACE, ACC) holds Use Cheeky Foveated DLSS, and Frame generation holds its own switch. What a switch shows follows straight under its card (`Card::follows`): DLSS-NR under DLSS, Frame generation under Foveated DLSS. Rendering keeps the method and the runtime composing the views.
  - The Preview shows the left eye, both side by side, or the right eye (a view, not a setting). The crop's outer side is each eye's temple side, so it mirrors; the focus shape's corners are the screen's in both eyes, as the compositor draws them.
- **The sim-racer review's top five** (a review agent playing a non-technical racer, `build/ui-screens/review-sim-racer.md`):
  - Problems say what's wrong in plain words, with a way out: "Foveated DLSS is on, but Cheeky isn't installed" with Turn Foveated DLSS off and How to install it (the README's section); "DLSS is on, but NVIDIA's DLSS file is missing" with Turn DLSS off. The chip reads "1 problem · Launch will ask you to confirm first". (The checks' wording is in `preflight.cpp`; its tests follow.)
  - Online racing, for every game, on the Game tab and the Overview, in one sentence: "Offline sessions only", or for LMU with its anti-cheat on "Online at your own risk: the anti-cheat can ban you". The help texts no longer contradict About.
  - One name per setting, the headset menu's: Periphery resolution, Focus density, Focus width and height; the crop's Outer side (temple) and Inner side (nose). The Overview, the profile menu, the presets' summaries and the sessions' change list say the same (display text; tests follow).
  - The layers card's choice is "Layers for: Launches from here | Every app"; Cheeky's layer row says it isn't the Picture tab's switch.
  - The Game tab leads with Launch checks, Online racing and Get help. OpenXR layers says what loads in a line, its list behind a More (Launch checks' Open OpenXR layers opens it); Diagnostics' capture tools are behind a More unless one is on.
- **The sim-racer review's findings 6–14, and the anti-cheat disclaimer:**
  - Run with anti-cheat asks before it turns on: Sim-QVFR and its developer aren't responsible for kicks or bans, and the player accepts that risk ("I accept the risk"; Cancel is the default). Off is at once.
  - Get help: Report an issue says it needs a free GitHub account; the zip, the summary and the logs folder are "other ways to share", with what to do without GitHub.
  - The menu in the headset is the Headset menu everywhere; Open or close without a key of its own shows "Any arrow key"; Hotkeys says how to set a wheel button for it.
  - Undo: every saved change the session history can describe ("Feather 20% → 30%") shows a bar with Undo for 8 s; Ctrl+Z and the profile menu undo it after that too. Reading the ini again (the headset menu, a game switched, a launch) is not a change to undo.
  - Overview: a first-run line until the first session, plain card lines ("Follows your eyes", "Foveated DLSS on", "4 of 4 games shown"), the presets' trade-off under the profile, and the last session's verdict (Get help's first cause, or smooth at the headset's rate).
  - Foveation: the shape is Rectangle or Custom corners, and a line under Feather says how to hide the area's edge.
  - Wording: "Off" alone (or "Off: …" where it says something), Show / Hide for the cards' extra rows, and the badges Changes now, Now when lowered, Next game start. Sharpening, Slow frames, Periphery preset and Let the runtime compose start their help with what they do for the player.
