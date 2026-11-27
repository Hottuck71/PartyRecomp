# PartyRecomp

Play **your own copy** of Wii Party on a Windows 10 or 11 PC, as a native PC program: real Wii Remotes (pointing
without a sensor bar), Nintendo Switch Joy-Cons, gamepads, up to four players, online play with friends, and an optional 60 fps mode.

By **Lane Carle (crazyman12)**. Built on [ResortRecomp](https://github.com/Hottuck71/ResortRecomp), the same
launcher and runtime made for Wii Sports Resort.

> **PartyRecomp contains no part of the game.** No code, no graphics, no sound, nothing made from them, in this
> repository or in its downloads. Each player provides their own Wii Party disc, and the launcher translates and
> builds that copy into a PC program **on their own PC**. Wii Party is © Nintendo. PartyRecomp is not made,
> sponsored, or endorsed by Nintendo.

**Status: early version (0.1.0).** The game builds, boots, and plays: the title, menus, party modes and minigames
tried so far work, with sound, Wii Remotes, gamepads, local multiplayer, and online play. Not every minigame and
board has been played through yet, so please report anything that goes wrong.

## Playing

1. From [Releases](../../releases), download **`PartyRecomp-<version>-Setup.exe`** and run it (it asks where to
   put PartyRecomp), or download the `.zip` and unzip it yourself. Allow about 15 GB once set up, not inside
   Program Files. Windows may warn about an unknown program: "More info", then "Run anyway".
2. You need: your own **Wii Party (USA, SUPE01)** as a disc image you made from your disc, and
   [Dolphin](https://dolphin-emu.org) installed (only its `DolphinTool.exe` is used, to extract the image).
3. Open `Launcher.exe`. On the **Play** tab pick your disc image and `DolphinTool.exe`, then press
   **Set up the game**. The launcher extracts the disc, translates the game's code with
   [DolRecomp](https://github.com/ExpansionPak/DolRecomp) (the main program and the ~100 modules Wii Party loads
   as it goes, one per minigame, board and menu), and compiles it with the bundled GCC: 30 minutes or more.
4. Press **Play**.

## Controllers and multiplayer

Up to four players. Wii Remotes take player 1, 2, 3 and 4 first, then gamepads, as they connect, even mid-game;
the keyboard and mouse always play as player 1 too.

- **Wii Remotes:** pair each once on the launcher's Wii Remote tab. The runtime drives each remote itself and
  switches its MotionPlus on, so a **Wii Remote Plus points with its gyro: no sensor bar needed** (the virtual
  sensor bar). A plain Wii Remote points by tilting. Hold the remote still a moment, then point at the middle of
  the screen and press A to centre it.
- **Gamepads** (Xbox, PlayStation, Switch Pro and others, through SDL3): A = A, B or right trigger = B, X = 1,
  Y = 2, Start = +, Back = −, Guide = HOME, d-pad or left stick = d-pad, right stick = pointer (click to centre),
  left bumper = shake, left trigger + left stick = tilt. A pad with a gyro also points and moves for real.
- **Nintendo Switch Joy-Cons**, each one a Wii Remote Plus of its own: pair them once on the launcher's **Joy-Con** tab
  (its own tab, apart from the Wii Remote one: hold the sync button on the rail until the lights move, then click
  **The lights are moving**; it also tests them) and they take seats as they connect,
  after any Wii Remotes. Their gyro and accelerometer are the remote's MotionPlus and motion, so they point (no
  sensor bar) and shake like a Wii Remote; rumble and player lights work. Held upright, top towards the screen:
  right Joy-Con A = A, ZR = B, stick = d-pad, Y = 1, B = 2, X = −, + = +; left Joy-Con right arrow = A, ZL = B,
  stick = d-pad, left arrow = 1, down arrow = 2, up arrow = +, − = −, Capture = HOME. R/L or pressing the stick
  re-centres the pointer. For sideways games, press **SL and SR together** to hold it sideways (stick on the left):
  bottom button = 2, right = 1, left = A, top = B, SL = −, SR = +.

## 60 fps (off by default)

Most of Wii Party already runs at 60 fps; some modes ask for 30, as on the Wii. **Launcher > Graphics >
"60 fps everywhere"** runs those at 60 too, by turning on a switch the game's developers left in: the game then
works out its own timing for 60, so it plays at its normal speed. Online games always use the default.

## Playing online

Everyone plays the host's game and save; only controller input travels (lockstep, like Dolphin's netplay), so it
stays smooth: about 50 ms of input delay on a home network. Players connect through a small relay server that
anyone can run, on **Windows** (double-click `relay\Start relay.bat`) or **Ubuntu** (one C file, a systemd
service). A relay already running for ResortRecomp serves PartyRecomp too. Step by step:
**[docs/ONLINE-SERVER.md](docs/ONLINE-SERVER.md)** (also in the download as `relay\SERVER-SETUP.txt`).

**Please don't share** your extracted game, your built `PartyRecomp-Game.exe`, or anything else made from your
copy. Everyone sets up from their own disc.

## How it works

The game's PowerPC code is translated statically to C by DolRecomp, then compiled for x86-64 with this project's
runtime: a hand-written replacement for the Wii's operating system, graphics (through
[aurora](https://github.com/encounter/aurora) and Dawn), sound mixer, disc and save access, Wii Remote driver,
and online lockstep. Wii Party adds REL modules, linked at run time: each is translated ahead of time for a fixed
address, and the runtime stands in for the Wii's module linker so every module lands where its translation
expects. See [docs/HOW-IT-WORKS.md](docs/HOW-IT-WORKS.md) and [docs/WII-PARTY.md](docs/WII-PARTY.md).

## Building

[docs/BUILDING.md](docs/BUILDING.md). Building the toolkit never needs the game.

## Licence

PartyRecomp is free software under the [GNU GPL, version 3](LICENSE), because it builds on DolRecomp (GPL-3.0).
Third-party parts keep their own licences: [native/toolkit/THIRD-PARTY.txt](native/toolkit/THIRD-PARTY.txt).
Mii pictures in the launcher: My Avatar Editor, © 2009 Trevor McCauley, MIT.
