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

## Audio: short repeating dropouts (not reproduced)

**Status:** not reproduced 2026-10-14; suspected cause outside the port.

Reported from stage 5 onwards in a full playthrough: a very short mute that
recurred on a regular interval. That session's default output was likely
the VB-Audio Virtual Cable, which runs on its own clock and buffer. With the
cable disabled and a hardware output (SSL 2), a 227-second late-stage session
logged no dropped samples and no recurring underflows: frame rate 59.96 on
average, audio produced and consumed balanced to 0.01%, and the only
underflows were at two save-state loads, which clear the audio buffer by
design. A 3-minute headless run was equally clean.

If it returns: run with `STG_AUDIOLOG=<file>` set, which logs the audio
pipeline's counters once a second (underflows, missing frames, buffer level,
frame rate). Through a virtual cable, `AudioSamples = 1024` in `config.ini`
(a larger device buffer) is the usual remedy; untested here.

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
intro, attract demo and stage 1, one full playthrough, and a session through
the ending.
Code no capture reached runs on the interpreter. A further frame-wait
routine reached only from compiled code would show up as a brief hitch or
freeze at a scene change, with an `[interp_cap]` line in the log.
