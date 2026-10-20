/*
 * rangecoder.h - adaptive binary range coder (LZMA style).
 *
 * Codes one bit at a time. Each bit is coded with a probability model `Prob`:
 * an 11-bit estimate of P(bit = 0). After each bit the model moves towards
 * what was seen by 1/2^PROB_SHIFT of the distance, so it learns the
 * statistics of the data as it goes. Bits with no useful model (signs, the
 * low bits of large numbers) are coded with a fixed probability 1/2
 * ("direct" bits), which costs exactly one bit each.
 *
 * ENCODER STATE
 *   low    the start of the current interval, 33 bits (bit 32 = carry)
 *   range  its width, 32 bits, kept >= 2^24 by shifting out bytes
 * A byte that leaves `low` can still change later if a carry arrives, so the
 * last such byte is held in `cache`, and a run of 0xFF bytes after it is only
 * counted (`cache_size`): a carry turns cache+1 and the run into 00 bytes.
 *
 * FILE LAYOUT
 *   The first byte written is always 0 (the initial cache). The decoder reads
 *   exactly as many bytes as the encoder writes, so a truncated stream always
 *   makes the decoder read past the end, which sets `err`.
 *
 * This file knows nothing about audio.
 */
#ifndef RANGECODER_H
#define RANGECODER_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#ifndef PROB_SHIFT
#define PROB_SHIFT 5                 /* adaptation speed (compile-time variant) */
#endif

#define RC_PROB_BITS 11
#define RC_PROB_ONE  (1u << RC_PROB_BITS)
#define RC_PROB_INIT (RC_PROB_ONE / 2)  /* P(0) = 1/2 before anything is seen */
#define RC_TOP       (1u << 24)          /* renormalise when range drops below */
#define RC_BUFSZ     (64 * 1024)

typedef uint16_t Prob;

/* Set n models to P(0) = 1/2. */
static inline void rc_prob_init(Prob *p, size_t n)
{
    for (size_t i = 0; i < n; i++) p[i] = RC_PROB_INIT;
}

/* ------------------------------ encoder --------------------------------- */

typedef struct {
    FILE    *f;
    uint64_t low;
    uint32_t range;
    uint8_t  cache;
    uint64_t cache_size;   /* cache byte + pending 0xFF bytes not yet written */
    uint64_t total_bytes;  /* bytes handed to the FILE so far */
    int      err;          /* sticky write error */
    size_t   pos;
    uint8_t  buf[RC_BUFSZ];
} RcEnc;

void rc_enc_init(RcEnc *e, FILE *f);
void rc_enc_shift_low(RcEnc *e);  /* move the top byte of low out (internal) */
int  rc_enc_finish(RcEnc *e);     /* flush; returns 0 on success */

/* Code `bit` with probability p (out of RC_PROB_ONE) for 0. No adaptation. */
static inline void rc_enc_bit_fixed(RcEnc *e, uint32_t p, int bit)
{
    uint32_t bound = (e->range >> RC_PROB_BITS) * p;
    if (!bit) {
        e->range = bound;
    } else {
        e->low  += bound;
        e->range -= bound;
    }
    while (e->range < RC_TOP) {
        e->range <<= 8;
        rc_enc_shift_low(e);
    }
}

/* Code `bit` with model *p, then adapt the model. */
static inline void rc_enc_bit(RcEnc *e, Prob *p, int bit)
{
    rc_enc_bit_fixed(e, *p, bit);
    if (!bit) *p += (Prob)((RC_PROB_ONE - *p) >> PROB_SHIFT);
    else      *p -= (Prob)(*p >> PROB_SHIFT);
}

/* Code the low n bits of v (n <= 32) with probability 1/2 each, MSB first. */
static inline void rc_enc_direct(RcEnc *e, uint32_t v, int n)
{
    while (n-- > 0) {
        e->range >>= 1;
        if ((v >> n) & 1) e->low += e->range;
        while (e->range < RC_TOP) {
            e->range <<= 8;
            rc_enc_shift_low(e);
        }
    }
}

/* Total compressed size in bytes; exact only after rc_enc_finish. */
static inline uint64_t rc_enc_bytes(const RcEnc *e) { return e->total_bytes + e->pos; }

/* ------------------------------ decoder --------------------------------- */

typedef struct {
    const uint8_t *buf;
    size_t         len, pos;
    uint32_t       range, code;
    int            err;     /* set when reading past the end; sticky */
} RcDec;

void rc_dec_init(RcDec *d, const uint8_t *buf, size_t len);

/* 0 if the whole stream was read without error and nothing is left over. */
int rc_dec_finish(const RcDec *d);

static inline uint8_t rc_dec_byte(RcDec *d)
{
    if (d->pos < d->len) return d->buf[d->pos++];
    d->err = 1;
    return 0;
}

static inline int rc_dec_bit_fixed(RcDec *d, uint32_t p)
{
    uint32_t bound = (d->range >> RC_PROB_BITS) * p;
    int bit;
    if (d->code < bound) {
        d->range = bound;
        bit = 0;
    } else {
        d->code  -= bound;
        d->range -= bound;
        bit = 1;
    }
    while (d->range < RC_TOP) {
        d->range <<= 8;
        d->code = (d->code << 8) | rc_dec_byte(d);
    }
    return bit;
}

static inline int rc_dec_bit(RcDec *d, Prob *p)
{
    int bit = rc_dec_bit_fixed(d, *p);
    if (!bit) *p += (Prob)((RC_PROB_ONE - *p) >> PROB_SHIFT);
    else      *p -= (Prob)(*p >> PROB_SHIFT);
    return bit;
}

static inline uint32_t rc_dec_direct(RcDec *d, int n)
{
    uint32_t v = 0;
    while (n-- > 0) {
        d->range >>= 1;
        uint32_t bit = d->code >= d->range;
        if (bit) d->code -= d->range;
        v = (v << 1) | bit;
        while (d->range < RC_TOP) {
            d->range <<= 8;
            d->code = (d->code << 8) | rc_dec_byte(d);
        }
    }
    return v;
}

#endif /* RANGECODER_H */
