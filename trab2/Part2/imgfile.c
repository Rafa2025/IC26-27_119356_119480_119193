/*
 * imgfile.c - exact reading and writing of RAW, PGM (P5) and PPM (P6) images.
 * See imgfile.h.
 */
#include "imgfile.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_SAMPLES (1ULL << 31)   /* sanity limit: 2^31 samples (4 GiB as 16-bit) */

/* ------------------------------------------------------------------ CRC-32 */
static uint32_t crc_table[256];
static int crc_ready = 0;

static void crc_init(void)
{
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int k = 0; k < 8; k++)
            c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        crc_table[i] = c;
    }
    crc_ready = 1;
}

/* Standard CRC-32 (same as zlib): start with crc = 0, feed the data in pieces. */
uint32_t crc32_update(uint32_t crc, const uint8_t *p, size_t n)
{
    if (!crc_ready)
        crc_init();
    crc = ~crc;
    while (n--)
        crc = crc_table[(crc ^ *p++) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

/* ------------------------------------------------------------------ helpers */
uint64_t img_samples(const Image *im)
{
    return (uint64_t)im->rows * im->cols * (uint64_t)im->channels;
}

uint64_t img_pixel_bytes(const Image *im)
{
    return img_samples(im) * (uint64_t)(im->bpp / 8);
}

void img_free(Image *im)
{
    free(im->header);
    free(im->trailer);
    free(im->pix);
    memset(im, 0, sizeof *im);
}

static int fail(char *err, const char *msg, const char *path)
{
    snprintf(err, ERRLEN, "%s: %s", path, msg);
    return -1;
}

uint8_t *read_all(const char *path, size_t *len, char *err)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        fail(err, "cannot open file", path);
        return NULL;
    }
    size_t cap = 1 << 20, n = 0;
    uint8_t *buf = malloc(cap);
    while (buf) {
        n += fread(buf + n, 1, cap - n, f);
        if (n < cap)
            break;
        uint8_t *nb = realloc(buf, cap * 2);
        if (!nb) {
            free(buf);
            buf = NULL;
            break;
        }
        buf = nb;
        cap *= 2;
    }
    int bad = ferror(f);
    fclose(f);
    if (!buf || bad) {
        free(buf);
        fail(err, bad ? "read error" : "out of memory", path);
        return NULL;
    }
    *len = n;
    return buf;
}

static int is_space(uint8_t c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f';
}

/* Read one decimal number of a PNM header, skipping whitespace and # comments. */
static int pnm_number(const uint8_t *b, size_t len, size_t *pos, uint32_t *out)
{
    size_t p = *pos;
    for (;;) {
        while (p < len && is_space(b[p]))
            p++;
        if (p < len && b[p] == '#') {
            while (p < len && b[p] != '\n' && b[p] != '\r')
                p++;
            continue;
        }
        break;
    }
    if (p >= len || b[p] < '0' || b[p] > '9')
        return -1;
    uint64_t v = 0;
    while (p < len && b[p] >= '0' && b[p] <= '9') {
        v = v * 10 + (uint64_t)(b[p] - '0');
        if (v > 0xFFFFFFFFu)
            return -1;
        p++;
    }
    *pos = p;
    *out = (uint32_t)v;
    return 0;
}

/* ------------------------------------------------------------------ read */
int img_read(const char *path, const RawParams *raw, Image *im, uint32_t *crc, char *err)
{
    memset(im, 0, sizeof *im);
    size_t len;
    uint8_t *buf = read_all(path, &len, err);
    if (!buf)
        return -1;
    if (crc)
        *crc = crc32_update(0, buf, len);

    size_t off;                                     /* where the pixel data starts */
    if (raw) {
        if (raw->rows == 0 || raw->cols == 0) {
            free(buf);
            return fail(err, "RAW image needs -rows and -cols (both > 0)", path);
        }
        if (raw->bpp != 8 && raw->bpp != 16) {
            free(buf);
            return fail(err, "RAW image: -bpp must be 8 or 16", path);
        }
        im->kind = KIND_RAW;
        im->rows = raw->rows;
        im->cols = raw->cols;
        im->channels = 1;
        im->bpp = raw->bpp;
        im->big_endian = raw->bpp == 16 ? raw->big_endian : 1;
        im->maxval = (1u << raw->bpp) - 1;
        off = 0;
    } else {
        if (len < 2 || buf[0] != 'P' || (buf[1] != '5' && buf[1] != '6')) {
            int other_pnm = len >= 2 && buf[0] == 'P' && buf[1] >= '1' && buf[1] <= '7';
            free(buf);
            if (other_pnm)
                return fail(err, "only binary PGM (P5) and PPM (P6) are supported", path);
            return fail(err, "not a PGM/PPM file (for RAW give -rows and -cols)", path);
        }
        size_t p = 2;
        uint32_t w, h, mv;
        if (pnm_number(buf, len, &p, &w) || pnm_number(buf, len, &p, &h) ||
            pnm_number(buf, len, &p, &mv) || p >= len || !is_space(buf[p])) {
            free(buf);
            return fail(err, "bad PGM/PPM header", path);
        }
        if (w == 0 || h == 0 || mv == 0 || mv > 65535) {
            free(buf);
            return fail(err, "PGM/PPM header: size must be > 0 and maxval in 1..65535", path);
        }
        off = p + 1;                                /* exactly one whitespace after maxval */
        im->kind = buf[1] == '5' ? KIND_PGM : KIND_PPM;
        im->rows = h;
        im->cols = w;
        im->channels = buf[1] == '5' ? 1 : 3;
        im->bpp = mv < 256 ? 8 : 16;
        im->big_endian = 1;
        im->maxval = mv;
        im->header_len = off;
        im->header = malloc(off);
        if (!im->header) {
            free(buf);
            return fail(err, "out of memory", path);
        }
        memcpy(im->header, buf, off);
    }

    uint64_t ns = img_samples(im), nbytes = img_pixel_bytes(im);
    if (ns > MAX_SAMPLES) {
        free(buf);
        img_free(im);
        return fail(err, "image too large", path);
    }
    if ((uint64_t)(len - off) < nbytes) {
        free(buf);
        img_free(im);
        return fail(err, "file is shorter than rows x cols x bytes per pixel", path);
    }
    im->pix = malloc(ns * sizeof(uint16_t));
    im->trailer_len = len - off - (size_t)nbytes;
    im->trailer = malloc(im->trailer_len ? im->trailer_len : 1);
    if (!im->pix || !im->trailer) {
        free(buf);
        img_free(im);
        return fail(err, "out of memory", path);
    }
    const uint8_t *d = buf + off;
    if (im->bpp == 8) {
        for (uint64_t i = 0; i < ns; i++)
            im->pix[i] = d[i];
    } else if (im->big_endian) {
        for (uint64_t i = 0; i < ns; i++)
            im->pix[i] = (uint16_t)(d[2 * i] << 8 | d[2 * i + 1]);
    } else {
        for (uint64_t i = 0; i < ns; i++)
            im->pix[i] = (uint16_t)(d[2 * i + 1] << 8 | d[2 * i]);
    }
    memcpy(im->trailer, d + nbytes, im->trailer_len);
    free(buf);
    return 0;
}

/* ------------------------------------------------------------------ write */
static int put(FILE *f, const uint8_t *p, size_t n, uint32_t *crc)
{
    if (crc)
        *crc = crc32_update(*crc, p, n);
    return fwrite(p, 1, n, f) == n ? 0 : -1;
}

int img_write(const char *path, const Image *im, uint32_t *crc, char *err)
{
    FILE *f = fopen(path, "wb");
    if (!f)
        return fail(err, "cannot create file", path);
    if (crc)
        *crc = 0;
    int bad = put(f, im->header, im->header_len, crc);

    enum { CHUNK = 1 << 16 };
    uint8_t out[2 * CHUNK];
    uint64_t ns = img_samples(im);
    for (uint64_t i = 0; i < ns && !bad; i += CHUNK) {
        size_t n = (size_t)(ns - i < CHUNK ? ns - i : CHUNK), k = 0;
        const uint16_t *s = im->pix + i;
        if (im->bpp == 8) {
            for (size_t j = 0; j < n; j++)
                out[k++] = (uint8_t)s[j];
        } else if (im->big_endian) {
            for (size_t j = 0; j < n; j++) {
                out[k++] = (uint8_t)(s[j] >> 8);
                out[k++] = (uint8_t)s[j];
            }
        } else {
            for (size_t j = 0; j < n; j++) {
                out[k++] = (uint8_t)s[j];
                out[k++] = (uint8_t)(s[j] >> 8);
            }
        }
        bad = put(f, out, k, crc);
    }
    if (!bad)
        bad = put(f, im->trailer, im->trailer_len, crc);
    if (fclose(f) != 0)
        bad = 1;
    if (bad) {
        remove(path);
        return fail(err, "write error", path);
    }
    return 0;
}
