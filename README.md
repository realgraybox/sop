

# sop

A small, self-contained MIDI player for Linux that renders MIDI files
through a SoundFont (SF2) synthesizer, with a General MIDI bank built
right into the binary.

It is the wavetable "cousin" of [mop](https://github.com/realgraybox/mop):
where mop emulates an OPL3 FM chip for the classic AdLib sound, sop plays
sampled instruments from a SoundFont. A static, uClibc-linked build with the
built-in bank comes in at around **254 KB**, of which the bank is 205 KB.

## Why

Wavetable playback usually means a big synth stack or a multi-megabyte
soundfont. sop goes the other way: [mmidi](https://github.com/realgraybox/mmidi),
a small SoundFont 2 synthesizer and MIDI player in C, plus a compact General
MIDI bank embedded in the executable. The result is one static file with no
external data needed.

## Features

- MIDI file formats 0, 1 and 2 (format 2 sequences are played back to back,
  with the channels reset in between)
- RIFF-wrapped MIDI (`.rmi` / RMID) is unpacked automatically
- Built-in soundbank, or load your own `.sf2` from the command line
- Note on/off, program change, pitch bend and controllers, with channel 10
  mapped to percussion
- 64-voice polyphony
- Channels start with General MIDI defaults (piano, channel 10 = drums), so
  files without program changes still play
- Per-instrument reverb and chorus from the SoundFont's own send levels (see
  [Reverb and chorus](#reverb-and-chorus))
- Release tails and the reverb ring out at the end of a song instead of
  being cut off
- Clean shutdown on Ctrl+C / SIGTERM
- OSS `/dev/dsp` and tinyalsa output, with OSS tried first and tinyalsa as
  the fallback

## How it works

- **MIDI and synthesis:** [mmidi](https://github.com/realgraybox/mmidi) (its
  `v2/` engine, bundled in `mmidi/`, zlib licensed) parses the file, plays it
  and renders it: SoundFont 2 voices with envelopes, LFOs, filters and
  exclusive classes, a small stereo reverb and a chorus. It is built as an
  `.sf2`-only player (`-DMM_NO_MODELS`) without its own OSS code
  (`-DMM_NO_OSS`). Its output does not depend on the C library (it has its
  own maths functions), and it is the same whatever block size sop renders.
- **Audio output:** a small `audio.h` layer that picks OSS or tinyalsa.

sop asks mmidi for 1024 frames at a time and writes them to the device;
playback is paced by those blocking writes.

## Reverb and chorus

SoundFonts store, per instrument, how much of its signal goes to a reverb and
a chorus unit (the generators *ReverbEffectsSend* and *ChorusEffectsSend*).
mmidi applies them as the SoundFont specification says:

- Each note sends its zone's amount to one shared reverb and one shared
  chorus, so a piano with a small send and strings with a large one get
  different amounts of effect.
- MIDI controllers 91 (reverb depth) and 93 (chorus depth) add to the bank's
  send per channel, as the SoundFont default modulators define it (up to
  +20 % at controller value 127). Defaults are 40 and 0, as in GM.
- A bank without any send values is played as earlier mmidi versions did:
  controller 91 alone sets each channel's reverb (0-100 %), and controller 93
  its chorus.

Build-time options for mmidi (add to `CFLAGS`):

| Option | Meaning |
| ------ | ------- |
| `-DMM_NO_REVERB` | Leave the reverb out (about 0.8 KB of code, 26 KB of RAM) |
| `-DMM_NO_CHORUS` | Leave the chorus out (about 0.9 KB of code, 8 KB of RAM) |
| `-DMAXVOICE=n` | Voices (default 64; each costs about 400 bytes of RAM) |

## Building

```
make
```

View the build options in the Makefile. Options are given in `CFLAGS` in the
environment, so they add to the Makefile's own, e.g.
`CFLAGS="-DMM_NO_CHORUS" make`.

- `-DAUDIO_HAVE_OSS` and `-DAUDIO_HAVE_TINYALSA` select the audio backends.
  Either one alone works too. With both, OSS is tried first and tinyalsa is
  the fallback.
- i386: the plain build uses the x87 FPU. `CFLAGS="-msse2 -mfpmath=sse" make`
  gives output identical to x86_64 builds on CPUs with SSE2; with gcc 3.4
  (uClibc toolchains) the x87 build was the faster and smaller one. The
  Makefile adds the stack alignment SSE2 needs (see `mmidi/README.md`).
- `mmidi/` is a copy of `v2/` from the mmidi repository (see
  `mmidi/README.md` for the version); update it by copying those files again.

### Changing the built-in bank

The embedded bank is `sop.sf2`, turned into the C array `sop_sf2` in
`sopbank.h`. To embed a different one:

```
cp bank.sf2 sop.sf2
xxd -i sop.sf2 sopbank.h          # or: make sopbank.h
```

## Usage

```
# Play a MIDI file with the built-in bank
./sop song.mid

# Play a MIDI file with an external SoundFont
./sop soundfont.sf2 song.mid
```

Press Ctrl+C to stop. The audio device is closed cleanly.

## Limitations

- No SysEx or meta-event handling beyond tempo (lyrics, text and markers
  are discarded)
- Only note, program, pitch bend and control-change messages are acted on
  (no channel or polyphonic aftertouch)
- Compressed `.sfo` banks are no longer read; convert them back to `.sf2`
  (with `sfotool` from TinySoundFont) to use them
- The built-in bank is small, so it trades some realism for size. Pass an
  external SoundFont for better sound
- The reverb and chorus are simple fixed algorithms; their parameters
  (room size, damping, chorus rate and depth) are set in mmidi's source, not
  by the SoundFont or by MIDI
- Output is fixed at 48 kHz stereo, and rendering shares a single thread
  with playback, so very slow CPUs may underrun

## License

`sop.c` and the rest of this project's own source are licensed as stated in
the source code (zlib license). The bundled mmidi (`mmidi/`) is zlib licensed
too (`mmidi/LICENSE`).

The built-in soundbank carries its own license; see the project it was made
from.

## Acknowledgments

- [schellingb](https://github.com/schellingb/TinySoundFont) for
  TinySoundFont, which sop was first built on
- The authors of the SoundFont the built-in bank is based on
- [mop](https://github.com/realgraybox/mop), the FM-synthesis sibling of
  this project
