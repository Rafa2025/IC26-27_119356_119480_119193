/*
 * rice.h - zigzag mapping and Rice coding of residuals.
 *
 * ZIGZAG
 * ------
 * Residuals are signed but Rice coding wants non-negative numbers. Zigzag
 * interleaves them: 0,-1,1,-2,2,-3,... -> 0,1,2,3,4,5,...
 *      e >= 0  ->  u = 2e          e < 0  ->  u = -2e - 1
 * Small |e| gives small u, which is what Rice coding likes.
 *
 * RICE CODE with parameter k
 * --------------------------
 * For a value u:   q = u >> k          (quotient)
 *                  r = u & (2^k - 1)   (remainder, k bits)
 * Written as:  q zeros, then a 1  (this is "unary"; the 1 marks the end)
 *              followed by r in exactly k bits.
 * Example, k = 2, u = 6:  q = 1, r = 2  ->  "01" + "10" = 0110  (4 bits).
 * Cost in bits: q + 1 + k. A good k keeps q around 0 to 2 for typical values.
 *
 * k is chosen per block (and per channel) by trying a few candidates around
 * log2(mean(u)) and keeping the one with the exact smallest total size.
 */
#ifndef RICE_H
#define RICE_H

#include <stddef.h>
#include <stdint.h>
#include "bitio.h"

#define RICE_MAX_K 30        /* k is stored in 5 bits; 30 is far more than ever needed */

/* signed -> unsigned. (uint32_t) casts avoid undefined behaviour on << of negatives. */
static inline uint32_t zigzag(int32_t e)
{
    return ((uint32_t)e << 1) ^ (uint32_t)(e >> 31);   /* e>>31 is 0 or all-ones */
}

/* unsigned -> signed (inverse of zigzag). */
static inline int32_t unzigzag(uint32_t u)
{
    return (int32_t)(u >> 1) ^ -(int32_t)(u & 1);
}

/* Best k in [0, RICE_MAX_K] for these n values. */
int  rice_choose_k(const uint32_t *u, size_t n);

/* Write n values with parameter k. */
void rice_encode_block(BitWriter *w, const uint32_t *u, size_t n, int k);

/* Read n values with parameter k. Check r->err afterwards. */
void rice_decode_block(BitReader *r, uint32_t *u, size_t n, int k);

#endif /* RICE_H */
