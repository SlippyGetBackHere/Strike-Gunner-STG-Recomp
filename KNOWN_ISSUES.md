# Known issues

## Audio: occasional pops

**Status:** open, parked 2026-10-13. More noticeable in stage 4's heaviest
section with `ReduceSlowdown = 1` (reported 2026-10-14), plausibly because
the scene then runs at full speed and triggers more sound effects.

Short pops or crackles at irregular moments during play. First noticed in
stage 2 with Spray Missiles, but they also occur with the default weapon and
in stage 1.

What is known:

- The pops are in the emulated sound chip's own output, before anything
  reaches the host audio device. A raw capture of the S-DSP output
  (`SNESRECOMP_DSPOUT`) contains them, so device, buffer size and sample
  rate settings are not the cause.
- The loudest ones line up with key-on events where the music driver
  restarts several voices at once, some still sounding loudly with no
  key-off first. Real hardware also cuts a restarted voice instantly, so
  this pattern alone is not proof of an emulation fault.
- The game runs echo on all eight voices (EON=$FF, ESA=$80, EDL=$0F,
  EFB=$28/$32, EVOL=$DD/$E7) -- the most intricate part of the chip and a
  plausible place for a difference.
- Handling the final block of a one-shot sample one block later than
  hardware was tested and ruled out: fixing it changed the output by about
  1% of full scale.
- Headless captures are deterministic, so a candidate fix can be checked
  sample for sample.

Next step when resumed: compare against an independent emulator with
`snesrecomp/tools/snesref` (needs a libretro SNES core and the SDL2
development package), driving both with the same input script and diffing
the DSP register trace and audio output to find the first divergence.

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

## Timing differs from the interpreted build

The compiled build's guest state diverges from an interpreter-only build of
the same code from frame 8 of boot: a loading routine gets a different amount
of work done per frame. Play testing judged the compiled build closer to the
original console, but the cause has not been found.

## Coverage

Native code is generated from coverage captures in `recomp/coverage/`: the
intro, attract demo and stage 1, and one full playthrough to the ending.
Code no capture reached runs on the interpreter. A further frame-wait
routine reached only from compiled code would show up as a brief hitch or
freeze at a scene change, with an `[interp_cap]` line in the log.
