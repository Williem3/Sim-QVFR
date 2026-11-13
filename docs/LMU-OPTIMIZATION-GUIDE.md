# Le Mans Ultimate: QVFR optimization guide

How to get the most frames out of Le Mans Ultimate (LMU) with Sim-QVFR, on both the graphics card (GPU) and the
processor (CPU). It covers every Sim-QVFR setting that changes performance in LMU, what each one costs, and what to
turn down first.

All settings named here are in **sim-qvfr-launcher.exe**. Most also appear in the **headset menu**
(Ctrl+Alt+arrow keys), so you can change them while you drive.

## Contents

1. [Quick start](#1-quick-start)
2. [Find out what holds you back: CPU or GPU](#2-find-out-what-holds-you-back-cpu-or-gpu)
3. [If the GPU holds you back](#3-if-the-gpu-holds-you-back)
4. [If the CPU holds you back](#4-if-the-cpu-holds-you-back)
5. [DLSS settings and their cost](#5-dlss-settings-and-their-cost)
6. [Settings that cost little or nothing](#6-settings-that-cost-little-or-nothing)
7. [When performance is worse than it should be](#7-when-performance-is-worse-than-it-should-be)
8. [Cost table](#8-cost-table)

---

## 1. Quick start

1. **Open the launcher, pick LMU in the game list and go to Overview.** Pick the **Performance**, **Balanced** or
   **Quality** profile. Start with **Balanced**.
2. **Turn on the performance overlay** (Headset tab, **Performance overlay**, or **Ctrl+Alt+P** in the game).
   It shows your frame rate and how long the CPU and GPU take for each frame.
3. **Drive a busy session:** a race start with a full grid, at night or in the rain if you race in those. An empty
   track in daylight tells you very little.
4. **Read what limits you** (section 2), then follow section 3 (GPU) or section 4 (CPU).
5. **Change one setting at a time**, on the same car and track, and compare. The launcher's **Diagnostics** page
   (Game tab) shows your last session next to the one before, with the settings you changed in between.

---

## 2. Find out what holds you back: CPU or GPU

A frame must be ready within the headset's **frame budget**: 1000 ms divided by its refresh rate.

| Headset refresh rate | Frame budget |
| --- | --- |
| 72 Hz | 13.9 ms |
| 80 Hz | 12.5 ms |
| 90 Hz | 11.1 ms |
| 120 Hz | 8.3 ms |

If a frame takes longer, the headset shows an old frame again or makes one up (motion smoothing or reprojection).
That looks like wobble on moving edges, even when the average frame rate looks fine.

**How to tell which one is slow:**

- **In the headset:** the performance overlay shows the CPU time and the GPU time of each frame. Whichever is
  closer to the frame budget is the one holding you back.
- **After a session:** the launcher's **Diagnostics** page shows **Limited by**: the game's render thread (CPU),
  the GPU, or nothing (you're at the headset's refresh rate). **Slow frames** shows how long your slowest frames
  take: when that's above the budget, you feel stutter.

**Why it matters:** lowering resolution only helps when the **GPU** limits you. If the **CPU** limits you, a lower
resolution gives you no more frames; you only lose sharpness. LMU in VR is often limited by the CPU in a busy race,
so check before you turn the picture down.

**Fair comparisons:** test on the same car, track, weather and time of day, with the same number of cars. A race
start with 20 cars costs far more than a practice session on your own.

---

## 3. If the GPU holds you back

Work down this list. The first items save the most for the least you'll notice.

### 3.1 Periphery resolution (Picture tab, Resolution)

The wide view around the focus area. You see it with your side vision, so it's the first thing to lower. It saves
GPU time and video memory roughly in line with its pixels: going from 30% to 20% removes about a third of the
periphery's pixels.

- **Eye tracking:** 15-25% works well, as your eyes keep the sharp area where you look.
- **No eye tracking (fixed focus):** 25-35%, as your eyes move off the centre more often.

The **Preview** card shows the pixels per frame of your settings, so you can see what a change saves.

### 3.2 Focus area size (Picture tab, Foveation)

**Focus width** and **Focus height** set the size of the sharp area. Its cost grows with its area: width times
height times density. A small area can take a high density cheaply.

- **Eye tracking:** around 25-30% each way.
- **Fixed focus:** larger (35-42% wide, 30-38% tall), as your eyes leave the centre.
- **Feather** (how the sharp area blends into the periphery) and **Corner radius** cost nothing: raise them to hide
  the edge instead of making the area bigger.

### 3.3 Focus density (Picture tab, Resolution)

How sharp the focus area is, as a share of the headset's own pixel density.

- **100-140%** is the useful range when Sim-QVFR composes the picture (SteamVR and most headsets). Above about 130%
  it smooths edges more but adds little real detail, as the image is resampled to the headset's own resolution.
- **With runtime composition** (Pimax and Varjo, **Let the runtime compose** on the Rendering card), a higher density
  does show more detail, as the headset's software draws the focus area at full resolution.

### 3.4 DLSS (Picture tab, DLSS) — NVIDIA RTX 20 series and newer

LMU has no DLSS of its own; Sim-QVFR adds it. DLSS **costs** GPU time on its own (it replaces anti-aliasing and
can upscale), so how it pays off depends on the next two settings. See section 5 for every DLSS option.

The two that save GPU time:

- **Turn LMU's MSAA off** (in LMU's own graphics settings) once DLSS is on. DLSS then does all the anti-aliasing.
  In testing this saved about **1.5 ms** of GPU time a frame against 8x MSAA (RTX 5070 Ti).
- **Focus views: Quality or Balanced** instead of DLAA. The focus area is drawn at 67% or 58% each way and upscaled:
  less GPU time, with distant textures in the focus area a little softer.

### 3.5 Crop the view (Picture tab, View & helmet)

Stops LMU drawing the edges of each eye that the lens or a helmet hides anyway. It saves GPU time and video memory in
line with the pixels cut. A little at the **Top**, **Bottom** and **Outer side (temple)** is usually invisible in the
headset. Turn on **Show a helmet** if you'd like the cut edges to look like a visor.

Use this crop, not LMU's own VR **FOV scale**: leave that at 100% x 100%. With any other value Sim-QVFR shows a
zoomed-in picture or a picture inside the picture (a known bug, to be fixed). If you had LMU's Y scale at 90%, about
8% off the **Top** and **Bottom** here gives a similar view.

### 3.6 LMU's own render scale and FSR

LMU's VR render scale and FSR work with Sim-QVFR. They lower every view's resolution on top of Sim-QVFR's sliders,
and FSR upscales each view. Prefer Sim-QVFR's sliders first: they lower the periphery, which you notice least, where
LMU's render scale lowers the focus area too.

### 3.7 LMU's graphics settings

Shadows, reflections, post-processing and MSAA in LMU's own menus all cost GPU time as they do without Sim-QVFR.
Change them in LMU as you normally would. With Sim-QVFR's DLSS on, keep MSAA off (3.4).

---

## 4. If the CPU holds you back

With a CPU limit, the GPU has time to spare: turning the picture **up** may cost you nothing, and turning it down
won't help. Sim-QVFR already does the biggest CPU saving for you: **multi-view rendering**.

### 4.1 Multi-view rendering (automatic)

Sim-QVFR's focus views would normally need LMU to draw each eye twice. Instead, each eye's pass also draws its focus
view, so LMU only does its own two eye passes. This saves CPU time on the game's render thread, and it's always on.

- **NVIDIA RTX 20 series and newer** use NVIDIA's multi-view rendering. The overlay shows **MVR**.
- **Other cards** (AMD, Intel, older NVIDIA) use Sim-QVFR's own, which draws every object twice in one call. The
  overlay shows **SMVR**. In testing it cost about 0.1 ms more CPU and about 0.4 ms more GPU a frame than NVIDIA's
  (RTX 5070 Ti), and still saved around 1.5 ms of CPU against drawing the focus views separately.
- **Eyes only** (yellow, in the overlay) means the focus views aren't being made: both eyes are drawn at full
  resolution, which is sharp but costs much more. See section 7.

### 4.2 Other cars' motion (Picture tab, DLSS, advanced options)

With DLSS on, Sim-QVFR tracks each other car's own motion so DLSS keeps passing cars clean instead of smearing them.
In a full field it costs about **0.5 ms of CPU** and **0.2-0.5 ms of GPU** a frame. It's on by default.

- **CPU-limited in big races?** Turn it off for that headroom. Cars alongside and ahead may smear a little.
- It changes at the **next launch**, and costs nothing with DLSS off.

### 4.3 The performance overlay

Showing the overlay costs a little CPU time. Turn it off (Ctrl+Alt+P) once you've finished tuning.

### 4.4 Run with anti-cheat (Game tab, Online racing)

Starting LMU through its anti-cheat (Easy Anti-Cheat) can make **everything** slower on the CPU. One tester measured
LMU's CPU time at about 2.5-3 times its normal value under the anti-cheat, with or without Sim-QVFR doing anything.
If you're CPU-limited and drive offline, keep **Run with anti-cheat** off. (It is also at your own risk: see the
README.)

### 4.5 LMU's own CPU load

The number of cars on track, the number of cars LMU draws, mirrors and shadow and reflection detail all add CPU work
to LMU's render thread, with or without Sim-QVFR. If you're still CPU-limited, lower those in LMU's settings first.
Each mirror and each extra car drawn is extra work on the CPU, every frame.

---

## 5. DLSS settings and their cost

On the **Picture** tab, **DLSS** card (NVIDIA RTX 20 series and newer). Changes apply while you drive, each with a
short stutter, unless the table says otherwise. Costs are from an RTX 5070 Ti; scale them for your card.

| Setting | Choices | What it does | Cost |
| --- | --- | --- | --- |
| **Use DLSS** | On / Off | DLAA on the periphery, DLAA or an upscale on the focus area. | About **2 ms GPU** for all four views at DLAA (preset K). Pays back when you turn LMU's MSAA off (about 1.5 ms). |
| **Focus views** | DLAA, Ultra quality (77%), Quality (67%), Balanced (58%), Performance (50%) | DLAA draws the focus area at full size. The others draw it smaller and upscale it. | Lower = less GPU time, softer distant textures in the focus area. |
| **Focus preset** | Default, K, M | NVIDIA's model for the focus area. Default is K, or M at Performance. | **M costs much more** than K (about 2.5 times in testing). |
| **Periphery preset** | K, F | K is steadier; F flickers a little more on edges. | **F costs about half** of K. Fine with eye tracking; keep K with a fixed focus. |
| **Texture sharpening** | Off, Focus only, Focus and periphery | Samples textures a little sharper where DLSS works. The gain is small, most in the periphery. | Almost nothing. Turned on from Off, it starts at the next launch. If fine patterns shimmer, go a step lower. |
| **Other cars' motion** | On / Off | Keeps passing cars clean under DLSS. | About **0.5 ms CPU + 0.2-0.5 ms GPU** in a full field. Next launch. |
| **DLSS-NR** | On / Off (own card) | NVIDIA's neural rendering: reworks lighting and materials. Experimental. Needs your own `nvngx_dlssnr.dll`. | **Heavy:** about 2 ms of GPU time and 130 MB of video memory per million pixels it works on, per view. **Periphery too** adds at least 2 ms per eye. |

**Cheapest DLSS that still looks good:** DLSS on, LMU's MSAA off, **Focus views: Quality**, **Periphery preset: F**
(with eye tracking), **Focus preset: Default**, **Other cars' motion** on unless you're CPU-limited.

**Best-looking DLSS:** DLSS on, LMU's MSAA off, **Focus views: DLAA**, **Periphery preset: K**, **Texture sharpening:
Focus and periphery**, **Other cars' motion** on.

---

## 6. Settings that cost little or nothing

Use these freely to make the picture look better:

| Setting | Where | Cost |
| --- | --- | --- |
| **Feather**, **Corner radius**, **Focus shape** | Picture tab, Foveation | None. They hide the edge of the sharp area. |
| **Gaze dead-zone**, **Gaze offset**, **Vertical position** | Picture tab, Foveation | None. |
| **Sharpening** | Picture tab, Resolution | A little GPU time. Crisper edges; too much makes the periphery shimmer. |
| **Show a helmet** | Picture tab, View & helmet | Nothing for the game; the headset composites one extra layer. |
| **Let the runtime compose** | Picture tab, Rendering (Pimax, Varjo) | Usually a sharper focus area for the same settings. With **Sharpening** on, the periphery is sharpened on a copy, which costs a little more. |
| **Show focus outline** | Picture tab, Foveation | None, but turn it off to drive. |

---

## 7. When performance is worse than it should be

- **The overlay says Eyes only (yellow).** The focus views aren't being made, so both eyes are drawn at full
  resolution: a sharp picture, but a far heavier one. Sim-QVFR tries again by itself, at once after you change a
  graphics setting in LMU and otherwise every 20 seconds at most. On a card that can't do multi-view rendering at all
  it stays that way for the session. If it keeps happening, send a report (launcher, Game tab, **Get help**, **Report
  an issue**): the log says why.
- **Good average frame rate, but it still stutters.** Look at **Slow frames** on the Diagnostics page. If they're over
  the frame budget, lower whichever of the CPU or GPU limits you, or try a lower headset refresh rate.
- **Everything is slow on the CPU, even in menus.** Check **Run with anti-cheat** is off (section 4.4), and that
  nothing heavy runs in the background (a browser with video, a game recorder, another game's launcher).
- **No change after lowering resolution.** You're CPU-limited (section 4).
- **A change didn't seem to do anything.** Some settings apply at the **next launch**: the launcher marks each card
  **Live** or **Next launch**. Settings marked "Now when lowered" apply at once when lowered, but raising them above
  what the game started with waits for the next launch.
- **You want to see what QVFR gives you.** On the Diagnostics page, **Next launch without QVFR** starts LMU once
  without foveated rendering, still measuring, so you can compare the same race with and without it.

---

## 8. Cost table

Measured on an NVIDIA RTX 5070 Ti with LMU 1.4.2.0. Your numbers will differ with your card, your headset's resolution
and the session; use them to compare settings, not as promises.

| Setting | GPU | CPU | When it applies |
| --- | --- | --- | --- |
| Periphery resolution | In line with its pixels: the biggest saving | None | Live when lowered |
| Focus density × focus size | In line with the focus area's pixels | None | Live when lowered |
| Crop the view | In line with the pixels cut (saving) | None | Live |
| DLSS on (four views, DLAA, preset K) | About +2 ms | Small | Live |
| LMU's MSAA off with DLSS on | About −1.5 ms against 8x MSAA | — | LMU's settings |
| DLSS Focus views below DLAA | Less, the lower the mode | None | Live |
| DLSS Focus preset M | About 2.5 times preset K | None | Live |
| DLSS Periphery preset F | About half of K | None | Live |
| Texture sharpening | Almost nothing | About 0.03 ms | Live (from Off: next launch) |
| Other cars' motion | +0.2 to 0.5 ms in a full field | About +0.5 ms | Next launch |
| DLSS-NR | About +2 ms per million pixels, per view | Small | Live |
| Sim-QVFR's multi-view (SMVR) instead of NVIDIA's (MVR) | About +0.4 ms | About +0.1 ms | Automatic |
| Sharpening | A little | None | Live |
| Performance overlay | — | A little while it shows | Live |
| Run with anti-cheat | — | Up to about 2.5-3 times LMU's CPU time (one tester) | Next launch |

Found a setting that made a big difference on your PC, better or worse? Tell us on the
[issues page](https://github.com/Williem3/Sim-QVFR/issues) with your graphics card, headset and the two sessions from
the Diagnostics page.
