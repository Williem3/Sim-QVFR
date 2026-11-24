# Online play and anti-cheat

[Back to the README](../../README.md)

## The short version

- **Online play is at your own risk, in every game.** Sim-QVFR doesn't guarantee that a game with online
  multiplayer won't kick or ban you for running it: league racing, online servers, convoys (TruckersMP, Convoy),
  online flying (VATSIM, IVAO) and the like.
- **Le Mans Ultimate** starts offline by default. Its **Run with anti-cheat** option for online racing **risks a ban
  on your account**. If your LMU account matters to you, leave it off.

## Le Mans Ultimate

By default the launcher starts LMU **without** its anti-cheat (Easy Anti-Cheat). LMU then runs offline only: you
can't join online races, and there's nothing for the anti-cheat to object to.

**Game › Online racing** in the launcher has **Run with anti-cheat**. With it on, the launcher starts LMU through
Easy Anti-Cheat with Sim-QVFR running, so you can race online. The launcher asks you to accept the risk before it
turns on.

**This is entirely at your own risk.** Sim-QVFR works inside LMU while it runs: it leaves LMU's code unchanged, but
it sits between LMU and its graphics calls. The anti-cheat may kick you or **ban your account** for that. By turning the option on, you knowingly take responsibility for it.

Sim-QVFR doesn't hide from, bypass or change the anti-cheat. Cheeky Foveated DLSS always stays off with this option
on.

## The other games

Assetto Corsa EVO, Assetto Corsa Competizione, X-Plane 12, American Truck Simulator and Euro Truck Simulator 2 have no
online option in the launcher. If you play them online with Sim-QVFR running, you do so at your own risk: a game or
multiplayer service may kick or ban you, and Sim-QVFR makes no promise that it won't.
