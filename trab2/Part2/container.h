/*
 * container.h - the ICL2 compressed file format (version 1).
 *
 * All numbers are little-endian.
 *
 *   offset  size  field
 *   0       4     magic "ICL2"
 *   4       1     version (1)
 *   5       1     type: 0 grayscale, 1 colour, 2 palette
 *   6       1     flags: bits 1-2 colour transform (0 none, 1 RCT, 2 YCoCg-R);
 *                 all other bits 0
 *   7       1     kind of the original file: 0 RAW, 1 PGM, 2 PPM
 *   8       1     bits per sample (8 or 16)
 *   9       1     byte order of 16-bit samples: 1 big-endian, 0 little-endian
 *   10      1     channels (1 or 3)
 *   11      1     reserved (0)
 *   12      4     rows
 *   16      4     cols
 *   20      4     maxval
 *   24      4     CRC-32 of the whole original file
 *   28      4     header length H, then H bytes: the original PNM header
 *   ...     4     trailer length T, then T bytes: bytes after the pixel data
 *   ...           palette images only: 4 bytes K (1..256), then K colours of
 *                 3 samples, 2 bytes each
 *   ...     4     tile size (tiles are tile x tile pixels, the last ones smaller)
 *   ...     4     number of tiles N
 *   ...     4N    compressed size of each tile, in raster order
 *   ...           the N tiles, one after the other
 *
 * Each tile starts with one byte, its method:
 *   0  stored: the samples of the tile, row by row (channels interleaved),
 *      1 byte each for 8 bpp, 2 bytes little-endian for 16 bpp
 *   1  predicted: range-coder bytes of the plane(s) coded by plane_code()
 *      (WGT prediction + 64 contexts, see plane.h); colour: the 3 transformed
 *      planes one after the other in the same stream; palette images: the
 *      colour indices coded by palette_code() (palette.h)
 * The encoder uses "stored" whenever coding would not make the tile smaller.
 */
#ifndef CONTAINER_H
#define CONTAINER_H

#include <stddef.h>
#include <stdint.h>

#include "imgfile.h"

#define ICL2_VERSION 1
enum { TYPE_GRAY = 0, TYPE_COLOUR = 1, TYPE_PALETTE = 2 };
enum { TILE_STORED = 0, TILE_PRED = 1 };

/* Growable byte buffer used for writing. */
typedef struct {
    uint8_t *p;
    size_t n, cap;
    int oom;                 /* set if an allocation failed */
} Buf;

void buf_put(Buf *b, const void *p, size_t n);
void buf_u32(Buf *b, uint32_t v);
void buf_free(Buf *b);

/* One byte: inline fast path (the range coder writes byte by byte). */
static inline void buf_u8(Buf *b, uint32_t v)
{
    if (b->n < b->cap) {
        b->p[b->n++] = (uint8_t)v;
        return;
    }
    uint8_t c = (uint8_t)v;
    buf_put(b, &c, 1);
}

/* Bounds-checked reader. Any read past the end sets bad and returns zeros. */
typedef struct {
    const uint8_t *p;
    size_t n, pos;
    int bad;
} Rd;

uint32_t rd_u8(Rd *r);
uint32_t rd_u32(Rd *r);
const uint8_t *rd_bytes(Rd *r, size_t n);

/* Everything in the container except the tile data. */
typedef struct {
    int type, flags;
    uint32_t crc;
    uint32_t tile;
    uint32_t ntiles;
    uint32_t *tile_size;     /* ntiles entries */
    int npal;                /* palette images: number of colours K */
    uint16_t pal[256 * 3];   /* palette images: the K colours (R, G, B) */
} Layout;

/* Number of tiles for an image and tile size. */
uint32_t tiles_count(uint32_t rows, uint32_t cols, uint32_t tile);

/* Write the container header (image description + layout). */
void icl2_write_header(Buf *b, const Image *im, const Layout *L);

/* Read the header: fills im (without pixels) and L. Returns 0 or -1 (err filled). */
int icl2_read_header(Rd *r, Image *im, Layout *L, char *err);

#endif
