/*
 * Copyright (C) M. Glargaard, aka graybox
 *
 * This software is provided "as-is", without any express or implied
 * warranty. In no event will the authors be held liable for any damages
 * arising from the use of this software.
 *
 * Permission is granted to anyone to use this software for any purpose,
 * including commercial applications, and to alter it and redistribute it
 * freely, subject to the following restrictions:
 *
 * 1. The origin of this software must not be misrepresented; you must not
 *    claim that you wrote the original software. If you use this software
 *    in a product, an acknowledgment in the product documentation would
 *    be appreciated but is not required.
 *
 * 2. Altered source versions must be plainly marked as such, and must not
 *    be misrepresented as being the original software.
 *
 * 3. This notice may not be removed or altered from any source distribution.
 */
 
// sop.c - minimal MIDI player (TinySoundFont + TinyMidiLoader)

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <signal.h>
#include <unistd.h>

#ifdef USE_VORBIS
#define STB_VORBIS_NO_PUSHDATA_API
#define STB_VORBIS_NO_STDIO
#include "stb_vorbis.c"
#endif

#define TSF_IMPLEMENTATION
#include "tsf.h"

#define TML_IMPLEMENTATION
#include "tml.h"

#include "sopbank.h"
#include "audio.h"	// unified audio - oss with tinyalsa fallback

#define SAMPLE_RATE      48000
#define BUFFER_SIZE      1024
#define MAX_VOICES       64
#define MAX_MIDI_BYTES   (64u * 1024u * 1024u)   // sanity cap for input files
#define TAIL_MAX_FRAMES  (SAMPLE_RATE * 3)       // max release-tail rendering

#if defined(__UCLIBC__)
#include <math.h>
float expf (float x) { return (float) exp( (double)x ); }
float powf (float x, float y) {	return (float) pow( (double)x, (double)y ); }
#endif

/* ---------- small helpers ---------- */

static volatile sig_atomic_t g_stop = 0;

static void on_signal(int sig) {
    (void)sig;
    g_stop = 1;
}

static unsigned rd_le32(const unsigned char *p) {
    return (unsigned)p[0] | (unsigned)p[1] << 8 |
           (unsigned)p[2] << 16 | (unsigned)p[3] << 24;
}

static unsigned rd_be32(const unsigned char *p) {
    return (unsigned)p[0] << 24 | (unsigned)p[1] << 16 |
           (unsigned)p[2] << 8 | (unsigned)p[3];
}

static unsigned rd_be16(const unsigned char *p) {
    return (unsigned)p[0] << 8 | (unsigned)p[1];
}

/* ---------- file loading ---------- */

// Reads a whole file into memory. Returns NULL (after printing why) on failure.
static unsigned char *read_file(const char *path, size_t *out_size) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "Could not open file %s\n", path);
        return NULL;
    }

    long len = -1;
    if (fseek(f, 0, SEEK_END) == 0) len = ftell(f);
    if (len <= 0 || (unsigned long)len > MAX_MIDI_BYTES || fseek(f, 0, SEEK_SET) != 0) {
        fprintf(stderr, "%s: empty, unreadable or too large\n", path);
        fclose(f);
        return NULL;
    }

    unsigned char *buf = (unsigned char *)malloc((size_t)len);
    if (!buf) {
        fprintf(stderr, "Could not allocate memory.\n");
        fclose(f);
        return NULL;
    }

    size_t got = fread(buf, 1, (size_t)len, f);
    fclose(f);
    if (got != (size_t)len) {
        fprintf(stderr, "%s: short read (%zu of %ld bytes)\n", path, got, len);
        free(buf);
        return NULL;
    }

    *out_size = (size_t)len;
    return buf;
}

// Finds the raw MIDI data inside a RIFF RMID container.
// Returns data unchanged if it is not RMID.
static const unsigned char *unwrap_riff_midi(const unsigned char *data, size_t *size) {
    if (*size < 20 || memcmp(data, "RIFF", 4) != 0 || memcmp(data + 8, "RMID", 4) != 0)
        return data;

    size_t off = 12;
    while (*size - off >= 8) {              // off <= *size always holds here
        size_t chunk = rd_le32(data + off + 4);
        size_t avail = *size - off - 8;

        if (memcmp(data + off, "data", 4) == 0) {
            if (chunk > avail) chunk = avail;   // tolerate a truncated final chunk
            *size = chunk;
            return data + off + 8;
        }

        if (chunk > avail) break;               // corrupt chunk size
        size_t step = chunk + (chunk & 1);      // chunks are word-aligned
        if (step > avail) break;
        off += 8 + step;
    }
    return data;
}

/* ---------- song container ---------- */

// tml_free() releases a single block, so a Format 2 song made of several
// tml_load_memory() results must free each block separately.
typedef struct {
    tml_message  *head;
    tml_message **blocks;
    size_t        nblocks;
} song_t;

static void song_free(song_t *s) {
    for (size_t i = 0; i < s->nblocks; i++)
        tml_free(s->blocks[i]);
    free(s->blocks);
    s->blocks = NULL;
    s->nblocks = 0;
    s->head = NULL;
}

// Parses MIDI data (Format 0, 1 or 2). Returns 0 on success, -1 on failure.
static int load_midi(const unsigned char *d, size_t n, const char *name, song_t *out) {
    memset(out, 0, sizeof *out);

    if (n < 14 || memcmp(d, "MThd", 4) != 0) {
        fprintf(stderr, "%s: not a MIDI file\n", name);
        return -1;
    }

    unsigned hdr_len = rd_be32(d + 4);
    unsigned format  = rd_be16(d + 8);
    unsigned tracks  = rd_be16(d + 10);

    if (hdr_len < 6 || hdr_len > n - 8) {
        fprintf(stderr, "%s: bad MIDI header\n", name);
        return -1;
    }
    if (format > 2) {
        fprintf(stderr, "%s: MIDI format %u unsupported\n", name, format);
        return -1;
    }

    if (format != 2) {
        tml_message *song = tml_load_memory(d, (int)n);
        if (!song) {
            fprintf(stderr, "%s: could not parse MIDI data\n", name);
            return -1;
        }
        out->blocks = (tml_message **)malloc(sizeof *out->blocks);
        if (!out->blocks) {
            tml_free(song);
            fprintf(stderr, "Could not allocate memory.\n");
            return -1;
        }
        out->blocks[0] = song;
        out->nblocks = 1;
        out->head = song;
        return 0;
    }

    /* Format 2: independent sequences. Play them one after another by loading
       each track as its own single-track file and shifting its timestamps. */
    if (tracks == 0) {
        fprintf(stderr, "%s: no tracks\n", name);
        return -1;
    }

    // every track we copy fits in the file, so 14 + 8 + track_len <= n
    unsigned char *tmp = (unsigned char *)malloc(n);
    out->blocks = (tml_message **)calloc(tracks, sizeof *out->blocks);
    if (!tmp || !out->blocks) {
        fprintf(stderr, "Could not allocate memory for Format 2.\n");
        free(tmp);
        free(out->blocks);
        out->blocks = NULL;
        return -1;
    }

    memcpy(tmp, d, 14);
    tmp[4] = 0; tmp[5] = 0; tmp[6] = 0; tmp[7] = 6;   // header length = 6
    tmp[8] = 0; tmp[9] = 1;                           // Format 1
    tmp[10] = 0; tmp[11] = 1;                         // one track

    size_t pos = 8 + (size_t)hdr_len;
    unsigned accumulated_ms = 0;
    tml_message *tail = NULL;

    for (unsigned i = 0; i < tracks; i++) {
        if (n - pos < 8) break;
        if (memcmp(d + pos, "MTrk", 4) != 0) break;

        size_t track_len = rd_be32(d + pos + 4);
        if (track_len > n - pos - 8) break;

        memcpy(tmp + 14, d + pos, 8 + track_len);
        tml_message *track_song = tml_load_memory(tmp, (int)(14 + 8 + track_len));
        pos += 8 + track_len;

        if (!track_song) continue;   // empty or unparsable track: skip it

        tml_message *last = NULL;
        for (tml_message *m = track_song; m; m = m->next) {
            m->time += accumulated_ms;
            last = m;
        }

        out->blocks[out->nblocks++] = track_song;
        if (!out->head) out->head = track_song;
        else if (tail)  tail->next = track_song;

        if (last) {
            tail = last;
            accumulated_ms = last->time;
        }
    }
    free(tmp);

    if (!out->head) {
        fprintf(stderr, "%s: no playable tracks\n", name);
        song_free(out);
        return -1;
    }
    return 0;
}

/* ---------- playback ---------- */

// Renders 'frames' frames and sends them to the audio device.
// Returns 0 on success, -1 on audio error.
static int render_frames(tsf *t, short *buf, uint64_t frames) {
    while (frames > 0 && !g_stop) {
        int chunk = frames > BUFFER_SIZE ? BUFFER_SIZE : (int)frames;
        tsf_render_short(t, buf, chunk, 0);
        if (audio_write(buf, chunk) < 0) {
            if (!g_stop) perror("audio_write");
            return -1;
        }
        frames -= (uint64_t)chunk;
    }
    return 0;
}

// Returns 0 if playback finished (or was interrupted), -1 on audio error.
static int play_song(tsf *t, tml_message *msg) {
    short buf[BUFFER_SIZE * 2];          // stereo interleaved
    uint64_t frames_done = 0;

    while (msg && !g_stop) {
        // integer frame clock: no rounding error accumulates between events
        uint64_t target = ((uint64_t)msg->time * SAMPLE_RATE + 500) / 1000;

        if (target > frames_done) {
            if (render_frames(t, buf, target - frames_done) < 0) return -1;
            frames_done = target;
        }

        switch (msg->type) {
        case TML_NOTE_ON:
            tsf_channel_note_on(t, msg->channel, msg->key, msg->velocity / 127.0f);
            break;
        case TML_NOTE_OFF:
            tsf_channel_note_off(t, msg->channel, msg->key);
            break;
        case TML_PROGRAM_CHANGE:
            tsf_channel_set_presetnumber(t, msg->channel, msg->program, (msg->channel == 9));
            break;
        case TML_PITCH_BEND:
            tsf_channel_set_pitchwheel(t, msg->channel, msg->pitch_bend);
            break;
        case TML_CONTROL_CHANGE:
            tsf_channel_midi_control(t, msg->channel, msg->control, msg->control_value);
            break;
        }
        msg = msg->next;
    }

    if (g_stop) return 0;

    // let release tails and reverb ring out instead of chopping them
    tsf_note_off_all(t);
    uint64_t tail = 0;
    while (tail < TAIL_MAX_FRAMES && tsf_active_voice_count(t) > 0 && !g_stop) {
        if (render_frames(t, buf, BUFFER_SIZE) < 0) return -1;
        tail += BUFFER_SIZE;
    }
    return 0;
}

/* ---------- main ---------- */

int main(int argc, char *argv[]) {
    if (argc < 2 || argc > 3) {
        fprintf(stderr, "Usage: %s [soundfont.sf2/.sfo] file.mid\n", argv[0]);
        return 1;
    }

    int rc = 1;
    tsf *g_tsf = NULL;
    unsigned char *file_buffer = NULL;
    song_t song;
    int audio_open = 0;
    const char *midi_filename;
    
    memset(&song, 0, sizeof song);   /* must happen before the first goto cleanup */

    // external soundfont if two arguments, built-in bank otherwise
    if (argc == 3) {
        g_tsf = tsf_load_filename(argv[1]);
        if (!g_tsf) {
            fprintf(stderr, "Could not load bank: %s\n", argv[1]);
            return 1;
        }
        midi_filename = argv[2];
    } else {
        g_tsf = tsf_load_memory(sop_sfo, sop_sfo_len);
        if (!g_tsf) {
            fprintf(stderr, "Could not load internal bank\n");
            return 1;
        }
        midi_filename = argv[1];
    }

    tsf_set_output(g_tsf, TSF_STEREO_INTERLEAVED, SAMPLE_RATE, 0.0f);
    tsf_set_volume(g_tsf, 0.35f);
    tsf_set_max_voices(g_tsf, MAX_VOICES);   // preallocate; needs a recent tsf.h

    // read, unwrap and parse the MIDI file
    size_t file_size = 0;
    file_buffer = read_file(midi_filename, &file_size);
    if (!file_buffer) goto cleanup;

    size_t midi_size = file_size;
    const unsigned char *raw_midi = unwrap_riff_midi(file_buffer, &midi_size);

    if (load_midi(raw_midi, midi_size, midi_filename, &song) < 0) goto cleanup;

    free(file_buffer);       // tml copies what it needs
    file_buffer = NULL;

    // audio (oss with fallback to tinyalsa)
    if (audio_init(SAMPLE_RATE, 2) < 0) {
        fprintf(stderr, "Unable to initialize audio\n");
        goto cleanup;
    }
    audio_open = 1;

    // Ctrl+C / kill: finish cleanly. No SA_RESTART, so a blocked write returns.
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    switch (audio_get_backend()) {
    case AUDIO_BACKEND_OSS:
        printf("Playing via /dev/dsp... Press Ctrl+C to stop.\n");
        break;
    case AUDIO_BACKEND_TINYALSA:
        printf("Playing via TinyALSA... Press Ctrl+C to stop.\n");
        break;
    default:
        break;
    }

    rc = (play_song(g_tsf, song.head) < 0) ? 1 : 0;

cleanup:
    if (audio_open) audio_close();
    song_free(&song);
    free(file_buffer);
    tsf_close(g_tsf);
    return rc;
}
