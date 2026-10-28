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

/* ---------- per-instrument reverb / chorus sends ---------- */

/*
 * tsf.h ignores the SoundFont generators ChorusEffectsSend (15) and
 * ReverbEffectsSend (16). tsf.h is left untouched; instead the bank's
 * hydra is read a second time here and a send level is kept for every
 * tsf region. Rendering then walks tsf's voices itself: a voice that has
 * a send is rendered into a scratch buffer, its dry signal goes to the
 * output and a scaled mono copy feeds one shared reverb and one shared
 * chorus. That is how SoundFont synths are specified: per-zone sends into
 * a common effect unit, not a global reverb on everything.
 *
 * This relies on tsf.h internals (struct tsf_voice, tsf_voice_render and
 * the hydra readers), so keep the tsf.h that is bundled with sop. The send
 * table is cross-checked against tsf's own regions (count and key/velocity
 * ranges); a preset that does not match is played without effects.
 *
 * A bank without any send values (or a failure here) simply gives plain
 * tsf rendering. Build with -DSOP_NO_FX to leave all of this out.
 */

#ifdef SOP_NO_FX

typedef struct fx_s fx_t;               // never defined, always NULL
#define fx_create(t, path)      ((void)(t), (void)(path), (fx_t *)NULL)
#define fx_destroy(fx)          ((void)(fx))
#define fx_render(fx, t, b, n)  ((void)(fx), tsf_render_short((t), (b), (n), 0))
#define fx_busy(fx)             ((void)(fx), 0)
#define fx_cc(fx, ch, cc, val)  ((void)(fx))

#else

#define FX_NONE          0xFFFFFFFFu

#ifndef SOP_FX_USE_CC
#define SOP_FX_USE_CC    1      // MIDI CC91/CC93 add to the bank's sends (SF2 default modulators)
#endif
#ifndef SOP_REVERB_LEVEL
#define SOP_REVERB_LEVEL 0.5f   // effect return levels (1.0 = a 100 % send is as loud as the dry signal), tune to taste
#endif
#ifndef SOP_CHORUS_LEVEL
#define SOP_CHORUS_LEVEL 0.5f
#endif
#define FX_PEAK_BUSY     1.0e-4f   // wet output above this (about -80 dB) still counts as ringing
#define FX_UNDENORM(x)   do { if ((x) > -1.0e-30f && (x) < 1.0e-30f) (x) = 0.0f; } while (0)

/* --- reverb: mono in, stereo out; 4 damped combs + 2 allpasses per side --- */

typedef struct { float *buf; int len, idx; float store; } rv_comb_t;
typedef struct { float *buf; int len, idx; } rv_ap_t;
typedef struct {
    rv_comb_t c[2][4];
    rv_ap_t   a[2][2];
    float     fb, damp;
    float    *mem;
} reverb_t;

static const unsigned short rv_comb_len[4] = { 1116, 1188, 1277, 1356 };   // at 44.1 kHz
static const unsigned short rv_ap_len[2]  = {  556,  441 };
#define RV_SPREAD     23           // right side delays are this much longer (stereo width)
#define RV_SCALE(n)   ((int)(((unsigned)(n) * (unsigned)SAMPLE_RATE + 22050u) / 44100u))
#define RV_FEEDBACK   0.88f
#define RV_DAMP       0.30f
#define RV_INPUT_GAIN 0.151f   // calibrated: white noise at send 100 % and level 1.0 gives equal RMS out

static int reverb_init(reverb_t *r) {
    int ch, k, total = 0;
    float *p;

    for (ch = 0; ch < 2; ch++) {
        for (k = 0; k < 4; k++) total += RV_SCALE(rv_comb_len[k] + ch * RV_SPREAD);
        for (k = 0; k < 2; k++) total += RV_SCALE(rv_ap_len[k] + ch * RV_SPREAD);
    }
    r->mem = (float *)calloc((size_t)total, sizeof(float));
    if (!r->mem) return -1;

    p = r->mem;
    for (ch = 0; ch < 2; ch++) {
        for (k = 0; k < 4; k++) {
            r->c[ch][k].len = RV_SCALE(rv_comb_len[k] + ch * RV_SPREAD);
            r->c[ch][k].buf = p;
            p += r->c[ch][k].len;
        }
        for (k = 0; k < 2; k++) {
            r->a[ch][k].len = RV_SCALE(rv_ap_len[k] + ch * RV_SPREAD);
            r->a[ch][k].buf = p;
            p += r->a[ch][k].len;
        }
    }
    r->fb = RV_FEEDBACK;
    r->damp = RV_DAMP;
    return 0;
}

// Adds the reverb of mono 'in' to interleaved stereo 'out'; returns the peak added.
static float reverb_run(reverb_t *r, const float *in, float *out, int n) {
    float peak = 0.0f;
    int i, ch, k;

    for (i = 0; i < n; i++) {
        float x = in[i] * RV_INPUT_GAIN;
        for (ch = 0; ch < 2; ch++) {
            float sum = 0.0f;
            for (k = 0; k < 4; k++) {
                rv_comb_t *c = &r->c[ch][k];
                float y = c->buf[c->idx];
                c->store = y * (1.0f - r->damp) + c->store * r->damp;
                FX_UNDENORM(c->store);
                c->buf[c->idx] = x + c->store * r->fb;
                if (++c->idx >= c->len) c->idx = 0;
                sum += y;
            }
            for (k = 0; k < 2; k++) {
                rv_ap_t *a = &r->a[ch][k];
                float b = a->buf[a->idx];
                float o = b - sum;
                a->buf[a->idx] = sum + b * 0.5f;
                if (++a->idx >= a->len) a->idx = 0;
                sum = o;
            }
            sum *= SOP_REVERB_LEVEL;
            out[2 * i + ch] += sum;
            if (sum > peak) peak = sum;
            else if (-sum > peak) peak = -sum;
        }
    }
    return peak;
}

/* --- chorus: mono in, stereo out; one modulated delay tap per side --- */

#define CH_LEN      2048u          // power of two, well above the longest delay
#define CH_RATE_HZ  0.5f
#define CH_BASE_MS  11.0f
#define CH_DEPTH_MS 3.5f

typedef struct { float buf[CH_LEN]; unsigned pos; float ph; } chorus_t;

static float ch_tap(const chorus_t *c, float delay) {
    float rd = (float)(c->pos + CH_LEN) - delay;
    unsigned i0 = (unsigned)rd;
    float fr = rd - (float)i0;
    float a = c->buf[i0 & (CH_LEN - 1)];
    float b = c->buf[(i0 + 1) & (CH_LEN - 1)];
    return a + (b - a) * fr;
}

static float chorus_run(chorus_t *c, const float *in, float *out, int n) {
    const float dph = CH_RATE_HZ / (float)SAMPLE_RATE;
    const float base = CH_BASE_MS * 0.001f * (float)SAMPLE_RATE;
    const float depth = CH_DEPTH_MS * 0.001f * (float)SAMPLE_RATE;
    float peak = 0.0f;
    int i;

    for (i = 0; i < n; i++) {
        float p = c->ph, q = p + 0.5f, lfoL, lfoR, l, r;
        if (q >= 1.0f) q -= 1.0f;
        lfoL = (p < 0.5f ? 4.0f * p - 1.0f : 3.0f - 4.0f * p);    // triangle, opposite phase per side
        lfoR = (q < 0.5f ? 4.0f * q - 1.0f : 3.0f - 4.0f * q);

        c->buf[c->pos] = in[i];
        l = ch_tap(c, base + depth * lfoL) * SOP_CHORUS_LEVEL;
        r = ch_tap(c, base + depth * lfoR) * SOP_CHORUS_LEVEL;
        c->pos = (c->pos + 1) & (CH_LEN - 1);
        c->ph += dph;
        if (c->ph >= 1.0f) c->ph -= 1.0f;

        out[2 * i] += l;
        out[2 * i + 1] += r;
        if (l > peak) peak = l; else if (-l > peak) peak = -l;
        if (r > peak) peak = r; else if (-r > peak) peak = -r;
    }
    return peak;
}

/* --- send table, read from the bank --- */

typedef struct {
    unsigned char *send;        // two bytes per tsf region: reverb, chorus (0..255 = 0..100 %)
    unsigned      *off;         // per tsf preset: first region slot in 'send', or FX_NONE
    int            npresets;
    int            any;         // at least one region has a send
    unsigned char  cc_rev[16];  // CC91 / CC93 per channel
    unsigned char  cc_cho[16];
    float          rv_peak, ch_peak;   // peak of the last block's effect output
    reverb_t       rv;
    chorus_t       ch;
    float          dry[BUFFER_SIZE * 2], tmp[BUFFER_SIZE * 2];
    float          rin[BUFFER_SIZE], cin[BUFFER_SIZE];
} fx_t;

typedef struct { int rev, cho; unsigned char lokey, hikey, lovel, hivel; } fxzone_t;

static void fxzone_clear(fxzone_t *z) {
    memset(z, 0, sizeof *z);
    z->hikey = z->hivel = 127;
}

static void fxzone_gen(fxzone_t *z, unsigned op, const union tsf_hydra_genamount *a) {
    switch (op) {
    case 15: z->cho = a->shortAmount; break;                              // ChorusEffectsSend
    case 16: z->rev = a->shortAmount; break;                              // ReverbEffectsSend
    case 43: z->lokey = a->range.lo; z->hikey = a->range.hi; break;       // KeyRange
    case 44: z->lovel = a->range.lo; z->hivel = a->range.hi; break;       // VelRange
    }
}

static unsigned char fx_byte(int v) {                    // 0.1 % units -> 0..255, range 0..1000
    if (v < 0) v = 0;
    if (v > 1000) v = 1000;
    return (unsigned char)((v * 255 + 500) / 1000);
}

/*
 * Walks one preset exactly the way tsf_load_presets() does and stores the send
 * of every region it would create, in the same order. Zone values are the
 * instrument zone (with the instrument's global zone as default) plus the preset
 * zone. Returns 0 if the result lines up with tsf's regions, otherwise -1.
 */
static int fx_preset(const struct tsf_hydra *h, int pi, const struct tsf_preset *tp, unsigned char *dst) {
    enum { GenInstrument = 41, GenSampleID = 53 };
    int pbStart = h->phdrs[pi].presetBagNdx, pbEnd = h->phdrs[pi + 1].presetBagNdx;
    int nreg = 0, bi;
    fxzone_t glob;

    fxzone_clear(&glob);
    if (pbStart > pbEnd || pbEnd > h->pbagNum - 1) return -1;

    for (bi = pbStart; bi < pbEnd; bi++) {
        fxzone_t pr = glob;
        int hadInst = 0, gi;
        int gs = h->pbags[bi].genNdx, ge = h->pbags[bi + 1].genNdx;
        if (gs > ge || ge > h->pgenNum) return -1;

        for (gi = gs; gi < ge; gi++) {
            const struct tsf_hydra_pgen *pg = &h->pgens[gi];
            if (pg->genOper == GenInstrument) {
                int wi = pg->genAmount.wordAmount, ibs, ibe, ii;
                fxzone_t ig;
                if (wi >= h->instNum) continue;
                if (wi + 1 >= h->instNum) return -1;
                ibs = h->insts[wi].instBagNdx;
                ibe = h->insts[wi + 1].instBagNdx;
                if (ibs > ibe || ibe > h->ibagNum - 1) return -1;

                fxzone_clear(&ig);
                for (ii = ibs; ii < ibe; ii++) {
                    fxzone_t z = ig;
                    int hadSample = 0, ji;
                    int js = h->ibags[ii].instGenNdx, je = h->ibags[ii + 1].instGenNdx;
                    if (js > je || je > h->igenNum) return -1;

                    for (ji = js; ji < je; ji++) {
                        const struct tsf_hydra_igen *g = &h->igens[ji];
                        if (g->genOper != GenSampleID) {
                            fxzone_gen(&z, g->genOper, &g->genAmount);
                            continue;
                        }
                        // the preset's key/velocity ranges act as a filter on the zone
                        if (z.hikey < pr.lokey || z.lokey > pr.hikey) continue;
                        if (z.hivel < pr.lovel || z.lovel > pr.hivel) continue;
                        if (pr.lokey > z.lokey) z.lokey = pr.lokey;
                        if (pr.hikey < z.hikey) z.hikey = pr.hikey;
                        if (pr.lovel > z.lovel) z.lovel = pr.lovel;
                        if (pr.hivel < z.hivel) z.hivel = pr.hivel;

                        if (nreg < tp->regionNum) {
                            const struct tsf_region *r = &tp->regions[nreg];
                            if (r->lokey != z.lokey || r->hikey != z.hikey || r->lovel != z.lovel || r->hivel != z.hivel)
                                return -1;
                            dst[2 * nreg]     = fx_byte(z.rev + pr.rev);
                            dst[2 * nreg + 1] = fx_byte(z.cho + pr.cho);
                        }
                        nreg++;
                        hadSample = 1;
                    }
                    if (ii == ibs && !hadSample) ig = z;     // the instrument's global zone
                }
                hadInst = 1;
            }
            else fxzone_gen(&pr, pg->genOper, &pg->genAmount);
        }
        if (bi == pbStart && !hadInst) glob = pr;            // the preset's global zone
    }
    return (nreg == tp->regionNum) ? 0 : -1;
}

// Reads the hydra of the bank from 'stream' and fills fx->send / fx->off.
static int fx_load_bank(fx_t *fx, const tsf *t, struct tsf_stream *stream) {
    struct tsf_riffchunk head, list, chunk;
    struct tsf_hydra hy;
    int rc = -1, n, i, pi;
    unsigned total;

    memset(&hy, 0, sizeof hy);
    if (!tsf_riffchunk_read(NULL, &head, stream) || !TSF_FourCCEquals(head.id, "sfbk")) return -1;

    #define FXH(name, size_in_file) \
        (TSF_FourCCEquals(chunk.id, #name) && !(chunk.size % (size_in_file))) { \
            int num_ = (int)(chunk.size / (size_in_file)), k_; \
            hy.name##Num = num_; \
            hy.name##s = (struct tsf_hydra_##name *)malloc((size_t)num_ * sizeof(struct tsf_hydra_##name)); \
            if (!hy.name##s) goto done; \
            for (k_ = 0; k_ < num_; k_++) tsf_hydra_read_##name(&hy.name##s[k_], stream); \
        }
    while (tsf_riffchunk_read(&head, &list, stream)) {
        if (TSF_FourCCEquals(list.id, "pdta")) {
            while (tsf_riffchunk_read(&list, &chunk, stream)) {
                if      FXH(phdr, 38) else if FXH(pbag, 4)  else if FXH(pgen, 4)
                else if FXH(inst, 22) else if FXH(ibag, 4)  else if FXH(igen, 4)
                else stream->skip(stream->data, chunk.size);
            }
        }
        else stream->skip(stream->data, list.size);
    }
    #undef FXH

    if (!hy.phdrs || !hy.pbags || !hy.pgens || !hy.insts || !hy.ibags || !hy.igens) goto done;
    n = hy.phdrNum - 1;
    if (n < 1 || n != t->presetNum) goto done;             // not the bank tsf loaded

    fx->off = (unsigned *)malloc((size_t)n * sizeof(unsigned));
    if (!fx->off) goto done;
    for (i = 0, total = 0; i < n; i++) {
        fx->off[i] = total;
        total += (unsigned)t->presets[i].regionNum;
    }
    fx->send = (unsigned char *)calloc(2 * (size_t)(total ? total : 1), 1);
    if (!fx->send) goto done;
    fx->npresets = n;

    for (pi = 0; pi < n; pi++) {
        int si = 0, oi;
        const struct tsf_hydra_phdr *p = &hy.phdrs[pi];
        for (oi = 0; oi < n; oi++) {                       // same sort order as tsf's preset list
            const struct tsf_hydra_phdr *o = &hy.phdrs[oi];
            if (oi == pi || o->bank > p->bank) continue;
            else if (o->bank < p->bank) si++;
            else if (o->preset > p->preset) continue;
            else if (o->preset < p->preset) si++;
            else if (oi < pi) si++;
        }
        if (fx_preset(&hy, pi, &t->presets[si], fx->send + 2 * (size_t)fx->off[si]) < 0)
            fx->off[si] = FX_NONE;                         // does not line up: no effects for it
    }

    for (i = 0; i < n; i++) {
        unsigned k, cnt;
        if (fx->off[i] == FX_NONE) continue;
        cnt = (unsigned)t->presets[i].regionNum;
        for (k = 0; k < 2 * cnt; k++)
            if (fx->send[2 * (size_t)fx->off[i] + k]) { fx->any = 1; break; }
        if (fx->any) break;
    }
    rc = 0;

done:
    free(hy.phdrs); free(hy.pbags); free(hy.pmods); free(hy.pgens); free(hy.insts);
    free(hy.ibags); free(hy.imods); free(hy.igens); free(hy.shdrs);
    return rc;
}

static void fx_destroy(fx_t *fx) {
    if (!fx) return;
    free(fx->rv.mem);
    free(fx->send);
    free(fx->off);
    free(fx);
}

// Returns NULL when the bank has no sends (or on any failure): plain tsf rendering is used then.
// 'path' is the external soundfont, or NULL for the built-in bank.
static fx_t *fx_create(const tsf *t, const char *path) {
    fx_t *fx = (fx_t *)calloc(1, sizeof *fx);
    struct tsf_stream st;
    struct tsf_stream_memory mem;
    FILE *fp = NULL;
    int rc;

    if (!fx) return NULL;
    st.read = (int (*)(void *, void *, unsigned int))&tsf_stream_stdio_read;
    st.skip = (int (*)(void *, unsigned int))&tsf_stream_stdio_skip;
    if (path) {
        fp = fopen(path, "rb");
        if (!fp) { free(fx); return NULL; }
        st.data = fp;
    } else {
        mem.buffer = (const char *)sop_sfo;
        mem.total = (unsigned int)sop_sfo_len;
        mem.pos = 0;
        st.data = &mem;
        st.read = (int (*)(void *, void *, unsigned int))&tsf_stream_memory_read;
        st.skip = (int (*)(void *, unsigned int))&tsf_stream_memory_skip;
    }
    rc = fx_load_bank(fx, t, &st);
    if (fp) fclose(fp);

    if (rc < 0 || !fx->any || reverb_init(&fx->rv) < 0) { fx_destroy(fx); return NULL; }
    memset(fx->cc_rev, 40, sizeof fx->cc_rev);               // GM default reverb depth; chorus depth is 0
    return fx;
}

// Track CC91 (reverb depth) and CC93 (chorus depth) per channel.
static void fx_cc(fx_t *fx, int ch, int cc, int val) {
    if (!fx || ch < 0 || ch > 15) return;
    if (cc == 91) fx->cc_rev[ch] = (unsigned char)(val & 127);
    else if (cc == 93) fx->cc_cho[ch] = (unsigned char)(val & 127);
}

// True while reverb or chorus output is still ringing (used to let the tail decay).
static int fx_busy(const fx_t *fx) {
    return fx && (fx->rv_peak > FX_PEAK_BUSY || fx->ch_peak > FX_PEAK_BUSY);
}

/* --- rendering --- */

static short fx_clip(float v) {                          // same conversion as tsf_render_short
    return (v < -1.00004566f ? (short)-32768 : (v > 1.00001514f ? (short)32767 : (short)(v * 32767.5f)));
}

// n <= BUFFER_SIZE
static void fx_block(fx_t *fx, tsf *t, short *out, int n) {
    struct tsf_voice *v = t->voices, *vEnd = v + t->voiceNum;
    float *dry = fx->dry;
    int i, rv_in = 0, ch_in = 0;

    memset(dry, 0, (size_t)n * 2 * sizeof(float));
    memset(fx->rin, 0, (size_t)n * sizeof(float));
    memset(fx->cin, 0, (size_t)n * sizeof(float));

    for (; v != vEnd; v++) {
        float rs = 0.0f, cs = 0.0f;
        if (v->playingPreset == -1) continue;

        if (v->playingPreset >= 0 && v->playingPreset < fx->npresets && fx->off[v->playingPreset] != FX_NONE) {
            const struct tsf_preset *p = &t->presets[v->playingPreset];
            unsigned ri = (unsigned)(v->region - p->regions);
            if (ri < (unsigned)p->regionNum) {
                const unsigned char *s = fx->send + 2 * ((size_t)fx->off[v->playingPreset] + ri);
                rs = (float)s[0] * (1.0f / 255.0f);
                cs = (float)s[1] * (1.0f / 255.0f);
                #if SOP_FX_USE_CC
                if (v->playingChannel >= 0 && v->playingChannel < 16) {
                    rs += (float)fx->cc_rev[v->playingChannel] * (0.2f / 127.0f);   // default modulator: 20 % at CC 127
                    cs += (float)fx->cc_cho[v->playingChannel] * (0.2f / 127.0f);
                }
                #endif
                if (rs > 1.0f) rs = 1.0f;
                if (cs > 1.0f) cs = 1.0f;
                if (rs < 0.002f) rs = 0.0f;
                if (cs < 0.002f) cs = 0.0f;
            }
        }

        if (rs == 0.0f && cs == 0.0f) {
            tsf_voice_render(t, v, dry, n);                    // no send: straight into the mix
            continue;
        }
        memset(fx->tmp, 0, (size_t)n * 2 * sizeof(float));
        tsf_voice_render(t, v, fx->tmp, n);
        for (i = 0; i < n; i++) {
            float l = fx->tmp[2 * i], r = fx->tmp[2 * i + 1], m = (l + r) * 0.70710678f;
            dry[2 * i] += l;
            dry[2 * i + 1] += r;
            fx->rin[i] += m * rs;
            fx->cin[i] += m * cs;
        }
        if (rs > 0.0f) rv_in = 1;
        if (cs > 0.0f) ch_in = 1;
    }

    // run an effect while it gets input or is still ringing
    if (rv_in || fx->rv_peak > FX_PEAK_BUSY) fx->rv_peak = reverb_run(&fx->rv, fx->rin, dry, n);
    else fx->rv_peak = 0.0f;
    if (ch_in || fx->ch_peak > FX_PEAK_BUSY) fx->ch_peak = chorus_run(&fx->ch, fx->cin, dry, n);
    else fx->ch_peak = 0.0f;

    for (i = 0; i < 2 * n; i++) out[i] = fx_clip(dry[i]);
}

// Drop-in replacement for tsf_render_short(t, out, n, 0); fx may be NULL.
static void fx_render(fx_t *fx, tsf *t, short *out, int n) {
    if (!fx) { tsf_render_short(t, out, n, 0); return; }
    while (n > 0) {
        int m = (n > BUFFER_SIZE) ? BUFFER_SIZE : n;
        fx_block(fx, t, out, m);
        out += 2 * m;
        n -= m;
    }
}

#endif /* SOP_NO_FX */

/* ---------- playback ---------- */

// Renders 'frames' frames and sends them to the audio device.
// Returns 0 on success, -1 on audio error.
static int render_frames(tsf *t, fx_t *fx, short *buf, uint64_t frames) {
    while (frames > 0 && !g_stop) {
        int chunk = frames > BUFFER_SIZE ? BUFFER_SIZE : (int)frames;
        fx_render(fx, t, buf, chunk);
        if (audio_write(buf, chunk) < 0) {
            if (!g_stop) perror("audio_write");
            return -1;
        }
        frames -= (uint64_t)chunk;
    }
    return 0;
}

// Returns 0 if playback finished (or was interrupted), -1 on audio error.
static int play_song(tsf *t, fx_t *fx, tml_message *msg) {
    short buf[BUFFER_SIZE * 2];          // stereo interleaved
    uint64_t frames_done = 0;

    while (msg && !g_stop) {
        // integer frame clock: no rounding error accumulates between events
        uint64_t target = ((uint64_t)msg->time * SAMPLE_RATE + 500) / 1000;

        if (target > frames_done) {
            if (render_frames(t, fx, buf, target - frames_done) < 0) return -1;
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
            fx_cc(fx, msg->channel, msg->control, msg->control_value);   // CC91 / CC93 depth
            break;
        }
        msg = msg->next;
    }

    if (g_stop) return 0;

    // let release tails and reverb ring out instead of chopping them
    tsf_note_off_all(t);
    uint64_t tail = 0;
    while (tail < TAIL_MAX_FRAMES && !g_stop && (tsf_active_voice_count(t) > 0 || fx_busy(fx))) {
        if (render_frames(t, fx, buf, BUFFER_SIZE) < 0) return -1;
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
    fx_t *fx = NULL;
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

    // tsf ignores notes on a channel until the channel has been set up, and a channel
    // that is first touched by a controller starts on preset 0. Start all 16 channels
    // as General MIDI does (piano, channel 10 = drum kit), so files that play notes
    // without a program change - e.g. percussion-only files - sound as intended.
    for (int ch = 0; ch < 16; ch++)
        tsf_channel_set_presetnumber(g_tsf, ch, 0, ch == 9);

    // read, unwrap and parse the MIDI file
    size_t file_size = 0;
    file_buffer = read_file(midi_filename, &file_size);
    if (!file_buffer) goto cleanup;

    size_t midi_size = file_size;
    const unsigned char *raw_midi = unwrap_riff_midi(file_buffer, &midi_size);

    if (load_midi(raw_midi, midi_size, midi_filename, &song) < 0) goto cleanup;

    free(file_buffer);       // tml copies what it needs
    file_buffer = NULL;

    // per-instrument reverb/chorus sends; NULL (plain tsf rendering) if the bank has none
    fx = fx_create(g_tsf, argc == 3 ? argv[1] : NULL);

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
    if (fx) printf("Per-instrument reverb/chorus sends active.\n");

    rc = (play_song(g_tsf, fx, song.head) < 0) ? 1 : 0;

cleanup:
    if (audio_open) audio_close();
    fx_destroy(fx);
    song_free(&song);
    free(file_buffer);
    tsf_close(g_tsf);
    return rc;
}
