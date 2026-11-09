/* rice.c - see rice.h for the coding scheme. */
#include "rice.h"

/* ------------------------------------------------------------------ */
/* Choosing k                                                          */
/* ------------------------------------------------------------------ */
/*
 * Total size of a block coded with parameter k is exactly
 *        n * (k + 1)  +  sum over all values of (u >> k)
 *        ^^^^^^^^^^^     ^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^
 *        1 end-bit + k remainder bits per value;  the unary zeros
 * so we can evaluate any k exactly in one pass, no trial encoding needed.
 *
 * The best k is close to log2(mean(u)), so we only test that value and its
 * two neighbours instead of all 31 possibilities.
 */
int rice_choose_k(const uint32_t *u, size_t n)
{
    if (n == 0) return 0;

    uint64_t total = 0;
    for (size_t i = 0; i < n; i++) total += u[i];
    uint64_t mean = total / n;

    int k0 = 0;                                   /* k0 = floor(log2(mean)), 0 if mean < 2 */
    while (k0 < RICE_MAX_K && (mean >> (k0 + 1)) != 0) k0++;

    int lo = k0 > 0 ? k0 - 1 : 0;
    int hi = k0 < RICE_MAX_K ? k0 + 1 : RICE_MAX_K;

    int      best_k = lo;
    uint64_t best_bits = UINT64_MAX;
    for (int k = lo; k <= hi; k++) {
        uint64_t bits = (uint64_t)n * (uint64_t)(k + 1);
        for (size_t i = 0; i < n; i++) bits += u[i] >> k;
        if (bits < best_bits) { best_bits = bits; best_k = k; }
    }
    return best_k;
}

/* ------------------------------------------------------------------ */
/* Encoding                                                            */
/* ------------------------------------------------------------------ */
void rice_encode_block(BitWriter *w, const uint32_t *u, size_t n, int k)
{
    const uint32_t mask = (1u << k) - 1u;         /* selects the k remainder bits */

    for (size_t i = 0; i < n; i++) {
        uint32_t v = u[i];
        uint32_t q = v >> k;
        uint64_t width = (uint64_t)q + 1 + (uint64_t)k;

        if (width <= 32) {
            /* Fast path: write "q zeros, 1, r" as ONE field of `width` bits.
               The value (1<<k)|r has its top set bit exactly where the unary 1
               belongs; the q leading zeros come for free from the field width. */
            bw_put(w, (1u << k) | (v & mask), (int)width);
        } else {
            /* Rare path (huge quotient): unary part and remainder separately. */
            bw_put_unary(w, q);
            bw_put(w, v & mask, k);
        }
    }
}

/* ------------------------------------------------------------------ */
/* Decoding                                                            */
/* ------------------------------------------------------------------ */
void rice_decode_block(BitReader *r, uint32_t *u, size_t n, int k)
{
    for (size_t i = 0; i < n; i++) {
        uint32_t q   = br_get_unary(r);           /* count zeros up to the terminating 1 */
        uint32_t rem = br_get(r, k);              /* k remainder bits */
        u[i] = (q << k) | rem;
        if (r->err) return;                       /* ran out of data: caller reports it */
    }
}
