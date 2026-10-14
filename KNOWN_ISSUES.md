# Known issues

## Audio: pops (explained, authentic)

**Status:** explained 2026-10-14. Not a port bug.

The pops are in the game's own sound. One looping instrument's sample data
decodes past full scale and the S-DSP wraps it to the opposite extreme, a
rail-to-rail spike lasting a sample or two. snes9x produces the same pops at
the same rate (stage 4 heavy section, one minute: reference 6, port 7), and
gameplay footage of the console has them; a console's analog output and a
TV soften them. They are more frequent with `ReduceSlowdown = 1` only because
busy scenes then run at full speed.

`AudioDeclick = 1` (the default) saturates the overflow instead and removes
the largest ones; the rest are the instrument's normal sharp edges and the
noise channel. A low-pass filter over the output and a fade on key-on were
tried first and rejected: the first dulled the music, the second had no
effect.

## Audio: short repeating dropouts in long sessions

**Status:** open, suspected cause not confirmed.

Reported from stage 5 onwards in a full playthrough: a very short mute that
recurs on a regular interval. That session presented frames at an average
59.70 per second (earlier sessions: 59.95). The S-DSP produces a fixed number
of samples per guest frame while the device drains 32,000 per second, so a
host frame rate below 60 slowly empties the output ring until it underruns
and refills -- which would sound like exactly this. Next step: measure the
ring's fill level over a long session. The pacing is the framework's, not
the game's.

## Slowdown in busy scenes

The game slows down on real hardware where its logic overruns a frame. Two
port bugs made it far worse (compiled vblank waits halving the scene rate,
compiled code charged 8 clocks per cycle); both are fixed. Measured against
snes9x from a save in stage 4's heaviest section (one minute, same input):
the reference finishes 3,536 of 3,600 frames on time, the port 3,516. The
remaining gap is compiled-mode only (the interpreter tier gets 3,532) and
not yet explained.

`ReduceSlowdown` in `strikegunner.ini` removes the slowdown: at `1` that
section runs 3,600 of 3,600 frames on time. Anything the game times by
counting CPU instructions rather than vblanks runs faster with it on.

## Timing differs from the interpreted build (resolved)

Caused by two compiled-mode timing bugs, both fixed: `$4212` reads added a
beam tick on top of charged time, and compiled code was charged 8 master
clocks per cycle. Against snes9x, compiled and interpreted builds now first
diverge from the reference at the same frame (3,162 of the reference route).

## Coverage

Native code is generated from coverage captures in `recomp/coverage/`: the
intro, attract demo and stage 1, and one full playthrough to the ending.
Code no capture reached runs on the interpreter. A further frame-wait
routine reached only from compiled code would show up as a brief hitch or
freeze at a scene change, with an `[interp_cap]` line in the log.
