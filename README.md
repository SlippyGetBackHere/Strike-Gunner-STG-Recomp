# Strike Gunner S.T.G Recomp

![Strike Gunner S.T.G title screen](docs/title-screen.png)

A native recompilation of **Strike Gunner S.T.G** (USA), built on
[snesrecomp](https://github.com/RetroPortingToolKit/snesrecomp).

A vertical shoot-'em-up from 1992, recompiled from the 65816 to native code:
the game's own logic runs as compiled C against an emulated SNES video chip,
sound chip and DMA, with the framework's interpreter as a fallback for code no
analysis has reached.


> **You must legally own a copy of the game.** No ROM data is distributed with
> this project, in the repository or in any release. The recompiled C is
> generated locally from your own copy and is never committed.

**Just want to play?** Download the latest release from the
[Releases page](https://github.com/SlippyGetBackHere/Strike-Gunner-STG-Recomp/releases)
and see [`PLAYING.md`](PLAYING.md): setup, controls, options and how to report
a problem. The rest of this file is for building and developing the port.

## Mods

Optional extras on the launcher's **Mods** page, all off by default, so the
game plays as the original unless you turn something on:

- **Custom music** (new in 0.2.0): replace the game's music with your own.
  Drop WAV, MP3, OGG, FLAC or MSU-1 tracks into a folder per stage or screen
  (title, weapon select, each stage, boss, stage clear, game over, ending)
  and they play in their place, in order or at random, matched to the
  game's loudness. Seamless intro + loop pairs, short fades between songs,
  a small "now playing" caption with its own on/off switch, a skip key, and
  save states and rewind that resume the same song where it was. Sound
  effects are untouched.
- **Infinite lives**: losing a life never uses up a spare one, for both
  players.
- **All weapons every stage**: no weapon is ever marked NO USE on the weapon
  select screen.
- **One-hit kills**: every enemy, bosses included, goes down to the first
  hit. An easy mode, or a quick way through the game.
- **Infinite special weapon**: the special weapon gauge never runs down.
- **Stage select**: after choosing your special weapon, pick which of the
  eight stages to play.

How to set them up is in [`PLAYING.md`](PLAYING.md#mods).

## Screenshots

| | |
|:---:|:---:|
| ![Weapon select](docs/weapon-select.png) | ![Stage 2: a bomber squadron](docs/stage2-bombers.png) |
| Weapon select: 15 weapons to choose from | Stage 2: a bomber squadron |
| ![Stage 4: the fleet at sea](docs/stage4-fleet.png) | ![Stage 4: attacking a warship](docs/stage4-warship.png) |
| Stage 4: the fleet at sea | Stage 4: attacking a warship |
| ![Stage 6: the mothership](docs/stage6-mothership.png) | ![Two players in stage 1](docs/two-player.png) |
| Stage 6: the mothership in the asteroid field | Two players at once (2P START) |

## Status

**Playable from start to finish** (as of 2026-10-14): intro, attract demo,
all stages, and the ending through the credits to "The End", in single-player
play on Windows. Two-player play starts and both ships answer their own
controller (headless check), and the launcher's second seat works with
either player on keyboard or pad; a full 2P playthrough has not been done.

- **Accuracy.** Checked against snes9x with identical scripted input: game
  events land on the same frames for the first 3,162 frames of a
  13,500-frame route, in both compiled and interpreted execution, and stage
  4's heaviest section slows down exactly as much as on the reference
  (3,536 of 3,600 frames on time).
- **Graphics.** Correct throughout play, including save states loaded while
  paused.
- **Audio.** Matches the reference. The remaining occasional pops are in the
  game's own sound (see [Options](#options)).
- **Native code.** 982 compiled variants, seeded from coverage captures of
  the intro, attract demo, stage 1, one full playthrough and the ending
  (`recomp/coverage/`). Uncovered code runs on the interpreter.
- **Save states** keep the frame driver's resume point, so a state resumes
  exactly where it was made (checked by saving, playing on, and reloading:
  identical memory and picture).
- **Rewind** (0.1.1): quarter-second steps, 25 seconds back by default,
  adjustable in the launcher. Committing a rewind lands exactly on the
  earlier frame (2 seconds back matched a plain run 120 frames earlier:
  memory, video memory, registers and picture), and committing, cancelling
  and opening work from keyboard and controller (checked by hand). Opening
  the save-state browser or rewind panel without committing leaves the game
  bit-identical.
- **Setup packs.** `scripts/package_release.sh` builds a setup pack that
  generates and builds the game on the player's PC from their ROM, with no
  Rust or other tools beyond what the launcher downloads. Checked on
  Windows from a fresh unzip: the code it generates matches a developer
  regeneration, and the game played through stage 1. Current release:
  0.2.0 (tag `v0.2.0`). The framework's documentation and test suites are
  left out of packs.
- **Mods**: features on the launcher's Mods page, all off by default
  (`mods/preloaded/packages/strikegunnerstg.gameplay`). Off, the game is
  unchanged (attract run identical to the stock build).
  - *Infinite lives* (0.1.2, `src/mod_infinite_lives.c`): neither player
    loses spare lives.
  - *All weapons every stage* (0.1.3, `src/mod_all_weapons.c`): the used-weapon
    flags ($1BED / $1BFC, one byte per weapon) are cleared every frame, so
    the weapon select screen never marks a weapon NO USE.
  - *One-hit kills* (`src/mod_one_hit_kill.c`): the two damage tables in ROM
    ($00:E599 main gun by shot power, low byte; $00:E579 special weapons)
    are raised in memory (restored when off). Enemy health is offset 6 of
    the object slot, 200 at most. Large background-drawn enemies mirror
    their health into $1ADE (400 for the stage 4 warships); a drop of
    exactly the patched $FF there is finished, along with the enemy's own
    copy.
  - *Infinite special weapon* (`src/mod_infinite_special.c`): the gauges,
    16-bit $023E / $0248 (full $0300), are put back when they drop.
  - *Stage select* (`src/mod_stage_select.c`): when the weapon is confirmed
    on the weapon select screen ($023D / $0247 set while its timer $0547
    runs, not in the demo, $17E6), the framework's choice list opens and the
    pick goes into $0230, which the game reads after the screen ($00:8088).
    Leaving stage 1 replays the game's new-game resets ($00:808D-80B9).
  - *Custom music* (0.2.0, `src/mod_custom_music.c`): song commands on APU port 0
    ($2140; $04 title, $03 weapon select, $01/$07/$06/$08/$09 stages, $02
    boss, $0A stage clear, $05 game over, $01 in stage 8 the ending, $14
    stop) are mapped to folders per stage and screen. A replaced song is
    swallowed, the driver is sent stop, and the player's track streams
    through the framework mixer, scaled to -21 dBFS RMS by default. Tracks
    load on a worker thread: WAV (smpl loops), MSU-1 `.pcm`, and MP3, OGG,
    FLAC through the public-domain decoders in `third_party/audio/`; an
    `.intro.`/`.loop.` pair loops from the seam. Changes fade over 0.35 s.
    The song playing and its position are a tagged record ('STGN') after
    the STG1 save-state chunk, so loading a state or rewinding resumes it;
    a "now playing" caption uses the framework's bottom-anchored OSD
    caption (small proportional font, wraps onto a second line).
    The power-on driver upload on the same port is skipped.
    `STG_MUSIC_LOG=<file>` logs every decision.

Open items and what has been ruled out are in [`KNOWN_ISSUES.md`](KNOWN_ISSUES.md).

## ROM identity

| | |
|---|---|
| File | `Strike Gunner S.T.G (USA).sfc` |
| Publisher | VAP (US licensee NTVIC) |
| Developer | Athena |
| Year | 1992 |
| Mapping | lorom |
| Region | USA (North America) |
| Coprocessor | none |
| CRC32 | `d054e288` |
| SHA-256 | `05f14e6ed3394d9273e2397769a8acf1a9db646be6066e82269521e8eec53562` |

`tools/regen.sh` refuses to run against anything else, so a mismatched
revision fails immediately instead of producing subtly wrong output.

## Build

On Windows, configure with Ninja and clang (the retcomm `cmake-clang-v1`
toolchain): the default Visual Studio generator uses MSVC, which cannot
compile the framework. The analyzer needs Rust (`cargo`) the first time.

```sh
git submodule update --init --recursive
bash tools/regen.sh --rom "/path/to/Strike Gunner S.T.G (USA).sfc"
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++
cmake --build build -j
```

The `snesrecomp` submodule comes from a fork,
[SlippyGetBackHere/snesrecomp](https://github.com/SlippyGetBackHere/snesrecomp),
branch `local/mode2-renderer-fix`: upstream plus sixteen changes this port needs
that are not upstream. `git clone --recursive` fetches it.
`patches/README.md` lists the fixes and how to re-apply them to upstream.

The ROM does not have to live in the repository — keeping it on your own
drive is the better habit, and `SNESRECOMP_ROM` sets the path once for a
shell. Without either, `tools/regen.sh` finds `Strike Gunner S.T.G (USA).sfc` at the repo root,
then the path the setup wizard recorded in `rom.cfg`; `.gitignore` blocks
both from ever being committed.

`tools/regen.sh` verifies the ROM, generates `src/gen/*.c`, and re-syncs
`recomp/funcs.h`. Re-run it whenever you change anything under `recomp/`.

## Run

```sh
./build/StrikeGunnerSTGSNESRecomp                     # launcher picks the ROM
./build/StrikeGunnerSTGSNESRecomp "/path/to/Strike Gunner S.T.G (USA).sfc" # or name it and skip the launcher
```

With no ROM on the command line the build opens the recomp-ui launcher: a ROM
picker with a verification badge, plus display / audio / input settings. Your
choice is cached in `rom.cfg` beside the executable, so the next launch opens
on it and "Skip launcher on boot" makes it immediate. If the launcher is not
built in (`--no-recomp-ui`), the host falls back to a native file picker.

Either way the ROM is checked against the digests above before anything boots
— a dump that could not have produced this build is refused at the door rather
than mis-executing ten frames in.

## Options

`strikegunner.ini` beside the executable (created on first launch):

| Key | Values |
|---|---|
| `ReduceSlowdown` | `0` authentic console speed (default); `1`, `2`, `3` run the SNES CPU 2x, 3x, 4x faster |
| `AudioDeclick` | `1` (default) caps sound samples that overflow instead of wrapping them, removing the loudest pops; `0` is the console's exact output |

The game slows down where its logic needs more than a frame; at `1` the
heaviest scene measured (stage 4, ocean section, extra fighters out) holds
60 fps. Frames that already fit are unchanged, so game speed and music stay
authentic. Restart to apply. Some pops remain with `AudioDeclick` on: they are
in the original game's sound (snes9x and footage of the console have them
too). It is a separate file because the host rewrites `config.ini` from the
settings it knows.

## Layout

| Path | What lives there |
|---|---|
| `recomp/` | Analysis input: `bank*.cfg`, `symbols.toml`, generated `funcs.h` |
| `recomp/coverage/` | Coverage captures that seed the analyzer (addresses and hit counts, no ROM data) |
| `rom_identity.txt` | ROM digests — read by the build, `tools/regen.sh` and CI |
| `src/` | Host code: `main.c`, `game_rtl.c` (frame driver, save-state hooks, options), `gen_stubs.c` (frame-wait and task-switch hand-backs) |
| `src/gen/` | Generated C. Never committed — regenerate locally |
| `snesrecomp/` | Framework submodule (owns `lib/recomp-net`, `lib/retcomm-rbengine`) |
| `tools/` | `regen.sh` — the ROM → C pipeline |
| `scripts/` | `package_release.sh` — player-facing zip |
| `patches/` | The local framework fixes, one patch per commit, with notes |
| `framework_pins.txt` | Exact framework commits this project is pinned to |
| `KNOWN_ISSUES.md` | Open issues, resolved ones, and what was ruled out |

## How the port works

- **Frame driver** (`src/game_rtl.c`). The framework's beam-aligned driver
  with one change: NMI is taken the way the CPU takes it, so the game's
  NMI handler -- which switches NMI off and runs for several frames during
  scene changes -- can span fields.
- **Frame waits and task switches** (`recomp/bank00.cfg`, `bank05.cfg`,
  `src/gen_stubs.c`). Compiled code cannot wait for the next frame, so the
  game's four wait routines and its cooperative task scheduler ($8D2E /
  $8D68) are declared `hle_func` and handed back to the driver's
  interpreter, which runs the original bytes.
- **Object handlers** (`recomp/symbols.toml`, `bank03.cfg`). All 165 entries
  of the object table at $03:8000 and the object loop are compiled, so
  enemies from every stage are native code.

## Continuing

- **Coverage.** Play with `--coverage-capture=on`, add the capture from
  `%LOCALAPPDATA%\snesrecomp-coverage\strikegunnerstg\` to
  `recomp/coverage/`, re-run `tools/regen.sh`, rebuild, and check the
  result plays the same. A new frame wait reached only from compiled code
  shows up as a hitch or freeze with an `[interp_cap]` line in the log;
  declare it like the others.
- **Diagnostics.** `STG_AUDIOLOG=<file>` logs the audio pipeline once a
  second. The framework's `tools/snesref` with a libretro SNES core is the
  reference for timing and audio comparisons.
- **Never edit `src/gen/`** or synthesise a result to get past uncovered
  code. Fix the config or the framework and regenerate.

## Multiplayer

Two players at once on one PC: **2P START** on the title screen, one
controller each (or rebind `[player2]` in `keybinds.ini`). Netplay is not
built into this target.

## License

This project's own source is under the license in `LICENSE`. The framework
carries its own terms — see `snesrecomp/LICENSE` and
`snesrecomp/THIRD_PARTY_ATTRIBUTION.md`. The audio decoders in
`third_party/audio/` are public domain (or MIT / MIT-0, your choice); see
`third_party/audio/README.md`. None of these covers the game data, which is
not distributed here.
