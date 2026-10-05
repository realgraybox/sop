

https://github.com/user-attachments/assets/ede252b4-3129-42c6-b703-b7a7c2ad964c

# sop

A small, self-contained MIDI player for Linux that renders MIDI files
through a SoundFont (SF2) synthesizer, with a compact soundbank built
right into the binary.

It is the wavetable "cousin" of [mop](https://github.com/realgraybox/mop):
where mop emulates an OPL3 FM chip for the classic AdLib sound, sop plays
sampled instruments from a SoundFont. A static, uClibc-linked build with the
built-in bank comes in at around **166 KB**.

## Why

Wavetable playback usually means a big synth stack or a multi-megabyte
soundfont. sop goes the other way: a minimal MIDI parser feeding
TinySoundFont, plus a General MIDI bank that is Vorbis-compressed
(`.sfo`) and embedded in the executable. The bank shrinks from 223 KB to
49 KB, and the Vorbis decoder costs about 45 KB, so the compressed bank is
a clear net win. The result is one static file with no external data
needed.

## Features

- MIDI file formats 0, 1 and 2 (format 2 sequences are played back to back)
- RIFF-wrapped MIDI (`.rmi` / RMID) is unpacked automatically
- Built-in soundbank, or load your own `.sf2` / `.sfo` from the command line
- Note on/off, program change, pitch bend and controllers, with channel 10
  mapped to percussion
- 64-voice polyphony
- Release tails ring out at the end of a song instead of being cut off
- Clean shutdown on Ctrl+C / SIGTERM
- OSS `/dev/dsp` and tinyalsa output, with OSS tried first and tinyalsa as
  the fallback

## How it works

- **MIDI parsing:** [tml.h](https://github.com/schellingb/TinySoundFont),
  the MIDI-loader half of TinySoundFont. Header-only, MIT licensed. Events
  arrive sorted, with tempo already resolved to millisecond timestamps.
- **Synthesis:** [tsf.h](https://github.com/schellingb/TinySoundFont), a
  header-only SoundFont2 synthesizer. MIT licensed.
- **Compressed banks:** tsf can load SoundFonts whose samples are
  Ogg/Vorbis compressed (`.sfo`), decoded by
  [stb_vorbis](https://github.com/nothings/stb) (public domain / MIT).
  Banks are converted with `sfotool` from the TinySoundFont project.
  Samples are decoded to 16-bit PCM when the bank loads, so the saving
  is in the binary and on disk, not in RAM.
- **Audio output:** a small `audio.h` layer that picks OSS or tinyalsa.

Playback is paced by blocking audio writes, and event times are tracked
with an integer frame counter so timing does not drift over a long song.

## Building

Everything is compiled as a single translation unit. A typical size-optimized
static build:

```
View the build options in Makefile.
```

- `-DUSE_VORBIS` includes `stb_vorbis.c` so that `.sfo` banks can be loaded.
  Leave it out if your embedded bank is a plain `.sf2`.
- `-DAUDIO_HAVE_OSS` and `-DAUDIO_HAVE_TINYALSA` select the audio backends.
  Either one alone works too. With both, OSS is tried first and tinyalsa is
  the fallback.
- Under uClibc, `sop.c` supplies its own `expf()` and `powf()`.

### Changing the built-in bank

The embedded bank lives in `sopbank.h` as the arrays `sop_sfo` and
`sop_sfo_len`. To embed a different one:

```
sfotool bank.sf2 sop.wav          # Dump PCM sample stream to .WAV file
oggenc sop.wav sop.ogg            # vorbis encoding
sfotool bank.sf2 sop.ogg sop.sfo  # Write new .SFO soundfont file using OGG sample stream from .OGG file
xxd -i sop.sfo > sopbank.h       # generate the C array
```

## Usage

```
# Play a MIDI file with the built-in bank
./sop song.mid

# Play a MIDI file with an external SoundFont
./sop soundfont.sf2 song.mid
./sop soundfont.sfo song.mid
```

Press Ctrl+C to stop. The audio device is closed cleanly.

## Limitations

- No SysEx or meta-event handling beyond tempo (lyrics, text and markers
  are discarded, since `tml.h` doesn't surface them)
- Only note, program, pitch bend and control-change messages are acted on
  (no channel or polyphonic aftertouch)
- Format 2 files are simply concatenated, one sequence after another
- The built-in bank (Nokia_6230i_RM-72_.sf2) is small, so it trades some realism for size. Pass an
  external SoundFont for better sound
- Output is fixed at 48 kHz stereo, and rendering shares a single thread
  with playback, so very slow CPUs may underrun

## License

`sop.c` and the rest of this project's own source are licensed as stated in
the source code (zlib license).

Bundled and linked dependencies carry their own licenses:

- [tml.h / tsf.h](https://github.com/schellingb/TinySoundFont): MIT
- [stb_vorbis](https://github.com/nothings/stb): public domain / MIT
- Built-in soundbank: [Nokia_6230i_RM-72_.sf2](https://archive.org/download/free-soundfonts-sf2-2019-04)

See each project's repository for full license text and attribution
requirements.

## Acknowledgments

- [schellingb](https://github.com/schellingb/TinySoundFont) for
  TinySoundFont (tsf.h, tml.h and sfotool)
- [Sean Barrett](https://github.com/nothings/stb) for stb_vorbis
- The authors of the SoundFont the built-in bank is based on
- [mop](https://github.com/realgraybox/mop), the FM-synthesis sibling of
  this project
