/* predictor.c - see predictor.h for the overview. */
#include <stdlib.h>
#include "predictor.h"

/* ------------------------------------------------------------------ */
/* Choosing the order                                                  */
/* ------------------------------------------------------------------ */
/*
 * For each sample we compute the residual of all 5 orders at once and add
 * up |residual| per order. The order with the smallest sum is (almost
 * always) the one that will need the fewest bits after Rice coding, and the
 * sum is much cheaper to compute than the real coded size.
 */
int predictor_choose_order(const int32_t *x, size_t n)
{
    int64_t sum[PRED_MAX_ORDER + 1] = {0, 0, 0, 0, 0};

    for (size_t i = 0; i < n; i++) {
        const int32_t *p = x + i;                 /* p[0] = x[i], p[-1] = x[i-1], ... */
        int32_t a = p[0], b = p[-1], c = p[-2], d = p[-3], e = p[-4];

        sum[0] += labs((long)a);
        sum[1] += labs((long)(a - b));
        sum[2] += labs((long)(a - 2 * b + c));
        sum[3] += labs((long)(a - 3 * b + 3 * c - d));
        sum[4] += labs((long)(a - 4 * b + 6 * c - 4 * d + e));
    }

    int best = 0;
    for (int k = 1; k <= PRED_MAX_ORDER; k++)
        if (sum[k] < sum[best]) best = k;         /* ties keep the lower (cheaper) order */
    return best;
}

/* ------------------------------------------------------------------ */
/* Encoder side: x -> residual                                         */
/* ------------------------------------------------------------------ */
void predictor_residuals(const int32_t *x, size_t n, int order, int32_t *res)
{
    /* The switch is outside the loops so each loop is tight and simple. */
    switch (order) {
    case 0:
        for (size_t i = 0; i < n; i++) res[i] = x[i];
        break;
    case 1:
        for (size_t i = 0; i < n; i++) {
            const int32_t *p = x + i;
            res[i] = p[0] - p[-1];
        }
        break;
    case 2:
        for (size_t i = 0; i < n; i++) {
            const int32_t *p = x + i;
            res[i] = p[0] - 2 * p[-1] + p[-2];
        }
        break;
    case 3:
        for (size_t i = 0; i < n; i++) {
            const int32_t *p = x + i;
            res[i] = p[0] - 3 * p[-1] + 3 * p[-2] - p[-3];
        }
        break;
    default: /* 4 */
        for (size_t i = 0; i < n; i++) {
            const int32_t *p = x + i;
            res[i] = p[0] - 4 * p[-1] + 6 * p[-2] - 4 * p[-3] + p[-4];
        }
        break;
    }
}

/* ------------------------------------------------------------------ */
/* Decoder side: residual -> x                                         */
/* ------------------------------------------------------------------ */
/*
 * Each x[i] needs the already rebuilt x[i-1], x[i-2], ..., so this loop
 * is inherently sequential and works left to right.
 *
 * The arithmetic is done in unsigned 32-bit on purpose: for valid streams
 * the result is identical, and for a corrupted stream it wraps around
 * instead of triggering signed-overflow undefined behaviour.
 */
void predictor_restore(int32_t *x, size_t n, int order, const int32_t *res)
{
    switch (order) {
    case 0:
        for (size_t i = 0; i < n; i++) x[i] = res[i];
        break;
    case 1:
        for (size_t i = 0; i < n; i++) {
            int32_t *p = x + i;
            p[0] = (int32_t)((uint32_t)res[i] + (uint32_t)p[-1]);
        }
        break;
    case 2:
        for (size_t i = 0; i < n; i++) {
            int32_t *p = x + i;
            p[0] = (int32_t)((uint32_t)res[i] + 2u * (uint32_t)p[-1] - (uint32_t)p[-2]);
        }
        break;
    case 3:
        for (size_t i = 0; i < n; i++) {
            int32_t *p = x + i;
            p[0] = (int32_t)((uint32_t)res[i] + 3u * (uint32_t)p[-1]
                             - 3u * (uint32_t)p[-2] + (uint32_t)p[-3]);
        }
        break;
    default: /* 4 */
        for (size_t i = 0; i < n; i++) {
            int32_t *p = x + i;
            p[0] = (int32_t)((uint32_t)res[i] + 4u * (uint32_t)p[-1]
                             - 6u * (uint32_t)p[-2] + 4u * (uint32_t)p[-3] - (uint32_t)p[-4]);
        }
        break;
    }
}
