PartyRecomp
===========
Play your own copy of Wii Party on a Windows 10 or 11 PC.
Built on ResortRecomp's launcher and runtime by Lane Carle (crazyman12).

PartyRecomp contains NO part of the game: no code, no pictures, no sounds,
nothing made from them. You need your own Wii Party disc (USA, SUPE01),
copied to your PC as a disc image, and Dolphin installed (only its
DolphinTool is used, to extract the disc). The launcher then translates your
copy into a PC program and builds it, here, on your own PC.

Wii Party is Nintendo's. PartyRecomp isn't made, sponsored, or endorsed by
Nintendo.

STATUS: EARLY VERSION (0.1.0). The game builds, boots, and plays: the title,
menus, party modes and minigames tested so far work, with sound, Wii Remotes,
gamepads, up to four players, and online play. Not every minigame and board
has been played through yet, so expect the odd problem, and please report it.


FIRST TIME
----------
1. Keep this folder somewhere with about 15 GB free (not inside Program Files).
   PartyRecomp-0.1.0-Setup.exe puts it in a folder you choose; the zip is the
   same thing to unzip yourself.
2. Open Launcher.exe.
3. Play tab: pick your disc image, and DolphinTool.exe from your Dolphin
   folder under "Disc tools". Press "Set up the game".
4. Wait. The disc is extracted (a minute or two), then the game's code is
   translated: main.dol, and the ~100 modules Wii Party loads as it runs (one
   per minigame, board and menu). Then everything is built with the compiler
   in the builder folder. Wii Party has about eight times Resort's code, so
   this takes much longer than Resort did: 30 minutes or more.
5. Press Play.

HOW WII PARTY DIFFERS FROM RESORT
---------------------------------
Wii Party loads its minigames as REL modules while it runs. Each module is
translated ahead of time for a fixed address (its "slot"), and the runtime
stands in for the Wii's module linker so every module the game loads lands in
its slot. See builder\src\rel.c.

CONTROLLERS AND MULTIPLAYER
---------------------------
Up to four players. Wii Remotes take player 1, 2, 3 and 4 first, then
gamepads, in the order they connect (even mid-game). The keyboard and mouse
always play as player 1 too.

Wii Remotes: pair each one once (Launcher > Wii Remote tab). No sensor bar is
needed with a Wii Remote Plus or a MotionPlus: its gyro points. Hold it still
for a moment after it connects, then point at the middle of the screen and
press A to centre it. C re-centres, F9 / F10 flip left-right / up-down.

Nintendo Switch Joy-Cons: pair them once (Launcher > Joy-Con tab: hold the
sync button on the rail until the lights move, then click "The lights are
moving"). Each Joy-Con is a Wii Remote of its own, seated after Wii Remotes
and before gamepads, and points with its gyro. Held upright, top towards the
screen:
  Right Joy-Con: A = A, ZR = B, stick = d-pad, Y = 1, B = 2, X = -, + = +
  Left Joy-Con:  right arrow = A, ZL = B, stick = d-pad, left arrow = 1,
                 down arrow = 2, up arrow = +, - = -, Capture = HOME
  R / L or pressing the stick re-centres the pointer.
SL and SR together switch a Joy-Con to sideways (stick on the left): bottom
button = 2, right = 1, left = A, top = B, SL = -, SR = +.

Gamepads (Xbox, PlayStation, Switch Pro and others):
  A (bottom) = A          B (right) or right trigger = B
  X (left)   = 1          Y (top) = 2
  Start = +   Back = -    Guide/PS/Home = HOME
  D-pad or left stick = d-pad
  Right stick = pointer (click it to centre)
  Left bumper = shake     hold left trigger + left stick = tilt
A pad with a gyro and accelerometer (PlayStation, Switch Pro, many others)
also points by turning it, and tilting and shaking it are real motion.

Pointing with no sensor bar (the virtual sensor bar): a Wii Remote Plus or
MotionPlus points with its gyro. A plain Wii Remote points by tilting it:
tip it up and down, and roll it left and right. F9 / F10 flip either way.

Lag: remotes report to the game 200 times a second, and a Wii Remote's
input goes straight to the game as it arrives. VSync makes no measurable
difference here, so leave it on.

60 FPS EVERYWHERE (off by default)
----------------------------------
Most of Wii Party already runs at 60 fps; some modes ask for 30, as on the
Wii. Launcher > Graphics > "60 fps everywhere" runs those at 60 too. It turns
on a switch the game's developers left in, so the game works out its own
timing for 60 and plays at its normal speed. It costs more processor and
graphics card time in those modes. If anything ever runs too fast, turn it
off again. Online games always use the default, so every PC matches.

ONLINE
------
Launcher > Online. Everyone plays the host's game and save; only controller
input travels, so it stays smooth. Input delay is measured when everyone has
joined: about 50 ms on a home network. Each PC plays as one player.
The easiest way is a relay server (no port forwarding for anyone but the
relay's computer). It runs on Ubuntu or Windows: see relay\SERVER-SETUP.txt,
or on Windows just double-click relay\Start relay.bat. A relay you already
run for Wii Sports Resort works for Wii Party too.

YOUR SAVE
---------
Kept in the "save" folder here. Play tab > Save data exports and imports it.

WHAT'S IN THIS FOLDER
---------------------
Launcher.exe            the launcher
PairRemote.exe          pairs a Wii Remote with Windows
PairJoyCon.exe          pairs Nintendo Switch Joy-Cons with Windows
builder\                the translator (DolRecomp), the runtime as source,
                        the graphics library, and a C compiler (GCC)
relay\                  the online room server, and how to run it
licenses\               the licence of every part (GPL-3.0)
work\                   the project's source and scripts (repo\), for
                        development
*.dll                   libraries the game and launcher use
After setting up, you'll also have: game\ (your extracted disc),
PartyRecomp-Game.exe (your build), save\, and build-log.txt.
Don't share those: they're made from your copy of the game.
