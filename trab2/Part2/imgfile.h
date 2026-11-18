/*
 * imgfile.h - exact reading and writing of RAW, PGM (P5) and PPM (P6) images.
 *
 * "Exact" means the decoder can rebuild the original file byte for byte:
 *   - the PNM header (magic, comments, whitespace, maxval) is kept as raw bytes;
 *   - any bytes after the pixel data are kept as raw bytes;
 *   - samples are kept as numbers (uint16_t), plus the byte order they had.
 */
#ifndef IMGFILE_H
#define IMGFILE_H

#include <stddef.h>
#include <stdint.h>

enum { KIND_RAW = 0, KIND_PGM = 1, KIND_PPM = 2 };

typedef struct {
    int kind;              /* KIND_RAW, KIND_PGM or KIND_PPM */
    uint32_t rows, cols;
    int channels;          /* 1 (RAW, PGM) or 3 (PPM, interleaved R,G,B) */
    int bpp;               /* bits per stored sample: 8 or 16 */
    int big_endian;        /* byte order of 16-bit samples in the file (PNM: always 1) */
    uint32_t maxval;       /* PNM: from the header; RAW: 2^bpp - 1 */
    uint8_t *header;       /* bytes before the pixel data (PNM only) */
    size_t header_len;
    uint8_t *trailer;      /* bytes after the pixel data (usually none) */
    size_t trailer_len;
    uint16_t *pix;         /* rows * cols * channels samples, row by row */
} Image;

/* Parameters a RAW file needs (it has no header). */
typedef struct {
    uint32_t rows, cols;   /* 0 = not given */
    int bpp;               /* 8 or 16 */
    int big_endian;        /* 16 bpp only */
} RawParams;

#define ERRLEN 256

/* Read an image. raw == NULL means "PGM/PPM"; otherwise the file is read as RAW.
 * crc (may be NULL) receives the CRC-32 of the whole file. Returns 0 or -1 (err filled). */
int img_read(const char *path, const RawParams *raw, Image *im, uint32_t *crc, char *err);

/* Write the image exactly as it was read. crc (may be NULL) receives the CRC-32 of
 * the bytes written. Returns 0 or -1 (err filled). */
int img_write(const char *path, const Image *im, uint32_t *crc, char *err);

void img_free(Image *im);

/* Number of samples and bytes of pixel data in the file. */
uint64_t img_samples(const Image *im);
uint64_t img_pixel_bytes(const Image *im);

uint32_t crc32_update(uint32_t crc, const uint8_t *p, size_t n);

/* Read a whole file into memory (caller frees). Returns NULL on error (err filled). */
uint8_t *read_all(const char *path, size_t *len, char *err);

#endif
