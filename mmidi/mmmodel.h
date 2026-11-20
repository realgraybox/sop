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
 * mmmodel.h - render the sample models of an .sfm bank (made by sf2synth.py).
 *
 * A model describes one sample as partials (sines with 9-point envelopes),
 * oscillators (whole harmonic series in the loop) and noise in 25 bands.
 * Rendering is deterministic and matches sf2synth.py's renderer.
 */
#ifndef MMMODEL_H
#define MMMODEL_H
#include <stddef.h>

/* sample length (points) stored in a model record, 0 if the record is bad */
unsigned mm_model_length(const unsigned char *rec, size_t size);

/* render a model record into out[0 .. len-1] (16-bit scale); 0 = ok */
int mm_model_render(const unsigned char *rec, size_t size, float *out, unsigned len);

/* free the shared noise tables (call once all models are rendered) */
void mm_model_done(void);

/* The maths functions mmmodel.c and mmidi.c use instead of the C library's
   (plain C99, double precision, within about 1 ulp of the exact result), so
   the output is the same whichever C library the player is built against.
   mm_cos/mm_sin are accurate for |x| < 1e7 (the arguments used stay far
   below); mm_exp2(x) = 2^x, mm_exp10(x) = 10^x. */
double mm_cos(double x);
double mm_sin(double x);
double mm_exp2(double x);
double mm_exp10(double x);
double mm_log10(double x);
double mm_tanh(double x);

#endif
