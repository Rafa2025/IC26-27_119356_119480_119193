/*
 * plane.c - prediction + context modelling of one plane of one tile. See plane.h.
 */
#include "plane.h"

#include <stdlib.h>
#include <string.h>

static inline int lev8(uint32_t v)            /* log-scale level 0..7 */
{
    int n = v ? 32 - __builtin_clz(v) : 0;
    return n < 7 ? n : 7;
}

static inline int64_t floordiv(int64_t a, int64_t b)   /* b > 0 */
{
    int64_t q = a / b;
    return (a % b != 0 && a < 0) ? q - 1 : q;
}

static inline int32_t iabs(int32_t x) { return x < 0 ? -x : x; }

/* 2^40 / s^2 for small s, filled once (same values as the division) */
#define WT_TABLE 4096
static int64_t wt_table[WT_TABLE];

static void wt_init(void)
{
    for (int64_t s = 1; s < WT_TABLE; s++)
        wt_table[s] = ((int64_t)1 << 40) / (s * s);
}

static inline int64_t weight(int64_t s)
{
    if (s < WT_TABLE)
        return wt_table[s];
    int64_t wt = ((int64_t)1 << 40) / (s * s);
    return wt < 1 ? 1 : wt;
}

int plane_code(int32_t *v, uint32_t h, uint32_t w, PlaneInfo pi, int32_t vmin, int32_t vmax,
               const int32_t *err_in, int32_t *err_out, RcEnc *e, RcDec *d)
{
    /* |error| of the 6 sub-predictors and of the final prediction, for the previous
     * and the current row; index c + 1, with a zero column on each side */
    size_t W2 = (size_t)w + 2;
    int32_t *mem = calloc(7 * 2 * W2, sizeof *mem);
    Model *models = malloc(NCTX * sizeof *models);
    if (!mem || !models) {
        free(mem);
        free(models);
        return -1;
    }
    for (int i = 0; i < NCTX; i++)
        model_init(&models[i]);
#pragma omp critical(wt_table_init)
    if (wt_table[1] == 0)
        wt_init();
    int32_t *esub[6][2], *efin[2];
    for (int i = 0; i < 6; i++)
        for (int k = 0; k < 2; k++)
            esub[i][k] = mem + (size_t)(i * 2 + k) * W2;
    efin[0] = mem + 12 * W2;
    efin[1] = mem + 13 * W2;

    int bad = 0;
    int32_t sub[6];
    for (uint32_t r = 0; r < h && !bad; r++) {
        int cur = r & 1, prv = cur ^ 1;
        if (r == 0) {                       /* "previous row" is outside the tile: zeros */
            for (int i = 0; i < 6; i++)
                memset(esub[i][prv], 0, W2 * sizeof(int32_t));
            memset(efin[prv], 0, W2 * sizeof(int32_t));
        }
        const int32_t *row = v + (size_t)r * w, *up = r ? row - w : NULL;
        for (uint32_t c = 0; c < w; c++) {
            int32_t W, N, NW, NE;
            if (r == 0) {
                W = c ? row[c - 1] : 0;
                N = NW = NE = W;
            } else {
                N = up[c];
                W = c ? row[c - 1] : N;
                NW = c ? up[c - 1] : N;
                NE = c + 1 < w ? up[c + 1] : N;
            }
            int32_t mx = W > N ? W : N, mn = W < N ? W : N;
            sub[0] = W;
            sub[1] = N;
            sub[2] = NE;
            sub[3] = NW;
            sub[4] = W + N - NW;
            sub[5] = NW >= mx ? mn : (NW <= mn ? mx : W + N - NW);

            /* WGT */
            int64_t num = 0, den = 0;
            for (int i = 0; i < 6; i++) {
                const int32_t *ec = esub[i][cur], *ep = esub[i][prv];
                int64_t s = 1 + (int64_t)ec[c] + ep[c + 1] + ep[c] + ep[c + 2];
                int64_t wt = weight(s);
                num += wt * sub[i];
                den += wt;
            }
            int32_t pred = (int32_t)floordiv(num + den / 2, den);
            if (pred < pi.lo) pred = pi.lo;
            if (pred > pi.hi) pred = pi.hi;

            /* context */
            uint32_t act = (uint32_t)(iabs(W - NW) + iabs(N - NW) + iabs(NE - N)) >> pi.shift;
            int32_t esum = efin[cur][c] + efin[prv][c + 1] + efin[prv][c] + efin[prv][c + 2];
            if (err_in)
                esum += err_in[(size_t)r * w + c];
            uint32_t err = (uint32_t)esum >> pi.shift;
            Model *m = &models[lev8(act) * 8 + lev8(err)];

            /* code the residual */
            int32_t x;
            if (e) {
                x = row[c];
                rce_residual(e, m, x - pred);
            } else {
                int64_t xx = (int64_t)pred + rcd_residual(d, m);
                if (xx < vmin || xx > vmax) {
                    bad = 1;
                    break;
                }
                x = (int32_t)xx;
                v[(size_t)r * w + c] = x;
            }

            /* errors for the next pixels */
            for (int i = 0; i < 6; i++)
                esub[i][cur][c + 1] = iabs(x - sub[i]);
            efin[cur][c + 1] = iabs(x - pred);
            if (err_out)
                err_out[(size_t)r * w + c] = efin[cur][c + 1];
        }
    }
    free(mem);
    free(models);
    return bad ? -1 : 0;
}
