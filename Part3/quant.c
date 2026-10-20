/* quant.c - see quant.h. */
#include "quant.h"

void quant_block(const double *c, int32_t *q, int n, double step, double r)
{
    const double inv = 1.0 / step;
    for (int i = 0; i < n; i++) q[i] = quant_one(c[i], inv, r);
}

void dequant_block(const int32_t *q, double *c, int n, double step)
{
    for (int i = 0; i < n; i++) c[i] = q[i] * step;
}

double quant_sq_error(const float *c, size_t n, double step, double r)
{
    const double inv = 1.0 / step;
    double sum = 0;
    for (size_t i = 0; i < n; i++) {
        double e = c[i] - quant_one(c[i], inv, r) * step;
        sum += e * e;
    }
    return sum;
}

/*
 * Samples (and M, S) are at most A = 2^(bits-1) in magnitude. One orthonormal
 * coefficient is a sum of at most 2N samples times sqrt(2/N) times a weight
 * <= 1, so |c| <= 2 sqrt(2N) A. Dividing by QUANT_MAX gives a step for which
 * |q| <= QUANT_MAX always holds.
 */
double quant_min_step(int bits, int n)
{
    double a = ldexp(1.0, bits - 1);
    return 2.0 * sqrt(2.0 * n) * a / QUANT_MAX;
}

double snr_db(double signal, double noise)
{
    if (noise <= 0) return INFINITY;
    return 10.0 * log10(signal / noise);
}
