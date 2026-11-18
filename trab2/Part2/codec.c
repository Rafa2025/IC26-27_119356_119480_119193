/*
 * codec.c - lossless image codec: image <-> ICL2 container. See codec.h.
 */
#include "codec.h"
#include "palette.h"
#include "plane.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    uint32_t r0, c0, h, w;   /* first row/column and size of the tile, in pixels */
} TileRect;

static TileRect tile_rect(const Image *im, uint32_t tile, uint32_t i)
{
    uint32_t tc = (uint32_t)(((uint64_t)im->cols + tile - 1) / tile);
    TileRect t;
    t.r0 = (i / tc) * tile;
    t.c0 = (i % tc) * tile;
    t.h = im->rows - t.r0 < tile ? im->rows - t.r0 : tile;
    t.w = im->cols - t.c0 < tile ? im->cols - t.c0 : tile;
    return t;
}

/* ------------------------------------------------------------------ stored tiles */
static void store_tile(const Image *im, TileRect t, Buf *b)
{
    int ch = im->channels;
    buf_u8(b, TILE_STORED);
    for (uint32_t r = 0; r < t.h; r++) {
        const uint16_t *s = im->pix + ((size_t)(t.r0 + r) * im->cols + t.c0) * ch;
        size_t n = (size_t)t.w * ch;
        if (im->bpp == 8) {
            for (size_t j = 0; j < n; j++)
                buf_u8(b, s[j]);
        } else {
            for (size_t j = 0; j < n; j++) {
                uint8_t c[2] = {(uint8_t)s[j], (uint8_t)(s[j] >> 8)};
                buf_put(b, c, 2);
            }
        }
    }
}

static int unstore_tile(Image *im, TileRect t, const uint8_t *p, size_t n)
{
    int ch = im->channels, bytes = im->bpp / 8;
    size_t row = (size_t)t.w * ch;
    if (n != row * t.h * bytes)
        return -1;
    for (uint32_t r = 0; r < t.h; r++) {
        uint16_t *d = im->pix + ((size_t)(t.r0 + r) * im->cols + t.c0) * ch;
        if (bytes == 1) {
            for (size_t j = 0; j < row; j++)
                d[j] = *p++;
        } else {
            for (size_t j = 0; j < row; j++, p += 2)
                d[j] = (uint16_t)(p[0] | p[1] << 8);
        }
    }
    return 0;
}

/* ------------------------------------------------------------------ predicted tiles */
static PlaneInfo gray_info(const Image *im)
{
    PlaneInfo pi;
    pi.lo = 0;
    pi.hi = (int32_t)im->maxval;
    int depth = 32 - __builtin_clz(im->maxval);
    if (im->kind == KIND_RAW)
        depth = im->bpp;
    pi.shift = depth > 8 ? depth - 8 : 0;
    return pi;
}

/* Code a grayscale tile; falls back to "stored" if that is not smaller. */
static void code_gray_tile(const Image *im, TileRect t, Buf *b)
{
    size_t n = (size_t)t.h * t.w;
    int32_t *v = malloc(n * sizeof *v);
    Buf tmp = {0};
    int ok = v != NULL;
    if (ok) {
        for (uint32_t r = 0; r < t.h; r++) {
            const uint16_t *s = im->pix + (size_t)(t.r0 + r) * im->cols + t.c0;
            for (uint32_t c = 0; c < t.w; c++)
                v[(size_t)r * t.w + c] = s[c];
        }
        buf_u8(&tmp, TILE_PRED);
        RcEnc e;
        rce_init(&e, &tmp);
        ok = plane_code(v, t.h, t.w, gray_info(im), 0, 0, NULL, NULL, &e, NULL) == 0;
        rce_flush(&e);
        ok = ok && !tmp.oom;
    }
    free(v);
    size_t stored = 1 + n * (size_t)(im->bpp / 8);
    if (ok && tmp.n < stored)
        buf_put(b, tmp.p, tmp.n);
    else
        store_tile(im, t, b);
    buf_free(&tmp);
}

static int decode_gray_tile(Image *im, TileRect t, const uint8_t *p, size_t n)
{
    size_t np = (size_t)t.h * t.w;
    int32_t *v = malloc(np * sizeof *v);
    if (!v)
        return -1;
    RcDec d;
    rcd_init(&d, p, n);
    int32_t vmax = (int32_t)((1u << im->bpp) - 1);
    int rc = plane_code(v, t.h, t.w, gray_info(im), 0, vmax, NULL, NULL, NULL, &d);
    if (rc == 0 && d.overrun)
        rc = -1;
    if (rc == 0)
        for (uint32_t r = 0; r < t.h; r++) {
            uint16_t *s = im->pix + (size_t)(t.r0 + r) * im->cols + t.c0;
            for (uint32_t c = 0; c < t.w; c++)
                s[c] = (uint16_t)v[(size_t)r * t.w + c];
        }
    free(v);
    return rc;
}

/* ------------------------------------------------------------------ colour */
/* floor(x / 2^k) for negative x too (>> on negative numbers is implementation-defined) */
static inline int32_t fl_shift(int32_t x, int k) { return x >= 0 ? x >> k : -((-x - 1) >> k) - 1; }

/* Forward colour transform of one pixel into the 3 planes, in coding order (D7):
 *   TF_NONE:  G, R, B
 *   TF_RCT:   Y = floor((R + 2G + B) / 4), U = R - G, V = B - G      (JPEG 2000)
 *   TF_YCOCG: Co = R - B, t = B + floor(Co/2), Cg = G - t, Y = t + floor(Cg/2)  (YCoCg-R)
 *             planes Y, Co, Cg */
static inline void tf_fwd(int tf, int32_t R, int32_t G, int32_t B, int32_t p[3])
{
    if (tf == TF_NONE) {
        p[0] = G, p[1] = R, p[2] = B;
    } else if (tf == TF_RCT) {
        p[0] = (R + 2 * G + B) >> 2, p[1] = R - G, p[2] = B - G;
    } else {
        int32_t co = R - B, t = B + fl_shift(co, 1), cg = G - t;
        p[0] = t + fl_shift(cg, 1), p[1] = co, p[2] = cg;
    }
}

static inline void tf_inv(int tf, const int32_t p[3], int32_t rgb[3])
{
    if (tf == TF_NONE) {
        rgb[0] = p[1], rgb[1] = p[0], rgb[2] = p[2];
    } else if (tf == TF_RCT) {
        int32_t g = p[0] - fl_shift(p[1] + p[2], 2);
        rgb[0] = p[1] + g, rgb[1] = g, rgb[2] = p[2] + g;
    } else {
        int32_t t = p[0] - fl_shift(p[2], 1), g = p[2] + t, b = t - fl_shift(p[1], 1);
        rgb[0] = b + p[1], rgb[1] = g, rgb[2] = b;
    }
}

/* Prediction range (from maxval) and validity range (from the 16/8-bit container)
 * of colour plane k. */
static void colour_info(const Image *im, int tf, int k, PlaneInfo *pi, int32_t *vmin, int32_t *vmax)
{
    *pi = gray_info(im);
    int32_t cmax = (int32_t)((1u << im->bpp) - 1);
    if (k == 0 || tf == TF_NONE) {
        *vmin = 0, *vmax = cmax;
    } else {
        pi->lo = -pi->hi;
        *vmin = -cmax, *vmax = cmax;
    }
}

/* Code a colour tile: the 3 planes one after the other in one range-coder stream,
 * each plane's error context including the earlier planes' errors (D12).
 * Falls back to "stored" if that is not smaller. */
static void code_colour_tile(const Image *im, TileRect t, int tf, Buf *b)
{
    size_t n = (size_t)t.h * t.w;
    int32_t *mem = malloc(5 * n * sizeof *mem);
    Buf tmp = {0};
    int ok = mem != NULL;
    if (ok) {
        int32_t *pl[3] = {mem, mem + n, mem + 2 * n}, *esum = mem + 3 * n, *eout = mem + 4 * n;
        for (uint32_t r = 0; r < t.h; r++) {
            const uint16_t *s = im->pix + ((size_t)(t.r0 + r) * im->cols + t.c0) * 3;
            for (uint32_t c = 0; c < t.w; c++, s += 3) {
                int32_t p[3];
                tf_fwd(tf, s[0], s[1], s[2], p);
                for (int k = 0; k < 3; k++)
                    pl[k][(size_t)r * t.w + c] = p[k];
            }
        }
        memset(esum, 0, n * sizeof *esum);
        buf_u8(&tmp, TILE_PRED);
        RcEnc e;
        rce_init(&e, &tmp);
        for (int k = 0; k < 3 && ok; k++) {
            PlaneInfo pi;
            int32_t vmin, vmax;
            colour_info(im, tf, k, &pi, &vmin, &vmax);
            ok = plane_code(pl[k], t.h, t.w, pi, 0, 0, k ? esum : NULL, eout, &e, NULL) == 0;
            for (size_t i = 0; i < n; i++)
                esum[i] += eout[i];
        }
        rce_flush(&e);
        ok = ok && !tmp.oom;
    }
    free(mem);
    size_t stored = 1 + n * 3 * (size_t)(im->bpp / 8);
    if (ok && tmp.n < stored)
        buf_put(b, tmp.p, tmp.n);
    else
        store_tile(im, t, b);
    buf_free(&tmp);
}

static int decode_colour_tile(Image *im, TileRect t, int tf, const uint8_t *p, size_t len)
{
    size_t n = (size_t)t.h * t.w;
    int32_t *mem = malloc(5 * n * sizeof *mem);
    if (!mem)
        return -1;
    int32_t *pl[3] = {mem, mem + n, mem + 2 * n}, *esum = mem + 3 * n, *eout = mem + 4 * n;
    memset(esum, 0, n * sizeof *esum);
    RcDec d;
    rcd_init(&d, p, len);
    int rc = 0;
    for (int k = 0; k < 3 && rc == 0; k++) {
        PlaneInfo pi;
        int32_t vmin, vmax;
        colour_info(im, tf, k, &pi, &vmin, &vmax);
        rc = plane_code(pl[k], t.h, t.w, pi, vmin, vmax, k ? esum : NULL, eout, NULL, &d);
        for (size_t i = 0; i < n; i++)
            esum[i] += eout[i];
    }
    if (rc == 0 && d.overrun)
        rc = -1;
    int32_t cmax = (int32_t)((1u << im->bpp) - 1);
    for (uint32_t r = 0; r < t.h && rc == 0; r++) {
        uint16_t *s = im->pix + ((size_t)(t.r0 + r) * im->cols + t.c0) * 3;
        for (uint32_t c = 0; c < t.w; c++, s += 3) {
            size_t i = (size_t)r * t.w + c;
            int32_t q[3] = {pl[0][i], pl[1][i], pl[2][i]}, rgb[3];
            tf_inv(tf, q, rgb);
            for (int k = 0; k < 3; k++) {
                if (rgb[k] < 0 || rgb[k] > cmax)
                    rc = -1;
                s[k] = (uint16_t)rgb[k];
            }
        }
    }
    free(mem);
    return rc;
}

/* ------------------------------------------------------------------ palette tiles */
static void code_palette_tile(const Image *im, const uint8_t *idx, int K, TileRect t, Buf *b)
{
    size_t n = (size_t)t.h * t.w;
    uint8_t *ti = malloc(n ? n : 1);
    Buf tmp = {0};
    int ok = ti != NULL;
    if (ok) {
        for (uint32_t r = 0; r < t.h; r++)
            memcpy(ti + (size_t)r * t.w, idx + (size_t)(t.r0 + r) * im->cols + t.c0, t.w);
        buf_u8(&tmp, TILE_PRED);
        RcEnc e;
        rce_init(&e, &tmp);
        ok = palette_code(ti, t.h, t.w, K, &e, NULL) == 0;
        rce_flush(&e);
        ok = ok && !tmp.oom;
    }
    free(ti);
    size_t stored = 1 + n * 3 * (size_t)(im->bpp / 8);
    if (ok && tmp.n < stored)
        buf_put(b, tmp.p, tmp.n);
    else
        store_tile(im, t, b);
    buf_free(&tmp);
}

static int decode_palette_tile(Image *im, const Layout *L, TileRect t, const uint8_t *p, size_t len)
{
    size_t n = (size_t)t.h * t.w;
    uint8_t *ti = malloc(n ? n : 1);
    if (!ti)
        return -1;
    RcDec d;
    rcd_init(&d, p, len);
    int rc = palette_code(ti, t.h, t.w, L->npal, NULL, &d);
    if (rc == 0 && d.overrun)
        rc = -1;
    for (uint32_t r = 0; r < t.h && rc == 0; r++) {
        uint16_t *s = im->pix + ((size_t)(t.r0 + r) * im->cols + t.c0) * 3;
        for (uint32_t c = 0; c < t.w; c++, s += 3) {
            const uint16_t *col = L->pal + 3 * ti[(size_t)r * t.w + c];
            s[0] = col[0], s[1] = col[1], s[2] = col[2];
        }
    }
    free(ti);
    return rc;
}

/* Fast choice of the colour transform (D7): on every 8th pair of rows, the cost
 * of the MED residuals of the 3 planes, ~ sum of their bit lengths. */
static int estimate_tf(const Image *im)
{
    uint64_t cost[3] = {0, 0, 0};
    for (uint32_t r = 1; r < im->rows; r += 8) {
        const uint16_t *up = im->pix + (size_t)(r - 1) * im->cols * 3, *row = up + (size_t)im->cols * 3;
        for (uint32_t c = 1; c < im->cols; c++) {
            for (int tf = 0; tf < 3; tf++) {
                int32_t x[3], w[3], n[3], nw[3];
                tf_fwd(tf, row[3 * c], row[3 * c + 1], row[3 * c + 2], x);
                tf_fwd(tf, row[3 * c - 3], row[3 * c - 2], row[3 * c - 1], w);
                tf_fwd(tf, up[3 * c], up[3 * c + 1], up[3 * c + 2], n);
                tf_fwd(tf, up[3 * c - 3], up[3 * c - 2], up[3 * c - 1], nw);
                for (int k = 0; k < 3; k++) {
                    int32_t mx = w[k] > n[k] ? w[k] : n[k], mn = w[k] < n[k] ? w[k] : n[k];
                    int32_t pr = nw[k] >= mx ? mn : (nw[k] <= mn ? mx : w[k] + n[k] - nw[k]);
                    int32_t res = x[k] - pr;
                    uint32_t u = res >= 0 ? 2u * (uint32_t)res : 2u * (uint32_t)(-(res + 1)) + 1u;
                    cost[tf] += u ? 32 - __builtin_clz(u) : 0;
                }
            }
        }
    }
    int best = 0;
    for (int tf = 1; tf < 3; tf++)
        if (cost[tf] < cost[best])
            best = tf;
    return best;
}

/* ------------------------------------------------------------------ encode */
/* tf >= 0: colour transform; idx != NULL: palette mode with K colours in pal */
static int encode_with(const Image *im, uint32_t crc, uint32_t tile, int tf, const uint8_t *idx,
                       int K, const uint16_t *pal, Buf *out, char *err)
{
    Layout L = {0};
    L.type = im->channels == 1 ? TYPE_GRAY : idx ? TYPE_PALETTE : TYPE_COLOUR;
    L.flags = L.type == TYPE_COLOUR ? tf << 1 : 0;
    if (idx) {
        L.npal = K;
        memcpy(L.pal, pal, (size_t)K * 3 * sizeof *pal);
    }
    L.crc = crc;
    L.tile = tile;
    L.ntiles = tiles_count(im->rows, im->cols, tile);
    Buf *tb = calloc(L.ntiles, sizeof *tb);
    L.tile_size = calloc(L.ntiles, sizeof *L.tile_size);
    if (!tb || !L.tile_size) {
        free(tb);
        free(L.tile_size);
        snprintf(err, ERRLEN, "out of memory");
        return -1;
    }

#pragma omp parallel for schedule(dynamic)
    for (long i = 0; i < (long)L.ntiles; i++) {
        TileRect t = tile_rect(im, tile, (uint32_t)i);
        if (im->channels == 1)
            code_gray_tile(im, t, &tb[i]);
        else if (idx)
            code_palette_tile(im, idx, K, t, &tb[i]);
        else
            code_colour_tile(im, t, tf, &tb[i]);
    }

    int oom = 0;
    for (uint32_t i = 0; i < L.ntiles; i++) {
        oom |= tb[i].oom || tb[i].n > 0xFFFFFFFFu;
        L.tile_size[i] = (uint32_t)tb[i].n;
    }
    if (!oom) {
        icl2_write_header(out, im, &L);
        for (uint32_t i = 0; i < L.ntiles; i++)
            buf_put(out, tb[i].p, tb[i].n);
        oom = out->oom;
    }
    for (uint32_t i = 0; i < L.ntiles; i++)
        buf_free(&tb[i]);
    free(tb);
    free(L.tile_size);
    if (oom) {
        snprintf(err, ERRLEN, "out of memory");
        return -1;
    }
    return 0;
}

int encode_image(const Image *im, uint32_t crc, uint32_t tile, int ctf, int use_palette, Buf *out,
                 char *err)
{
    if (im->channels == 1)
        return encode_with(im, crc, tile, TF_NONE, NULL, 0, NULL, out, err);
    if (use_palette) {                                 /* D8: <= 256 colours -> palette */
        uint16_t pal[PAL_MAX * 3];
        uint8_t *idx = malloc((size_t)im->rows * im->cols);
        if (!idx) {
            snprintf(err, ERRLEN, "out of memory");
            return -1;
        }
        int K = palette_build(im, pal, idx);
        int rc = K ? encode_with(im, crc, tile, 0, idx, K, pal, out, err) : 1;
        free(idx);
        if (rc <= 0)
            return rc;
    }
    if (ctf != TF_BEST)
        return encode_with(im, crc, tile, ctf == TF_AUTO ? estimate_tf(im) : ctf, NULL, 0, NULL, out, err);
    /* exact choice: code with every transform, keep the smallest */
    for (int tf = 0; tf < 3; tf++) {
        Buf b = {0};
        if (encode_with(im, crc, tile, tf, NULL, 0, NULL, &b, err)) {
            buf_free(&b);
            return -1;
        }
        if (tf == 0 || b.n < out->n) {
            buf_free(out);
            *out = b;
        } else {
            buf_free(&b);
        }
    }
    return 0;
}

/* ------------------------------------------------------------------ decode */
int decode_image(const uint8_t *data, size_t n, Image *im, uint32_t *crc, char *err)
{
    Rd r = {data, n, 0, 0};
    Layout L;
    if (icl2_read_header(&r, im, &L, err))
        return -1;
    *crc = L.crc;
    im->pix = malloc(img_samples(im) * sizeof(uint16_t));
    size_t *off = malloc(((size_t)L.ntiles + 1) * sizeof *off);
    if (!im->pix || !off) {
        free(off);
        free(L.tile_size);
        img_free(im);
        snprintf(err, ERRLEN, "out of memory");
        return -1;
    }
    off[0] = r.pos;
    for (uint32_t i = 0; i < L.ntiles; i++)
        off[i + 1] = off[i] + L.tile_size[i];

    int failed = 0;
#pragma omp parallel for schedule(dynamic) reduction(| : failed)
    for (long i = 0; i < (long)L.ntiles; i++) {
        const uint8_t *p = data + off[i];
        size_t len = L.tile_size[i];
        TileRect t = tile_rect(im, L.tile, (uint32_t)i);
        if (len < 1)
            failed |= 1;
        else if (p[0] == TILE_STORED)
            failed |= unstore_tile(im, t, p + 1, len - 1) != 0;
        else if (p[0] == TILE_PRED && im->channels == 1)
            failed |= decode_gray_tile(im, t, p + 1, len - 1) != 0;
        else if (p[0] == TILE_PRED && L.type == TYPE_PALETTE)
            failed |= decode_palette_tile(im, &L, t, p + 1, len - 1) != 0;
        else if (p[0] == TILE_PRED)
            failed |= decode_colour_tile(im, t, (L.flags >> 1) & 3, p + 1, len - 1) != 0;
        else
            failed |= 1;
    }
    int extra = off[L.ntiles] != n;          /* bytes after the last tile */
    free(off);
    free(L.tile_size);
    if (failed || extra) {
        img_free(im);
        snprintf(err, ERRLEN, "compressed file: %s", failed ? "corrupt tile data" : "unexpected bytes at the end");
        return -1;
    }
    return 0;
}
