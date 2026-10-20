/*
 * ratectl.h - find the largest quantization step that reaches a target SNR.
 *
 *  1. first guess from the uniform-noise model (noise power = step^2 / 12):
 *         s0 = sqrt(12 * Es / (samples * 10^(DB/10)))
 *  2. bracket [lo, hi] around the answer, starting at s0/8 .. s0*8
 *  3. bisect on log(step), 24 rounds; the SNR of each candidate is
 *     estimated from the stored coefficients (Parseval), no inverse transform:
 *         En = weight * sum (c - q*step)^2
 *  4. round the step to float32 (what the file stores) and verify it with a
 *     real decode supplied by the caller; while the target is missed, reduce
 *     the step by 0.5%, at most 20 times.
 *
 * The real decode is a callback so that it runs exactly the decoder's code
 * (inverse transform, overlap-add, stereo, rounding, clipping), which lives
 * in codec_lossy.c.
 */
#ifndef RATECTL_H
#define RATECTL_H

#include <stddef.h>
#include <stdint.h>

typedef double (*RealSnrFn)(void *ctx, double step);

typedef struct {
    const float *coef;       /* every coefficient of the file */
    size_t       ncoef;
    double       weight;     /* error weight: 2 for mid/side, 1 otherwise */
    double       signal;     /* Es: sum of squared original samples */
    uint64_t     nsamples;   /* frames * channels */
    double       min_step, max_step;
    double       round;      /* quantizer rounding offset R */
    double       target_db;
    RealSnrFn    real_snr;
    void        *ctx;
} RateCtl;

/* Returns the step (already a float32 value) and the measured SNR. */
double ratectl_find_step(const RateCtl *rc, double *achieved_db);

#endif /* RATECTL_H */
