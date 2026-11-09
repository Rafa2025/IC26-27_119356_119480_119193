/*
 * coefcoder.h - entropy coding of quantized coefficient blocks.
 *
 * The only file that knows the statistics of the coefficients.
 * Per block and channel:
 *
 *   nzb = ceil((last nonzero index + 1) / 16)        0 .. N/16
 *         coded with an adaptive bit tree of log2(N/16)+1 bits
 *   q[0] .. q[16*nzb - 1], each binarized as
 *     1. q == 0 ?                 model Z[band][nctx]
 *     2. sign                     direct bit
 *     3. |q| > 1 ? .. |q| > 14 ?  model U[nctx][i] (unary, stops at the first "no")
 *     4. |q| >= 15: |q| - 15 as Exp-Golomb (order 0)
 *                   prefix: model EG[i]; suffix: direct bits
 *   the rest of the block is zero and not coded.
 *
 * CONTEXTS (computed identically by encoder and decoder in ctx_of())
 *   band = min(floor(log2(k + 1)), 11)
 *   nctx = class of m = 2|q[k-1]| + |q[k-2]| + |p[k-1]| + 2|p[k]| + |p[k+1]|,
 *          p = previous block of the same channel; classes
 *          0, 1, 2, 3-4, 5-6, 7-9, 10-14, 15+
 *
 * Compile-time variant CTX_MODE (see "Build variants" in CODEC_LOSSY.md):
 *   0 no contexts, 1 band only, 2 neighbours only (default), 3 both.
 * Experiment 4 showed that the band adds nothing once the neighbours are
 * known (busy regions are the low frequencies anyway), so the default
 * uses the neighbours only: fewer models, each one learns faster.
 *
 * Each channel has its own models and its own previous block.
 */
#ifndef COEFCODER_H
#define COEFCODER_H

#include <stdint.h>
#include "rangecoder.h"

#ifndef CTX_MODE
#define CTX_MODE 2
#endif

#define CC_NBANDS   12
#define CC_NCTX     8
#define CC_NUNARY   14          /* |q| > 1 .. |q| > 14 */
#define CC_ESCAPE   15          /* |q| >= 15 goes to Exp-Golomb */
#define CC_EG_MAX   29          /* longest Exp-Golomb prefix (|q| <= 2^30 needs 29) */
#define CC_GROUP    16          /* nzb counts groups of 16 coefficients */
#define CC_MAG_CAP  15          /* stored context magnitudes are capped here */

typedef struct {
    Prob nzb[1024];                      /* bit tree; 2^bits <= 2 * (8192/16) */
    Prob z[CC_NBANDS][CC_NCTX];
    Prob u[CC_NCTX][CC_NUNARY];
    Prob eg[CC_EG_MAX + 1];
} ChanModels;

typedef struct {
    int        n;                        /* coefficients per block */
    int        nzb_bits;                 /* log2(N/16) + 1 */
    ChanModels m[2];
    /* |q| capped at CC_MAG_CAP, with zero margins so neighbours never go
       out of range: index -2, -1 and n are always 0. */
    uint8_t   *cur[2], *prev[2];
    uint8_t   *mem;
} CoefCoder;

/* n: power of two, 64 .. 8192. Returns 0 on success. */
int  cc_init(CoefCoder *cc, int n);
void cc_free(CoefCoder *cc);

/* Code one block of channel ch (0 or 1). |q[k]| must be <= QUANT_MAX. */
void cc_encode_block(CoefCoder *cc, RcEnc *e, int ch, const int32_t *q);

/* Returns 0 on success, -1 if the data cannot have come from the encoder.
   The caller also checks d->err for a truncated stream. */
int  cc_decode_block(CoefCoder *cc, RcDec *d, int ch, int32_t *q);

#endif /* COEFCODER_H */
