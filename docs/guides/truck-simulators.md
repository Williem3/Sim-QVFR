# American Truck Simulator and Euro Truck Simulator 2

> **Experimental.** Support for SCS Software's truck sims is new. Tell us how it runs on your PC
> (the launcher's **Game › Get help**).

[Back to the README](../../README.md)

## Getting started

1. **Use the game's normal Steam branch** (no beta). Sim-QVFR works with **American Truck Simulator 1.62.2.2** and
   **Euro Truck Simulator 2 1.62.3.3**; a beta branch or any other build is a different game file, and Sim-QVFR then
   stays off and the game runs as usual.
2. **Start your headset's software** (see [Headset setup](headsets.md)) and Steam.
3. **Pick the game in the launcher and click Launch.** The launcher starts it in VR mode for you (it switches the
   game's experimental VR on), so Sim-QVFR doesn't need any launch options of your own in Steam.
4. **Leave the game's own Scaling setting at 100%.** For a sharper dash and mirrors, raise Sim-QVFR's **Focus
   density** instead (**Picture › Resolution**, or the headset menu while you drive): it sharpens what you look at
   without paying for the whole view.

A fresh install starts the truck sims at a periphery resolution of 40%, a focus density of 150% and a focus area a
quarter of the view each way. Without eye tracking (Quest 2 and 3, for example) the focus area stays in the middle:
make it larger in **Picture › Foveation** if you notice its edge when you look at the mirrors.

## Good to know

- **Online play** (TruckersMP, Convoy) is at your own risk: Sim-QVFR doesn't guarantee you won't be kicked or banned; see
  [Online play and anti-cheat](online-play.md).
- **Mods** you load in the game's mod manager are the game's own business; Sim-QVFR doesn't touch them.
- **Graphics cards:** NVIDIA RTX cards use NVIDIA's multi-view rendering, other cards Sim-QVFR's own (any card with
  current Direct3D 11 drivers). AMD and Intel cards haven't been tested yet: reports are welcome.
- **Busy scenes,** where frames matter: city traffic and big junctions. The truck sims often lean on the CPU there,
  and Sim-QVFR saves graphics-card work, so check the overlay (Ctrl+Alt+P) to see which one holds you back.

What works: the sharp area (following your eyes on a headset with eye tracking), the headset menu, the performance
overlay and sharpening. Not yet: the view crop and the helmet overlay.
