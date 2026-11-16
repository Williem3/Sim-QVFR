# How it works

For the curious. You don't need any of this to use Sim-QVFR.

[Back to the README](../../README.md)

## Quad View Foveated Rendering

Your eyes only see fine detail in a small area around where you look. Sim-QVFR uses that: instead of drawing each
eye's view once at full resolution, the game draws **two views per eye** (four in all, hence "quad views"):

- a **wide view** (the periphery) covering your whole field of view at a lower resolution, and
- a small **focus view** at full or higher sharpness, in the middle or wherever eye tracking says you look.

Sim-QVFR then puts each eye's two views together, blending the edge between them, and hands the picture to your
headset's software. Far fewer pixels are drawn than for two full-resolution eyes, so the graphics card does less
work.

In Le Mans Ultimate on Pimax and Varjo, Sim-QVFR can instead hand all four views to the headset's own software,
which puts the focus view on the display at its full resolution
([runtime composition](headsets.md#pimax-and-varjo)). In every other game it puts the picture together itself.

**Sharpening** is AMD FidelityFX Contrast Adaptive Sharpening, applied as Sim-QVFR puts the views together: each
pixel is sharpened by how much contrast room it has, so soft detail gains most. Both views get the same strength,
so the focus area's edge stays even. With runtime composition, the periphery is sharpened on a copy.

Sim-QVFR works through OpenXR, as an **API layer**: a small add-on that sits between the game and your headset's
software. It is loaded only when you start a game from the launcher.

## Multi-view rendering

Drawing four views could mean the game doing four passes over the scene, which costs CPU. Where it can, Sim-QVFR
uses **multi-view rendering** instead: each eye's pass also draws that eye's focus view in the same pass, so the game
still does only its own two eye passes.

- **NVIDIA RTX 20 series and newer** use NVIDIA's multi-view rendering (the overlay's **MVR** badge).
- **Other cards** (AMD, Intel, older NVIDIA) use Sim-QVFR's own: every draw is drawn twice in one call
  (instancing), once for the eye and once for its focus view (the **SMVR** badge). It needs a graphics driver that
  lets a vertex shader choose the viewport (Direct3D 11.3). It draws each vertex twice, so it costs a little more GPU
  time. If NVIDIA's driver ever refuses its own during a session, Sim-QVFR's takes over.

## Le Mans Ultimate

Multi-view rendering is always on in LMU and is the only way Sim-QVFR runs there; the earlier way, four passes made
by changing LMU's code, has been removed.

**It doesn't patch LMU's code.** Sim-QVFR works through OpenXR and Direct3D 11 only: it doesn't change, hook or call
any of LMU's own code. In LMU's memory it reads a few values (the cameras, the eye being drawn, the render scale) and
writes nothing. Each view's resolution, and its live changes while you drive, are set in the Direct3D 11 calls: LMU
renders at its own size and Sim-QVFR draws the eye into the part of its image the periphery's resolution needs.

**DLSS** runs through NVIDIA's NGX on LMU's views: DLAA on the periphery, DLAA or an upscale on the focus views,
with Sim-QVFR's own camera jitter and motion vectors. On a PC without an RTX card, AMD FSR runs in its place with
the same inputs.

Diagrams of every part of LMU, Direct3D 11 and OpenXR involved:

- [Multi-view rendering with NVIDIA's MVR](../architecture/LMU-MVR-ARCHITECTURE.svg)
- [Sim-QVFR's own multi-view rendering, on any graphics card](../architecture/LMU-NEUTRAL-MVR-ARCHITECTURE.svg)
- [DLSS (and FSR)](../architecture/LMU-DLSS-ARCHITECTURE.svg)

## Assetto Corsa EVO

Sim-QVFR works at the graphics level: the game draws its own two eyes (with its own single-pass stereo, which
Sim-QVFR turns on), and Sim-QVFR draws the focus views from the game's own drawing work, without changing any of the
game's code. This uses less CPU than the earlier method and the same GPU time for the same picture, and the
periphery's resolution can be lowered while you drive. Finding the game's single-pass stereo setting by name follows
ACEvoPerf's approach (MIT licence, by ecstra).

## Assetto Corsa Competizione

ACC's VR reaches Sim-QVFR through OpenComposite, which turns its SteamVR calls into OpenXR. Sim-QVFR has ACC draw a
focus view for each eye as well as the eye itself, and puts them together. Each eye's focus view is aimed at what
you're looking at, so the two meet at the right distance. ACC's own bloom is shared with the focus views, so glow
looks the same in both.

## X-Plane 12

X-Plane's renderer can draw more than two views in a frame. Sim-QVFR is loaded before X-Plane starts and has it draw
four (each eye and its focus view), then puts them together. **Object detail** adjusts X-Plane's level of detail for
the focus views, which is where its main-thread cost comes from.

## American Truck Simulator and Euro Truck Simulator 2

Sim-QVFR works through the OpenXR and Direct3D 11 layers alone: no game code hooked and no game memory written. The
focus view is drawn by multi-view rendering in the game's own eye passes.

## Building and settings files

Building from source, the settings files and test tools are in [CONTRIBUTING.md](../../CONTRIBUTING.md).
