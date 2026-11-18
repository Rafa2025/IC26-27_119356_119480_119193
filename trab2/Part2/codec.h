/*
 * codec.h - lossless image codec: image <-> ICL2 container.
 *
 * The image is cut into tiles of tile x tile pixels, coded independently
 * (in parallel with OpenMP): grayscale and colour planes with WGT prediction +
 * 64 contexts (plane.h); colour images first get a colour transform; colour
 * images with at most 256 colours use palette mode (palette.h).
 */
#ifndef CODEC_H
#define CODEC_H

#include "container.h"
#include "imgfile.h"

#define TILE_DEFAULT 512
#define TILE_MIN 16
#define TILE_MAX 16384

/* Colour transforms (header flag bits 1-2), and the encoder's ways to choose one */
enum { TF_NONE = 0, TF_RCT = 1, TF_YCOCG = 2, TF_AUTO = -1, TF_BEST = -2 };

/* Encode im (read with crc = CRC-32 of the original file) into out. ctf: a TF_* value
 * (TF_AUTO: fast estimate; TF_BEST: try all three). use_palette: colour images with
 * at most 256 colours are coded in palette mode. 0 or -1. */
int encode_image(const Image *im, uint32_t crc, uint32_t tile, int ctf, int use_palette, Buf *out,
                 char *err);

/* Decode a whole ICL2 file held in memory. Fills im (caller frees) and the CRC-32
 * the original file had. 0 or -1. */
int decode_image(const uint8_t *data, size_t n, Image *im, uint32_t *crc, char *err);

#endif
