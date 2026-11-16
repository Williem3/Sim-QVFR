# Foveated DLSS in Assetto Corsa Competizione (optional, advanced)

> **Is this for me?** Only if you play **ACC** with an **NVIDIA RTX** card and are comfortable with a setup that takes
> two downloads, one installer and some file copying. Sim-QVFR already works in ACC without it. It isn't used in Le
> Mans Ultimate (Sim-QVFR has [its own DLSS](le-mans-ultimate.md#dlss-and-fsr) there) or Assetto Corsa EVO.

[Back to the ACC guide](assetto-corsa-competizione.md) · [Back to the README](../../README.md)

Sim-QVFR can use [Cheeky Foveated DLSS](https://github.com/ClarkCheekyKent/CheekyFoveatedDLSS) by
[ClarkCheekyKent](https://github.com/ClarkCheekyKent), **integrated with his permission**. It adds:

- **Foveated DLSS:** cheaper DLSS on the outer view, full-quality DLSS in the sharp area.
- **Extra sharp-area options (experimental):** supersampling, a fix for motion blur, and DLSS-NR.

Cheeky Foveated DLSS is a separate, free download (GNU GPL v3). It isn't included with Sim-QVFR.

## What to download

You need **two** downloads. On each page, scroll down to **Assets** and download the ZIP named here:

| Download | Where | What it's for |
| --- | --- | --- |
| `CheekyFoveatedDLSS-0.6.3-Standalone-sim-qvfr-support.zip` | [Cheeky Foveated DLSS v0.6.3-sim-qvfr-support](https://github.com/Williem3/CheekyFoveatedDLSS/releases/tag/v0.6.3-sim-qvfr-support) | The DLSS files themselves. This build includes the fixes ACC needs (the official build crashes ACC at start). Older builds such as `v0.6.3-ace.2` don't work with ACC. |
| `CheekyFoveatedDLSS-0.6.3-Standalone-TEST-SIGNED.zip` | [Cheeky Foveated DLSS v0.6.3](https://github.com/ClarkCheekyKent/CheekyFoveatedDLSS/releases/tag/v0.6.3) (official) | Only for its installer, `CheekyOpenXRSetup.exe`. |

Once an official Cheeky release includes the fixes, its own download will replace both.

## Setting it up

1. **Turn on DLSS** in ACC's graphics settings (any mode). Foveated DLSS builds on the game's own DLSS.
2. **Update ACC's DLSS.** ACC comes with DLSS 2.2.15, which is too old: the sharp area works but the cheaper outer
   view doesn't. Replace `nvngx_dlss.dll` in
   `Assetto Corsa Competizione\AC2\Plugins\DLSS\Binaries\ThirdParty\Win64` with DLSS 310 or newer (a tool such as
   DLSS Swapper does it for you). **Launch checks** tells you when it's too old and shows you the file. Steam's
   **Verify integrity of game files** puts the original back.
3. **Run Cheeky's installer.** In the official ZIP, open the `OpenXR` folder and run `CheekyOpenXRSetup.exe`. It
   installs Cheeky's OpenXR layer on your PC (unlike Sim-QVFR itself, which installs nothing). To remove it later,
   see [Cheeky Foveated DLSS's page](https://github.com/ClarkCheekyKent/CheekyFoveatedDLSS).
4. **Copy Cheeky's folder into Sim-QVFR.** In the sim-qvfr-support ZIP there's a folder called
   `CheekyFoveatedDLSS`. Copy that whole folder into the `layer` folder inside your Sim-QVFR folder. (To keep it
   somewhere else, use **Choose…** in the launcher's **Picture › Foveated DLSS** card to point to
   `CheekyFoveatedDLSSRuntime.dll`.)
5. **Don't copy `dxgi.dll` or `version.dll`** from Cheeky's ZIPs into the game's folder. If they're already there,
   delete them. Sim-QVFR starts Cheeky itself, and having both makes it fail.
6. **If you use OpenXR Toolkit:** it has to be *above* Cheeky in the list of OpenXR layers, or the game crashes at
   start. **Game › Launch checks** tells you if this is wrong; the free
   [OpenXR API Layers GUI](https://github.com/fredemmott/OpenXR-API-Layers-GUI) fixes the order.
7. **Turn it on:** in **Picture › Foveated DLSS**, turn on **Use Cheeky Foveated DLSS**. The card tells you whether it
   found everything, and after one launch it shows Cheeky's version.
8. **Click Launch.**

**For DLSS-NR,** put NVIDIA's `nvngx_dlssnr.dll` in the same `CheekyFoveatedDLSS` folder (ACC doesn't come with one).

## Good to know

- Most DLSS settings change straight away, in the launcher or the headset menu. With Foveated DLSS on, resolution
  changes take effect the next time you start the game.
- **Show the center region** draws an outline around the full-quality area, so you can check it follows your eyes.
- Cheeky Foveated DLSS always stays off with Le Mans Ultimate's **Run with anti-cheat**.
