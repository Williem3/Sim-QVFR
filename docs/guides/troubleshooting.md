# Troubleshooting

**First, always:** open the launcher's **Game › Launch checks**. It lists anything that would stop Sim-QVFR working,
with how to fix it. The status in the top bar shows the first problem, and after you click Launch it tells you
whether the game started with Sim-QVFR.

[Back to the README](../../README.md)

## Common problems

**The game starts on the desktop, not in the headset**
- Start your headset's software (and SteamVR, if that's your OpenXR runtime) **before** clicking Launch. See
  [Headset setup](headsets.md).
- Assetto Corsa Competizione needs its one-time [VR setup through OpenComposite](../acc-opencomposite.md).
- You don't need VR launch options of your own: the launcher adds what each game needs.
- Still flat? Use **Report an issue…** (below): the logs show what happened.

**The game runs, but Sim-QVFR doesn't seem to be on**
- Start the game **from the launcher**, not from Steam or a desktop shortcut.
- Check your game's version: each game needs exactly the version the [README](../../README.md#is-it-for-me) names
  (Le Mans Ultimate 1.4.2.0, Assetto Corsa Competizione 1.10.4, X-Plane 12 12.4.3-r2, American Truck Simulator
  1.62.2.2, Euro Truck Simulator 2 1.62.3.3), on the game's normal branch, not a beta. **After a game update**, Sim-QVFR turns itself off and the game runs as
  normal until a new Sim-QVFR comes out; your headset shows a message for the first 30 seconds.
- In Assetto Corsa EVO, ACEvoPerf users: see the [ACE guide](assetto-corsa-evo.md).

**The picture looks zoomed in, or there's a picture inside the picture (Le Mans Ultimate)**
- Set LMU's own **VR FOV scale** back to **100% x 100%**. To trim the view's edges, use Sim-QVFR's **View crop**
  instead. See the [LMU guide](le-mans-ultimate.md).

**Frames are no better, or worse, than before**
- Your CPU may be what holds you back: Sim-QVFR saves graphics-card work. X-Plane and the truck sims in busy
  scenes often lean on the CPU. The performance overlay (Ctrl+Alt+P) shows
  which one is busy. For LMU, the [optimization guide](../LMU-OPTIMIZATION-GUIDE.md#2-find-out-what-holds-you-back-cpu-or-gpu)
  explains how to tell.
- Close other heavy programs, and turn off the game's own foveated rendering (Assetto Corsa EVO) or any other
  foveated-rendering mod.
- Try the **Performance** profile on the Overview, then lower **Periphery resolution**.

**The sharp area doesn't follow my eyes**
- Your headset may have no eye tracking (Quest 2 and 3 don't): the sharp area stays in the middle.
- With eye tracking, check it's on in your headset's software and that **Picture › Foveation** is set to
  **Eye tracked**. See [Eye tracking](headsets.md#eye-tracking).
- In Le Mans Ultimate on Pimax or Varjo (the runtime composes the views), the headset's own eye tracking places it.

**The launcher can't find my game**
- Make sure the game is installed and, for a Steam game, that Steam is running.
- Use **Choose folder…** in **Game › Launch checks** and pick the game's folder.

**My settings aren't saved**
- Move the Sim-QVFR folder out of `Program Files` (for example to `Documents\Sim-QVFR`).

**Assetto Corsa Competizione closes at start with "SECURE CRT: Invalid parameter detected"**
- Windows' ransomware protection is blocking ACC: see
  [If something goes wrong](../acc-opencomposite.md#if-something-goes-wrong).

## Asking for help

- **Report an issue…** in the launcher's **Game › Get help**: pick the kind of problem, describe what happened, and it
  bundles up the logs and opens a GitHub issue for you (you need a free GitHub account). Drag the ZIP's tile onto
  the issue to attach it, then send. Names and account IDs are taken out of the game's own crash reports before
  they go in the ZIP.
- **Someone asked for your logs?** **Save the logs as a zip** (under **Other ways to share**, in the same Get help
  card) makes the same ZIP without opening an issue.
- **Asked for detailed logs?** Switch on **Detailed logs** under **Game › Diagnostics**, play the problem again,
  then save the logs as a zip. They last three launches and then turn themselves off.
- **Don't report problems to the game's makers** that happen while Sim-QVFR is running.

## Good to know

- **Removing Sim-QVFR:** delete its folder.
- **Running the launcher as administrator** (or with UAC off) works too. Each launch then adds Sim-QVFR's own
  entries to Windows' list of OpenXR add-ons for all users and takes them out again when the game closes. Nothing
  else in that list changes. A normal double-click is all you need.
- **Disk space:** the launcher keeps the logs of your newest 30 sessions and removes older ones. Frame captures you
  made stay until you press **Clear captures** under **Diagnostics**.
- **Updates:** click the version at the bottom of the launcher's game list (a dot there means an update is out). Its
  **About** panel checks for new versions and installs them; your settings and profiles are kept.
- **Coming from the alpha (ace-quadview)?** The beta is a fresh start: unzip it into a new folder and set your
  options again. The alpha's launcher won't offer the beta as an update, and the beta doesn't read the alpha's
  settings. When you're happy with the beta, delete the old ace-quadview folder.
