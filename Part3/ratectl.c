/* ratectl.c - see ratectl.h. */
#include <math.h>
#include <stdio.h>
#include "quant.h"
#include "ratectl.h"

#define BISECT_ROUNDS 24
#define VERIFY_TRIES  20
#define VERIFY_SHRINK 0.995
#define GROW_SLACK_DB 0.1      /* overshoot that triggers the final adjustment */
#define GROW_ROUNDS   5

/*
 * Mean of round(e)^2 for e ~ N(0, s2): the error left after the decoder
 * rounds its output to integers, when the samples are integers and the
 * error before rounding has variance s2. For large s2 this is s2 + 1/12
 * (rounding adds independent uniform noise); for s2 << 1 it goes to 0
 * (rounding snaps back to the original integer). The coefficient domain
 * cannot see this; without it the estimate is too optimistic above ~60 dB.
 */
static double rounded_var(double s2)
{
    if (s2 <= 0) return 0;
    if (s2 > 64) return s2 + 1.0 / 12;
    const double sd = sqrt(2.0 * s2);
    double v = 0;
    for (int k = 1; k < 64; k++) {                /* P(round(e) = +-k) */
        double p = erfc((k - 0.5) / sd) - erfc((k + 0.5) / sd);   /* both signs */
        v += (double)k * k * p;
        if (p < 1e-12) break;
    }
    return v;
}

/* SNR estimated from the coefficients (Parseval), including output rounding. */
static double est_snr(const RateCtl *rc, double step)
{
    double en = rc->weight * quant_sq_error(rc->coef, rc->ncoef, step, rc->round);
    double n = (double)rc->nsamples;
    return snr_db(rc->signal, n * rounded_var(en / n));
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
    /*
     * Near the 16-bit limit the rounding model can be pessimistic, so the
     * real SNR overshoots the target (more bits than needed). Then search
     * upwards with real decodes: SNR changes by about -20 log10 of the step
     * ratio, which gives the upper end. Normal targets land within a few
     * hundredths of a dB and never get here.
     */
    if (real >= db + GROW_SLACK_DB && step < rc->max_step) {
        double lo = step, lo_snr = real;
        double hi = fmin(step * pow(10.0, (real - db) / 20.0) * 1.02, rc->max_step);
        for (int i = 0; i < GROW_ROUNDS; i++) {
            double mid = to_float(i == 0 ? hi : sqrt(lo * hi), rc->min_step);
            double s = rc->real_snr(rc->ctx, mid);
            if (s >= db) { lo = mid; lo_snr = s; if (i == 0) break; }
            else hi = mid;
        }
        step = lo;
        real = lo_snr;
    }

    if (real < db)
        fprintf(stderr, "warning: target %.2f dB not reached, got %.2f dB with step %g\n",
                db, real, step);
    *achieved_db = real;
    return step;
}
