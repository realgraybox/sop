/* zlib License
 * 
 * Copyright (c) 2026 M.Glargaard aka realgraybox
 * 
 * This software is provided 'as-is', without any express or implied
 * warranty. In no event will the authors be held liable for any damages
 * arising from the use of this software.
 * 
 * Permission is granted to anyone to use this software for any purpose,
 * including commercial applications, and to alter it and redistribute it
 * freely, subject to the following restrictions:
 * 
 * 1. The origin of this software must not be misrepresented; you must not
 *    claim that you wrote the original software. If you use this software
 *    in a product, an acknowledgment in the product documentation would be
 *    appreciated but is not required.
 * 2. Altered source versions must be plainly marked as such, and must not be
 *    misrepresented as being the original software.
 * 3. This notice may not be removed or altered from any source distribution.
 */

/*
 * mmidi.h - minimal embedded General MIDI player.
 *
 * Self-contained: a small General MIDI bank is compiled in (mm_bank.h) as
 * sample models and rendered at start-up, so no soundfont file is needed.
 *
 * Typical use as a fallback for an external player, in a forked child:
 *
 *     pid = fork();
 *     if (pid == 0) _exit(mm_play_file(name) ? 1 : 0);
 *
 * mm_play_file() plays through OSS (/dev/dsp) and returns promptly on
 * SIGTERM/SIGINT, discarding buffered audio, so the device is released fast.
 *
 * The API can also replace TinySoundFont and TinyMidiLoader in a program:
 * see README.txt, section 3.4, for the call-by-call mapping.
 *
 * A SoundFont bank can replace the built-in one: an .sf2 file, or an .sfm
 * file (an SF2 whose samples are stored as models; made by sf2synth.py and
 * rendered at load time by mmmodel.c).
 *
 * Threads: all calls on one mm_synth must come from one thread at a time, as
 * with TinySoundFont. A program that sends mm_midi() from another thread than
 * the one calling mm_render() holds its own lock around both. Different
 * mm_synth objects are independent and can be used from different threads.
 *
 * Compile options for mmidi.c: -DMM_NO_OSS, -DMM_NO_REVERB, -DMM_NO_BUILTIN,
 * -DMAXVOICE=n (see the top of mmidi.c).  Reverb strength follows each channel's CC 91
 * (GM default 40); the room itself is set by the RV_* defines in mmidi.c.
 */
#ifndef MMIDI_H
#define MMIDI_H
#include <stddef.h>

/* API version: major.minor.patch, 8 bits each. The minor number goes up with
   each addition, the major only if an existing call changes meaning.
   mm_version() returns the version the library was built with. */
#define MMIDI_VERSION 0x020100
int mm_version(void);

typedef struct mm_synth mm_synth;

mm_synth *mm_create(int rate);                 /* output sample rate, e.g. 44100 */
void mm_free(mm_synth *s);

/* load a Standard MIDI File (format 0/1/2, also RIFF RMID); 0 = ok */
int mm_load_file(mm_synth *s, const char *path);
int mm_load_mem(mm_synth *s, const void *data, size_t size);

/* render interleaved stereo 16-bit frames; returns frames written,
   fewer than asked (possibly 0) once the song has ended. The output does not
   depend on how many frames each call asks for */
int mm_render(mm_synth *s, short *out, int frames);

/* the same as floats, full scale 1.0, after the master gain but before the
   soft knee: for a program with its own limiter. Can be mixed with
   mm_render() calls on the same synth */
int mm_render_float(mm_synth *s, float *out, int frames);

/* sounding voices, released ones still ringing included. A voice is one
   layer of a note, so one note can use several */
int mm_active_voices(const mm_synth *s);

/* all sound off at once (voices and reverb), all channels back to their GM
   defaults, and a loaded song back to its start; the bank and song stay */
void mm_reset(mm_synth *s);

/* release every sounding note on all channels, as CC 123 on each; notes
   ring out with their release */
void mm_all_notes_off(mm_synth *s);

/* the events of the loaded song, in play order, as copies: a program that runs
   its own sequencer reads them and plays them with mm_midi() */
typedef struct {
    double time;      /* seconds from the song start, tempo changes applied */
    int status;       /* MIDI status byte 0x80..0xEF (note off/on, control, program,
                         pitch bend; channel in the low 4 bits), 0xFF = tempo, or
                         0xFE = format 2: the next sequence starts (see mm_midi) */
    int d1, d2;       /* data bytes (0 for tempo) */
    long tempo;       /* tempo events: microseconds per beat, else 0 */
} mm_event;
int mm_event_count(const mm_synth *s);               /* 0 = no song */
int mm_get_event(mm_synth *s, int i, mm_event *e);   /* 0 = ok, -1 = out of range */
double mm_song_length(mm_synth *s);                  /* seconds to the end of the song */

/* 1 (default): mm_render plays the loaded song. 0: it does not, so a program
   can load a song for its events, feed them itself and render as a live
   synth (mm_render then never ends). Switching back to 1 plays the song from
   its start. The setting stays across song loads */
void mm_set_playing(mm_synth *s, int on);

/* use a SoundFont bank (.sf2 or .sfm) instead of the built-in one;
   0 = ok, otherwise the synth keeps what it had: -1 = not a usable bank,
   -2 = an .sfm in a build with -DMM_NO_MODELS. Without a built-in bank
   (-DMM_NO_BUILTIN, -DMM_NO_MODELS) a synth is silent until one is loaded:
   mm_preset_count() is 0 until then */
int mm_load_bank(mm_synth *s, const char *path);
int mm_load_bank_mem(mm_synth *s, const void *data, size_t size);

/* presets of the current bank (built-in or loaded), index 0 .. count-1.
   mm_preset_info copies bank, program and the name (up to 20 characters,
   0-terminated) into the caller's variables, any of them NULL; 0 = ok,
   -1 = no such preset. mm_preset_find returns the index of bank/program,
   -1 = none. mm_channel_preset returns the preset a note on channel ch
   (0..15) would play now (its CC 0 bank and program, the drum kit on
   channel 10, with the same fallbacks to bank 0 / kit 0), -1 = none */
int mm_preset_count(const mm_synth *s);
int mm_preset_info(const mm_synth *s, int i, int *bank, int *prog, char name[21]);
int mm_preset_find(const mm_synth *s, int bank, int prog);
int mm_channel_preset(const mm_synth *s, int ch);

/* master gain, default 0.35; above about 0.5 dense songs reach the soft
   limiter (no hard clipping) */
void mm_set_volume(mm_synth *s, float gain);

/* send a live MIDI channel message (no file needed). It takes effect at the
   next 32-frame control block, at most 31 frames (0.7 ms at 44.1 kHz) later.
   Status 0xFE (an mm_event of a format 2 song) releases all notes and resets
   the channels, as the song does between its sequences */
void mm_midi(mm_synth *s, int status, int d1, int d2);

#ifndef MM_NO_OSS
/* blocking playback through OSS; 0 = ok, -1 = no device, no usable song,
   or no bank (a build without the built-in bank needs one) */
int mm_play_file(const char *path);
/* the same with a SoundFont bank (NULL = the built-in bank) */
int mm_play_file_bank(const char *path, const char *bank);
#endif

#endif
