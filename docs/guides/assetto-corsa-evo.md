# Assetto Corsa EVO

[Back to the README](../../README.md)

## Getting started

1. **Turn off the game's own foveated rendering** in its graphics settings. Sim-QVFR does this job instead.
2. **Choose MSAA or DLSS** (any DLSS mode; DLSS needs an NVIDIA RTX card) for anti-aliasing in the game's settings.
   Other upscalers such as FSR haven't been tested yet: if something looks wrong with one, switch to MSAA and tell us.
3. **Start your headset's software** (see [Headset setup](headsets.md)) and Steam.
4. **Pick Assetto Corsa EVO in the launcher and click Launch.** The launcher starts it in VR mode for you.

Sim-QVFR works with Assetto Corsa EVO's Steam build current when the release came out. After a game update it turns
itself off until a new Sim-QVFR comes out.

## Good to know

- **DLSS:** with DLSS on in the game, Sim-QVFR runs its own DLSS for the sharp area too. Two headset-menu settings
  control it: **Focus views** (full resolution, or drawn smaller and upscaled for more frames) and **Focus preset**
  (NVIDIA's DLSS model; leave it on its default unless you're curious).
- **Changes while you drive:** you can lower the periphery's resolution while you drive, and raise it back to what the
  game started with. Above that, it waits for the next start (the card's **Now when lowered** label; see
  [Settings guide](settings.md#the-launchers-tabs)).
- **View crop and the helmet overlay** work in ACE.
- **Online play** is at your own risk: see [Online play and anti-cheat](online-play.md).

## ACEvoPerf users

ACEvoPerf and Sim-QVFR work together, with one setting to check: **in `acevo_perf.ini`, don't set
`single_pass_stereo = false`.** Leave the line out or set it to `true`. Set to `false`, it turns off what Sim-QVFR
needs, and the sharp area won't appear.

## If something looks wrong: the earlier method

Sim-QVFR's earlier way of running in Assetto Corsa EVO is still there for a while. Try it only if you see a problem
with Sim-QVFR on (a wrong picture, a crash) that doesn't happen without it, and tell us about it with **Report an
issue…** (**Game › Get help**): it will be removed in a later release, so we need to know what it fixes for you.

To use it, pick Assetto Corsa EVO in the launcher and close the launcher. Then open `sim-qvfr.ini` (next to
`sim-qvfr-launcher.exe`) with Notepad and add these two lines at the end (or the second
line under an existing `[experiment]` line):

```
[experiment]
ace_legacy_views4=1
```

Delete the line again to go back.

How Sim-QVFR works in Assetto Corsa EVO: [How it works](how-it-works.md#assetto-corsa-evo).
