# Playing Strike Gunner S.T.G

A native PC version of the 1992 SNES shooter **Strike Gunner S.T.G**, made by
recompiling the game's code. If anything looks, sounds or plays differently
from the console, please report it.

**You need your own copy of the game.** The download contains no game data.
The first time you run it, it builds the game on your PC from your ROM file.

## What you need

- Windows 10 or 11, 64-bit
- A ROM of **Strike Gunner S.T.G (USA)**, which the setup checks:
  - CRC32 `d054e288`
  - SHA-256 `05f14e6ed3394d9273e2397769a8acf1a9db646be6066e82269521e8eec53562`

  Other versions or modified ROMs are refused.
- About 2 GB of free disk space
- An internet connection the first time, if the build tools are not already
  on your PC (see below)

## First run

1. Unzip the download into a folder of its own and start
   `StrikeGunnerSTGSNESRecomp.exe`.
2. Pick your ROM file. The launcher checks it and shows a badge.
3. Click **Generate & rebuild**. If the build tools (`cmake-clang-v1`) are not
   on your PC yet, the launcher offers to download them first. They are
   shared with other recompiled games and only downloaded once.
4. The launcher closes and a console window titled *Strike Gunner S.T.G -
   rebuilding* opens. It builds the game, which takes a few minutes. Let it
   finish; the launcher then reopens with a **Play** button.

If the console stops with "Configure failed" or "Build failed", it waits for
you. Please copy its last lines into your report.

Windows may warn that the program is from an unknown publisher, because the
release is not signed.

## Playing again

The finished game is in the `build` folder: start
`build\StrikeGunnerSTGSNESRecomp.exe`. Your settings, save states and logs
are kept in `build` as well.

## Controls

Game controllers (Xbox, PlayStation, Switch Pro and similar) are detected
when plugged in. On the keyboard:

| SNES button | Key |
|---|---|
| D-pad | Arrow keys |
| B | Z |
| A | X |
| Y | A |
| X | S |
| L / R | C / V |
| Start | Enter |
| Select | Right Shift |

**Two players:** choose **2P START** on the title screen. Both players start
on the same keyboard keys, so give the second player a controller, or change
the `[player2]` keys in `build\keybinds.ini`.

Keyboard shortcuts:

| Key | Does |
|---|---|
| Shift+F1 … Shift+F10 | Save state to slot 1–10 |
| F1 … F10 | Load state from slot 1–10 |
| F11 | Save-state browser |
| F12 | Rewind |
| P | Pause |
| Tab (hold) | Fast forward |
| Alt+Enter | Fullscreen |
| Ctrl+R | Reset |
| Keypad + / − | Volume |

On a controller, Select+R opens the save-state browser and Select+R3 opens
rewind.

Rewind steps back a quarter of a second at a time, up to 25 seconds. The
launcher's settings can turn it off or change the step and how far back it
reaches.

## Options

`build\strikegunner.ini` (created on first play; restart to apply):

- `ReduceSlowdown = 0` plays at console speed, slowing down where the original
  did. `1`, `2` and `3` run the console's CPU 2, 3 or 4 times faster, which
  removes most slowdown.
- `AudioDeclick = 1` removes the loudest of the pops in the game's own sound
  effects; `0` plays the sound exactly as the console's sound chip produces
  it. A few pops remain either way: they are in the original game too.

Display, sound and input settings are in the launcher and in
`build\config.ini`.

## Mods

Optional changes to the game are on the launcher's **Mods** page. Everything
there is off by default, so the game plays as the original unless you turn
something on. Change them before clicking **Play**.

- **Gameplay options → Infinite lives**: losing a life no longer uses up a
  spare life, so the game never ends. Works for both players. Extra lives,
  new games and continues work as normal.

When reporting a problem, say which mods were on.

## Known issues

See `KNOWN_ISSUES.md`. In short:

- Some sound effects pop. The original game does this too.
- Slowdown in busy scenes is the original game's; `ReduceSlowdown` removes it.

## Reporting a problem

Please include:

1. What happened and what you expected, and where in the game.
2. Whether it happens every time.
3. `build\last_run_report.json` from the session where it happened (it is
   overwritten on the next start, so copy it first). It records where your
   ROM file is on your PC; edit that path out if you would rather not share
   it.
4. For sound problems, a short recording or video helps a lot.
5. For setup problems, the last lines of the console window, and
   `build\snesrecomp_rebuild.log`.

A save state from just before the problem (the files in `build\saves`) helps
most of all, but save states hold the console's video and sound memory,
which is copied from the game. Don't post them publicly: say you have one,
and send it privately if asked.
