# Local framework patches

The `snesrecomp` submodule is pinned to a commit that exists only in this
checkout (branch `local/mode2-renderer-fix`). It is upstream
`e54427bad93fb75729ce8214fdd7a46e197e6223` plus four local commits, one per
patch file below, applied in this order.

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

## Re-applying them

A fresh clone cannot fetch the local commit, and a framework update drops it.
In either case, from the project root:

    git -C snesrecomp checkout <upstream commit>
    git -C snesrecomp apply ../patches/snesrecomp-mode2-renderer.patch
    git -C snesrecomp apply ../patches/snesrecomp-timing.patch
    git -C snesrecomp apply ../patches/snesrecomp-cpu-clock-scale.patch
    git -C snesrecomp apply ../patches/snesrecomp-dsp-declick.patch

then commit inside `snesrecomp` and commit the new submodule pointer here.
Drop any patch upstream has already absorbed.
