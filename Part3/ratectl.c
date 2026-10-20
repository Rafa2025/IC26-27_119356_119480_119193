/* ratectl.c - see ratectl.h. */
#include <math.h>
#include <stdio.h>
#include "quant.h"
#include "ratectl.h"

#define BISECT_ROUNDS 24
#define VERIFY_TRIES  20
#define VERIFY_SHRINK 0.995

/* SNR estimated in the coefficient domain. */
static double est_snr(const RateCtl *rc, double step)
{
    return snr_db(rc->signal, rc->weight * quant_sq_error(rc->coef, rc->ncoef, step, rc->round));
}

static double clampd(double v, double lo, double hi) { return v < lo ? lo : v > hi ? hi : v; }

/* Round to the float32 the file will hold, without going below min_step. */
static double to_float(double s, double min_step)
{
    float f = (float)s;
    if ((double)f < min_step) f = nextafterf((float)min_step, INFINITY);
    return f;
}

double ratectl_find_step(const RateCtl *rc, double *achieved_db)
{
    const double db = rc->target_db;
    double step;

    if (rc->signal <= 0 || rc->nsamples == 0) {      /* silence: SNR undefined */
        step = to_float(clampd(1.0, rc->min_step, rc->max_step), rc->min_step);
        *achieved_db = INFINITY;
        return step;
    }

    double s0 = sqrt(12.0 * rc->signal / ((double)rc->nsamples * pow(10.0, db / 10.0)));
    s0 = clampd(s0, rc->min_step, rc->max_step);

    /* Bracket: SNR(lo) >= db > SNR(hi), unless a limit is hit first. */
    double lo = clampd(s0 / 8, rc->min_step, rc->max_step);
    double hi = clampd(s0 * 8, rc->min_step, rc->max_step);
    while (est_snr(rc, lo) < db && lo > rc->min_step) {
        hi = lo;
        lo = clampd(lo / 8, rc->min_step, rc->max_step);
    }
    while (est_snr(rc, hi) >= db && hi < rc->max_step) {
        lo = hi;
        hi = clampd(hi * 8, rc->min_step, rc->max_step);
    }

    if (est_snr(rc, hi) >= db) {
        step = hi;                                    /* even the largest step is enough */
    } else if (est_snr(rc, lo) < db) {
        step = lo;                                    /* the smallest step is not enough */
    } else {
        for (int i = 0; i < BISECT_ROUNDS; i++) {
            double mid = sqrt(lo * hi);               /* bisect on log(step) */
            if (est_snr(rc, mid) >= db) lo = mid; else hi = mid;
        }
        step = lo;
    }

    /* Verify on the real output, shrink a little if needed. */
    step = to_float(step, rc->min_step);
    double real = rc->real_snr(rc->ctx, step);
    for (int i = 0; i < VERIFY_TRIES && real < db && step > rc->min_step; i++) {
        step = to_float(fmax(step * VERIFY_SHRINK, rc->min_step), rc->min_step);
        real = rc->real_snr(rc->ctx, step);
    }
    if (real < db)
        fprintf(stderr, "warning: target %.2f dB not reached, got %.2f dB with step %g\n",
                db, real, step);
    *achieved_db = real;
    return step;
}
