/*
 * palette.c - palette mode. See palette.h.
 */
#include "palette.h"

#include <stdlib.h>
#include <string.h>

#ifndef PAL_INC
#define PAL_INC 32
#endif

/* ------------------------------------------------------------------ building */
static inline uint64_t key3(const uint16_t *p)
{
    return (uint64_t)p[0] << 32 | (uint64_t)p[1] << 16 | p[2];
}

static int cmp_u64(const void *a, const void *b)
{
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return x < y ? -1 : x > y;
}

/* open-addressing hash set of up to 256 colours (1024 slots), slot -> index */
#define HSIZE 1024
static inline uint32_t hslot(uint64_t k) { return (uint32_t)((k * 0x9E3779B97F4A7C15ull) >> 54); }

int palette_build(const Image *im, uint16_t *pal, uint8_t *idx)
{
    uint64_t keys[HSIZE], list[PAL_MAX];
    uint8_t used[HSIZE] = {0}, slot_idx[HSIZE];
    int K = 0;
    size_t np = (size_t)im->rows * im->cols;
    for (size_t i = 0; i < np; i++) {
        uint64_t k = key3(im->pix + 3 * i);
        uint32_t h = hslot(k);
        while (used[h] && keys[h] != k)
            h = (h + 1) & (HSIZE - 1);
        if (!used[h]) {
            if (K == PAL_MAX)
                return 0;                     /* more than 256 colours */
            used[h] = 1;
            keys[h] = k;
            list[K++] = k;
        }
    }
    qsort(list, (size_t)K, sizeof list[0], cmp_u64);
    for (int j = 0; j < K; j++) {
        pal[3 * j] = (uint16_t)(list[j] >> 32);
        pal[3 * j + 1] = (uint16_t)(list[j] >> 16);
        pal[3 * j + 2] = (uint16_t)list[j];
        uint32_t h = hslot(list[j]);
        while (keys[h] != list[j])
            h = (h + 1) & (HSIZE - 1);
        slot_idx[h] = (uint8_t)j;
    }
    for (size_t i = 0; i < np; i++) {
        uint64_t k = key3(im->pix + 3 * i);
        uint32_t h = hslot(k);
        while (keys[h] != k)
            h = (h + 1) & (HSIZE - 1);
        idx[i] = slot_idx[h];
    }
    return K;
}

/* ------------------------------------------------------------------ coding */
int palette_code(uint8_t *idx, uint32_t h, uint32_t w, int K, RcEnc *e, RcDec *d)
{
    uint32_t *f = malloc((size_t)K * K * sizeof *f), *tot = malloc((size_t)K * sizeof *tot);
    if (!f || !tot) {
        free(f);
        free(tot);
        return -1;
    }
    for (size_t i = 0; i < (size_t)K * K; i++)
        f[i] = 1;
    for (int i = 0; i < K; i++)
        tot[i] = (uint32_t)K;

    int bad = 0;
    for (uint32_t r = 0; r < h && !bad; r++) {
        uint8_t *row = idx + (size_t)r * w;
        for (uint32_t c = 0; c < w; c++) {
            int ctx = c ? row[c - 1] : (r ? row[c - (size_t)w] : 0);
            uint32_t *fc = f + (size_t)ctx * K;
            int s;
            if (e) {
                s = row[c];
                uint32_t start = 0;
                for (int j = 0; j < s; j++)
                    start += fc[j];
                rce_encode(e, start, fc[s], tot[ctx]);
            } else {
                uint32_t rr, v = rcd_target(d, tot[ctx], &rr), start = 0;
                s = 0;
                while (s < K - 1 && start + fc[s] <= v)
                    start += fc[s++];
                if (start + fc[s] <= v) {          /* only with corrupt data */
                    bad = 1;
                    break;
                }
                rcd_consume(d, start, fc[s], rr);
                row[c] = (uint8_t)s;
            }
            fc[s] += PAL_INC;
            tot[ctx] += PAL_INC;
            if (tot[ctx] > (1u << 16)) {
                uint32_t t = 0;
                for (int j = 0; j < K; j++) {
                    fc[j] = (fc[j] + 1) >> 1;
                    t += fc[j];
                }
                tot[ctx] = t;
            }
        }
    }
    free(f);
    free(tot);
    return bad ? -1 : 0;
}
