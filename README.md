<p align="center">
  <picture>
    <source media="(prefers-color-scheme: dark)" srcset="assets/brand/sqvfr-wordmark-on-dark.png">
    <img src="assets/brand/sqvfr-wordmark-on-light.png" alt="Sim-QVFR" width="360">
  </picture>
</p>

# Sim-QVFR

**More frames per second in VR, with a sharp picture where you're looking.**

Sim-QVFR makes your sim draw the part of the view you're looking at in full sharpness and the rest at a lower
resolution. Your eyes only see fine detail in a small area, so you barely notice the difference, but your graphics
card does far less work. You get smoother VR, or room to turn the graphics up.

- **Without eye tracking** (Quest 2, Quest 3, Index, Reverb…) the sharp area stays in the middle of your view. It
  still works: you just keep it a bit larger.
- **With eye tracking** (Quest Pro, Pimax Crystal, Varjo, Bigscreen Beyond 2e…) the sharp area follows your eyes, so it
  can be small and the savings are bigger.

How many frames you gain differs widely with your headset, PC, game and game settings, so we don't quote numbers.
It saves graphics-card work, so it helps most
when your graphics card is what holds you back. **See your own gain:** in the launcher, **Game › Diagnostics ›
Next launch without QVFR** starts the game once without it; drive the same session both ways and the Diagnostics
page compares the two.

## Is it for me?

| Game | Status | Before you start |
| --- | --- | --- |
| **Le Mans Ultimate** | Beta | Steam, version 1.4.2.0. [LMU guide](docs/guides/le-mans-ultimate.md) |
| **Assetto Corsa EVO** | Beta | Steam. Turn off the game's own foveated rendering. [ACE guide](docs/guides/assetto-corsa-evo.md) |
| **Assetto Corsa Competizione** | Beta | Steam, version 1.10.4. Needs a one-time VR setup first. [ACC guide](docs/guides/assetto-corsa-competizione.md) |
| **X-Plane 12** | Beta | Steam, version 12.4.3-r2. [X-Plane guide](docs/guides/x-plane-12.md) |
| **American Truck Simulator** (1.62.2.2), **Euro Truck Simulator 2** (1.62.3.3) | Experimental | Steam. [Truck sims guide](docs/guides/truck-simulators.md) |

Each game works with the version named (Assetto Corsa EVO: its current Steam build when this release came out).
**After a game update**, Sim-QVFR turns itself off and the game runs as normal until a new Sim-QVFR comes out: watch
the [Releases page](https://github.com/Williem3/Sim-QVFR/releases), or the launcher's update dot. Le Mans Ultimate is
the exception: it keeps running on newer builds and beta branches, untested until a release names them.

**Headsets:** any PC VR headset whose software supports OpenXR, which today is nearly all of them: Meta Quest and
Pico (through Virtual Desktop; Quest also through Quest Link or Air Link), Pimax, Varjo, Bigscreen, Valve Index and
other SteamVR headsets. [Headset setup](docs/guides/headsets.md) says what to pick in each headset's software.

**Graphics cards:** any card that runs your game in VR. DLSS needs an NVIDIA RTX card (Le Mans Ultimate, Assetto Corsa
EVO, and Foveated DLSS in ACC); Le Mans Ultimate offers AMD FSR on other cards.

## Before you install: three things to know

> - **Online play is at your own risk.** Sim-QVFR doesn't guarantee that a game with online multiplayer won't kick or
>   ban you for running it (league racing, convoys, online flying). Le Mans Ultimate starts offline by default; its
>   opt-in **Run with anti-cheat** option may get your account **banned**. Read
>   [Online play and anti-cheat](docs/guides/online-play.md) before you play online.
> - **Unofficial and experimental.** Sim-QVFR isn't made or approved by Kunos Simulazioni, Laminar Research,
>   Studio 397 or SCS Software. It can crash the game, and a game update can stop it working until Sim-QVFR is
>   updated too (it then turns itself off and the game runs as normal).
> - **Report problems here, not to the game's makers,** when they happen with Sim-QVFR running
>   (see [Getting help](#getting-help)).

Nothing is installed on your PC: Sim-QVFR runs from its own folder, and you remove it by deleting that folder. (The
one exception is optional: [Foveated DLSS](docs/guides/acc-foveated-dlss.md) in ACC needs Cheeky's own installer.)

## Install

1. **Download** the newest `sim-qvfr-….zip` from the
   [Releases page](https://github.com/Williem3/Sim-QVFR/releases) (under **Assets**).
2. **Unzip it** into a folder of your own, for example `Documents\Sim-QVFR`. Not under `Program Files`: the launcher
   can't save your settings there.
3. **Start your headset's software** (Virtual Desktop, Pimax Play, Meta Quest Link, SteamVR…), and Steam for a
   Steam game. Quest and Pico owners: see [Headset setup](docs/guides/headsets.md#virtual-desktop-quest-pico)
   for which option to pick.
4. **Double-click `sim-qvfr-launcher.exe`** in the folder you unzipped.
5. **Pick your game** in the list on the left and **click Launch** (top right). The launcher starts the game in VR
   mode with Sim-QVFR on: you don't need any VR launch options of your own.

**Always start the game from the launcher** when you want Sim-QVFR. Started from Steam or a shortcut, it runs
without it.

The launcher's **Game › Launch checks** lists anything that would stop Sim-QVFR working, with how to fix it. If
the launcher can't find your game, **Choose folder…** there lets you point to it.

## Your first session

1. **On the launcher's Overview, pick Balanced.** **Performance** gives more frames, **Quality** a sharper picture.
2. **Press Ctrl+Alt+P in the game** to show the performance overlay: your frame rate and how hard the GPU and CPU
   work.
3. **Try a busy scene,** where frames matter: a race start with a full grid, city traffic in a truck sim, a big
   airport in X-Plane.
4. **Still short of frames? Lower the periphery resolution** (the outer part of your view) first: you see it out of
   the corner of your eye, so it costs the least picture quality.
5. **Change things while you play** with the headset menu: **Ctrl+Alt+arrow keys** (up and down pick a setting,
   left and right change it). Changes are saved.

| Keys | What it does |
| --- | --- |
| Ctrl+Alt+arrows | Open and use the headset menu |
| Ctrl+Alt+P | Show or hide the performance overlay |
| Ctrl+Alt+V | Open or close the helmet visor (when the helmet overlay is on) |

You can set wheel, joystick or controller buttons for these in **Headset › Hotkeys**. Every setting, what it does and what to try first:
[Settings guide](docs/guides/settings.md). Le Mans Ultimate players chasing frames: the
[LMU optimization guide](docs/LMU-OPTIMIZATION-GUIDE.md).

## Getting help

1. **Open Game › Launch checks in the launcher.** It explains most problems and how to fix them.
2. **Check [Troubleshooting](docs/guides/troubleshooting.md)** for the common ones: the game starts on the desktop
   instead of in VR, Sim-QVFR doesn't seem to be on, the picture looks zoomed in, frames are no better.
3. **Still stuck? Use Report an issue…** in the launcher's **Game › Get help**. Describe what happened and it
   collects the logs and opens a GitHub issue for you (you need a free GitHub account).

**Updates:** a dot by the version at the bottom of the launcher's game list means a new version is out. Click it
to update; your settings are kept.

## All guides

**Setting up**
- [Headset setup](docs/guides/headsets.md): Quest and Virtual Desktop, Pimax and Varjo, SteamVR, eye tracking
- [Settings guide](docs/guides/settings.md): every launcher setting in plain words, and the optional extras
- [Online play and anti-cheat](docs/guides/online-play.md)
- [Troubleshooting](docs/guides/troubleshooting.md)

**Your game**
- [Le Mans Ultimate](docs/guides/le-mans-ultimate.md), and the
  [LMU optimization guide](docs/LMU-OPTIMIZATION-GUIDE.md) for the most frames
- [Assetto Corsa EVO](docs/guides/assetto-corsa-evo.md)
- [Assetto Corsa Competizione](docs/guides/assetto-corsa-competizione.md): its one-time
  [VR setup through OpenComposite](docs/acc-opencomposite.md) and the optional
  [Foveated DLSS](docs/guides/acc-foveated-dlss.md)
- [X-Plane 12](docs/guides/x-plane-12.md)
- [American Truck Simulator and Euro Truck Simulator 2](docs/guides/truck-simulators.md)

**For the curious**
- [How it works](docs/guides/how-it-works.md): foveated rendering, multi-view rendering, what Sim-QVFR touches in
  each game

## Show your appreciation

If Sim-QVFR has given you smoother VR, extra FPS, or room to turn up the resolution, please consider donating.

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

Thanks to ecstra for [ACEvoPerf](https://github.com/Williem3/ACEvoPerf), open source under the MIT licence.
Its way of finding Assetto Corsa EVO's engine settings by name solved the problem of turning on the game's own
single-pass stereo, which QVFR in Assetto Corsa EVO is built on, without depending on addresses that change with
each game update.

## Licence

Sim-QVFR is free to use, change and share for any **noncommercial** purpose under the
[PolyForm Noncommercial License 1.0.0](LICENSE.md). Commercial use needs the author's permission.

**Credit is required.** If you share any part of Sim-QVFR, changed or not, include `LICENSE.md`
(or a link to it) and this line:

```
Required Notice: Copyright (c) 2026 Williem3 (https://github.com/Williem3/Sim-QVFR)
```

Third-party parts keep their own licences: see [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md). Releases built
with DLSS contain NVIDIA's DLSS SDK (its NGX library in `sim-qvfr.dll`, and `nvngx_dlss.dll`). These parts are
NVIDIA's and aren't covered by the PolyForm licence. Their terms are in
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md), with NVIDIA's licence in the release's `licenses` folder. In
short: use them only as part of Sim-QVFR; don't modify, reverse engineer or redistribute them (to share
Sim-QVFR, share the link to its release); NVIDIA gives no warranty for them. Releases with AMD FSR include AMD's
FidelityFX files under AMD's licence (`licenses\AMD-FidelityFX-SDK-LICENSE.md`).

## For developers

Building from source, settings files, test tools and how the layer works are in
[CONTRIBUTING.md](CONTRIBUTING.md).
