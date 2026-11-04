# Assetto Corsa Competizione in VR through OpenComposite

> **Sim-QuadView doesn't support Assetto Corsa Competizione yet.** Support is in development. This guide
> covers the first step it will need: running ACC's VR through OpenXR. Many players run ACC this way
> anyway, because it often runs smoother than through SteamVR.

## Why this is needed

ACC's VR only speaks **OpenVR** (SteamVR's interface) or the Oculus interface. Sim-QuadView works through
**OpenXR**, the newer standard that headset software such as Pimax Play, Meta Quest Link, Virtual Desktop
and Varjo Base provides. **OpenComposite** sits in between: ACC thinks it's talking to SteamVR, and
OpenComposite passes everything on to your headset's OpenXR software.

OpenComposite is a separate free project ([GitLab](https://gitlab.com/znixian/OpenOVR), GNU GPL v3).
Sim-QuadView doesn't include it, and isn't made by its authors.

## What you need

- **Assetto Corsa Competizione** on Steam.
- **Your headset's own software set as the active OpenXR runtime.** Most headset apps have a setting
  for this (Pimax Play, Meta Quest Link, Virtual Desktop, Varjo Base). SteamVR can also be the OpenXR
  runtime, but then you lose most of the benefit.

## Set it up (for ACC only)

This changes one file in ACC's folder. Your other SteamVR games aren't affected.

1. **Close ACC.**
2. **Download OpenComposite's 64-bit DLL:**
   <https://znix.xyz/OpenComposite/download.php?arch=x64&branch=openxr>
   (ACC is a 64-bit game, so use this one, not the 32-bit one.) If it arrives as a ZIP, unzip it: you
   need the `openvr_api.dll` inside.
3. **Open ACC's folder:** in Steam, right-click Assetto Corsa Competizione, then **Manage → Browse
   local files**.
4. **Go to** `Engine\Binaries\ThirdParty\OpenVR\OpenVRv1_5_17\Win64`.
5. **Keep a backup:** rename the `openvr_api.dll` that's there to `openvr_api.dll.original`.
6. **Copy the downloaded `openvr_api.dll` into that folder.**

## Start ACC in VR

1. **Start Steam and your headset's software.** You don't need to start SteamVR.
2. **Start ACC in SteamVR mode:** double-click `acc_steamvr.bat` in ACC's main folder (the one Steam
   opened in step 3). It runs `acc.exe -vr hmd=SteamVR`.
   Don't use `acc_vr.bat`: that's Oculus mode, which goes around OpenComposite.

If it worked, the game appears in your headset and SteamVR doesn't open.

## After an ACC update

Updates can put the original `openvr_api.dll` back. If SteamVR starts with the game again, repeat steps
4 to 6 (rename the new original first).

## Undo it

Either delete OpenComposite's `openvr_api.dll` and rename `openvr_api.dll.original` back to
`openvr_api.dll`, or in Steam right-click ACC, then **Properties → Installed Files → Verify integrity of
game files**, which restores the original.

## If something goes wrong

- **The picture is upside down:** OpenComposite's `invertUsingShaders` option may fix it. It goes in a
  file called `opencomposite.ini` next to OpenComposite's `openvr_api.dll`; see its README (below) for
  how to write it. Spell options exactly: a wrong one makes the game crash at start.
- **A message pops up and the game closes:** ACC asked for something OpenComposite doesn't support yet.
  Undo the change (above) and play through SteamVR.
- **No controller models:** a known OpenComposite gap; it doesn't affect driving.

OpenComposite's own instructions, including a system-wide switch for all your SteamVR games, are in its
[README](https://gitlab.com/znixian/OpenOVR/-/blob/openxr/README.md).
