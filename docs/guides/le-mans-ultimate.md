# Le Mans Ultimate

[Back to the README](../../README.md) · For the most frames: the
[LMU optimization guide](../LMU-OPTIMIZATION-GUIDE.md)

## Must-knows

> **Leave LMU's own VR FOV scale at 100% x 100%.** With any other value, Sim-QVFR shows a zoomed-in picture or a
> picture inside the picture (a known bug, to be fixed). To trim the edges of your view for speed, use Sim-QVFR's
> **View crop** instead (the launcher's **Picture › View & helmet**, or the headset menu). If you had LMU's Y scale
> at 90%, about 8% off the top and bottom gives a similar view.

- **Made for version 1.4.2.0** (the Steam build of 23 October 2026). It also runs on other builds (a game update, a
  beta or alpha branch), DLSS and FSR included; Launch checks then say the build isn't tested yet. If something looks
  wrong there, look for a Sim-QVFR update.
- **Always start LMU from the launcher.** Started from Steam, it runs without Sim-QVFR.
- **Offline by default.** LMU's **Run with anti-cheat** option for online racing **risks a ban on your account**: read
  [Online play and anti-cheat](online-play.md) before you ever turn it on.
- **Works on any graphics card** (NVIDIA, AMD, Intel). Tested mostly on an NVIDIA RTX 5070 Ti so far; reports from AMD
  and Intel cards are welcome.
- **Changes while you drive:** the resolution sliders, the focus area's size and the crop change straight away, in
  the launcher or the headset menu.

What works in LMU: the sharp area that follows your eyes, the headset menu, the performance overlay, the view crop,
the helmet overlay, sharpening, Eye off, and Sim-QVFR's own DLSS (NVIDIA RTX) or FSR (other cards).

## DLSS and FSR

LMU has no DLSS of its own, so Sim-QVFR adds one. It's experimental. **On an NVIDIA RTX card** (20 series and newer)
the launcher shows the **Picture › DLSS** card; **on any other card** it shows **FSR (experimental)** in its place,
with the same options. Both are also in the headset menu.

- **Use DLSS / Use FSR** turns it on. The periphery is then smoothed at its full resolution (NVIDIA calls this DLAA,
  AMD native AA): no upscaling there.
- **Focus views** sets the sharp area: full resolution, or drawn smaller and upscaled (**Ultra quality** 77%,
  **Quality** 67%, **Balanced** 58%, **Performance** 50% of its size each way). Smaller is faster, with softer
  distant textures.
- **LMU's MSAA can go off** with DLSS or FSR on: they do the anti-aliasing then, for more frames.
- Changes apply while you drive, each with a short stutter.

**DLSS details**
- **NVIDIA's `nvngx_dlss.dll` is included** (DLSS 310.9.1). To use a newer one, pick it from a game's folder with
  **Choose…** on the DLSS card (DLSS 310 or newer); **Use included** goes back.
- **Presets** are NVIDIA's DLSS models, named by letters. **Leave them on their defaults** unless you're curious:
  **Periphery preset K** (the default) is the steadier; **F** costs about half as much and shimmers more, which eye
  tracking keeps out of where you look. **Focus preset:** NVIDIA's default for the mode, or K or M.
- **Texture sharpening** (off by default; **Focus only** or **Focus and periphery**): textures a little sharper where
  DLSS works. The gain is subtle. Turned on from Off, it starts at the next launch. If fine patterns shimmer, go a
  step lower.
- **Other cars' motion** (on by default, under the DLSS card's advanced options): keeps passing cars clean instead of
  smearing. In a full field it costs about 0.5 ms of CPU and 0.2-0.5 ms of GPU a frame (at 90 Hz a frame has 11 ms);
  turn it off if you're just short of frames.
  Applies at the next launch.
- **Known limits:** below full resolution, distant textures in the sharp area are softer. Tested on SteamVR and
  Pimax's software.

**FSR details**
- AMD FSR 4 where AMD's software offers it for your card (Radeon RX 9000, and newer drivers on RX 7000 and 6000),
  FSR 3.1 elsewhere. AMD's FSR files are included. The card says which one your PC gets.
- So far tested only as FSR 3.1 on an NVIDIA card; FSR 4 is untested. Reports are welcome.

## Other settings and tools

- **LMU's own render scale and FSR** (LMU's VR graphics settings) work with Sim-QVFR. They lower every view's
  resolution on top of Sim-QVFR's sliders. This is LMU's own FSR, separate from Sim-QVFR's **FSR (experimental)**
  card above.
- **MFAA** (an anti-aliasing option in the NVIDIA Control Panel) works: you can leave it on.
- **Pimax and Varjo:** the headset's software composes the views itself; see [Pimax and Varjo](headsets.md#pimax-and-varjo).
- **VectorXR** (a separate tool that adds frame generation, made-up in-between frames) works alongside. Turn it on in
  the launcher's **Game › OpenXR layers**. Use one frame generator at a time.
- **Not in LMU:** Cheeky Foveated DLSS (it builds on a game's own DLSS, which LMU doesn't have).

## The performance overlay's badge

The overlay (Ctrl+Alt+P) shows a badge beside its CPU heading:

- **MVR** or **SMVR**: Sim-QVFR is drawing the sharp area (by NVIDIA's multi-view rendering, or Sim-QVFR's own on
  other cards). **+ DLSS** while DLSS is running.
- **Eyes only** (yellow): the sharp area isn't being made right now, so both eyes are drawn at full resolution. The
  picture is correct and sharp, but costs more. Sim-QVFR tries again by itself, at once after you change an LMU
  graphics setting and otherwise every 20 seconds at most (a failed try shows as a brief soft blink). On a graphics
  card that can't do it at all it stays that way; the log says why.

How this works, and what Sim-QVFR touches in LMU: [How it works](how-it-works.md#le-mans-ultimate).

## Experimental, not yet tested in the headset

**DLSS-NR** (off by default; the **DLSS-NR** card under DLSS): NVIDIA's neural rendering, which reworks
lighting and materials. It's heavy (about 2 ms of GPU time for every million pixels on an RTX 5070 Ti, so the sharp
area only unless you turn on **Periphery too**) and **you need your own `nvngx_dlssnr.dll`**: it isn't part of
NVIDIA's public SDK, so Sim-QVFR can't include it. A game that ships DLSS-NR has it in its folder; **Choose…** points
Sim-QVFR at it. Not tested in the headset yet.

## LMU-Quad-View-Mod

Dan Suzuki's [LMU-Quad-View-Mod](https://github.com/DSuzuki35/LMU-Quad-View-Mod) inspired this work. Sim-QVFR's LMU
support is written separately and works differently: it draws the views inside the game and puts them together
itself (or on Pimax and Varjo hands them to the headset's software), so you install nothing else, while
LMU-Quad-View-Mod hands them to a quad-views runtime, Quad-Views-Foveated or VectorXR. Try both on your own PC, in
the same car, track and weather, and use the one that runs best for you. Don't run both at once.

Uses NVIDIA DLSS through the NVIDIA NGX SDK. NVIDIA, RTX and DLSS are trademarks of NVIDIA Corporation; Sim-QVFR is
not sponsored or endorsed by NVIDIA. AMD, FSR and FidelityFX are trademarks of Advanced Micro Devices, Inc.
