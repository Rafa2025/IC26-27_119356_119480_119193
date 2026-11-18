/*
 * rc.h - adaptive multi-symbol range coder (header-only, for speed).
 *
 * Range coder: 32-bit range, 64-bit low with carry propagation (the scheme of
 * LZMA's range coder), byte-oriented output. One division per symbol.
 *
 * Model: adaptive frequency table over NSYM = 64 symbols (the residual tokens).
 * Every symbol starts with count 1; a coded symbol gets +32; when the total
 * passes 2^16 all counts are halved (keeps at least 1), so the model also
 * follows changes inside the tile. This is exactly the model simulated in
 * stage0/stage0b.py.
 *
 * Raw bits (the "extra bits" of large residuals) are coded with a flat
 * distribution, up to 16 at a time.
 *
 * Why not rANS (decision D19 in DECISIONS.md): with adaptive tables the totals
 * are not powers of two, so rANS also needs a division per symbol, and it must
 * encode in reverse order (store every symbol of the tile first). The range
 * coder streams forward and keeps the same speed.
 */
#ifndef RC_H
#define RC_H

#include <stdint.h>
#include <string.h>

#include "container.h"

#define RC_TOP (1u << 24)
#define NSYM 64
#define MODEL_INC 32
#define MODEL_LIMIT (1u << 16)

/* ------------------------------------------------------------------ model */
typedef struct {
    uint32_t f[NSYM];
    uint32_t tot;
} Model;

static inline void model_init(Model *m)
{
    for (int i = 0; i < NSYM; i++)
        m->f[i] = 1;
    m->tot = NSYM;
}

static inline void model_update(Model *m, int s)
{
    m->f[s] += MODEL_INC;
    m->tot += MODEL_INC;
    if (m->tot > MODEL_LIMIT) {
        uint32_t t = 0;
        for (int i = 0; i < NSYM; i++) {
            m->f[i] = (m->f[i] + 1) >> 1;
            t += m->f[i];
        }
        m->tot = t;
    }
}

/* ------------------------------------------------------------------ encoder */
typedef struct {
    uint64_t low;
    uint32_t range;
    uint8_t cache;
    uint64_t cache_size;
    Buf *out;
} RcEnc;

static inline void rce_init(RcEnc *e, Buf *out)
{
    e->low = 0;
    e->range = 0xFFFFFFFFu;
    e->cache = 0;
    e->cache_size = 1;
    e->out = out;
}

static inline void rce_shift_low(RcEnc *e)
{
    if ((uint32_t)e->low < 0xFF000000u || (e->low >> 32) != 0) {
        uint8_t carry = (uint8_t)(e->low >> 32);
        uint8_t temp = e->cache;
        do {
            buf_u8(e->out, (uint8_t)(temp + carry));
            temp = 0xFF;
        } while (--e->cache_size != 0);
        e->cache = (uint8_t)(e->low >> 24);
    }
    e->cache_size++;
    e->low = (e->low & 0x00FFFFFFu) << 8;
}

static inline void rce_encode(RcEnc *e, uint32_t start, uint32_t size, uint32_t tot)
{
    uint32_t r = e->range / tot;
    e->low += (uint64_t)start * r;
    e->range = size * r;
    while (e->range < RC_TOP) {
        e->range <<= 8;
        rce_shift_low(e);
    }
}

/* n raw bits, n <= 16 */
static inline void rce_bits(RcEnc *e, uint32_t v, int n)
{
    if (n == 0)
        return;
    e->range >>= n;
    e->low += (uint64_t)v * e->range;
    while (e->range < RC_TOP) {
        e->range <<= 8;
        rce_shift_low(e);
    }
}

static inline void rce_symbol(RcEnc *e, Model *m, int s)
{
    uint32_t start = 0;
    for (int i = 0; i < s; i++)
        start += m->f[i];
    rce_encode(e, start, m->f[s], m->tot);
    model_update(m, s);
}

static inline void rce_flush(RcEnc *e)
{
    for (int i = 0; i < 5; i++)
        rce_shift_low(e);
}

/* ------------------------------------------------------------------ decoder */
typedef struct {
    uint32_t code, range;
    const uint8_t *p, *end;
    int overrun;           /* set if the decoder read past the end of its data */
} RcDec;

static inline uint8_t rcd_byte(RcDec *d)
{
    if (d->p < d->end)
        return *d->p++;
    d->overrun = 1;
    return 0;
}

static inline void rcd_init(RcDec *d, const uint8_t *p, size_t n)
{
    d->p = p;
    d->end = p + n;
    d->overrun = 0;
    d->code = 0;
    d->range = 0xFFFFFFFFu;
    for (int i = 0; i < 5; i++)
        d->code = (d->code << 8) | rcd_byte(d);
}

static inline void rcd_normalize(RcDec *d)
{
    while (d->range < RC_TOP) {
        d->code = (d->code << 8) | rcd_byte(d);
        d->range <<= 8;
    }
}

static inline uint32_t rcd_bits(RcDec *d, int n)
{
    if (n == 0)
        return 0;
    d->range >>= n;
    uint32_t v = d->code / d->range;
    if (v >> n)                      /* only possible with corrupt data */
        v = (1u << n) - 1;
    d->code -= v * d->range;
    rcd_normalize(d);
    return v;
}

/* Generic decoding with any frequency table (used by the palette models):
 * rcd_target gives the cumulative value to look up, rcd_consume removes the symbol
 * [start, start + size) once it is found. */
static inline uint32_t rcd_target(RcDec *d, uint32_t tot, uint32_t *r)
{
    *r = d->range / tot;
    uint32_t v = d->code / *r;
    return v < tot ? v : tot - 1;        /* v >= tot only with corrupt data */
}

static inline void rcd_consume(RcDec *d, uint32_t start, uint32_t size, uint32_t r)
{
    d->code -= start * r;
    d->range = size * r;
    rcd_normalize(d);
}

static inline int rcd_symbol(RcDec *d, Model *m)
{
    uint32_t r = d->range / m->tot;
    uint32_t v = d->code / r;
    if (v >= m->tot)                 /* only possible with corrupt data */
        v = m->tot - 1;
    int s = 0;
    uint32_t start = 0;
    while (start + m->f[s] <= v)
        start += m->f[s++];
    d->code -= start * r;
    d->range = m->f[s] * r;
    rcd_normalize(d);
    model_update(m, s);
    return s;
}

/* ------------------------------------------------------------------ residuals
 * A residual r is mapped to u = zigzag(r) (0,-1,1,-2,... -> 0,1,2,3,...), then
 *   u < 16 : token u, no extra bits
 *   u >= 16: n = floor(log2 u); token 16 + 2(n-4) + (bit n-1 of u); extra = the
 *            n-1 low bits of u, sent raw.
 * Supports |r| < 2^24 (token <= 57), far more than 16-bit images need. */
static inline void rce_residual(RcEnc *e, Model *m, int32_t r)
{
    uint32_t u = r >= 0 ? 2u * (uint32_t)r : 2u * (uint32_t)(-(r + 1)) + 1u;
    if (u < 16) {
        rce_symbol(e, m, (int)u);
        return;
    }
    int n = 31 - __builtin_clz(u);
    rce_symbol(e, m, 16 + 2 * (n - 4) + (int)((u >> (n - 1)) & 1));
    int eb = n - 1;
    uint32_t extra = u & ((1u << eb) - 1);
    if (eb > 16) {
        rce_bits(e, extra >> 16, eb - 16);
        eb = 16;
        extra &= 0xFFFF;
    }
    rce_bits(e, extra, eb);
}

static inline int32_t rcd_residual(RcDec *d, Model *m)
{
    int t = rcd_symbol(d, m);
    uint32_t u;
    if (t < 16) {
        u = (uint32_t)t;
    } else {
        int n = (t - 16) / 2 + 4;
        if (n > 25)                  /* only possible with corrupt data */
            n = 25;
        int eb = n - 1;
        uint32_t extra = 0;
        if (eb > 16) {
            extra = rcd_bits(d, eb - 16) << 16;
            eb = 16;
        }
        extra |= rcd_bits(d, eb);
        u = (1u << n) | ((uint32_t)((t - 16) & 1) << (n - 1)) | extra;
    }
    return (u & 1) ? -(int32_t)(u >> 1) - 1 : (int32_t)(u >> 1);
}

#endif
