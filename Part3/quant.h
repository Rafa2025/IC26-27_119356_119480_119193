/*
 * quant.h - uniform scalar quantizer with one step for everything.
 *
 *   q  = sign(c) * floor(|c| / step + R)      R = 0.5: plain rounding,
 *                                             R < 0.5: small dead zone
 *   c' = q * step                             (the decoder only needs this)
 *
 * |q| is limited to QUANT_MAX so it always fits the coefficient coder.
 * quant_min_step gives the smallest step for which this limit can never be
 * reached, whatever the input; the encoder never uses a smaller step.
 * quant_max_step gives a step so large that every coefficient quantizes to
 * 0; a larger step can never be useful.
 *
 * The default R = 0.4 was chosen by Experiment 6: rounding values just
 * above a boundary towards zero gives more zeros, which the entropy coder
 * codes very cheaply, and at equal SNR this saves 2-6% at 20-30 dB.
 */
#ifndef QUANT_H
#define QUANT_H

#include <math.h>
#include <stddef.h>
#include <stdint.h>

#define QUANT_MAX (1 << 30)
#define QUANT_ROUND_DEFAULT 0.4

static inline int32_t quant_one(double c, double inv_step, double r)
{
    double a = floor(fabs(c) * inv_step + r);
    if (a > QUANT_MAX) a = QUANT_MAX;
    return c < 0 ? -(int32_t)a : (int32_t)a;
}

void quant_block(const double *c, int32_t *q, int n, double step, double r);
void dequant_block(const int32_t *q, double *c, int n, double step);

/* Sum of (c - q*step)^2 over n coefficients stored as float. */
double quant_sq_error(const float *c, size_t n, double step, double r);

/* Smallest safe step for `bits`-bit samples and N-point blocks. */
double quant_min_step(int bits, int n);

/* Step above which every coefficient quantizes to 0. */
double quant_max_step(int bits, int n);

/* 10 log10(signal / noise); +inf for zero noise. */
double snr_db(double signal, double noise);

#endif /* QUANT_H */
