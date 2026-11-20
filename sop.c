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
 
// sop.c - minimal MIDI player (mmidi, a SoundFont 2 synthesizer and MIDI player)

#include <stdio.h>
#include <string.h>
#include <signal.h>

#include "mmidi.h"
#include "sopbank.h"
#include "audio.h"	// unified audio - oss with tinyalsa fallback

#define SAMPLE_RATE      48000
#define BUFFER_SIZE      1024

static volatile sig_atomic_t g_stop = 0;

static void on_signal(int sig) {
    (void)sig;
    g_stop = 1;
}

int main(int argc, char *argv[]) {
    if (argc < 2 || argc > 3) {
        fprintf(stderr, "Usage: %s [soundfont.sf2] file.mid\n", argv[0]);
        return 1;
    }

    const char *midi_filename = argv[argc - 1];
    mm_synth *s = mm_create(SAMPLE_RATE);
    if (!s) {
        fprintf(stderr, "Could not allocate memory.\n");
        return 1;
    }

    // external soundfont if two arguments, built-in bank otherwise
    int r = argc == 3 ? mm_load_bank(s, argv[1]) : mm_load_bank_mem(s, sop_sf2, sop_sf2_len);
    if (r < 0) {
        if (argc < 3) fprintf(stderr, "Could not load internal bank\n");
        else if (r == -2) fprintf(stderr, "%s: .sfm banks are not supported by this build\n", argv[1]);
        else fprintf(stderr, "Could not load bank: %s\n", argv[1]);
        mm_free(s);
        return 1;
    }

    // Standard MIDI file, format 0, 1 or 2 (sequences back to back), also RIFF RMID.
    // mmidi starts every channel with the General MIDI defaults (piano, channel 10 = drums).
    if (mm_load_file(s, midi_filename) < 0) {
        fprintf(stderr, "%s: cannot open or not a usable MIDI file\n", midi_filename);
        mm_free(s);
        return 1;
    }

    // audio (oss with fallback to tinyalsa)
    if (audio_init(SAMPLE_RATE, 2) < 0) {
        fprintf(stderr, "Unable to initialize audio\n");
        mm_free(s);
        return 1;
    }

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

    // mmidi plays the song, lets the release tails and the reverb ring out,
    // and returns fewer frames than asked once it has finished
    short buf[BUFFER_SIZE * 2];          // stereo interleaved
    int rc = 0, n;
    while (!g_stop && (n = mm_render(s, buf, BUFFER_SIZE)) > 0) {
        if (audio_write(buf, n) < 0) {
            if (!g_stop) { perror("audio_write"); rc = 1; }
            break;
        }
    }

    audio_close();
    mm_free(s);
    return rc;
}
