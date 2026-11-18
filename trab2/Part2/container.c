/*
 * container.c - the ICL2 compressed file format. See container.h.
 */
#include "container.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ writing */
void buf_put(Buf *b, const void *p, size_t n)
{
    if (b->oom || n == 0)
        return;
    if (b->n + n > b->cap) {
        size_t cap = b->cap ? b->cap : 4096;
        while (cap < b->n + n)
            cap *= 2;
        uint8_t *np = realloc(b->p, cap);
        if (!np) {
            b->oom = 1;
            return;
        }
        b->p = np;
        b->cap = cap;
    }
    memcpy(b->p + b->n, p, n);
    b->n += n;
}

void buf_u32(Buf *b, uint32_t v)
{
    uint8_t c[4] = {(uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24)};
    buf_put(b, c, 4);
}

void buf_free(Buf *b)
{
    free(b->p);
    memset(b, 0, sizeof *b);
}

/* ------------------------------------------------------------------ reading */
const uint8_t *rd_bytes(Rd *r, size_t n)
{
    if (r->bad || n > r->n - r->pos) {
        r->bad = 1;
        return NULL;
    }
    const uint8_t *p = r->p + r->pos;
    r->pos += n;
    return p;
}

uint32_t rd_u8(Rd *r)
{
    const uint8_t *p = rd_bytes(r, 1);
    return p ? p[0] : 0;
}

uint32_t rd_u32(Rd *r)
{
    const uint8_t *p = rd_bytes(r, 4);
    return p ? (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24 : 0;
}

/* ------------------------------------------------------------------ header */
uint32_t tiles_count(uint32_t rows, uint32_t cols, uint32_t tile)
{
    uint64_t tr = ((uint64_t)rows + tile - 1) / tile, tc = ((uint64_t)cols + tile - 1) / tile;
    uint64_t n = tr * tc;
    return n > 0xFFFFFFFFu ? 0xFFFFFFFFu : (uint32_t)n;
}

void icl2_write_header(Buf *b, const Image *im, const Layout *L)
{
    buf_put(b, "ICL2", 4);
    buf_u8(b, ICL2_VERSION);
    buf_u8(b, (uint32_t)L->type);
    buf_u8(b, (uint32_t)L->flags);
    buf_u8(b, (uint32_t)im->kind);
    buf_u8(b, (uint32_t)im->bpp);
    buf_u8(b, (uint32_t)im->big_endian);
    buf_u8(b, (uint32_t)im->channels);
    buf_u8(b, 0);
    buf_u32(b, im->rows);
    buf_u32(b, im->cols);
    buf_u32(b, im->maxval);
    buf_u32(b, L->crc);
    buf_u32(b, (uint32_t)im->header_len);
    buf_put(b, im->header, im->header_len);
    buf_u32(b, (uint32_t)im->trailer_len);
    buf_put(b, im->trailer, im->trailer_len);
    if (L->type == TYPE_PALETTE) {
        buf_u32(b, (uint32_t)L->npal);
        for (int i = 0; i < 3 * L->npal; i++) {
            uint8_t c2[2] = {(uint8_t)L->pal[i], (uint8_t)(L->pal[i] >> 8)};
            buf_put(b, c2, 2);
        }
    }
    buf_u32(b, L->tile);
    buf_u32(b, L->ntiles);
    for (uint32_t i = 0; i < L->ntiles; i++)
        buf_u32(b, L->tile_size[i]);
}

static int bad(char *err, const char *msg)
{
    snprintf(err, ERRLEN, "compressed file: %s", msg);
    return -1;
}

int icl2_read_header(Rd *r, Image *im, Layout *L, char *err)
{
    memset(im, 0, sizeof *im);
    memset(L, 0, sizeof *L);
    const uint8_t *magic = rd_bytes(r, 4);
    if (!magic || memcmp(magic, "ICL2", 4) != 0)
        return bad(err, "not an ICL2 file");
    if (rd_u8(r) != ICL2_VERSION)
        return bad(err, "unsupported version");
    L->type = (int)rd_u8(r);
    L->flags = (int)rd_u8(r);
    im->kind = (int)rd_u8(r);
    im->bpp = (int)rd_u8(r);
    im->big_endian = (int)rd_u8(r);
    im->channels = (int)rd_u8(r);
    uint32_t reserved = rd_u8(r);
    im->rows = rd_u32(r);
    im->cols = rd_u32(r);
    im->maxval = rd_u32(r);
    L->crc = rd_u32(r);
    if (r->bad)
        return bad(err, "truncated header");

    if (L->type != TYPE_GRAY && L->type != TYPE_COLOUR && L->type != TYPE_PALETTE)
        return bad(err, "unknown image type");
    /* flags: bits 1-2 = colour transform (0..2, colour images only); nothing else */
    int tf_ok = L->type == TYPE_COLOUR ? (L->flags & ~6) == 0 && (L->flags >> 1) <= 2 : L->flags == 0;
    if (!tf_ok || reserved != 0)
        return bad(err, "unsupported flags");
    if (im->kind < KIND_RAW || im->kind > KIND_PPM || (im->bpp != 8 && im->bpp != 16) ||
        (im->big_endian != 0 && im->big_endian != 1))
        return bad(err, "invalid image description");
    if (im->channels != (im->kind == KIND_PPM ? 3 : 1) ||
        (L->type != TYPE_GRAY) != (im->channels == 3))
        return bad(err, "invalid number of channels");
    if (im->rows == 0 || im->cols == 0 || img_samples(im) > (1ULL << 31))
        return bad(err, "invalid image size");
    if (im->kind == KIND_RAW ? im->maxval != (1u << im->bpp) - 1
                             : (im->maxval == 0 || im->maxval > 65535 ||
                                (im->maxval < 256) != (im->bpp == 8)))
        return bad(err, "invalid maxval");

    size_t hl = rd_u32(r);
    const uint8_t *h = rd_bytes(r, hl);
    if (r->bad || (im->kind == KIND_RAW) != (hl == 0))
        return bad(err, "invalid original header");
    size_t tl = rd_u32(r);
    const uint8_t *t = rd_bytes(r, tl);
    if (r->bad)
        return bad(err, "truncated trailer");
    if (L->type == TYPE_PALETTE) {
        uint32_t k = rd_u32(r);
        if (r->bad || k < 1 || k > 256)
            return bad(err, "invalid palette");
        L->npal = (int)k;
        for (int i = 0; i < 3 * L->npal; i++) {
            const uint8_t *q = rd_bytes(r, 2);
            if (!q)
                return bad(err, "truncated palette");
            L->pal[i] = (uint16_t)(q[0] | q[1] << 8);
            if (L->pal[i] > (1u << im->bpp) - 1)
                return bad(err, "invalid palette");
        }
    }

    L->tile = rd_u32(r);
    L->ntiles = rd_u32(r);
    if (r->bad || L->tile == 0 || L->ntiles != tiles_count(im->rows, im->cols, L->tile))
        return bad(err, "invalid tile layout");
    if ((uint64_t)L->ntiles * 4 > r->n - r->pos)
        return bad(err, "truncated tile table");
    L->tile_size = malloc((size_t)L->ntiles * sizeof *L->tile_size);
    im->header = malloc(hl ? hl : 1);
    im->trailer = malloc(tl ? tl : 1);
    if (!L->tile_size || !im->header || !im->trailer) {
        free(L->tile_size);
        L->tile_size = NULL;
        img_free(im);
        return bad(err, "out of memory");
    }
    memcpy(im->header, h, hl);
    im->header_len = hl;
    memcpy(im->trailer, t, tl);
    im->trailer_len = tl;

    uint64_t total = 0;
    for (uint32_t i = 0; i < L->ntiles; i++) {
        L->tile_size[i] = rd_u32(r);
        total += L->tile_size[i];
    }
    if (r->bad || total > r->n - r->pos) {
        free(L->tile_size);
        L->tile_size = NULL;
        img_free(im);
        return bad(err, "truncated tile data");
    }
    return 0;
}
