# Settings guide

Every setting is in the launcher window (`sim-qvfr-launcher.exe`), and your choices are saved for you. Most can
also be changed in the headset while you play. This guide explains what to set first and what each part of the
launcher does, in plain words.

[Back to the README](../../README.md) · Le Mans Ultimate players: the
[LMU optimization guide](../LMU-OPTIMIZATION-GUIDE.md) goes further.

## Contents

- [Start here](#start-here)
- [Words you'll see](#words-youll-see)
- [The launcher's tabs](#the-launchers-tabs)
- [Changing settings while you play](#changing-settings-while-you-play)
- [Optional extras](#optional-extras)
- [Profiles](#profiles)

## Start here

1. **Pick your game** in the list on the left. Each game keeps its own settings.
2. **On the Overview, pick Balanced.** **Performance** gives more frames, **Quality** a sharper picture. Try the
   others once you've played with Balanced.
3. **Try a busy scene with the performance overlay on** (Ctrl+Alt+P) and watch the frame rate: a race start, city
   traffic in a truck sim, a big airport in X-Plane.
4. **Short of frames?** Change one thing at a time, in this order:
   1. Lower **Periphery resolution** (**Picture › Resolution**). It's the outer view, seen out of the corner of your
      eye, so it costs the least picture quality.
   2. Make the **focus area** smaller (**Picture › Foveation**), especially with eye tracking.
   3. Lower **Focus density**.
5. **Frames to spare?** Raise **Focus density** first: it makes what you look at sharper.

Hover over the **ⓘ** next to any setting in the launcher to see what it does and what it costs.

## Words you'll see

| Word | What it means |
| --- | --- |
| **Focus area** (sharp area) | The part of the view drawn at full sharpness: the middle, or where your eyes look with eye tracking. |
| **Periphery** | Everything around the focus area, drawn at a lower resolution. |
| **Periphery resolution** | How sharp the periphery is, as a share of your headset's recommended resolution. Lower = more frames. |
| **Focus density** | How sharp the focus area is. 100% matches your headset's own sharpness where you look; above 100% is sharper still (more detail and smoother edges and text) and costs more, the image sent to the headset growing with it; below is softer. 50% to 400%. |
| **Eye tracked / Fixed** | Whether the focus area follows your eyes or stays in the middle. |
| **QVFR** | Quad View Foveated Rendering: the method Sim-QVFR uses (two views per eye, one wide and soft, one small and sharp). |

## The launcher's tabs

- **Overview:** the **Performance / Balanced / Quality** profiles, and how your last sessions ran.
- **Picture:** **Resolution** (periphery resolution, focus density, sharpening), **Foveation** (the focus area's size
  and shape, and whether it follows your eyes), plus per-game cards such as DLSS.
- **Headset:** the performance overlay, the helmet overlay, **Hotkeys** (keyboard keys, or wheel, joystick or
  controller buttons, for the headset menu and the overlay).
- **Game:** **Launch checks** (anything that would stop Sim-QVFR working, with the fix), **Diagnostics** (your last
  session compared with the one before), **Get help**, and game-specific options.

Each card says when its settings apply:

- **Changes now:** reaches the game while it runs.
- **Now when lowered:** lowering it reaches the running game at once; raising it above what the game started with
  waits for the next start.
- **Next game start:** takes effect the next time you launch.

**Find a setting** (Ctrl+K) jumps to any setting by its name, and **Undo** (Ctrl+Z) takes back the last change.

## Changing settings while you play

Press **Ctrl+Alt+arrow keys** (or the button you set in **Headset › Hotkeys**) to open the **headset menu**.
Up and down choose a setting, left and right change it. Changes apply straight away and are saved.

| Keys | What it does |
| --- | --- |
| Ctrl+Alt+arrows | Open and use the headset menu |
| Ctrl+Alt+P | Show or hide the performance overlay (frame rate, GPU and CPU load) |
| Ctrl+Alt+V | Open or close the helmet visor (when the helmet overlay is on) |

## Optional extras

These are off until you turn them on.

- **Performance overlay** (all games): a small panel in your headset with your frame rate and how hard the GPU and
  CPU work. **Ctrl+Alt+P** in the game turns it on and off at any time, or the launcher's **Headset › Performance
  overlay**.
- **Sharpening** (all games; **Picture › Resolution** or the headset menu): a crisper focus area. If the view looks soft,
  start low and raise it a little at a time while you play; too much makes it shimmer. X-Plane 12 sharpens on its own too, so start lower there.
- **View crop** (Le Mans Ultimate, Assetto Corsa EVO, Assetto Corsa Competizione): stops the game drawing the edges
  of your view that a helmet would hide anyway, for extra frames.
- **Helmet overlay** (Le Mans Ultimate, Assetto Corsa EVO, Assetto Corsa Competizione): a helmet visor around your
  view. You can swap in your own helmet images.
- **Eye off** (Le Mans Ultimate; **Headset**): for a player who sees with one eye. The chosen eye shows black and the
  game draws much less for it. With both eyes seeing, leave it at None.
- **DLSS** (Le Mans Ultimate, Assetto Corsa EVO; NVIDIA RTX) **or FSR** (Le Mans Ultimate, other cards): see the
  [LMU guide](le-mans-ultimate.md#dlss-and-fsr) and the [ACE guide](assetto-corsa-evo.md#good-to-know).
- **Foveated DLSS** (Assetto Corsa Competizione, NVIDIA RTX): see [ACC Foveated DLSS](acc-foveated-dlss.md).

## Profiles

The profile menu in the top bar saves your own settings under a name. **Manage profiles…** there can **Share** a
profile as a line of text to swap with friends, and **Paste** one they sent you.
