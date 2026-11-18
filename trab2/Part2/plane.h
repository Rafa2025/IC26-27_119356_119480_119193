/*
 * plane.h - prediction + context modelling of one plane of one tile.
 *
 * A plane is a 2-D array of integers (a grayscale image, or one colour plane
 * after the colour transform). Each pixel, in raster order:
 *
 *   1. WGT prediction: 6 sub-predictors (W, N, NE, NW, W+N-NW, MED), each with
 *      weight 2^40 / s^2, s = 1 + its |error| at W, N, NW, NE; integer maths,
 *      rounded, clipped to [lo, hi]                                  (D6)
 *   2. context = activity level (8) x error level (8) = 64 models     (D10)
 *        activity = |W-NW| + |N-NW| + |NE-N|
 *        error    = WGT's |error| at W, N, NW, NE (+ for colour planes 2 and 3,
 *                   the earlier planes' |error| at this pixel)
 *      both shifted right by (depth - 8) and put on a log scale (0..7)
 *   3. the residual (value - prediction) is coded with the model of that
 *      context (rc.h)
 *
 * Neighbours outside the tile: first row uses W for N, NW, NE; first column
 * uses N for W and NW; last column uses N for NE; stored errors outside the
 * tile count as 0. The decoder sees exactly the same values, so the encoder
 * and decoder share one function (plane_code) and cannot drift apart.
 */
#ifndef PLANE_H
#define PLANE_H

#include <stdint.h>

#include "rc.h"

#define NCTX 64

typedef struct {
    int32_t lo, hi;     /* range of the plane's values (prediction is clipped to it) */
    int shift;          /* depth - 8, for 16-bit data (0 for 8-bit) */
} PlaneInfo;

/* Encode (e != NULL) or decode (d != NULL) the h x w plane v (row by row).
 * When decoding, v is filled; values outside [vmin, vmax] mean corrupt data.
 * err_in (may be NULL): per-pixel |errors| of the colour planes already coded,
 *   added to the error context (D12).
 * err_out (may be NULL): receives this plane's |error| at every pixel.
 * Returns 0, or -1 on out-of-memory / corrupt data. */
int plane_code(int32_t *v, uint32_t h, uint32_t w, PlaneInfo pi, int32_t vmin, int32_t vmax,
               const int32_t *err_in, int32_t *err_out, RcEnc *e, RcDec *d);

#endif
