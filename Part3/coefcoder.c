/* coefcoder.c - see coefcoder.h for the binarization and the contexts. */
#include <stdlib.h>
#include <string.h>
#include "coefcoder.h"

#if CTX_MODE & 2
/* m (capped at 15) -> neighbour class: 0, 1, 2, 3-4, 5-6, 7-9, 10-14, 15+ */
static const uint8_t mclass[16] = {0, 1, 2, 3, 3, 4, 4, 5, 5, 5, 6, 6, 6, 6, 6, 7};
#endif

/*
 * The one context function used by both sides. cur/prev point at index 0;
 * cur[-1], cur[-2], prev[-1] and prev[n] are always zero.
 */
static inline void ctx_of(const uint8_t *cur, const uint8_t *prev, int k,
                          int *band, int *nctx)
{
#if CTX_MODE & 1
    int b = 31 - __builtin_clz((unsigned)k + 1);       /* floor(log2(k+1)) */
    *band = b < CC_NBANDS - 1 ? b : CC_NBANDS - 1;
#else
    *band = 0;
#endif
#if CTX_MODE & 2
    int m = 2 * cur[k - 1] + cur[k - 2] + prev[k - 1] + 2 * prev[k] + prev[k + 1];
    *nctx = mclass[m < 15 ? m : 15];
#else
    (void)cur; (void)prev; (void)k;
    *nctx = 0;
#endif
}

int cc_init(CoefCoder *cc, int n)
{
    memset(cc, 0, sizeof *cc);
    if (n < 64 || n > 8192 || (n & (n - 1)) != 0) return 1;
    cc->n = n;
    cc->nzb_bits = 1;
    while ((CC_GROUP << (cc->nzb_bits - 1)) < n) cc->nzb_bits++;   /* log2(N/16) + 1 */

    /* Both arrays: 2 zeros, n values, 1 zero. Only the n values are ever
       written, so the margins stay zero and cur/prev can simply be swapped. */
    size_t span = (size_t)n + 3;
    cc->mem = calloc(4 * span, 1);
    if (!cc->mem) return 1;
    for (int c = 0; c < 2; c++) {
        cc->cur[c]  = cc->mem + (2 * c) * span + 2;
        cc->prev[c] = cc->mem + (2 * c + 1) * span + 2;

        ChanModels *m = &cc->m[c];
        rc_prob_init(m->nzb, sizeof m->nzb / sizeof(Prob));
        rc_prob_init(&m->z[0][0], CC_NBANDS * CC_NCTX);
        rc_prob_init(&m->u[0][0], CC_NCTX * CC_NUNARY);
        rc_prob_init(m->eg, CC_EG_MAX + 1);
    }
    return 0;
}

void cc_free(CoefCoder *cc)
{
    free(cc->mem);
    memset(cc, 0, sizeof *cc);
}

/* After a block: clear the uncoded tail and make it the previous block. */
static void end_block(CoefCoder *cc, int ch, int coded)
{
    memset(cc->cur[ch] + coded, 0, (size_t)(cc->n - coded));
    uint8_t *t = cc->cur[ch];
    cc->cur[ch]  = cc->prev[ch];
    cc->prev[ch] = t;
}

void cc_encode_block(CoefCoder *cc, RcEnc *e, int ch, const int32_t *q)
{
    const int   n = cc->n;
    ChanModels *m = &cc->m[ch];
    uint8_t    *cur = cc->cur[ch];
    const uint8_t *prev = cc->prev[ch];

    int last = n - 1;
    while (last >= 0 && q[last] == 0) last--;
    int nzb = (last + 1 + CC_GROUP - 1) / CC_GROUP;

    for (int b = cc->nzb_bits - 1, idx = 1; b >= 0; b--) {
        int bit = (nzb >> b) & 1;
        rc_enc_bit(e, &m->nzb[idx], bit);
        idx = 2 * idx + bit;
    }

    const int len = nzb * CC_GROUP;
    for (int k = 0; k < len; k++) {
        int band, nc;
        ctx_of(cur, prev, k, &band, &nc);
        uint32_t a = q[k] < 0 ? (uint32_t)(-(int64_t)q[k]) : (uint32_t)q[k];

        rc_enc_bit(e, &m->z[band][nc], a != 0);
        if (a != 0) {
            rc_enc_direct(e, q[k] < 0, 1);
            for (uint32_t i = 1; i <= CC_NUNARY; i++) {
                int more = a > i;
                rc_enc_bit(e, &m->u[nc][i - 1], more);
                if (!more) break;
            }
            if (a >= CC_ESCAPE) {
                /* Exp-Golomb: v+1 = 1xxx (L bits after the leading 1);
                   prefix = L ones and a zero, suffix = the L bits. */
                uint32_t v1 = a - CC_ESCAPE + 1;
                int L = 31 - __builtin_clz(v1);
                for (int i = 0; i < L; i++) rc_enc_bit(e, &m->eg[i], 1);
                rc_enc_bit(e, &m->eg[L], 0);
                rc_enc_direct(e, v1, L);          /* the low L bits */
            }
        }
        cur[k] = (uint8_t)(a < CC_MAG_CAP ? a : CC_MAG_CAP);
    }
    end_block(cc, ch, len);
}

int cc_decode_block(CoefCoder *cc, RcDec *d, int ch, int32_t *q)
{
    const int   n = cc->n;
    ChanModels *m = &cc->m[ch];
    uint8_t    *cur = cc->cur[ch];
    const uint8_t *prev = cc->prev[ch];

    int nzb = 0;
    for (int b = cc->nzb_bits - 1, idx = 1; b >= 0; b--) {
        int bit = rc_dec_bit(d, &m->nzb[idx]);
        idx = 2 * idx + bit;
        nzb = (nzb << 1) | bit;
    }
    if (nzb > n / CC_GROUP) return -1;

    const int len = nzb * CC_GROUP;
    for (int k = 0; k < len; k++) {
        int band, nc;
        ctx_of(cur, prev, k, &band, &nc);
        uint32_t a = 0;
        int neg = 0;

        if (rc_dec_bit(d, &m->z[band][nc])) {
            neg = (int)rc_dec_direct(d, 1);
            a = 1;
            while (a <= CC_NUNARY && rc_dec_bit(d, &m->u[nc][a - 1])) a++;
            if (a >= CC_ESCAPE) {
                int L = 0;
                while (rc_dec_bit(d, &m->eg[L])) {
                    if (++L > CC_EG_MAX) return -1;
                }
                uint32_t v1 = (1u << L) | rc_dec_direct(d, L);
                a = v1 - 1 + CC_ESCAPE;
            }
        }
        q[k] = neg ? -(int32_t)a : (int32_t)a;
        cur[k] = (uint8_t)(a < CC_MAG_CAP ? a : CC_MAG_CAP);
        if (d->err) return 0;                /* truncated: caller sees d->err */
    }
    memset(q + len, 0, (size_t)(n - len) * sizeof *q);
    end_block(cc, ch, len);
    return 0;
}
