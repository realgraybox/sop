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
 * mmmodel.c - render the sample models of an .sfm bank (see mmmodel.h).
 *
 * This mirrors render_model() in sf2synth.py step by step, so a bank sounds
 * the same here as in the .sf2 that sf2synth.py renders for listening.
 * Record layout: see the "model file" section of sf2synth.py.
 *
 * Its own C99 maths functions (mm_cos, mm_exp10 ... below, also used by
 * mmidi.c), so the output does not depend on the C library (uClibc).
 * With -DMM_NO_MODELS only these maths functions are built.
 */
#include "mmmodel.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdint.h>
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define NBP      9                 /* envelope points */
#define NB       25                /* noise bands: 0-80 Hz, then 1/3 octaves from 80 Hz */
#define FLOOR_DB (-90.0)
#define NOISE_N  32768             /* shared band noise length */
#define NOISE_SEED 11
#define LOOP_SEED  7

/* ---------- maths (see mmmodel.h) ----------
   The C library's cos/sin/pow differ between libraries in speed (uClibc's are
   much slower than glibc's) and in their last bits, so the renderer and the
   synth use these instead. Only sqrt, floor, ceil and fmod still come from
   libm: their results are exact in every library. */

/* x = n * pi/2 + r with |r| <= about pi/4: n * P1 and n * P2 are exact for
   |n| < 2^23 (the pieces have 30 bits), so r keeps its full precision */
#define PIO2_1 0x1.921fb54p+0
#define PIO2_2 0x1.10b46118p-30
#define PIO2_3 0x1.313198ap-61
#define PIO2_4 0x1.701b839a25205p-92
#define TWO_OVER_PI 0.6366197723675814

/* Taylor series on |r| <= pi/4: cos to r^18, sin to r^17 (truncation below
   1e-18, so the error is that of rounding, about 1 ulp) */
static double k_cos(double r) {
    double z = r * r;
    return 1.0 + z * (-0.5 + z * (0.041666666666666664 + z * (-0.001388888888888889 +
           z * (2.48015873015873e-05 + z * (-2.755731922398589e-07 + z * (2.08767569878681e-09 +
           z * (-1.1470745597729725e-11 + z * (4.779477332387385e-14 + z * -1.5619206968586225e-16))))))));
}
static double k_sin(double r) {
    double z = r * r;
    return r + r * z * (-0.16666666666666666 + z * (0.008333333333333333 + z * (-0.0001984126984126984 +
           z * (2.7557319223985893e-06 + z * (-2.505210838544172e-08 + z * (1.6059043836821613e-10 +
           z * (-7.647163731819816e-13 + z * 2.8114572543455206e-15)))))));
}

/* mm_cos(x + q * pi/2) */
static double trig(double x, unsigned q) {
    if (!(x > -1e15 && x < 1e15)) return x - x;      /* NaN for inf/NaN, 0 beyond range */
    double t = x * TWO_OVER_PI;
    long long n = (long long)(t < 0 ? t - 0.5 : t + 0.5);
    double dn = (double)n, r = ((x - dn * PIO2_1) - dn * PIO2_2) - dn * PIO2_3;
    r -= dn * PIO2_4;
    switch (((unsigned)n + q) & 3) {
    case 0:  return k_cos(r);
    case 1:  return -k_sin(r);
    case 2:  return -k_cos(r);
    default: return k_sin(r);
    }
}
double mm_cos(double x) { return trig(x, 0); }
double mm_sin(double x) { return trig(x, 3); }       /* sin x = mm_cos(x - pi/2) */

/* 2^k for an integer k, built from its bits */
static double pow2i(int k) {
    union { double d; uint64_t u; } v;
    if (k > 1023) return HUGE_VAL;
    if (k < -1074) return 0;
    if (k < -1022) return pow2i(k + 64) * 0x1p-64;   /* subnormal */
    v.u = (uint64_t)(k + 1023) << 52;
    return v.d;
}

/* e^r - 1 for |r| <= ln2 / 2: Taylor to r^13 (truncation below 1e-17) */
static double k_em1(double r) {
    return r * (1.0 + r * (0.5 + r * (0.16666666666666666 + r * (0.041666666666666664 +
           r * (0.008333333333333333 + r * (0.001388888888888889 + r * (0.0001984126984126984 +
           r * (2.48015873015873e-05 + r * (2.7557319223985893e-06 + r * (2.755731922398589e-07 +
           r * (2.505210838544172e-08 + r * (2.08767569878681e-09 + r * 1.6059043836821613e-10))))))))))));
}
static double k_exp(double r) { return 1.0 + k_em1(r); }

/* x with the low 27 bits of its mantissa cleared: hi * (a 26-bit constant) is
   exact. Done on the bits, so it holds however the FPU rounds. */
static double hi26(double x) {
    union { double d; uint64_t u; } v = { x };
    v.u &= ~(uint64_t)0x7ffffff;
    return v.d;
}

#define LN2   0.6931471805599453
#define LN10  2.302585092994046
#define LOG2_10 3.321928094887362
#define LN10_LO 8.723385281292425e-08      /* ln10 - 0x1.26bb1bp+1 */

double mm_exp2(double x) {                           /* 2^x = 2^k * e^(f ln2), |f| <= 1/2 */
    if (!(x < 1025)) return x > 0 ? x * 2 : x + 1;   /* inf, or NaN stays NaN */
    if (!(x > -1100)) return 0;
    double k = (double)(long)(x < 0 ? x - 0.5 : x + 0.5);
    return pow2i((int)k) * k_exp((x - k) * LN2);
}

double mm_exp10(double x) {                          /* 10^x = 2^k * e^r, r = x ln10 - k ln2 */
    if (!(x < 309)) return x > 0 ? x * 2 : x + 1;
    if (!(x > -330)) return 0;
    double t = x * LOG2_10, k = (double)(long)(t < 0 ? t - 0.5 : t + 0.5);
    /* x ln10 with x and ln10 split in halves (xh * LN10_HI exact), and k ln2
       in two parts (k * LN2_HI exact), so r keeps its full precision */
    double xh = hi26(x), xl = x - xh;
    double r = (xh * 0x1.26bb1bp+1 - k * 0x1.62e42feep-1) +
               ((xh * LN10_LO + xl * LN10) - k * 1.9082149292705877e-10);
    return pow2i((int)k) * k_exp(r);
}

static double m_exp(double x) {                      /* e^x = 2^k * e^r, |r| <= ln2 / 2 */
    if (!(x < 710)) return x > 0 ? x * 2 : x + 1;
    if (!(x > -746)) return 0;
    double t = x * 1.4426950408889634, k = (double)(long)(t < 0 ? t - 0.5 : t + 0.5);
    return pow2i((int)k) * k_exp((x - k * 0x1.62e42feep-1) - k * 1.9082149292705877e-10);
}

double mm_tanh(double x) {                           /* by e^-2|x| - 1 = u: -u / (2 + u) */
    double a = x < 0 ? -x : x, u;
    if (a < 1e-9) return x;
    if (a < 0.17) u = k_em1(-2 * a);                 /* small: the series keeps precision */
    else { u = m_exp(-2 * a) - 1; }
    double y = -u / (2 + u);
    return x < 0 ? -y : y;
}

double mm_log10(double x) {                          /* x = m * 2^e, log m by its atanh series */
    if (!(x > 0)) return x == 0 ? -HUGE_VAL : (x - x) / 0.0;
    if (x > 1.7976931348623157e308) return x;
    union { double d; uint64_t u; } v = { x };
    int e = 0;
    if (!(v.u >> 52)) { v.d *= 0x1p64; e = -64; }   /* subnormal */
    e += (int)(v.u >> 52) - 1023;
    v.u = (v.u & 0x000fffffffffffffULL) | 0x3ff0000000000000ULL;   /* m in [1, 2) */
    double m = v.d;
    if (m > 1.4142135623730951) { m *= 0.5; e++; }  /* m in [0.707, 1.414] */
    double s = (m - 1) / (m + 1), z = s * s;        /* |s| <= 0.172: series to s^23 */
    double lm = 2 * s * (1 + z * (0.3333333333333333 + z * (0.2 + z * (0.14285714285714285 +
                z * (0.1111111111111111 + z * (0.09090909090909091 + z * (0.07692307692307693 +
                z * (0.06666666666666667 + z * (0.058823529411764705 + z * (0.05263157894736842 +
                z * (0.047619047619047616 + z * 0.043478260869565216)))))))))));
    return (e * 0x1.62e42feep-1 + (lm + e * 1.9082149292705877e-10)) * 0.4342944819032518;
}

#ifndef MM_NO_MODELS
static double band_edge(int i) { return i == 0 ? 0.0 : 80 * mm_exp2((i - 1) / 3.0); }

/* ---------- uniform numbers: 32-bit LCG, as rng() in sf2synth.py ---------- */
static void rng(double *out, unsigned n, unsigned seed) {
    uint32_t x = (uint32_t)(seed * 2654435761u + 1u);
    for (unsigned i = 0; i < n; i++) {
        x = x * 1664525u + 1013904223u;
        out[i] = (x >> 8) / 16777216.0;
    }
}

/* ---------- FFT: radix 2, and Bluestein for other lengths ---------- */
static void fft2(double *re, double *im, unsigned n, int inv) {
    for (unsigned i = 1, j = 0; i < n; i++) {             /* bit reversal */
        unsigned b = n >> 1;
        for (; j & b; b >>= 1) j ^= b;
        j |= b;
        if (i < j) {
            double t = re[i]; re[i] = re[j]; re[j] = t;
            t = im[i]; im[i] = im[j]; im[j] = t;
        }
    }
    /* twiddles exp(-+2 pi i k / n), computed once per call */
    double *wr = malloc((n / 2 + 1) * 2 * sizeof *wr), *wi = wr ? wr + n / 2 + 1 : NULL;
    for (unsigned k = 0; wr && k < n / 2; k++) {
        double a = (inv ? 2 : -2) * M_PI * k / n;
        wr[k] = mm_cos(a); wi[k] = mm_sin(a);
    }
    for (unsigned len = 2; len <= n; len <<= 1) {
        double a = (inv ? 2 : -2) * M_PI / len;
        for (unsigned k = 0; k < len / 2; k++) {
            unsigned q = k * (n / len);
            double cr = wr ? wr[q] : mm_cos(a * k), ci = wr ? wi[q] : mm_sin(a * k);
            for (unsigned s = k; s < n; s += len) {
                unsigned t = s + len / 2;
                double xr = re[t] * cr - im[t] * ci, xi = re[t] * ci + im[t] * cr;
                re[t] = re[s] - xr; im[t] = im[s] - xi;
                re[s] += xr; im[s] += xi;
            }
        }
    }
    free(wr);
}

/* DFT of any length (no 1/n on the inverse); 0 = ok */
static int dft(double *re, double *im, unsigned n, int inv) {
    if (!(n & (n - 1))) { fft2(re, im, n, inv); return 0; }
    unsigned m = 1;
    while (m < 2 * n - 1) m <<= 1;
    double *ar = calloc(4 * (size_t)m + 2 * n, sizeof *ar);
    if (!ar) return -1;
    double *ai = ar + m, *br = ai + m, *bi = br + m, *cr = bi + m, *ci = cr + n;
    for (unsigned k = 0; k < n; k++) {                   /* chirp exp(-+i pi k^2 / n) */
        double ph = M_PI * (double)(((uint64_t)k * k) % (2 * (uint64_t)n)) / n;
        cr[k] = mm_cos(ph); ci[k] = inv ? mm_sin(ph) : -mm_sin(ph);
        ar[k] = re[k] * cr[k] - im[k] * ci[k];
        ai[k] = re[k] * ci[k] + im[k] * cr[k];
        br[k] = cr[k]; bi[k] = -ci[k];
        if (k) { br[m - k] = cr[k]; bi[m - k] = -ci[k]; }
    }
    fft2(ar, ai, m, 0);
    fft2(br, bi, m, 0);
    for (unsigned k = 0; k < m; k++) {
        double r = ar[k] * br[k] - ai[k] * bi[k];
        ai[k] = ar[k] * bi[k] + ai[k] * br[k];
        ar[k] = r;
    }
    fft2(ar, ai, m, 1);
    for (unsigned k = 0; k < n; k++) {
        double r = ar[k] / m, i = ai[k] / m;
        re[k] = r * cr[k] - i * ci[k];
        im[k] = r * ci[k] + i * cr[k];
    }
    free(ar);
    return 0;
}

/* real signal from its n/2+1 spectrum bins (like numpy irfft); 0 = ok */
static int irfft(const double *xr, const double *xi, unsigned n, double *out) {
    double *re = calloc(2 * (size_t)n, sizeof *re);
    if (!re) return -1;
    double *im = re + n;
    re[0] = xr[0];
    for (unsigned k = 1; 2 * k < n; k++) {
        re[k] = xr[k]; im[k] = xi[k];
        re[n - k] = xr[k]; im[n - k] = -xi[k];
    }
    if (!(n & 1)) re[n / 2] = xr[n / 2];
    int r = dft(re, im, n, 1);
    for (unsigned k = 0; k < n && !r; k++) out[k] = re[k] / n;
    free(re);
    return r;
}

/* ---------- shared band noises, one set per sample rate ---------- */
/* The white noise spectrum is made once per rate; each band is split off
   only when a model first needs it (most banks use few of them). */
typedef struct { unsigned rate; double *re, *im; float *b[NB]; } Bands;
static Bands bands[4];

static void free_bands(Bands *s) {
    free(s->re);
    for (int i = 0; i < NB; i++) free(s->b[i]);
    memset(s, 0, sizeof *s);
}

/* band i of the unit-level noise for this rate (NOISE_N points), NULL = no memory */
static const float *band_noise(unsigned rate, int band) {
    Bands *s = NULL;
    for (int i = 0; i < 4 && !s; i++) if (bands[i].re && bands[i].rate == rate) s = &bands[i];
    for (int i = 0; i < 4 && !s; i++) if (!bands[i].re) s = &bands[i];
    if (!s) { free_bands(&bands[3]); s = &bands[3]; }
    unsigned n = NOISE_N, nf = n / 2 + 1;
    if (!s->re) {
        if (!(s->re = malloc(2 * (size_t)n * sizeof *s->re))) return NULL;
        s->im = s->re + n;
        s->rate = rate;
        rng(s->re, n, NOISE_SEED);
        for (unsigned i = 0; i < n; i++) { s->re[i] = 2 * s->re[i] - 1; s->im[i] = 0; }
        fft2(s->re, s->im, n, 0);
    }
    if (s->b[band]) return s->b[band];
    float *d = malloc(n * sizeof *d);
    double *mr = malloc((2 * (size_t)nf + n) * sizeof *mr);
    if (!d || !mr) { free(d); free(mr); return NULL; }
    double *mi = mr + nf, *o = mi + nf, lo = band_edge(band), hi = band_edge(band + 1);
    int any = 0;
    for (unsigned k = 0; k < nf; k++) {
        double f = k * (double)rate / n;
        int in = f >= lo && f < hi;
        mr[k] = in ? s->re[k] : 0; mi[k] = in ? s->im[k] : 0;
        any |= in;
    }
    if (!any) memset(d, 0, n * sizeof *d);
    else {
        irfft(mr, mi, n, o);                          /* power of 2: cannot fail */
        double e = 0;
        for (unsigned k = 0; k < n; k++) e += o[k] * o[k];
        e = sqrt(e / n) + 1e-12;
        for (unsigned k = 0; k < n; k++) d[k] = (float)(o[k] / e);
    }
    free(mr);
    return s->b[band] = d;
}

void mm_model_done(void) {
    for (int i = 0; i < 4; i++) free_bands(&bands[i]);
}

/* ---------- record reading ---------- */
typedef struct { const unsigned char *p, *end; int bad; } Rd;
static unsigned rd8(Rd *r) { if (r->p + 1 > r->end) { r->bad = 1; return 0; } return *r->p++; }
static unsigned rd16(Rd *r) { unsigned a = rd8(r); return a | rd8(r) << 8; }
static uint32_t rd32(Rd *r) { uint32_t a = rd16(r); return a | (uint32_t)rd16(r) << 16; }
static double rdf(Rd *r) { uint32_t u = rd32(r); float f; memcpy(&f, &u, 4); return f; }
static double db8(unsigned v) { return v >= 180 ? FLOOR_DB : -(double)v / 2; }
static double ph16(unsigned v) { return (v >= 32768 ? (double)v - 65536 : (double)v) * M_PI / 32768; }

/* ---------- envelopes ---------- */
/* amplitude at t = 0..n-1 from dB at the points (like numpy interp) */
static void env_curve(const unsigned *bp, const double *db, unsigned n, double *a) {
    for (unsigned t = 0; t < n; t++) {
        double d;
        if (t >= bp[NBP - 1]) d = db[NBP - 1];
        else if (t <= bp[0] && t < bp[1]) d = db[0];
        else {
            int j = NBP - 2;
            while (j > 0 && bp[j] > t) j--;
            if (bp[j] == t) d = db[j];
            else d = (db[j + 1] - db[j]) / ((double)bp[j + 1] - bp[j]) * ((double)t - bp[j]) + db[j];
        }
        a[t] = d <= FLOOR_DB + 0.5 ? 0.0 : mm_exp10(d / 20.0);
    }
}

static double ramp(const unsigned *bp, unsigned ls, unsigned t) {
    double b0 = bp[NBP - 2], den = (double)ls - b0;
    double r = ((double)t - b0) / (den > 1 ? den : 1);
    return r < 0 ? 0 : r > 1 ? 1 : r;
}

/* ---------- the model ---------- */
typedef struct { unsigned L, rate, ls, le, loop, onset, bp[NBP]; double peak; } Hdr;

unsigned mm_model_length(const unsigned char *rec, size_t size) {
    Rd r = {rec, rec + size, 0};
    unsigned L = rd32(&r);
    return r.bad ? 0 : L;
}

int mm_model_render(const unsigned char *rec, size_t size, float *out, unsigned len) {
    Rd r = {rec, rec + size, 0};
    Hdr h;
    h.L = rd32(&r); h.rate = rd32(&r); h.ls = rd32(&r); h.le = rd32(&r);
    h.loop = rd8(&r); h.peak = rdf(&r); h.onset = rd16(&r);
    for (int i = 0; i < NBP; i++) h.bp[i] = rd32(&r);
    if (r.bad || !h.rate) return -1;
    unsigned L = h.L, ls = h.ls, Ll = h.le - h.ls;
    if (h.loop && (h.le <= h.ls || h.le > L)) return -1;
    double *y = calloc(3 * (size_t)L + 1, sizeof *y), *amp = y + L, *tmp = amp + L;
    if (!y) return -1;
    int err = 0;
    unsigned np = rd16(&r);
    double env[NBP];

    for (unsigned q = 0; q < np && !r.bad && !err; q++) {
        unsigned fl = rd8(&r), pv = rd16(&r), ph = rd16(&r);
        for (int i = 0; i < NBP; i++) env[i] = db8(rd8(&r));
        unsigned ld = rd8(&r);
        double f = fl & 8 ? pv * (double)h.rate / Ll : 10.0 * mm_exp2(((double)pv - 16) / 4096.0);
        double phase = ph16(ph), loop_db = fl & 16 ? db8(ld) : FLOOR_DB;
        int xf = fl & 3;
        if (fl & 4) {                                 /* oscillator: harmonic series in the loop */
            unsigned n = rd16(&r), fLl = rd32(&r), H = rd16(&r);
            if (r.bad || !fLl || fLl != Ll) { err = 1; break; }
            unsigned nf = fLl / 2 + 1;
            double *xr = calloc(2 * (size_t)nf + fLl, sizeof *xr), *xi = xr + nf, *w = xi + nf;
            if (!xr) { err = 1; break; }
            for (unsigned k = 0; k < H; k++) {
                unsigned hh = rd16(&r);
                double a = mm_exp10(db8(rd8(&r)) / 20.0) * h.peak * fLl / 2.0, p = ph16(rd16(&r));
                if ((size_t)hh * n < nf) { xr[hh * n] = a * mm_cos(p); xi[hh * n] = a * mm_sin(p); }
            }
            if (r.bad || irfft(xr, xi, fLl, w)) { free(xr); err = 1; break; }
            if (xf == 1) for (unsigned t = 0; t < L; t++) amp[t] = mm_sin(0.5 * M_PI * ramp(h.bp, ls, t));
            else {
                env_curve(h.bp, env, ls + 1, amp);
                for (unsigned t = ls + 1; t < L; t++) amp[t] = 1.0;
            }
            for (unsigned t = 0; t < L; t++) {
                long k = ((long)t - (long)ls) % (long)fLl;
                y[t] += amp[t] * w[k < 0 ? k + fLl : k];
            }
            free(xr);
            continue;
        }
        if (xf == 1) {
            double a = mm_exp10(loop_db / 20.0);
            for (unsigned t = 0; t < L; t++) amp[t] = a * mm_sin(0.5 * M_PI * ramp(h.bp, ls, t));
        } else if (xf == 2) {
            env_curve(h.bp, env, ls + 1, amp);
            for (unsigned t = ls + 1; t < L; t++) amp[t] = 0;
            for (unsigned t = 0; t < L; t++) amp[t] *= mm_cos(0.5 * M_PI * ramp(h.bp, ls, t));
        } else if (h.loop) {
            env[NBP - 1] = loop_db;
            env_curve(h.bp, env, ls + 1, amp);
            for (unsigned t = ls + 1; t < L; t++) amp[t] = amp[ls];
        } else env_curve(h.bp, env, L, amp);
        for (unsigned t = 0; t < L; t++)
            if (amp[t] != 0) y[t] += h.peak * amp[t] * mm_cos(2 * M_PI * f * t / h.rate + phase);
    }

    /* noise in bands, then (looped) the loop's own noise */
    unsigned kind = rd8(&r);
    if (!err && !r.bad && kind) {
        unsigned end = h.loop ? ls : L;
        for (int i = 0; i < NB && !err; i++) {
            int silent = 1;
            for (int k = 0; k < NBP; k++) { env[k] = db8(rd8(&r)); silent &= env[k] <= FLOOR_DB + 0.5; }
            if (silent || !end) continue;                 /* adds nothing */
            const float *b = band_noise(h.rate, i);
            if (!b) { err = 1; break; }
            env_curve(h.bp, env, L, amp);
            for (unsigned t = 0; t < end; t++)
                if (amp[t] != 0) tmp[t] += h.peak * amp[t] * b[t % NOISE_N];
        }
        if (kind == 2 && !err) {
            double floor_[NB];
            for (int i = 0; i < NB; i++) floor_[i] = rdf(&r);
            unsigned nf = Ll / 2 + 1;
            double *xr = calloc(2 * (size_t)nf + Ll + nf, sizeof *xr), *xi = xr + nf, *lp = xi + nf, *u = lp + Ll;
            if (!xr) err = 1;
            else {
                rng(u, nf, LOOP_SEED);
                for (unsigned k = 1; k < nf; k++) {
                    double fk = k * (double)h.rate / Ll, mag = 0;
                    for (int i = 0; i < NB; i++)
                        if (fk >= band_edge(i) && fk < band_edge(i + 1)) mag = sqrt(floor_[i]);
                    xr[k] = mag * mm_cos(2 * M_PI * u[k]);
                    xi[k] = mag * mm_sin(2 * M_PI * u[k]);
                }
                if (irfft(xr, xi, Ll, lp)) err = 1;
                else {
                    unsigned fade = ls < 64 ? ls : 64;
                    for (unsigned j = 0; j < fade; j++) {
                        double k = fade > 1 ? (double)j / (fade - 1) : 0.0;
                        unsigned t = ls - fade + j;
                        tmp[t] = tmp[t] * (1 - k) + k * lp[(Ll - fade % Ll + j) % Ll];
                    }
                    for (unsigned t = ls; t < L; t++) tmp[t] += lp[(t - ls) % Ll];
                }
                free(xr);
            }
        }
    }
    if (r.bad) err = 1;
    if (!err) {
        unsigned n = h.onset < L ? h.onset : L;
        for (unsigned t = 0; t < L; t++) {
            double v = h.peak > 0 ? y[t] + tmp[t] : 0;
            if (t < n) v *= 0.5 - 0.5 * mm_cos(M_PI * t / n);
            if (t < len) out[t] = (float)v;
        }
        for (unsigned t = L; t < len; t++) out[t] = 0;
    }
    free(y);
    return err ? -1 : 0;
}
#endif /* MM_NO_MODELS */
