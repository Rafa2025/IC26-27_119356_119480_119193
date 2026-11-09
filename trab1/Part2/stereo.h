/*
 * stereo.h - exact (lossless) mid/side transform.
 *
 * Why: in real music L and R are very similar, so L-R (SIDE) is small and
 * cheap to code, while (L+R)/2 (MID) carries the actual signal.
 *
 * Forward:   MID  = (L + R) >> 1     (floor division by 2, loses 1 bit)
 *            SIDE =  L - R           (NOT halved, keeps full information)
 *
 * Inverse:   L + R and L - R always have the same parity (both even or both
 *            odd), so the bit lost by MID is exactly the lowest bit of SIDE:
 *               sum = 2*MID + (SIDE & 1)      -> equals L + R exactly
 *               L = (sum + SIDE) / 2
 *               R = (sum - SIDE) / 2
 *
 * Both functions work IN PLACE: a[] holds L then MID, b[] holds R then SIDE.
 */
#ifndef STEREO_H
#define STEREO_H

#include <stddef.h>
#include <stdint.h>

void ms_forward(int32_t *a, int32_t *b, size_t n);   /* (L,R) -> (MID,SIDE) */
void ms_inverse(int32_t *a, int32_t *b, size_t n);   /* (MID,SIDE) -> (L,R) */

#endif /* STEREO_H */
