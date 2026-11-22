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
 * mmidi.c - minimal embedded General MIDI player (see mmidi.h).
 *
 * Synth: sample voices with the SoundFont 2 voice model (see README.txt,
 * "What is supported"), playing a SoundFont bank: the built-in one from
 * mm_bank.h, or an .sf2 / .sfm file loaded with mm_load_bank().  Samples of
 * an .sfm bank (also the built-in one) are rendered from models by mmmodel.c.
 * A small stereo reverb (Schroeder/Freeverb style) is fed by CC 91.
 *
 * Build: cc -O2 -c mmidi.c mmmodel.c
 *   -DMM_NO_OSS       leave out the OSS player (mm_play_file)
 *   -DMM_NO_REVERB    leave out the reverb
 *   -DMM_NO_CHORUS    leave out the chorus
 *   -DMM_NO_BUILTIN   leave out the built-in bank (silent until a bank is loaded)
 *   -DMAXVOICE=n      number of voices (default 64; each costs about 400 bytes)
 *   -DMM_NO_MODELS    .sf2 banks only: no .sfm model rendering, implies -DMM_NO_BUILTIN
 *                     (mmmodel.c is still needed, for its maths functions)
 */
#include "mmidi.h"
#include "mmmodel.h"
#if defined MM_NO_MODELS && !defined MM_NO_BUILTIN
#define MM_NO_BUILTIN                  /* the built-in bank is models */
#endif
#ifndef MM_NO_BUILTIN
#include "mm_bank.h"
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* SoundFont 2 generators used by the synth */
enum {
    G_MODLFO_PITCH = 5, G_VIBLFO_PITCH = 6, G_MODENV_PITCH = 7,
    G_FC = 8, G_Q = 9, G_MODLFO_FC = 10, G_MODENV_FC = 11, G_MODLFO_VOL = 13,
    G_CHORUS = 15, G_REVERB = 16, G_PAN = 17,
    G_MODLFO_DELAY = 21, G_MODLFO_FREQ = 22, G_VIBLFO_DELAY = 23, G_VIBLFO_FREQ = 24,
    G_MODENV_DELAY = 25, G_MODENV_ATK = 26, G_MODENV_HOLD = 27, G_MODENV_DEC = 28,
    G_MODENV_SUS = 29, G_MODENV_REL = 30,
    G_DLYV = 33, G_ATKV = 34, G_HOLDV = 35, G_DECV = 36, G_SUSV = 37, G_RELV = 38,
    G_INST = 41, G_KEYRANGE = 43, G_VELRANGE = 44, G_ATTEN = 48,
    G_COARSE = 51, G_FINE = 52, G_SAMPLE = 53, G_MODE = 54,
    G_SCALE = 56, G_EXCL = 57, G_ROOT = 58
};

#define NGEN      61
#ifndef MAXVOICE
#define MAXVOICE  64          /* voices; -DMAXVOICE=128 or 256 for dense songs */
#endif
#define BLOCK     32          /* control-rate block (samples) */
#define MASTER    0.35f       /* default master gain (mm_set_volume) */

/* ---------- samples and zones ---------- */
/* d: sample data; loop ls..le and end in points from d; rate: playback rate
   that sounds MIDI note 60 when root is 60 (root/corr: SF2 sample header) */
typedef struct { float *d; int ls, le, end; double rate; int root, corr; } Wave;

typedef struct { short g[NGEN]; unsigned char lo, hi, vlo, vhi; unsigned short ref; } Zn;

/* ---------- synth state ---------- */
enum { ST_DELAY, ST_ATTACK, ST_HOLD, ST_DECAY, ST_SUSTAIN, ST_RELEASE };

typedef struct {
    int on, ch, key, held, stage;
    int fresh;                       /* started, not rendered yet */
    int excl;                        /* drum choke group, 0 = none */
    unsigned age;
    const Wave *w;
    int ls, le, end, mode;           /* loop and end (points), SF2 sample mode */
    double pos, ratio, note;         /* note: effective MIDI pitch at rest */
    double t, ea, cb;                /* envelope time, attack amp, attenuation cB */
    double dly, atk, hold, dec, rel, sus;
    double atten;                    /* static attenuation cB (gen + velocity) */
    double ml_ph, ml_inc, ml_dly, vl_ph, vl_inc, vl_dly;
    double ml_vol, ml_pitch, vl_pitch, pan;
    double mstage, mt, mval, mdly, matk, mhold, mdec, msus, mrel;   /* modulation envelope */
    double me_pitch, me_fc, ml_fc, fc, q;                     /* its targets; filter (cents, cB) */
    float gain, b0, b1, b2, a1, a2, x1, x2, y1, y2;
    double pan_p;                    /* the pan the gains below were made for */
    float gl, gr;                    /* constant-power pan gains */
    double att_a;                    /* cb2a(atten), for blocks without LFO volume */
    int filt;                        /* 1 static lowpass, 2 modulated (recomputed per block) */
    float rsend, csend;              /* the zone's reverb / chorus send, 0..1 (SF2 gen 16 / 15) */
} Voice;

typedef struct {
    int prog, vol, expr, pan, mod, sus, rpn, range, rev, cho;   /* rev, cho: CC 91, CC 93 */
    double bend;
    int bank;                        /* CC 0 (used with a loaded bank) */
} Chan;

typedef struct { uint32_t tick, seq, tempo; uint8_t type, ch, a, b; } Ev;
enum { EV_NOTE_OFF, EV_NOTE_ON, EV_CONTROL, EV_PROGRAM, EV_PITCH, EV_TEMPO, EV_END,
       EV_RESET };                    /* format 2: a new track (sequence) starts */

#ifndef MM_NO_REVERB
/* ---------- reverb: 4 damped combs + 2 allpasses per side (Freeverb) ---------- */
#define NCOMB 4
#define NALLP 2
#define RV_FEEDBACK 0.82f        /* room size: longer tail when closer to 1 */
#define RV_DAMP     0.25f        /* high-frequency damping in the tail */
#define RV_WET      2.5f         /* reverb output level */
typedef struct { float *b, st; int n, i; } Comb;
typedef struct { float *b; int n, i; } Allp;
typedef struct { Comb c[2][NCOMB]; Allp a[2][NALLP]; float *mem; } Reverb;

static int rv_init(Reverb *r, int rate) {
    static const int cl[NCOMB] = {1116, 1277, 1422, 1557}, al[NALLP] = {556, 341};
    int len[2][NCOMB + NALLP], total = 0;
    for (int ch = 0; ch < 2; ch++)          /* right side 23 samples longer: stereo */
        for (int k = 0; k < NCOMB + NALLP; k++)
            total += len[ch][k] = ((k < NCOMB ? cl[k] : al[k - NCOMB]) + 23 * ch) * rate / 44100 + 1;
    float *m = r->mem = calloc(total, sizeof *m);
    if (!m) return -1;
    for (int ch = 0; ch < 2; ch++)
        for (int k = 0; k < NCOMB + NALLP; k++) {
            if (k < NCOMB) { r->c[ch][k].b = m; r->c[ch][k].n = len[ch][k]; }
            else { r->a[ch][k - NCOMB].b = m; r->a[ch][k - NCOMB].n = len[ch][k]; }
            m += len[ch][k];
        }
    return 0;
}

static void rv_run(Reverb *r, const float *in, float *mix, int n) {
    for (int k = 0; k < n; k++) {
        float x = in[k] * 0.03f + 1e-18f;   /* tiny offset keeps the loops out of denormals */
        for (int ch = 0; ch < 2; ch++) {
            float y = 0;
            for (int j = 0; j < NCOMB; j++) {
                Comb *c = &r->c[ch][j];
                float o = c->b[c->i];
                c->st = o * (1 - RV_DAMP) + c->st * RV_DAMP;
                c->b[c->i] = x + c->st * RV_FEEDBACK;
                if (++c->i >= c->n) c->i = 0;
                y += o;
            }
            for (int j = 0; j < NALLP; j++) {
                Allp *a = &r->a[ch][j];
                float bo = a->b[a->i];
                a->b[a->i] = y + bo * 0.5f;
                if (++a->i >= a->n) a->i = 0;
                y = bo - y;
            }
            mix[2 * k + ch] += y * RV_WET;
        }
    }
}
#endif

#ifndef MM_NO_CHORUS
/* ---------- chorus: mono in, stereo out; one modulated delay tap per side ----------
   As in sop: an 11 ms delay swept by +-3.5 ms at 0.5 Hz, the two sides in
   opposite phase. It runs only while it is fed or still ringing. */
#define CH_RATE_HZ  0.5
#define CH_BASE_MS  11.0
#define CH_DEPTH_MS 3.5
#define CH_LEVEL    0.5f         /* chorus output level: a 100 % send is half as loud as dry */
typedef struct { float *b; unsigned mask, pos; double ph, dph, base, depth; float peak; } Chorus;

static int ch_init(Chorus *c, int rate) {
    unsigned n = 16;
    while (n < (CH_BASE_MS + CH_DEPTH_MS) * 0.001 * rate + 4) n <<= 1;
    if (!(c->b = calloc(n, sizeof *c->b))) return -1;
    c->mask = n - 1;
    c->dph = CH_RATE_HZ / rate;
    c->base = CH_BASE_MS * 0.001 * rate; c->depth = CH_DEPTH_MS * 0.001 * rate;
    return 0;
}

static float ch_tap(const Chorus *c, double delay) {
    double rd = c->pos + c->mask + 1 - delay;
    unsigned i0 = (unsigned)rd;
    float fr = (float)(rd - i0), a = c->b[i0 & c->mask], b = c->b[(i0 + 1) & c->mask];
    return a + (b - a) * fr;
}

static void ch_run(Chorus *c, const float *in, float *mix, int n, int fed) {
    if (!fed && c->peak < 1e-4f) { c->peak = 0; return; }          /* silent: skipped */
    float peak = 0;
    for (int k = 0; k < n; k++) {
        double p = c->ph, q = p + .5 < 1 ? p + .5 : p - .5;
        double lfl = p < .5 ? 4 * p - 1 : 3 - 4 * p, lfr = q < .5 ? 4 * q - 1 : 3 - 4 * q;   /* triangles */
        c->b[c->pos] = in[k];
        float l = ch_tap(c, c->base + c->depth * lfl) * CH_LEVEL, r = ch_tap(c, c->base + c->depth * lfr) * CH_LEVEL;
        c->pos = (c->pos + 1) & c->mask;
        if ((c->ph += c->dph) >= 1) c->ph -= 1;
        mix[2 * k] += l; mix[2 * k + 1] += r;
        if (l > peak) peak = l; else if (-l > peak) peak = -l;
        if (r > peak) peak = r; else if (-r > peak) peak = -r;
    }
    c->peak = peak;
}
#endif

/* ---------- a loaded SoundFont bank (.sf2, or .sfm with sample models) ---------- */
typedef struct Bank {
    Zn *iz, *pz;                     /* instrument and preset zones */
    int *ifirst, *pfirst;            /* zones of instrument / preset k: first[k] .. first[k+1]-1 */
    unsigned short *pbank, *pprog;   /* MIDI bank and program per preset */
    char (*pname)[20];               /* preset names (phdr), not 0-terminated when 20 long */
    int ninst, npre, nsmp;
    int has_rev, has_cho;            /* some zone sets a reverb / chorus send */
    Wave *smp;
    float *pcm;
} Bank;

struct mm_synth {
    int rate;
    Bank *bank;                      /* NULL: the built-in instruments */
    Voice v[MAXVOICE];
    Chan c[16];
    unsigned age;
    Ev *ev;
    int nev, cap, cur;
    double *evtime;                   /* seconds per event, built when first asked for */
    int noplay;                       /* mm_set_playing(s, 0): the song is not played */
    unsigned tpq;
    double tempo, now, next_t, last_tick_t;
    uint32_t last_tick;
    int tail, quiet, ended;
    int loaded;                       /* a file is loaded: the song can end */
    float master;                     /* master gain */
    float ob[2 * BLOCK];              /* the current control block after the master gain,
                                         served to mm_render / mm_render_float */
    int obn, obpos;                   /* its frames, and how many are handed out */                  /* samples after the last event, with and without voices */
#ifndef MM_NO_REVERB
    Reverb rv;
    float rv_in[BLOCK];
#endif
#ifndef MM_NO_CHORUS
    Chorus ch;
    float ch_in[BLOCK];
    int ch_fed;                       /* a voice fed the chorus in this block */
#endif
};

int mm_version(void) { return MMIDI_VERSION; }

static double tc2s(int tc) { return mm_exp2(tc / 1200.0); }
static double cb2a(double cb) { return mm_exp10(-cb / 200); }
static double tri(double ph) { return ph < .25 ? 4 * ph : ph < .75 ? 2 - 4 * ph : 4 * ph - 4; }

static void reset_chans(mm_synth *s) {
    for (int i = 0; i < 16; i++)
        s->c[i] = (Chan){.vol = 100, .expr = 127, .pan = 64, .rpn = 0x3fff, .range = 2,
                         .rev = 40};                  /* GM defaults: reverb 40, chorus 0 */
}

mm_synth *mm_create(int rate) {
    mm_synth *s = calloc(1, sizeof *s);
    if (!s) return NULL;
    s->rate = rate;
    s->master = MASTER;
    reset_chans(s);
    s->tempo = 500000;
    s->tpq = 96;
#ifndef MM_NO_REVERB
    if (rv_init(&s->rv, rate) < 0) { free(s); return NULL; }
#endif
#ifndef MM_NO_CHORUS
    if (ch_init(&s->ch, rate) < 0) { mm_free(s); return NULL; }
#endif
#ifndef MM_NO_BUILTIN
    if (mm_load_bank_mem(s, mm_builtin_bank, sizeof mm_builtin_bank) < 0) { mm_free(s); return NULL; }
#endif
    return s;
}

/* ---------- SoundFont loading ---------- */
static void free_bank(Bank *b) {
    if (!b) return;
    free(b->iz); free(b->pz); free(b->ifirst); free(b->pfirst);
    free(b->pbank); free(b->pprog); free(b->pname); free(b->smp); free(b->pcm);
    free(b);
}

static unsigned le16(const uint8_t *p) { return p[0] | p[1] << 8; }
static uint32_t le32(const uint8_t *p) { return le16(p) | (uint32_t)le16(p + 2) << 16; }

typedef struct { const uint8_t *p; uint32_t n; } Chunk;

/* sub-chunks of a LIST body; the wanted ids are filled in */
static void list_chunks(const uint8_t *p, uint32_t n, const char *const *ids, Chunk *out, int nid) {
    for (uint32_t o = 0; o + 8 <= n;) {
        uint32_t sz = le32(p + o + 4);
        if (sz > n - o - 8) break;
        for (int i = 0; i < nid; i++)
            if (!memcmp(p + o, ids[i], 4)) { out[i].p = p + o + 8; out[i].n = sz; }
        o += 8 + sz + (sz & 1);
    }
}

/* zones of an instrument (inst = 1, terminal sampleID) or preset (terminal
   instrument) list, read as SF2 players do: a zone ends at its terminal
   generator, keyRange is only valid first, velRange first or after keyRange,
   a first zone without terminal is the global zone (defaults for the rest) */
static int read_zones(const uint8_t *hdr, int nhdr, int hsize, int boff, const Chunk *bag, const Chunk *gen,
                      int inst, Zn **zout, int **fout) {
    int nbag = bag->n / 4, ngen = gen->n / 4, n = 0, term = inst ? G_SAMPLE : G_INST;
    Zn *z = calloc(nbag + 1, sizeof *z);
    int *first = calloc(nhdr + 1, sizeof *first);
    if (!z || !first) { free(z); free(first); return -1; }
    for (int k = 0; k < nhdr; k++) {
        first[k] = n;
        int b0 = le16(hdr + k * hsize + boff), b1 = le16(hdr + (k + 1) * hsize + boff);
        Zn base = {{0}, 0, 127, 0, 127, 0};
        if (inst) {
            static const int tc[] = {G_MODLFO_DELAY, G_VIBLFO_DELAY, G_MODENV_DELAY, G_MODENV_ATK,
                                     G_MODENV_HOLD, G_MODENV_DEC, G_MODENV_REL,
                                     G_DLYV, G_ATKV, G_HOLDV, G_DECV, G_RELV};
            for (unsigned i = 0; i < sizeof tc / sizeof *tc; i++) base.g[tc[i]] = -12000;
            base.g[G_FC] = 13500; base.g[G_SCALE] = 100; base.g[G_ROOT] = -1;
        }
        for (int bi = b0; bi < b1 && bi + 1 < nbag; bi++) {
            int g0 = le16(bag->p + bi * 4), g1 = le16(bag->p + (bi + 1) * 4), has = 0;
            Zn cur = base;
            if (g1 > ngen) g1 = ngen;
            for (int j = g0; j < g1; j++) {
                int op = le16(gen->p + j * 4), v = le16(gen->p + j * 4 + 2);
                if (op == term) { cur.ref = v; has = 1; break; }
                if (op == G_KEYRANGE) { if (j == g0) { cur.lo = v & 255; cur.hi = v >> 8; } }
                else if (op == G_VELRANGE) {
                    if (j == g0 || (j == g0 + 1 && le16(gen->p + g0 * 4) == G_KEYRANGE)) { cur.vlo = v & 255; cur.vhi = v >> 8; }
                }
                else if (op < NGEN) cur.g[op] = (short)v;
            }
            if (has) z[n++] = cur;
            else if (bi == b0) base = cur;                 /* global zone */
        }
    }
    first[nhdr] = n;
    *zout = z; *fout = first;
    return 0;
}

int mm_load_bank_mem(mm_synth *s, const void *data, size_t size) {
    const uint8_t *d = data;
    if (size < 12 || memcmp(d, "RIFF", 4) || memcmp(d + 8, "sfbk", 4)) return -1;
    static const char *const sid[] = {"smpl", "smdl"};
    static const char *const pid[] = {"phdr", "pbag", "pgen", "inst", "ibag", "igen", "shdr"};
    Chunk sd[2] = {{0, 0}}, pd[7] = {{0, 0}};   /* all fields: gcc 3.4 -Wextra asks for them */
    uint32_t total = le32(d + 4) + 8 < size ? le32(d + 4) + 8 : (uint32_t)size;
    for (uint32_t o = 12; o + 12 <= total;) {
        uint32_t sz = le32(d + o + 4);
        if (sz > total - o - 8) break;
        if (!memcmp(d + o, "LIST", 4) && !memcmp(d + o + 8, "sdta", 4)) list_chunks(d + o + 12, sz - 4, sid, sd, 2);
        if (!memcmp(d + o, "LIST", 4) && !memcmp(d + o + 8, "pdta", 4)) list_chunks(d + o + 12, sz - 4, pid, pd, 7);
        o += 8 + sz + (sz & 1);
    }
    for (int i = 0; i < 7; i++) if (!pd[i].p) return -1;
#ifdef MM_NO_MODELS
    if (!sd[0].p) return -2;                        /* an .sfm: not in this build */
#else
    if (!sd[0].p && !sd[1].p) return -1;
#endif
    int npre = pd[0].n / 38 - 1, ninst = pd[3].n / 22 - 1, nsmp = pd[6].n / 46 - 1;
    if (npre < 1 || ninst < 1 || nsmp < 1) return -1;
    Bank *b = calloc(1, sizeof *b);
    if (!b) return -1;
    b->npre = npre; b->ninst = ninst; b->nsmp = nsmp;
    b->pbank = malloc(npre * sizeof *b->pbank);
    b->pprog = malloc(npre * sizeof *b->pprog);
    b->pname = malloc(npre * sizeof *b->pname);
    b->smp = calloc(nsmp, sizeof *b->smp);
    if (!b->pbank || !b->pprog || !b->pname || !b->smp ||
        read_zones(pd[3].p, ninst, 22, 20, &pd[4], &pd[5], 1, &b->iz, &b->ifirst) ||
        read_zones(pd[0].p, npre, 38, 24, &pd[1], &pd[2], 0, &b->pz, &b->pfirst)) { free_bank(b); return -1; }
    for (int i = 0; i < b->ifirst[ninst]; i++) { b->has_rev |= b->iz[i].g[G_REVERB] != 0; b->has_cho |= b->iz[i].g[G_CHORUS] != 0; }
    for (int i = 0; i < b->pfirst[npre]; i++) { b->has_rev |= b->pz[i].g[G_REVERB] != 0; b->has_cho |= b->pz[i].g[G_CHORUS] != 0; }
    for (int i = 0; i < npre; i++) {
        b->pprog[i] = le16(pd[0].p + i * 38 + 20);
        b->pbank[i] = le16(pd[0].p + i * 38 + 22);
        memcpy(b->pname[i], pd[0].p + i * 38, 20);
    }
    /* sample data: 16-bit PCM, or models rendered into the same places */
    uint32_t npts = 0;
    for (int i = 0; i < nsmp; i++) {
        uint32_t e = le32(pd[6].p + i * 46 + 24);
        if (e > npts) npts = e;
    }
    if (sd[0].p && sd[0].n / 2 < npts) npts = sd[0].n / 2;
    b->pcm = calloc((size_t)npts + 64, sizeof *b->pcm);
    if (!b->pcm) { free_bank(b); return -1; }
    if (sd[0].p)
        for (uint32_t i = 0; i < npts; i++) b->pcm[i] = (int16_t)le16(sd[0].p + 2 * i) / 32768.0f;
#ifndef MM_NO_MODELS
    else {
        for (uint32_t q = 0; q + 8 <= sd[1].n;) {
            uint32_t idx = le32(sd[1].p + q), n = le32(sd[1].p + q + 4);
            if (n > sd[1].n - q - 8) break;
            if (idx < (uint32_t)nsmp) {
                const uint8_t *h = pd[6].p + idx * 46;
                uint32_t st = le32(h + 20), en = le32(h + 24);
                if (st < en && en <= npts) {
                    float *o = b->pcm + st;
                    if (mm_model_render(sd[1].p + q + 8, n, o, en - st) == 0)
                        for (uint32_t k = 0; k < en - st; k++) o[k] /= 32768.0f;
                }
            }
            q += 8 + n;
        }
        mm_model_done();
    }
#endif
    for (int i = 0; i < nsmp; i++) {
        const uint8_t *h = pd[6].p + i * 46;
        uint32_t st = le32(h + 20), en = le32(h + 24), ls = le32(h + 28), le = le32(h + 32), rate = le32(h + 36);
        Wave *w = &b->smp[i];
        if (st >= npts || en > npts || st >= en || !rate || (le16(h + 44) & 0x8000)) { w->d = b->pcm; continue; }
        w->d = b->pcm + st;
        w->end = en - st;
        w->ls = ls >= st && ls < en ? ls - st : 0;
        w->le = le > ls && le <= en ? le - st : 0;
        w->rate = rate;
        w->root = h[40] <= 127 ? h[40] : 60;
        w->corr = (signed char)h[41];
    }
    free_bank(s->bank);
    s->bank = b;
    for (int i = 0; i < MAXVOICE; i++) s->v[i].on = 0;
    return 0;
}

int mm_load_bank(mm_synth *s, const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    long n = -1;
    void *buf = NULL;
    if (!fseek(f, 0, SEEK_END) && (n = ftell(f)) > 0 && !fseek(f, 0, SEEK_SET) &&
        (buf = malloc(n)) && fread(buf, 1, n, f) != (size_t)n) n = -1;
    fclose(f);
    int r = n > 0 && buf ? mm_load_bank_mem(s, buf, n) : -1;
    free(buf);
    return r;
}

void mm_free(mm_synth *s) {
    if (!s) return;
    free_bank(s->bank);
#ifndef MM_NO_REVERB
    free(s->rv.mem);
#endif
#ifndef MM_NO_CHORUS
    free(s->ch.b);
#endif
    free(s->ev);
    free(s->evtime);
    free(s);
}

/* ---------- voices ---------- */
static Voice *alloc_voice(mm_synth *s) {
    Voice *best = NULL;
    double bestscore = 1e9;
    for (int i = 0; i < MAXVOICE; i++) {
        Voice *v = &s->v[i];
        if (!v->on) return v;
        /* steal: released voices first, then the quietest, then the oldest */
        double score = (v->fresh ? 100 : v->gain) + (v->stage == ST_RELEASE ? 0 : 10) +
                       (s->age - v->age) * -1e-6;
        if (score < bestscore) { bestscore = score; best = v; }
    }
    return best;
}

/* RBJ lowpass coefficients for cutoff fc (cents) and resonance q (cB) */
static void set_filter(Voice *v, double fc_cents, double q_cb, int rate) {
    double fc = 8.176 * tc2s((int)fc_cents);
    if (fc > 0.45 * rate) fc = 0.45 * rate;
    if (fc < 20) fc = 20;
    double q = mm_exp10((q_cb / 10.0 - 3.01) / 20);
    double c = mm_cos(2 * M_PI * fc / rate), al = sqrt(1 - c * c) / (2 * q), a0 = 1 + al;
    v->b0 = v->b2 = (float)((1 - c) / 2 / a0); v->b1 = (float)((1 - c) / a0);
    v->a1 = (float)(-2 * c / a0); v->a2 = (float)((1 - al) / a0);
}

/* start a voice: g = summed generators, note = MIDI pitch at which the wave
   plays at its own rate (see Wave), mode = SF2 sample mode */
static void start_voice(mm_synth *s, int ch, int key, int vel, const short *g, const Wave *wave,
                        double note, int mode, int excl) {
    Voice *v = alloc_voice(s);
    memset(v, 0, sizeof *v);
    v->on = 1; v->ch = ch; v->key = key; v->age = ++s->age;
    v->fresh = 1; v->excl = excl;
    v->w = wave;
    v->note = note;
    v->ratio = wave->rate / s->rate;
    v->mode = mode;
    /* sample offsets (fine + 32768 x coarse), kept inside the sample */
    int st = g[0] + 32768 * g[4], en = wave->end + g[1] + 32768 * g[12];
    v->ls = wave->ls + g[2] + 32768 * g[45];
    v->le = wave->le + g[3] + 32768 * g[50];
    if (en > wave->end) en = wave->end;
    if (st < 0 || st >= en) st = 0;
    if (v->le > en) v->le = en;
    if (v->ls < st || v->ls >= v->le) { v->ls = st; if (v->le <= v->ls) v->mode = 0; }
    v->end = en;
    v->pos = st;
    /* envelopes; hold and decay may follow the key (keynumTo...: per key below 60) */
    v->dly = tc2s(g[G_DLYV]); v->atk = tc2s(g[G_ATKV]);
    v->hold = tc2s(g[G_HOLDV] + g[39] * (60 - key));
    v->dec = tc2s(g[G_DECV] + g[40] * (60 - key)); v->rel = tc2s(g[G_RELV]);
    if (v->rel < .006) v->rel = .006;
    v->sus = g[G_SUSV] < 0 ? 0 : g[G_SUSV] > 1440 ? 1440 : g[G_SUSV];
    v->mdly = tc2s(g[G_MODENV_DELAY]); v->matk = tc2s(g[G_MODENV_ATK]);
    v->mhold = tc2s(g[G_MODENV_HOLD] + g[31] * (60 - key));
    v->mdec = tc2s(g[G_MODENV_DEC] + g[32] * (60 - key)); v->mrel = tc2s(g[G_MODENV_REL]);
    v->msus = 1 - (g[G_MODENV_SUS] < 0 ? 0 : g[G_MODENV_SUS] > 1000 ? 1000 : g[G_MODENV_SUS]) / 1000.0;
    v->me_pitch = g[G_MODENV_PITCH]; v->me_fc = g[G_MODENV_FC]; v->ml_fc = g[G_MODLFO_FC];
    /* attenuation generator scaled 0.4 like EMU hardware / FluidSynth;
       velocity follows the SF2 default concave curve: gain = (vel/127)^2 */
    v->atten = g[G_ATTEN] * 0.4 - 400 * mm_log10(vel / 127.0);
    v->att_a = cb2a(v->atten);
    v->ml_dly = tc2s(g[G_MODLFO_DELAY]); v->ml_inc = 8.176 * tc2s(g[G_MODLFO_FREQ]) / s->rate;
    v->vl_dly = tc2s(g[G_VIBLFO_DELAY]); v->vl_inc = 8.176 * tc2s(g[G_VIBLFO_FREQ]) / s->rate;
    v->ml_vol = g[G_MODLFO_VOL]; v->ml_pitch = g[G_MODLFO_PITCH]; v->vl_pitch = g[G_VIBLFO_PITCH];
    v->pan = g[G_PAN] / 1000.0;
    v->rsend = g[G_REVERB] / 1000.0f;
    v->csend = g[G_CHORUS] / 1000.0f;
    v->stage = ST_DELAY;
    v->fc = g[G_FC]; v->q = g[G_Q];
    if (v->me_fc || v->ml_fc) v->filt = 2;               /* recomputed every block */
    else if (g[G_FC] < 13500 && 8.176 * tc2s(g[G_FC]) < 0.45 * s->rate) {
        set_filter(v, v->fc, v->q, s->rate);
        v->filt = 1;
    }
}

static void release(Voice *v) {
    if (v->stage == ST_RELEASE) return;
    v->mstage = ST_RELEASE;
    if (v->stage <= ST_ATTACK) v->cb = v->ea > 1e-5 ? -200 * mm_log10(v->ea) : 960;
    v->stage = ST_RELEASE;
}

/* fast fade (20 ms) for choked or retriggered-in-group voices */
static void choke(Voice *v) {
    if (v->rel > .02) v->rel = .02;
    v->held = 0;
    release(v);
}

static void choke_class(mm_synth *s, int ch, int excl) {
    for (int i = 0; i < MAXVOICE; i++)
        if (s->v[i].on && s->v[i].ch == ch && s->v[i].excl == excl) choke(&s->v[i]);
}

static int find_preset(const Bank *b, int bank, int prog) {
    for (int i = 0; i < b->npre; i++)
        if (b->pbank[i] == bank && b->pprog[i] == prog) return i;
    return -1;
}

/* the preset a note on channel ch plays: the channel's bank and program, a
   drum kit on channel 10; falling back to bank 0 / kit 0 */
static int chan_preset(const mm_synth *s, int ch) {
    const Bank *b = s->bank;
    int prog = s->c[ch].prog, p;
    if (ch == 9) { if ((p = find_preset(b, 128, prog)) < 0) p = find_preset(b, 128, 0); }
    else if ((p = find_preset(b, s->c[ch].bank, prog)) < 0) p = find_preset(b, 0, prog);
    return p;
}

void mm_set_volume(mm_synth *s, float gain) { s->master = gain > 0 ? gain : 0; }

int mm_preset_count(const mm_synth *s) { return s->bank ? s->bank->npre : 0; }

int mm_preset_info(const mm_synth *s, int i, int *bank, int *prog, char name[21]) {
    if (!s->bank || i < 0 || i >= s->bank->npre) return -1;
    if (bank) *bank = s->bank->pbank[i];
    if (prog) *prog = s->bank->pprog[i];
    if (name) {
        memcpy(name, s->bank->pname[i], 20);
        name[20] = 0;
    }
    return 0;
}

int mm_preset_find(const mm_synth *s, int bank, int prog) { return s->bank ? find_preset(s->bank, bank, prog) : -1; }

int mm_channel_preset(const mm_synth *s, int ch) { return s->bank && ch >= 0 && ch < 16 ? chan_preset(s, ch) : -1; }

static void bank_note_on(mm_synth *s, int ch, int key, int vel) {
    const Bank *b = s->bank;
    int p = chan_preset(s, ch);
    if (p < 0) return;
    const Zn *hp[32], *hi[32];
    int n = 0;
    for (int i = b->pfirst[p]; i < b->pfirst[p + 1]; i++) {
        const Zn *P = &b->pz[i];
        if (key < P->lo || key > P->hi || vel < P->vlo || vel > P->vhi || P->ref >= b->ninst) continue;
        for (int j = b->ifirst[P->ref]; j < b->ifirst[P->ref + 1] && n < 32; j++) {
            const Zn *I = &b->iz[j];
            if (key < I->lo || key > I->hi || vel < I->vlo || vel > I->vhi || I->ref >= b->nsmp) continue;
            if (I->g[G_EXCL]) choke_class(s, ch, I->g[G_EXCL]);   /* before the new voices start */
            hp[n] = P; hi[n++] = I;
        }
    }
    for (int z = 0; z < n; z++) {
        short g[NGEN];
        for (int k = 0; k < NGEN; k++) g[k] = hi[z]->g[k] + hp[z]->g[k];   /* preset values add */
        const Wave *w = &b->smp[hi[z]->ref];
        int root = hi[z]->g[G_ROOT] >= 0 ? hi[z]->g[G_ROOT] : w->root;   /* instrument-only */
        double note = 60 + (key - root) * g[G_SCALE] / 100.0 + g[G_COARSE] + (g[G_FINE] + w->corr) / 100.0;
        int mode = hi[z]->g[G_MODE] & 3;
        start_voice(s, ch, key, vel, g, w, note, mode == 2 ? 0 : mode, hi[z]->g[G_EXCL]);
    }
}

static void note_on(mm_synth *s, int ch, int key, int vel) {
    for (int i = 0; i < MAXVOICE; i++) {                       /* repeated key */
        Voice *v = &s->v[i];
        if (v->on && v->ch == ch && v->key == key && v->stage != ST_RELEASE) { v->held = 0; release(v); }
    }
    if (s->bank) bank_note_on(s, ch, key, vel);
}

static void note_off(mm_synth *s, int ch, int key) {
    for (int i = 0; i < MAXVOICE; i++) {
        Voice *v = &s->v[i];
        if (v->on && v->ch == ch && v->key == key && !v->held && v->stage != ST_RELEASE) {
            if (s->c[ch].sus) v->held = 1;
            else release(v);
        }
    }
}

static void control(mm_synth *s, int ch, int cc, int val) {
    Chan *c = &s->c[ch];
    switch (cc) {
    case 0: c->bank = val; break;                             /* bank select */
    case 1: c->mod = val; break;
    case 6: if (c->rpn == 0) c->range = val > 24 ? 24 : val; break;   /* bend range */
    case 7: c->vol = val; break;
    case 10: c->pan = val; break;
    case 11: c->expr = val; break;
    case 91: c->rev = val; break;                             /* reverb send */
    case 93: c->cho = val; break;                             /* chorus send */
    case 64:
        c->sus = val >= 64;
        if (!c->sus)
            for (int i = 0; i < MAXVOICE; i++)
                if (s->v[i].on && s->v[i].ch == ch && s->v[i].held) { s->v[i].held = 0; release(&s->v[i]); }
        break;
    case 100: c->rpn = (c->rpn & 0x3f80) | val; break;
    case 101: c->rpn = (c->rpn & 0x7f) | val << 7; break;
    case 120:                                                 /* all sound off */
        for (int i = 0; i < MAXVOICE; i++) if (s->v[i].ch == ch) s->v[i].on = 0;
        break;
    case 121:                                                 /* reset controllers */
        c->mod = 0; c->expr = 127; c->bend = 0; c->rpn = 0x3fff;
        control(s, ch, 64, 0);
        break;
    case 123: case 124: case 125: case 126: case 127:        /* all notes off */
        for (int i = 0; i < MAXVOICE; i++)
            if (s->v[i].on && s->v[i].ch == ch) { s->v[i].held = 0; release(&s->v[i]); }
        break;
    }
}

void mm_midi(mm_synth *s, int status, int d1, int d2) {
    int ch = status & 15;
    d1 &= 127; d2 &= 127;
    switch (status & 0xf0) {
    case 0x80: note_off(s, ch, d1); break;
    case 0x90: if (d2) note_on(s, ch, d1, d2); else note_off(s, ch, d1); break;
    case 0xb0: control(s, ch, d1, d2); break;
    case 0xc0: s->c[ch].prog = d1; break;
    case 0xe0: s->c[ch].bend = ((d2 << 7 | d1) - 8192) / 8192.0; break;
    case 0xf0:
        if (status == 0xfe)       /* format 2: a new sequence starts. The sounding notes are
                                     released; each channel keeps its program, bank, controllers
                                     and pitch bend, which later sequences often rely on */
            for (int i = 0; i < MAXVOICE; i++)
                if (s->v[i].on) { s->v[i].held = 0; release(&s->v[i]); }
        break;
    }
}

/* render n (<= BLOCK) frames of all voices, adding into mix */
static void render_block(mm_synth *s, float *mix, int n) {
    double dt = (double)n / s->rate;
    for (int i = 0; i < MAXVOICE; i++) {
        Voice *v = &s->v[i];
        if (!v->on) continue;
        Chan *c = &s->c[v->ch];

        /* volume envelope */
        double eg;
        switch (v->stage) {
        case ST_DELAY:
            if ((v->t += dt) >= v->dly) { v->stage = ST_ATTACK; v->t = 0; }
            break;
        case ST_ATTACK:
            if ((v->ea += dt / v->atk) >= 1) { v->ea = 1; v->stage = ST_HOLD; v->t = 0; }
            break;
        case ST_HOLD:
            if ((v->t += dt) >= v->hold) v->stage = ST_DECAY;
            break;
        case ST_DECAY:
            if ((v->cb += 1440 * dt / v->dec) >= v->sus) { v->cb = v->sus; v->stage = ST_SUSTAIN; }
            break;
        case ST_RELEASE:
            v->cb += 1440 * dt / v->rel;
            break;
        }
        if (v->stage >= ST_DECAY && v->cb >= 960) { v->on = 0; continue; }
        eg = v->stage <= ST_ATTACK ? v->ea : v->stage == ST_HOLD ? 1 : cb2a(v->cb);

        /* modulation envelope (linear 0..1), only when something uses it */
        if (v->me_pitch || v->me_fc) {
            switch ((int)v->mstage) {
            case ST_DELAY:
                if ((v->mt += dt) >= v->mdly) { v->mstage = ST_ATTACK; v->mt = 0; }
                break;
            case ST_ATTACK:
                if ((v->mval += dt / v->matk) >= 1) { v->mval = 1; v->mstage = ST_HOLD; v->mt = 0; }
                break;
            case ST_HOLD:
                if ((v->mt += dt) >= v->mhold) v->mstage = ST_DECAY;
                break;
            case ST_DECAY:
                if ((v->mval -= dt / v->mdec) <= v->msus) { v->mval = v->msus; v->mstage = ST_SUSTAIN; }
                break;
            case ST_RELEASE:
                if ((v->mval -= dt / v->mrel) < 0) v->mval = 0;
                break;
            }
        }

        /* LFOs */
        double ml = 0, vl = 0;
        if ((v->ml_dly -= dt) < 0) { ml = tri(v->ml_ph); v->ml_ph = fmod(v->ml_ph + v->ml_inc * n, 1); }
        if ((v->vl_dly -= dt) < 0) { vl = tri(v->vl_ph); v->vl_ph = fmod(v->vl_ph + v->vl_inc * n, 1); }

        double vol = (c->vol / 127.0) * (c->vol / 127.0) * (c->expr / 127.0) * (c->expr / 127.0);
        double att;
        if (ml * v->ml_vol != 0) att = cb2a(v->atten + ml * v->ml_vol);
        else att = v->att_a;
        float g1 = (float)(eg * att * vol);
        double pitch = v->note + c->bend * c->range +
                       (vl * (v->vl_pitch + c->mod * 50 / 127.0) + ml * v->ml_pitch + v->mval * v->me_pitch) / 100;
        if (v->filt == 2) set_filter(v, v->fc + v->mval * v->me_fc + ml * v->ml_fc, v->q, s->rate);
        double step = v->ratio * mm_exp2((pitch - 60) / 12);
#ifndef MM_NO_REVERB
        /* reverb send: a bank with send values follows SF2 (the zone's value,
           plus up to 20 % from CC 91); a bank without any, CC 91 alone, 0-100 % */
        float *rin = s->rv_in, send = c->rev / 127.0f;
        if (s->bank->has_rev) {
            send = v->rsend + send * 0.2f;
            send = send < 0 ? 0 : send > 1 ? 1 : send;
        }
#endif
#ifndef MM_NO_CHORUS
        /* chorus send, the same way: zone value + CC 93 x 20 %, or CC 93 alone */
        float *cin = s->ch_in, csend = c->cho / 127.0f;
        if (s->bank->has_cho) {
            csend = v->csend + csend * 0.2f;
            csend = csend < 0 ? 0 : csend > 1 ? 1 : csend;
        }
        if (csend > 0) s->ch_fed = 1;
#endif
        double p = v->pan + (c->pan - 64) / 128.0;
        p = p < -.5 ? -.5 : p > .5 ? .5 : p;
        /* constant-power pan, recomputed only when the pan moves */
        if (v->fresh || p != v->pan_p) {
            v->pan_p = p;
            v->gl = (float)mm_cos((p + .5) * M_PI / 2); v->gr = (float)mm_cos((.5 - p) * M_PI / 2);
        }
        float gl = v->gl, gr = v->gr;

        const float *d = v->w->d;
        int ls = v->ls, le = v->le, loop = v->mode == 1 || (v->mode == 3 && v->stage != ST_RELEASE);
        float g0 = v->gain, dg = (g1 - g0) / n;
        double pos = v->pos;
        for (int k = 0; k < n; k++) {
            int ip = (int)pos;
            float fr = (float)(pos - ip), x = d[ip] + (d[ip + 1] - d[ip]) * fr;
            if (v->filt) {
                float y = v->b0 * x + v->b1 * v->x1 + v->b2 * v->x2 - v->a1 * v->y1 - v->a2 * v->y2;
                v->x2 = v->x1; v->x1 = x; v->y2 = v->y1; v->y1 = y; x = y;
            }
            x *= g0 + dg * k;
            mix[2 * k] += x * gl;
            mix[2 * k + 1] += x * gr;
#ifndef MM_NO_REVERB
            rin[k] += x * send;
#endif
#ifndef MM_NO_CHORUS
            cin[k] += x * csend;
#endif
            pos += step;
            if (loop) { while (pos >= le) pos -= le - ls; }
            else if (pos >= v->end - 1) { v->on = 0; break; }      /* one-shot: sample ended */
        }
        v->pos = pos;
        v->gain = g1;
        v->fresh = 0;
    }
}

/* ---------- sequencer ---------- */
static double ticks_to_samples(mm_synth *s, uint32_t dticks) {
    return dticks * (s->tempo / 1e6) / s->tpq * s->rate;
}

static void set_next(mm_synth *s) {
    if (s->cur < s->nev)
        s->next_t = s->last_tick_t + ticks_to_samples(s, s->ev[s->cur].tick - s->last_tick);
}

static void play_event(mm_synth *s, const Ev *e) {
    s->last_tick_t += ticks_to_samples(s, e->tick - s->last_tick);
    s->last_tick = e->tick;
    switch (e->type) {
    case EV_TEMPO: if (e->tempo) s->tempo = e->tempo; break;
    case EV_RESET:                   /* the notes still sounding fade out on their release */
        mm_midi(s, 0xfe, 0, 0);
        s->tempo = 500000;
        break;
    case EV_NOTE_ON: mm_midi(s, 0x90 | e->ch, e->a, e->b); break;
    case EV_NOTE_OFF: mm_midi(s, 0x80 | e->ch, e->a, 0); break;
    case EV_CONTROL: mm_midi(s, 0xb0 | e->ch, e->a, e->b); break;
    case EV_PROGRAM: mm_midi(s, 0xc0 | e->ch, e->a, 0); break;
    case EV_PITCH: mm_midi(s, 0xe0 | e->ch, e->a, e->b); break;
    }
}

/* render the next control block into s->ob: up to BLOCK frames, cut short at
   the next event; 0 = the song has ended. Blocks follow the song alone, never
   the size of the mm_render() calls, so the output does not depend on it */
static int render_step(mm_synth *s) {
    float mix[2 * BLOCK];
    while (!s->noplay && s->cur < s->nev && s->next_t <= s->now) {
        play_event(s, &s->ev[s->cur++]);
        set_next(s);
    }
    int n = BLOCK;
    if (!s->noplay && s->cur < s->nev && s->next_t - s->now < n) {
        n = (int)ceil(s->next_t - s->now);
        if (n < 1) n = 1;
    }
    if (s->loaded && !s->noplay && s->cur >= s->nev && !s->ended) {   /* song over: release stuck notes */
        s->ended = 1;
        for (int i = 0; i < MAXVOICE; i++)
            if (s->v[i].on) { s->v[i].held = 0; release(&s->v[i]); }
    }
    if (s->loaded && !s->noplay && s->cur >= s->nev) {   /* let voices ring out (max 3 s) */
        int any = 0;
        for (int i = 0; i < MAXVOICE; i++) any |= s->v[i].on;
        if (any) s->tail += n; else s->quiet += n;
#ifndef MM_NO_REVERB
        if (s->quiet > s->rate * 3 / 2) return 0;      /* then 1.5 s of reverb tail */
#else
        if (s->quiet) return 0;
#endif
        if (s->tail > 3 * s->rate) return 0;
    }
    memset(mix, 0, sizeof mix);
#ifndef MM_NO_REVERB
    memset(s->rv_in, 0, sizeof s->rv_in);
#endif
#ifndef MM_NO_CHORUS
    memset(s->ch_in, 0, sizeof s->ch_in);
    s->ch_fed = 0;
#endif
    render_block(s, mix, n);
#ifndef MM_NO_REVERB
    rv_run(&s->rv, s->rv_in, mix, n);
#endif
#ifndef MM_NO_CHORUS
    ch_run(&s->ch, s->ch_in, mix, n, s->ch_fed);
#endif
    for (int k = 0; k < 2 * n; k++) s->ob[k] = mix[k] * s->master;
    s->now += n;
    s->obn = n; s->obpos = 0;
    return n;
}

/* hand out up to 'frames' frames of control blocks, as 16-bit or as float */
static int serve(mm_synth *s, short *out, float *fout, int frames) {
    int done = 0;
    while (done < frames) {
        if (s->obpos == s->obn && !render_step(s)) break;
        int n = s->obn - s->obpos;
        if (n > frames - done) n = frames - done;
        const float *in = s->ob + 2 * s->obpos;
        if (fout) memcpy(fout + 2 * done, in, n * 2 * sizeof *fout);
        else
            for (int k = 0; k < 2 * n; k++) {
                float x = in[k], a = x < 0 ? -x : x;
                if (a > .75f) {               /* soft knee: smooth instead of hard clipping */
                    a = .75f + .25f * (float)mm_tanh((a - .75f) * 4);
                    x = x < 0 ? -a : a;
                }
                out[2 * done + k] = (short)(x * 32767);
            }
        s->obpos += n;
        done += n;
    }
    return done;
}

int mm_render(mm_synth *s, short *out, int frames) { return serve(s, out, NULL, frames); }

int mm_render_float(mm_synth *s, float *out, int frames) { return serve(s, NULL, out, frames); }

/* all voices off, channels to GM defaults, the song back to its start */
static void rewind_song(mm_synth *s) {
    memset(s->v, 0, sizeof s->v);
    reset_chans(s);
    s->cur = 0; s->now = 0; s->last_tick = 0; s->last_tick_t = 0; s->tail = s->quiet = s->ended = 0;
    s->obn = s->obpos = 0;                          /* drop the rest of the last block */
    s->tempo = 500000;
    set_next(s);
}

void mm_reset(mm_synth *s) {
    rewind_song(s);
#ifndef MM_NO_REVERB
    for (int ch = 0; ch < 2; ch++) {                /* silence the reverb tail */
        for (int k = 0; k < NCOMB; k++) { Comb *cb = &s->rv.c[ch][k]; memset(cb->b, 0, cb->n * sizeof *cb->b); cb->st = 0; cb->i = 0; }
        for (int k = 0; k < NALLP; k++) { Allp *a = &s->rv.a[ch][k]; memset(a->b, 0, a->n * sizeof *a->b); a->i = 0; }
    }
#endif
#ifndef MM_NO_CHORUS
    memset(s->ch.b, 0, (s->ch.mask + 1) * sizeof *s->ch.b);  /* and the chorus */
    s->ch.pos = 0; s->ch.ph = 0; s->ch.peak = 0;
#endif
}

void mm_all_notes_off(mm_synth *s) {
    for (int i = 0; i < MAXVOICE; i++)
        if (s->v[i].on) { s->v[i].held = 0; release(&s->v[i]); }
}

int mm_active_voices(const mm_synth *s) {
    int n = 0;
    for (int i = 0; i < MAXVOICE; i++) n += s->v[i].on != 0;
    return n;
}

void mm_set_playing(mm_synth *s, int on) {
    if (on && s->noplay) {               /* the song starts again from here */
        s->cur = 0; s->last_tick = 0; s->last_tick_t = s->now; s->tail = s->quiet = s->ended = 0;
        s->tempo = 500000;
        set_next(s);
    }
    s->noplay = !on;
}

/* ---------- event access ---------- */
/* the end-of-song marker, if any, is the last event and is not handed out */
static int nev_out(const mm_synth *s) { return s->nev && s->ev[s->nev - 1].type == EV_END ? s->nev - 1 : s->nev; }

int mm_event_count(const mm_synth *s) { return nev_out(s); }

/* one tempo-map pass, as the sequencer plays: a tempo change applies after its event */
static int build_times(mm_synth *s) {
    if (s->evtime) return 0;
    if (!s->nev || !(s->evtime = malloc(s->nev * sizeof *s->evtime))) return -1;
    double t = 0, tempo = 500000;
    uint32_t last = 0;
    for (int i = 0; i < s->nev; i++) {
        const Ev *e = &s->ev[i];
        t += (e->tick - last) * (tempo / 1e6) / s->tpq;
        last = e->tick;
        s->evtime[i] = t;
        if (e->type == EV_TEMPO && e->tempo) tempo = e->tempo;
        if (e->type == EV_RESET) tempo = 500000;
    }
    return 0;
}

int mm_get_event(mm_synth *s, int i, mm_event *out) {
    if (i < 0 || i >= nev_out(s) || build_times(s) < 0) return -1;
    static const unsigned char st[] = {0x80, 0x90, 0xb0, 0xc0, 0xe0, 0xff, 0, 0xfe};
    const Ev *e = &s->ev[i];
    int meta = e->type == EV_TEMPO || e->type == EV_RESET;
    out->time = s->evtime[i];
    out->status = meta ? st[e->type] : st[e->type] | e->ch;
    out->d1 = meta ? 0 : e->a;
    out->d2 = meta || e->type == EV_PROGRAM ? 0 : e->b;
    out->tempo = e->type == EV_TEMPO ? (long)e->tempo : 0;
    return 0;
}

double mm_song_length(mm_synth *s) { return build_times(s) < 0 ? 0 : s->evtime[s->nev - 1]; }

/* ---------- Standard MIDI File parser ---------- */
static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }
static uint16_t be16(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }

static int read_vlq(const uint8_t *b, size_t n, size_t *pos, uint32_t *out) {
    uint32_t v = 0;
    for (int i = 0; i < 4; i++) {
        if (*pos >= n) return -1;
        uint8_t c = b[(*pos)++];
        v = v << 7 | (c & 127);
        if (!(c & 128)) { *out = v; return 0; }
    }
    return -1;
}

static int add_event(mm_synth *s, uint32_t tick, int type, int ch, int a, int b, uint32_t tempo) {
    if (s->nev == s->cap) {
        int cap = s->cap ? s->cap * 2 : 1024;
        Ev *e = realloc(s->ev, cap * sizeof *e);
        if (!e) return -1;
        s->ev = e; s->cap = cap;
    }
    s->ev[s->nev] = (Ev){tick, (uint32_t)s->nev, tempo, (uint8_t)type, (uint8_t)ch, (uint8_t)a, (uint8_t)b};
    s->nev++;
    return 0;
}

/* tolerant parser: a damaged track keeps the events read before the damage.
   Its ticks start at 'base'. *end is where the track ends: its end-of-track
   event, at most 8 beats after its last other event (a damaged time value must
   not give hours of silence), or without one the tick of that last event.
   seq (format 2): the end-of-track event is placed at *end, not at its tick. */
static int parse_track(mm_synth *s, const uint8_t *buf, size_t size, uint32_t base, int seq,
                       uint32_t *end) {
    size_t pos = 0;
    uint32_t tick = base, delta, len, last = base;
    uint8_t running = 0;
    int eot = 0;
    while (pos < size) {
        if (read_vlq(buf, size, &pos, &delta) < 0 || pos >= size) break;
        tick += delta;
        uint8_t st = buf[pos];
        if (st & 0x80) pos++;
        else if (running) st = running;
        else break;
        if (st == 0xff) {                                    /* meta event */
            if (pos >= size) break;
            uint8_t meta = buf[pos++];
            if (read_vlq(buf, size, &pos, &len) < 0 || len > size - pos) break;
            if (meta == 0x51 && len == 3) {
                if (add_event(s, tick, EV_TEMPO, 0, 0, 0,
                        (uint32_t)buf[pos] << 16 | buf[pos + 1] << 8 | buf[pos + 2]) < 0) return -1;
                last = tick;
            }
            if (meta == 0x2f) { eot = 1; break; }            /* end of track */
            pos += len;
            continue;
        }
        if (st == 0xf0 || st == 0xf7) {                      /* sysex */
            if (read_vlq(buf, size, &pos, &len) < 0 || len > size - pos) break;
            pos += len;
            continue;
        }
        if (st > 0xf0) {                                     /* stray system common */
            pos += st == 0xf2 ? 2 : st == 0xf1 || st == 0xf3 ? 1 : 0;
            continue;
        }
        running = st;
        int cmd = st & 0xf0, ch = st & 15, need = cmd == 0xc0 || cmd == 0xd0 ? 1 : 2;
        if (pos + need > size) break;
        int a = buf[pos] & 127, b = need == 2 ? buf[pos + 1] & 127 : 0, r = 0;
        pos += need;
        switch (cmd) {
        case 0x80: r = add_event(s, tick, EV_NOTE_OFF, ch, a, b, 0); break;
        case 0x90: r = add_event(s, tick, b ? EV_NOTE_ON : EV_NOTE_OFF, ch, a, b, 0); break;
        case 0xb0: r = add_event(s, tick, EV_CONTROL, ch, a, b, 0); break;
        case 0xc0: r = add_event(s, tick, EV_PROGRAM, ch, a, 0, 0); break;
        case 0xe0: r = add_event(s, tick, EV_PITCH, ch, a, b, 0); break;
        default: continue;                                   /* aftertouch: ignored */
        }
        if (r < 0) return -1;
        last = tick;
    }
    *end = eot && tick - last <= 8 * (uint32_t)s->tpq ? tick : eot ? last + 8 * (uint32_t)s->tpq : last;
    /* the song lasts until the end of its tracks */
    if (eot && add_event(s, seq ? *end : tick, EV_END, 0, 0, 0, 0) < 0) return -1;
    return 0;
}

/* order by tick; at equal ticks note-offs first, then file order (stable) */
static int ev_cmp(const void *x, const void *y) {
    const Ev *a = x, *b = y;
    if (a->tick != b->tick) return a->tick < b->tick ? -1 : 1;
    int oa = a->type != EV_NOTE_OFF, ob = b->type != EV_NOTE_OFF;
    if (oa != ob) return oa - ob;
    return a->seq < b->seq ? -1 : a->seq > b->seq;
}

int mm_load_mem(mm_synth *s, const void *data, size_t size) {
    const uint8_t *buf = data;
    size_t pos = 0;
    /* RIFF RMID wrapper: find the embedded MThd */
    if (size >= 20 && !memcmp(buf, "RIFF", 4) && !memcmp(buf + 8, "RMID", 4)) {
        for (pos = 12; pos + 4 <= size && memcmp(buf + pos, "MThd", 4); pos++) ;
    }
    if (size < pos + 14 || memcmp(buf + pos, "MThd", 4) || be32(buf + pos + 4) < 6) return -1;
    unsigned format = be16(buf + pos + 8), tracks = be16(buf + pos + 10), division = be16(buf + pos + 12);
    if (format > 2 || (division & 0x8000) || !division) return -1;
    s->tpq = division;
    pos += 8 + be32(buf + pos + 4);
    s->nev = 0;
    s->loaded = 0;
    free(s->evtime);
    s->evtime = NULL;
    /* format 0 and 1: all tracks play together from tick 0. Format 2: each
       track is a sequence of its own; they play one after another, each
       starting where the previous one ended, with the sounding notes
       released and the tempo back to 120 bpm in between (EV_RESET); the
       channels keep their state */
    uint32_t base = 0;
    for (unsigned t = 0; t < tracks && pos + 8 <= size; ) {
        uint32_t len = be32(buf + pos + 4), end;
        size_t avail = size - pos - 8;
        if (len > avail) len = (uint32_t)avail;              /* truncated: take what is there */
        if (!memcmp(buf + pos, "MTrk", 4)) {
            int n0 = s->nev;
            if (format == 2 && t > 0 && add_event(s, base, EV_RESET, 0, 0, 0, 0) < 0) return -1;
            if (parse_track(s, buf + pos + 8, len, base, format == 2, &end) < 0) return -1;
            if (format == 2) {                               /* an empty track adds nothing */
                int k = n0;
                while (k < s->nev && (s->ev[k].type == EV_RESET || s->ev[k].type == EV_END)) k++;
                if (k == s->nev) s->nev = n0;
                else base = end;
            }
            t++;
        }                                                    /* other chunks are skipped */
        pos += 8 + (size_t)len;
    }
    if (s->nev) qsort(s->ev, s->nev, sizeof *s->ev, ev_cmp);
    /* keep one end-of-track marker, at most 8 beats after the last other event
       (a damaged time value must not give hours of silence) */
    uint32_t last = 0, end = 0;
    int m = 0;
    for (int i = 0; i < s->nev; i++) {
        if (s->ev[i].type == EV_END) { if (s->ev[i].tick > end) end = s->ev[i].tick; }
        else { last = s->ev[i].tick; s->ev[m++] = s->ev[i]; }
    }
    s->nev = m;
    if (m && end > last) {
        if (end - last > 8 * s->tpq) end = last + 8 * s->tpq;
        add_event(s, end, EV_END, 0, 0, 0, 0);      /* fits: an event was removed */
    }
    rewind_song(s);
    s->loaded = s->nev > 0;
    return s->loaded ? 0 : -1;
}

int mm_load_file(mm_synth *s, const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    long n = -1;
    void *buf = NULL;
    if (!fseek(f, 0, SEEK_END) && (n = ftell(f)) > 0 && !fseek(f, 0, SEEK_SET) &&
        (buf = malloc(n)) && fread(buf, 1, n, f) != (size_t)n) n = -1;
    fclose(f);
    int r = n > 0 && buf ? mm_load_mem(s, buf, n) : -1;
    free(buf);
    return r;
}

/* ---------- OSS playback ---------- */
#ifndef MM_NO_OSS
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/soundcard.h>

static volatile sig_atomic_t mm_stop;
static void mm_on_signal(int sig) { (void)sig; mm_stop = 1; }

int mm_play_file(const char *path) { return mm_play_file_bank(path, NULL); }

int mm_play_file_bank(const char *path, const char *bank) {
    int fd = open("/dev/dsp", O_WRONLY);
    if (fd < 0) { perror("/dev/dsp"); return -1; }
    int frag = 0x0008000b, fmt = AFMT_S16_NE, chans = 2, rate = 44100;   /* 8 x 2 KB */
    ioctl(fd, SNDCTL_DSP_SETFRAGMENT, &frag);
    if (ioctl(fd, SNDCTL_DSP_SETFMT, &fmt) < 0 || fmt != AFMT_S16_NE ||
        ioctl(fd, SNDCTL_DSP_CHANNELS, &chans) < 0 || chans < 1 || chans > 2 ||
        ioctl(fd, SNDCTL_DSP_SPEED, &rate) < 0 || rate < 8000) {
        fprintf(stderr, "/dev/dsp: cannot set 16-bit output\n");
        close(fd);
        return -1;
    }
    mm_synth *s = mm_create(rate);
    int r = s && bank ? mm_load_bank(s, bank) : 0;
    if (r < 0 || (s && !mm_preset_count(s))) {        /* no bank to play with */
        if (r == -2) fprintf(stderr, "%s: .sfm banks are not supported in this build\n", bank);
        else if (r < 0) fprintf(stderr, "%s: cannot load bank%s\n", bank, mm_preset_count(s) ? ", using the built-in one" : "");
        else fprintf(stderr, "no bank given, and this build has no built-in bank\n");
        if (!mm_preset_count(s)) { mm_free(s); close(fd); return -1; }
    }
    if (!s || mm_load_file(s, path) < 0) {
        fprintf(stderr, "%s: cannot load MIDI file\n", path);
        mm_free(s); close(fd);
        return -1;
    }
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = mm_on_signal;          /* no SA_RESTART: write() returns EINTR */
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);

    short buf[2 * 1024];
    int n;
    while (!mm_stop && (n = mm_render(s, buf, 1024)) > 0) {
        if (chans == 1)
            for (int i = 0; i < n; i++) buf[i] = (short)((buf[2 * i] + buf[2 * i + 1]) / 2);
        const char *p = (const char *)buf;
        size_t left = (size_t)n * chans * sizeof(short);
        while (left && !mm_stop) {
            ssize_t w = write(fd, p, left);
            if (w < 0) { if (errno == EINTR) continue; mm_stop = 1; break; }
            p += w; left -= (size_t)w;
        }
    }
    ioctl(fd, mm_stop ? SNDCTL_DSP_RESET : SNDCTL_DSP_SYNC, 0);   /* drop or drain */
    close(fd);
    mm_free(s);
    return 0;
}
#endif
