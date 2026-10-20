/* rangecoder.c - the non-inline parts of the range coder (see rangecoder.h). */
#include "rangecoder.h"

static void put_byte(RcEnc *e, uint8_t b)
{
    e->buf[e->pos++] = b;
    if (e->pos == RC_BUFSZ) {
        if (fwrite(e->buf, 1, e->pos, e->f) != e->pos) e->err = 1;
        e->total_bytes += e->pos;
        e->pos = 0;
    }
}

void rc_enc_init(RcEnc *e, FILE *f)
{
    e->f = f;
    e->low = 0;
    e->range = 0xFFFFFFFFu;
    e->cache = 0;
    e->cache_size = 1;
    e->total_bytes = 0;
    e->err = 0;
    e->pos = 0;
}

/*
 * Bits 24..31 of low are the next output byte, bit 32 is a carry into the
 * bytes already pending. The pending bytes can be written once we know no
 * carry can reach them any more: either a carry just happened (it is added
 * now), or the top byte is below 0xFF (a future carry stops there).
 */
void rc_enc_shift_low(RcEnc *e)
{
    if ((uint32_t)e->low < 0xFF000000u || (e->low >> 32) != 0) {
        uint8_t carry = (uint8_t)(e->low >> 32);
        uint8_t b = e->cache;
        do {
            put_byte(e, (uint8_t)(b + carry));
            b = 0xFF;
        } while (--e->cache_size != 0);
        e->cache = (uint8_t)(e->low >> 24);
    }
    e->cache_size++;
    e->low = (e->low & 0x00FFFFFFu) << 8;
}

/*
 * Push all 32 bits of low out. After four shifts low is 0, so the fifth
 * writes every pending byte; only a final 0 stays in the cache, and the
 * decoder never reads it (it reads 5 bytes at the start instead of 4).
 */
int rc_enc_finish(RcEnc *e)
{
    for (int i = 0; i < 5; i++) rc_enc_shift_low(e);
    if (e->pos) {
        if (fwrite(e->buf, 1, e->pos, e->f) != e->pos) e->err = 1;
        e->total_bytes += e->pos;
        e->pos = 0;
    }
    return e->err;
}

void rc_dec_init(RcDec *d, const uint8_t *buf, size_t len)
{
    d->buf = buf;
    d->len = len;
    d->pos = 0;
    d->err = 0;
    d->range = 0xFFFFFFFFu;
    d->code = 0;
    /* The first byte is the encoder's initial cache (always 0); together
       with the next four it fills the 32-bit code. */
    for (int i = 0; i < 5; i++) d->code = (d->code << 8) | rc_dec_byte(d);
}

int rc_dec_finish(const RcDec *d)
{
    return d->err || d->pos != d->len;
}
