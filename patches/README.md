# Local framework patches

The `snesrecomp` submodule is pinned to branch `local/mode2-renderer-fix` of
the fork https://github.com/SlippyGetBackHere/snesrecomp. It is upstream
`e54427bad93fb75729ce8214fdd7a46e197e6223` plus fourteen commits, one per patch
file below, applied in this order.

## snesrecomp-mode2-renderer.patch

Fixes background mode 2 in the framework's optimized PPU renderer
(`NewRenderer = 1`, the default): wrong sprite/background priority order
and a wrong offset-per-tile lookup. Without it, stage 1 shows no enemies,
bullets or exhaust, and part of the sidebars is blank.

## snesrecomp-timing.patch

Two timing fixes, checked against snes9x. `snes_set_beam_from_master_clock()`
makes a `$4212` read sync the beam to the CPU clock instead of adding a
fixed tick (the frame driver in `src/game_rtl.c` turns it on); without it
compiled vblank waits halved the game's scene rate. And compiled code is
charged bus-accurate master clocks (fetches at ROM speed, data at its
region's speed, internal cycles at 6) instead of 8 per cycle.

The generated C depends on the emitter change: regenerate after applying.

## snesrecomp-cpu-clock-scale.patch

An optional main-CPU clock scale (`g_snes_cpu_clock_q8`, 256 = console
speed) applied to every 65816 charge, interpreted and compiled, while the
beam, APU and DMA keep real time. The game's "reduce slowdown" option
(`strikegunner.ini`) sets it. Regenerate after applying: the emitter wraps
its charges in `SNES_CPU_CLOCKS()`.

## snesrecomp-dsp-declick.patch

`dsp_set_declick()`: BRR-decode and interpolation overflows saturate instead
of wrapping. The game's "AudioDeclick" option (`strikegunner.ini`) sets it;
off, the output is hardware-exact.

## snesrecomp-aot-charge-fixes.patch

Two compiled-code charging fixes in the emitter: block moves (MVN/MVP) are
charged per byte as the interpreter charges them (52 master clocks, not 56),
and a block whose last instruction is handed to the interpreter (an
unresolved indirect jump) no longer charges that instruction as well.
Regenerate after applying.

## snesrecomp-generate-coverage-dir.patch

`generate --coverage-dir <dir>` passes every capture in a directory to the
emitter as a historical manifest (each validated on its own, so captures
from different builds combine), and the launcher's Generate & rebuild passes
`recomp/coverage` when it exists. Without it a player's first run compiles
none of the coverage-seeded code. `tools/regen.sh` uses the same call.

## snesrecomp-setup-pack-analyzer.patch

The setup-pack packager builds the native analyzer and ships it in the pack,
so a player's first run does not need Rust. Packaging needs Cargo.

## snesrecomp-packager-python.patch

The packager's zip fallback calls the Python it already found (`python3` or
`python`) instead of `python3`, which Windows does not install.

## snesrecomp-generate-funcs-h-first.patch

`generate` syncs `recomp/funcs.h` before emitting as well as after. The
emitter reads names from it and it is not committed, so without this a
fresh tree (a player's first run) generated slightly different code from a
tree that had been regenerated before.

## snesrecomp-rebuild-helper-env.patch

The launcher's Windows rebuild script sets the toolchain environment itself
instead of calling the toolchain's `env.bat`, which (with LF line endings)
made `cmd` stop the script before it built anything: the launcher closed
after generating and asked to rebuild again on the next start. It also
reports a failed build as a failure instead of starting the old binary.

## snesrecomp-rewind-defaults.patch

Two optional host fields, `rewind_default_depth` and
`rewind_default_interval`, set the rewind ring a fresh install starts with.
`src/main.c` uses them for quarter-second steps reaching 25 seconds back.

## snesrecomp-rewind-hotkey.patch

The rewind hotkey (F12) is cleared before the filmstrip opens. It was left
set, so the strip reopened every frame: committing or cancelling a rewind
opened by F12 did nothing visible and the strip could not be left.

## snesrecomp-pack-exclude-dir.patch

`stage_setup_host.sh --exclude-dir PATH` leaves a tracked directory out of a
setup pack. `scripts/package_release.sh` uses it to drop the framework's
documentation and test suites (about 11 MB) that a player's rebuild never
reads.

## snesrecomp-mod-audio-stream.patch

One streamed voice in the mod mixer (`snes_mod_audio_stream_*`): a trusted
plugin supplies samples on demand and the mixer resamples and mixes them.
The Custom music mod streams the player's songs through it.

## Re-applying them

Moving to a newer upstream framework drops them, and they are the way back if
the fork is ever unavailable. In either case, from the project root:

    git -C snesrecomp checkout <upstream commit>
    git -C snesrecomp apply ../patches/snesrecomp-mode2-renderer.patch
    git -C snesrecomp apply ../patches/snesrecomp-timing.patch
    git -C snesrecomp apply ../patches/snesrecomp-cpu-clock-scale.patch
    git -C snesrecomp apply ../patches/snesrecomp-dsp-declick.patch
    git -C snesrecomp apply ../patches/snesrecomp-aot-charge-fixes.patch
    git -C snesrecomp apply ../patches/snesrecomp-generate-coverage-dir.patch
    git -C snesrecomp apply ../patches/snesrecomp-setup-pack-analyzer.patch
    git -C snesrecomp apply ../patches/snesrecomp-packager-python.patch
    git -C snesrecomp apply ../patches/snesrecomp-generate-funcs-h-first.patch
    git -C snesrecomp apply ../patches/snesrecomp-rebuild-helper-env.patch
    git -C snesrecomp apply ../patches/snesrecomp-rewind-defaults.patch
    git -C snesrecomp apply ../patches/snesrecomp-rewind-hotkey.patch
    git -C snesrecomp apply ../patches/snesrecomp-pack-exclude-dir.patch
    git -C snesrecomp apply ../patches/snesrecomp-mod-audio-stream.patch

then commit inside `snesrecomp` and commit the new submodule pointer here.
Drop any patch upstream has already absorbed.
