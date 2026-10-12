/* wavfile.c - see wavfile.h for the overall idea. */
#include <string.h>
#include "wavfile.h"

/* Little-endian readers (WAV is always little-endian, whatever the CPU is). */
static uint32_t rd16(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8); }
static uint32_t rd32(const uint8_t *p) { return rd16(p) | (rd16(p + 2) << 16); }

/* ------------------------------------------------------------------ */
/* wav_parse: find the "fmt " and "data" chunks                        */
/* ------------------------------------------------------------------ */
/*
 * A WAV file is a RIFF container:
 *
 *   "RIFF" <size> "WAVE"  then a list of chunks, each:  <4-char id> <size32> <body>
 *
 * Chunk bodies are padded to an even number of bytes (the pad byte is not
 * counted in <size>). We only need two chunks:
 *   "fmt " -> channels, bits per sample, ...
 *   "data" -> the audio itself
 * Every other chunk (LIST, fact, ...) is skipped; because it lies in the
 * prefix or suffix it is preserved verbatim anyway.
 */
int wav_parse(const uint8_t *file, uint64_t len, WavInfo *info)
{
    if (len < 12 || memcmp(file, "RIFF", 4) != 0 || memcmp(file + 8, "WAVE", 4) != 0)
        return 0;

    int      have_fmt = 0;
    uint32_t tag = 0, channels = 0, bits = 0, align = 0, rate = 0;
    uint64_t pos = 12;                         /* first chunk starts after "WAVE" */

    while (pos + 8 <= len) {
        const uint8_t *hdr  = file + pos;
        uint64_t       size = rd32(hdr + 4);   /* chunk body size */
        uint64_t       body = pos + 8;         /* offset of chunk body */

        if (memcmp(hdr, "fmt ", 4) == 0) {
            if (size < 16 || body + 16 > len) return 0;
            tag      = rd16(file + body);       /* 1 = PCM, 0xFFFE = extensible */
            channels = rd16(file + body + 2);
            rate     = rd32(file + body + 4);
            align    = rd16(file + body + 12);  /* bytes per frame */
            bits     = rd16(file + body + 14);
            have_fmt = 1;
        } else if (memcmp(hdr, "data", 4) == 0) {
            if (!have_fmt) return 0;                       /* fmt must come first */
            if (tag != 1 && tag != 0xFFFE) return 0;       /* not integer PCM */
            if (bits != 8 && bits != 16 && bits != 24) return 0;
            if (channels != 1 && channels != 2) return 0;
            if (align != channels * bits / 8) return 0;    /* inconsistent header */

            /* Streamed or truncated files may declare more data than exists:
               trust the real file length over the declared size. */
            uint64_t avail = len - body;
            if (size > avail) size = avail;

            info->channels = (int)channels;
            info->bits     = (int)bits;
            info->rate     = rate;
            info->data_off = body;
            info->nframes  = size / align;     /* a partial last frame goes to the suffix */
            return 1;
        }
        pos = body + size + (size & 1);        /* skip body and the even-size pad byte */
    }
    return 0;                                  /* no data chunk found */
}

/* ------------------------------------------------------------------ */
/* wav_deinterleave: bytes -> one int32 array per channel              */
/* ------------------------------------------------------------------ */
void wav_deinterleave(const uint8_t *file, const WavInfo *info, int32_t **chan)
{
    const uint8_t *p  = file + info->data_off;
    const uint64_t n  = info->nframes;
    const int      nc = info->channels;

    for (uint64_t i = 0; i < n; i++) {
        for (int c = 0; c < nc; c++) {
            int32_t v;
            switch (info->bits) {
            case 8:                              /* unsigned 0..255 -> signed -128..127 */
                v = (int32_t)p[0] - 128;
                p += 1;
                break;
            case 16:                             /* 2 bytes little-endian, signed */
                v = (int16_t)rd16(p);
                p += 2;
                break;
            default: {                           /* 24: 3 bytes little-endian, signed */
                uint32_t u = (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16);
                if (u & 0x800000u) u |= 0xFF000000u;   /* sign-extend bit 23 */
                v = (int32_t)u;
                p += 3;
                break;
            }
            }
            chan[c][i] = v;
        }
    }
}

/* ------------------------------------------------------------------ */
/* wav_rebuild: the inverse of parse + deinterleave                    */
/* ------------------------------------------------------------------ */
#define OUTBUF 65536

int wav_rebuild(FILE *out,
                const uint8_t *prefix, uint64_t prefix_len,
                int32_t *const *chan, int channels, int bits, uint64_t nframes,
                const uint8_t *suffix, uint64_t suffix_len)
{
    /* 1) everything before the audio, byte for byte */
    if (prefix_len && fwrite(prefix, 1, prefix_len, out) != prefix_len) return 1;

    /* 2) the audio: pack samples into a buffer and write it out in chunks */
    uint8_t  buf[OUTBUF];
    size_t   used = 0;
    const int bytes = bits / 8;

    for (uint64_t i = 0; i < nframes; i++) {
        /* flush early so one whole frame (max 2 ch * 3 bytes) always fits */
        if (used + 8 > OUTBUF) {
            if (fwrite(buf, 1, used, out) != used) return 1;
            used = 0;
        }
        for (int c = 0; c < channels; c++) {
            int32_t v = chan[c][i];
            if (bits == 8) {
                buf[used++] = (uint8_t)(v + 128);          /* back to unsigned */
            } else {
                uint32_t u = (uint32_t)v;
                for (int b = 0; b < bytes; b++)            /* little-endian */
                    buf[used++] = (uint8_t)(u >> (8 * b));
            }
        }
    }
    if (used && fwrite(buf, 1, used, out) != used) return 1;

    /* 3) everything after the audio, byte for byte */
    if (suffix_len && fwrite(suffix, 1, suffix_len, out) != suffix_len) return 1;
    return 0;
}
