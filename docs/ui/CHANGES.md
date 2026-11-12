# Launcher redesign: what moved where

The launcher has a new look and layout. Every setting is still there, saved in the same `sim-qvfr.ini` keys, so your settings, profiles and per-game copies carry over unchanged.

## The new layout

- **Games** are a rail on the left. The selected game is the yellow block. Ctrl+Tab still switches games.
- **Four tabs** across the top replace the 13-page sidebar:
  - **Overview**
  - **Picture**
  - **Headset**
  - **Game**
- **The top bar** also holds:
  - the theme button (follow Windows, light or dark);
  - **Find a setting** (Ctrl+K);
  - the **profile menu**;
  - the status;
  - **Launch**, always visible.
- **Cards:** settings are grouped in cards. A card says when its settings apply: Live, Live when lowered, or Next launch.
- **Progressive disclosure:** settings that depend on a switch are hidden while it is off, and their values are kept.

## Where each page went

| Before | Now |
|---|---|
| Launch checks (the start page) | Game › Launch checks. The launcher now opens on **Overview**; the top bar's status shows problems and opens the checks. |
| Profiles | Overview's Performance / Balanced / Quality / Custom, and the profile menu (Save, Paste, **Manage profiles…**) |
| Resolution | Picture › Resolution; runtime composition is in Picture › Rendering; crop and helmet are in Picture › View & helmet |
| Foveation | Picture › Foveation (the legacy method choice is in Picture › Rendering, shown only in Two-loop) |
| Focus overlay | Merged into Picture › Foveation (Show focus outline) |
| Preview column | Picture › Preview |
| DLSS | Picture › Rendering (on or off), then the DLSS and DLSS-NR cards (Le Mans Ultimate), or the Foveated DLSS and Frame generation cards (ACE, ACC) |
| Headset menu | Headset › Settings menu, Performance overlay and Placement |
| Hotkeys | Headset › Hotkeys, a table. A clash shows a warning on both actions. |
| Games | Game › Games in the rail (colour presets and Custom…), and Game › LMU options (Run with anti-cheat) |
| OpenXR layers | Game › OpenXR layers. "Windows' order" is now the card's **⋯ › Reset this card…** |
| Diagnostics | Game › Diagnostics (verdict first, details behind "More: details"), Game › Get help, and Overview's Last session |
| Report an issue | Game › Get help › **Report an issue…**, which opens over the window. A crash link in the status opens it too. |
| About | The version at the bottom of the rail (a dot when an update is out) |
| Restore Defaults | Each card's **⋯ › Reset this card…** (it asks first), and **Restore defaults for <game>** in the profile menu (Undo and Ctrl+Z still work) |

## New

- **Overview:**
  - The selected game's profile and the last session: fps against the headset's refresh, slow frames, and pixels saved.
  - A chart of the last five sessions.
  - A summary card for each part. A card opens its settings.
- **Find a setting** (Ctrl+K): type a setting's name and press Return. It opens the tab and highlights the setting, or what shows it.
- **Theme:** Light and dark follow Windows, or you can choose with the theme button (`[launcher] ui_theme`, the only new key).
- **Fonts:** Jost and Inter, bundled with the launcher.
- **Screen readers** (Narrator and others) can read and use every control. Text, card titles, values and the session chart are read too.

## Unchanged

- Launching, the OpenXR layer, profiles, the headset menu, and every `sim-qvfr.ini` key and value.
- Keyboard use: Tab, the arrow keys, Space and Return. Return still launches when no button has the focus.
