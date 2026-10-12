/*
 * bitio.h - buffered bit-level writer/reader (header-only).
 *
 * Bit order: MSB-first. The first bit written is the top bit of the first
 * byte. Writer pads the last byte with zero bits on flush.
 *
 * Both sides use a fixed 64 KiB buffer and a 64-bit accumulator, so memory
 * use is constant regardless of file size.
 *
 * Limits: bw_put / br_get handle 0..32 bits per call.
 */
#ifndef BITIO_H
#define BITIO_H

#include <stdint.h>
#include <stdio.h>

#define BITIO_BUFSZ (64 * 1024)

/* ------------------------------ writer ---------------------------------- */

typedef struct {
    FILE    *f;
    uint64_t acc;          /* pending bits live in the low `nbits` bits */
    int      nbits;        /* 0..7 between calls */
    size_t   pos;          /* bytes currently in buf */
    uint64_t total_bytes;  /* bytes handed to the FILE so far */
    int      err;          /* sticky error flag */
    uint8_t  buf[BITIO_BUFSZ];
} BitWriter;

static inline void bw_init(BitWriter *w, FILE *f)
{
    w->f = f; w->acc = 0; w->nbits = 0; w->pos = 0;
    w->total_bytes = 0; w->err = 0;
}

static inline void bw_drain(BitWriter *w)
{
    if (w->pos) {
        if (fwrite(w->buf, 1, w->pos, w->f) != w->pos) w->err = 1;
        w->total_bytes += w->pos;
        w->pos = 0;
    }
}

/* Write the low n bits of value (0 <= n <= 32), MSB first. */
static inline void bw_put(BitWriter *w, uint32_t value, int n)
{
    if (n == 0) return;
    uint64_t v = (n == 32) ? value : (value & ((1u << n) - 1u));
    w->acc = (w->acc << n) | v;      /* nbits<8 and n<=32 -> fits in 64 */
    w->nbits += n;
    while (w->nbits >= 8) {
        w->nbits -= 8;
        w->buf[w->pos++] = (uint8_t)(w->acc >> w->nbits);
        if (w->pos == BITIO_BUFSZ) bw_drain(w);
    }
    w->acc &= (1ull << w->nbits) - 1ull;  /* keep only the pending bits */
}

/* Unary code: q zeros followed by a single 1. Used for the Rice quotient. */
static inline void bw_put_unary(BitWriter *w, uint32_t q)
{
    while (q >= 31) { bw_put(w, 0, 31); q -= 31; }
    bw_put(w, 1, (int)q + 1);        /* q zeros then the terminating 1 */
}

/* Pad with zero bits to a byte boundary and flush to the FILE. */
static inline int bw_flush(BitWriter *w)
{
    if (w->nbits) bw_put(w, 0, 8 - w->nbits);
    bw_drain(w);
    if (fflush(w->f) != 0) w->err = 1;
    return w->err;
}

/* ------------------------------ reader ---------------------------------- */

typedef struct {
    FILE    *f;
    uint64_t acc;          /* valid bits are the low `nbits` bits */
    int      nbits;        /* 0..64 */
    size_t   pos, len;     /* read cursor and fill level of buf */
    int      err;          /* set when a read ran past end of data */
    uint8_t  buf[BITIO_BUFSZ];
} BitReader;

static inline void br_init(BitReader *r, FILE *f)
{
    r->f = f; r->acc = 0; r->nbits = 0; r->pos = 0; r->len = 0; r->err = 0;
}

static inline void br_refill(BitReader *r)
{
    while (r->nbits <= 56) {
        if (r->pos == r->len) {
            r->len = fread(r->buf, 1, BITIO_BUFSZ, r->f);
            r->pos = 0;
            if (r->len == 0) return;           /* EOF */
        }
        r->acc = (r->acc << 8) | r->buf[r->pos++];
        r->nbits += 8;
    }
}

/* Read n bits (0 <= n <= 32), MSB first. Sets r->err on premature EOF. */
static inline uint32_t br_get(BitReader *r, int n)
{
    if (n == 0) return 0;
    if (r->nbits < n) {
        br_refill(r);
        if (r->nbits < n) { r->err = 1; return 0; }
    }
    r->nbits -= n;
    return (uint32_t)((r->acc >> r->nbits) & ((1ull << n) - 1ull));
}

/* Read a unary code: count zeros up to and including the terminating 1. */
static inline uint32_t br_get_unary(BitReader *r)
{
    uint32_t zeros = 0;
    for (;;) {
        if (r->nbits == 0) {
            br_refill(r);
            if (r->nbits == 0) { r->err = 1; return 0; }
        }
        uint64_t mask = (r->nbits == 64) ? ~0ull : ((1ull << r->nbits) - 1ull);
        uint64_t w = r->acc & mask;
        if (w == 0) {                       /* all pending bits are zeros */
            zeros += (uint32_t)r->nbits;
            r->nbits = 0;
            continue;
        }
        int lz = __builtin_clzll(w) - (64 - r->nbits);
        zeros += (uint32_t)lz;
        r->nbits -= lz + 1;                 /* consume the zeros and the 1 */
        return zeros;
    }
}

#endif /* BITIO_H */