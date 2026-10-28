

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
- Channels start with General MIDI defaults (piano, channel 10 = drums), so
  files without program changes still play.
- Per-instrument reverb and chorus: the SoundFont's own send levels (see
  [Reverb and chorus](#reverb-and-chorus)) feed one small shared reverb and one
  chorus, with no change to `tsf.h`
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

## Reverb and chorus

SoundFonts store, per instrument, how much of its signal goes to a reverb and
a chorus unit (the generators *ReverbEffectsSend* and *ChorusEffectsSend*).
`tsf.h` ignores them, and the upstream code is left untouched. Instead `sop.c`
reads those values from the bank when it starts and applies them itself:

- The send level of every sample region is read from the bank's `pdta` data,
  with the same zone rules `tsf.h` uses, and checked against `tsf.h`'s own
  regions (count and key/velocity ranges). A preset that does not line up is
  simply played without effects.
- While rendering, a voice that has a send is mixed into the output as
  normal, and a scaled mono copy of it feeds one shared reverb and one shared
  chorus. So a piano with a small send and strings with a large one get
  different amounts of effect, which a global reverb cannot do.
- MIDI controllers 91 (reverb depth) and 93 (chorus depth) add to the bank's
  send per channel, the way the SoundFont default modulators define it
  (up to +20 % at controller value 127). Defaults are 40 and 0, as in GM.
- The effects ring out at the end of a song instead of being cut off.

If the bank has no send values at all none of this is active and the output is 
bit-for-bit what plain `tsf.h` produces. When sends are found, sop prints
`Per-instrument reverb/chorus sends active.` at start.

The effect code uses `tsf.h` internals (`tsf_voice_render`, the voice struct
and the hydra readers), so keep the `tsf.h` that is bundled here. If a future
`tsf.h` changes those, build with `-DSOP_NO_FX`.

Build-time options (add to `CFLAGS`):

| Option | Meaning |
| ------ | ------- |
| `-DSOP_NO_FX` | Leave the effects out completely (about 5 KB smaller) |
| `-DSOP_REVERB_LEVEL=0.5f` | Reverb return level. 1.0 means a 100 % send is as loud as the dry signal |
| `-DSOP_CHORUS_LEVEL=0.5f` | Chorus return level |
| `-DSOP_FX_USE_CC=0` | Ignore controllers 91 and 93, use the bank's sends only |

The effects add level on top of the dry signal, so a bank with large sends can
need a lower `tsf_set_volume()` value or lower return levels to avoid clipping.

Cost, measured on x86-64: about 5 KB of code, about 85 KB of RAM (most of it
the reverb delay lines), and a fixed CPU cost of roughly 40 ns per frame
while an effect is active, plus a small amount per voice that has a send.
Effects are skipped while nothing feeds them and they have finished ringing.
The algorithms are deliberately simple (a damped-comb reverb and a
two-tap modulated-delay chorus), and they use floating point like `tsf.h`.

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
oggenc sop.wav -o sop.ogg         # vorbis encoding
sfotool bank.sf2 sop.ogg sop.sfo  # Write new .SFO soundfont file using OGG sample stream from .OGG file
xxd -i sop.sfo > sopbank.h        # generate the C array
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
- The reverb and chorus are simple fixed algorithms; their parameters
  (room size, damping, chorus rate and depth) are set in the source, not by
  the SoundFont or by MIDI
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
