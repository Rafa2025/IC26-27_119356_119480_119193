/*
 * predictor.h - fixed polynomial predictors (orders 0 to 4).
 *
 * A predictor guesses each sample from the previous ones; we then code only
 * the error ("residual") e[n] = x[n] - pred[n], which is much smaller than
 * x[n] for smooth audio. The decoder makes the same guess and adds e[n] back,
 * so nothing is lost.
 *
 *   order  pred[n]
 *     0    0
 *     1    x[n-1]
 *     2    2x[n-1] - x[n-2]
 *     3    3x[n-1] - 3x[n-2] + x[n-3]
 *     4    4x[n-1] - 6x[n-2] + 4x[n-3] - x[n-4]
 *
 * (Order k is the same as taking the k-th difference of the signal.)
 *
 * HISTORY ACROSS BLOCKS
 * ---------------------
 * The functions below take a pointer x to the FIRST sample of the block but
 * read up to PRED_MAX_ORDER samples BEFORE it. The caller keeps each channel
 * in one long array that starts with PRED_MAX_ORDER zeros, so:
 *   - inside the file, the predictor continues smoothly over block borders
 *     (no warm-up samples need to be stored);
 *   - at the very start, the "previous samples" are simply zeros.
 *
 * Overflow: for 25-bit values (24-bit SIDE channel) the largest possible
 * |residual| is 16 * 2^24 = 2^28, which fits in int32 with room to spare.
 */
#ifndef PREDICTOR_H
#define PREDICTOR_H

#include <stddef.h>
#include <stdint.h>

#define PRED_MAX_ORDER 4

/* Try every order on this block, return the one with the smallest sum of |residual|. */
int  predictor_choose_order(const int32_t *x, size_t n);

/* res[i] = x[i] - prediction, for i in [0, n). */
void predictor_residuals(const int32_t *x, size_t n, int order, int32_t *res);

/* Inverse: rebuild x[i] = res[i] + prediction, for i in [0, n), in place.
   x[-1..-4] must already hold the previous (already decoded) samples. */
void predictor_restore(int32_t *x, size_t n, int order, const int32_t *res);

#endif /* PREDICTOR_H */
