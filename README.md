# Sim-QVFR

**More frames per second in VR, with a sharp picture where you're looking.**
For Assetto Corsa EVO, Assetto Corsa Competizione and X-Plane 12.

Sim-QVFR makes your sim draw the middle of your view (or wherever your eyes are looking, if your
headset tracks your eyes) at full sharpness, and the rest of the view at a lower resolution. You
barely notice the difference, but your graphics card does far less work, so you get smoother VR or
room to turn the graphics up.

> **Please read first**
> - **Unofficial and experimental.** Sim-QVFR isn't made or approved by the makers of the games it
>   works with: Kunos Simulazioni (Assetto Corsa EVO and Competizione) and Laminar Research
>   (X-Plane 12). Use it at your own risk: it can crash the game, and a game update can stop it working
>   until Sim-QVFR is updated too.
> - **Offline only.** Use it in single-player sessions, not online.
> - **Don't report problems to the game's makers** that happen while Sim-QVFR is running. Report them
>   here instead (see [Getting help](#getting-help)).

## What you need

- **One of the games:**
  - **Assetto Corsa EVO** on Steam.
  - **Assetto Corsa Competizione** on Steam, with its VR running through OpenComposite (see
    [Assetto Corsa Competizione](#assetto-corsa-competizione-alpha)).
  - **X-Plane 12** (version 12.4.3-r2), from Steam or elsewhere (see [X-Plane 12](#x-plane-12)).
- **A PC VR headset.** Eye tracking is a bonus: with it, the sharp area follows your eyes. Without it,
  the sharp area stays in the middle of your view.
- **Steam** running, for the Steam games.

## Install and play

1. **Download** the newest `sim-qvfr-….zip` from the
   [Releases page](https://github.com/Williem3/Sim-QVFR/releases).
2. **Unzip it** into any folder you like (for example `Documents\Sim-QVFR`). There's no installer.
3. **In Assetto Corsa EVO, turn off the game's own foveated rendering** in its settings. Sim-QVFR does
   this job instead.
4. **Start Steam and your VR software** (SteamVR, Virtual Desktop, Pimax Play, and so on).
5. **Open `sim-qvfr-launcher.exe`** from the folder you unzipped, with a normal double-click (not as
   administrator: a game started from it ignores Sim-QVFR).
6. **Pick your game's tab** at the top of the window and **click Launch**. The game starts in VR with
   Sim-QVFR turned on.

That's it. Always start the game from the launcher when you want Sim-QVFR. If you start the game
from Steam as normal, it runs without Sim-QVFR.

**Game not found?** If a game isn't installed through Steam, or Steam can't tell the launcher where it
is, **Launch checks** offers **Choose folder…**: pick the game's folder.

**To remove it**, delete the folder. Nothing else on your PC was changed.

**Coming from the alpha (ace-quadview)?** The beta is a fresh start: unzip it into a new folder and
set your options again in the launcher. The alpha's launcher won't offer the beta as an update, and
the beta doesn't read the alpha's settings. When you're happy with the beta, delete the old
ace-quadview folder.

## Choosing your settings

Every setting is in the launcher window, and your choices are saved for you. Each game keeps its own
settings, so switching tabs switches them. Hover over the **ⓘ** next to any setting to see what it does
and what it costs. Each section is tagged **Live** (it changes the running game straight away) or
**Next launch**.

- **Not sure where to start?** Open **Profiles** and pick **Performance**, **Balanced** or **Quality**.
- **Launch checks**, where the launcher opens, lists anything that would stop Sim-QVFR working, with how
  to fix it. The bar at the bottom of the window shows the first problem, and after you click Launch it
  tells you whether the game started with Sim-QVFR.
- **Resolution** sets how sharp the outer view and the sharp area are (**Inner resolution** goes from
  50% to 400%). **Foveation** sets the sharp area's size and shape, and whether it follows your eyes.
- **Profiles** also lets you save your own settings, and **Share** or **Paste** them as a line of text
  to swap with friends.
- **Hotkeys** lets you pick keyboard keys or wheel buttons for the in-game menu and other actions.
- **Diagnostics** shows how your last session ran and compares it with the one before.

### Changing settings while you play

Press **Ctrl+Alt+arrow keys** (or a wheel button you set in **Hotkeys**) to open the **headset menu**.
Up and down choose a setting, left and right change it. Changes apply straight away and are saved.

Other in-game hotkeys:

| Keys | What it does |
| --- | --- |
| Ctrl+Alt+arrows | Open and use the headset menu |
| Ctrl+Alt+P | Show or hide the performance overlay (frame rate, GPU and CPU load) |
| Ctrl+Alt+V | Open or close the helmet visor (when the helmet overlay is on) |

### Optional extras

All of these are off until you turn them on in the launcher:

- **Performance overlay:** a small panel in your headset showing frame rate and how hard your PC is
  working.
- **View crop** (Assetto Corsa EVO and Competizione): stops the game drawing the edges of your view
  that a helmet would hide anyway, for extra speed.
- **Helmet overlay** (Assetto Corsa EVO and Competizione): draws a helmet's visor around your view. You
  can swap in your own helmet images.
- **Foveated DLSS and frame generation:** for NVIDIA RTX cards, see the next section.

## Foveated DLSS (optional, NVIDIA RTX only)

In Assetto Corsa EVO and Competizione (X-Plane 12 has no DLSS), if you have an NVIDIA RTX graphics card,
Sim-QVFR can use
[Cheeky Foveated DLSS](https://github.com/ClarkCheekyKent/CheekyFoveatedDLSS) by
[ClarkCheekyKent](https://github.com/ClarkCheekyKent), **integrated with his permission**. It adds:

- **Foveated DLSS:** cheaper DLSS on the outer view, full-quality DLSS in the sharp area.
- **Extra sharp-area options (experimental):** supersampling, a fix for motion blur, and DLSS-NR.
- **Frame generation (experimental):** extra frames made between the game's own frames.

Cheeky Foveated DLSS is a separate, free download (GNU GPL v3). It isn't included with Sim-QVFR.

### What to download

You need **two** downloads from the links below. In each, scroll down to **Assets** and download the
ZIP named here:

| Download | Where | What it's for |
| --- | --- | --- |
| `CheekyFoveatedDLSS-0.6.3-Standalone-sim-qvfr-support.zip` | [Cheeky Foveated DLSS v0.6.3-sim-qvfr-support](https://github.com/Williem3/CheekyFoveatedDLSS/releases/tag/v0.6.3-sim-qvfr-support) | The DLSS files themselves. This build includes fixes both games need: Assetto Corsa EVO (the official build shows the outer view shrunk, like "picture in picture") and Assetto Corsa Competizione (the official build crashes ACC at start and can't tell its views apart). Older builds such as `v0.6.3-ace.2` don't work with ACC. |
| `CheekyFoveatedDLSS-0.6.3-Standalone-TEST-SIGNED.zip` | [Cheeky Foveated DLSS v0.6.3](https://github.com/ClarkCheekyKent/CheekyFoveatedDLSS/releases/tag/v0.6.3) (official) | Only for its installer, `CheekyOpenXRSetup.exe`. |

Once an official Cheeky release includes the fixes, its own download will replace both.

### Setting it up

1. **Turn on DLSS** in the game's graphics settings (Assetto Corsa EVO or Competizione). Foveated DLSS works
   on the game's own DLSS, so it has to be on.
2. **Run Cheeky's installer.** In the official ZIP, open the `OpenXR` folder and run
   `CheekyOpenXRSetup.exe`.
3. **Copy Cheeky's folder into Sim-QVFR.** In the beta ZIP there's a folder called
   `CheekyFoveatedDLSS`. Copy that whole folder into the `layer` folder inside your Sim-QVFR folder.
   (If you'd rather keep it somewhere else, use **Choose…** in the launcher's **Foveated DLSS** page to
   point to `CheekyFoveatedDLSSRuntime.dll`.)
4. **Don't copy `dxgi.dll` or `version.dll`** from Cheeky's ZIPs into the game's folder. If they're
   already there, delete them. Sim-QVFR starts Cheeky itself, and having both makes it fail.
5. **If you use OpenXR Toolkit:** it has to be *above* Cheeky in the list of OpenXR layers, or the game
   crashes when it starts. The launcher's **Launch checks** tells you if this is wrong, and you can fix
   the order in the free [OpenXR API Layers GUI](https://github.com/fredemmott/OpenXR-API-Layers-GUI).
6. **Turn it on.** In the launcher, open **Foveated DLSS** and tick **Use Cheeky Foveated DLSS**. The page
   tells you whether it found everything. After you've launched once, it also shows Cheeky's version.
7. **Click Launch.**

**In Assetto Corsa Competizione, also:**
- **Update ACC's DLSS.** ACC comes with DLSS 2.2.15, which is too old for Foveated DLSS: the sharp area works,
  but the cheaper outer view doesn't. Replace `nvngx_dlss.dll` in
  `Assetto Corsa Competizione\AC2\Plugins\DLSS\Binaries\ThirdParty\Win64` with DLSS 310 or newer (a tool such
  as DLSS Swapper does it). **Launch checks** tells you when it's too old and shows you the file. Steam's
  **Verify integrity of game files** puts the original back.
- **For DLSS-NR,** put NVIDIA's `nvngx_dlssnr.dll` in the same `CheekyFoveatedDLSS` folder you copied in step 3
  (Assetto Corsa EVO has one in its own folder; ACC doesn't come with one).

**Frame generation** (Assetto Corsa EVO only for now): tick **Frame generation** on the same page. Before you do:
- Turn off **Turbo mode** in OpenXR Toolkit (the game crashes with it).
- Don't use another frame generator at the same time, such as VectorXR's.

**Good to know:**
- Most DLSS settings change straight away, in the launcher or the headset menu. Resolution changes
  take effect the next time you start the game.
- **Show the center region** draws an outline around the full-quality area, so you can check it
  follows your eyes.

## Assetto Corsa Competizione (alpha)

Sim-QVFR runs QVFR in ACC: a sharp focus area that follows your eyes, a lower-resolution
periphery, the headset menu and the performance overlay. Pick the **Assetto Corsa Competizione** tab
at the top of the launcher, then **Launch**. Eye tracking works without another OpenXR layer.

ACC's VR has to run through OpenComposite: [Assetto Corsa Competizione in VR through
OpenComposite](docs/acc-opencomposite.md) explains the one-time setup, and **Launch checks** tells you
if it's missing.

The field of view crop, the helmet overlay, resolution changes while the game runs and Foveated DLSS
(Cheeky) work in ACC. In ACC the focus area's sharpness across follows the periphery's, so a sharper focus
area needs a higher periphery there. Without Foveated DLSS, turn DLSS off in ACC's settings: with ACC's own
DLSS the sharp area can look slightly darker or lighter than the rest of the view. Foveated DLSS needs a newer
DLSS than the one ACC comes with (2.2.15): replace ACC's `nvngx_dlss.dll` with DLSS 310 or newer (a tool such as
DLSS Swapper does it). **Launch checks** tells you when it's too old. Cheeky's frame generation isn't
available in ACC yet.

## X-Plane 12

Sim-QVFR runs QVFR in X-Plane 12: a sharp focus area that follows your eyes, a lower-resolution
periphery, the headset menu and the performance overlay. Pick the **X-Plane 12** tab at the top of the
launcher, then **Launch**.

- **Always start X-Plane from the launcher.** Sim-QVFR has to be loaded before X-Plane starts, so
  X-Plane started from Steam or its own shortcut runs without QVFR.
- **Version 12.4.3-r2.** On any other X-Plane build, QVFR stays off and X-Plane runs as usual.
- **Changes while you fly:** the resolution sliders and the focus size change straight away, in the
  launcher or the headset menu.
- **Object detail** (the launcher's **Resolution** page, and the headset menu) sets how much detail
  trees and objects get. 100%, the default, gives them the detail the sharp area needs. Lower values
  cost X-Plane's main thread less, which is often what limits the frame rate with QVFR.
- **Not in X-Plane:** the view crop, the helmet overlay and Foveated DLSS.
- **Not from Steam?** **Launch checks** asks for X-Plane's folder.

## Getting help

- **Something not working?** Open the launcher's **Launch checks** first: it explains most problems
  and how to fix them.
- **Game crashing, starting on the desktop instead of in VR, or acting strangely?** Use the launcher's
  **Report an issue** page. Pick the kind of problem, describe what happened, and it bundles up the logs
  and opens a GitHub issue for you. Drag the ZIP's tile onto the issue to attach it, then send. Names
  and account IDs are taken out of the game's own crash reports before they go in the ZIP.
- **Someone asked for your logs?** **Export logs**, on the same page, makes the same ZIP without opening
  an issue.
- **Can't find the game?** Make sure the game is installed and, for a Steam game, that Steam is running.
  If the launcher still can't find it, use **Choose folder…** in **Launch checks**.
- **After a game update**, Sim-QVFR turns itself off and the game runs as normal until a new version
  comes out. Your headset shows a message for the first 30 seconds to let you know.
- **Updates:** the launcher's **About** page can check for new versions and install them for you. Your
  settings and profiles are kept.

## Show your appreciation

If SIM QVFR has given you smoother VR, extra FPS, or room to turn up the resolution, please consider
donating.

[![Donate on Ko-fi](https://img.shields.io/badge/Donate-Ko--fi-FF69B4?style=for-the-badge&logo=ko-fi&logoColor=white)](https://ko-fi.com/williem3)

## Thanks

Thanks to [DSuzuki35](https://github.com/DSuzuki35) for the breakthrough in
[LMU-Quad-View-Mod](https://github.com/DSuzuki35/LMU-Quad-View-Mod) and for the starter launcher,
both of which helped the early development of Sim-QVFR.

Thanks to [ClarkCheekyKent](https://github.com/ClarkCheekyKent) for
[Cheeky Foveated DLSS](https://github.com/ClarkCheekyKent/CheekyFoveatedDLSS), which provides the Foveated
DLSS, DLSS-NR and frame generation features, and for his permission to integrate it into Sim-QVFR.
Cheeky Foveated DLSS is free software under the GNU GPL v3. Sim-QVFR loads it as a separate download
and doesn't include it. The launcher and the headset menu show its version wherever they control it.

## Licence

Sim-QVFR is free to use, change and share for any **noncommercial** purpose under the
[PolyForm Noncommercial License 1.0.0](LICENSE.md). Commercial use needs the author's permission.

**Credit is required.** If you share any part of Sim-QVFR, changed or not, include `LICENSE.md`
(or a link to it) and this line:

```
Required Notice: Copyright (c) 2026 Williem3 (https://github.com/Williem3/Sim-QVFR)
```

Third-party parts keep their own licences: see [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

## For developers

Building from source, settings files, test tools and how the layer works are in
[CONTRIBUTING.md](CONTRIBUTING.md).
