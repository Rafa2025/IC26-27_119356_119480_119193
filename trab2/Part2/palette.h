/*
 * palette.h - palette mode for images with at most 256 colours (D8).
 *
 * The colours are stored once in the container header; every pixel becomes
 * the index of its colour (0..K-1). In each tile the indices are coded in raster
 * order with an adaptive model chosen by the index of the pixel on the left (W)
 * (first column: the pixel above; first pixel: index 0): K models of K symbols.
 * Each model starts with every count 1, adds PAL_INC (32) per coded symbol and halves
 * all counts when the total passes 2^16.
 */
#ifndef PALETTE_H
#define PALETTE_H

#include <stdint.h>

#include "imgfile.h"
#include "rc.h"

#define PAL_MAX 256

/* Find the colours of a colour image. Returns K (1..256) and fills pal (K x 3
 * samples, sorted) and idx (rows x cols indices), or returns 0 if the image has
 * more than 256 colours (idx is then undefined). */
int palette_build(const Image *im, uint16_t *pal, uint8_t *idx);

/* Encode (e != NULL) or decode (d != NULL) the h x w tile of indices idx.
 * Decoded indices >= K mean corrupt data. Returns 0, or -1 (memory / corrupt). */
int palette_code(uint8_t *idx, uint32_t h, uint32_t w, int K, RcEnc *e, RcDec *d);

#endif
