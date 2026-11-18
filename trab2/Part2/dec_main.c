/*
 * img_lossless_dec - lossless image decoder.
 *
 *   img_lossless_dec compressed output
 *
 * The output is byte-for-byte the original file. The CRC-32 of the original,
 * stored by the encoder, is checked; on a mismatch the output is removed.
 */
#include <stdio.h>
#include <stdlib.h>

#include "codec.h"

int main(int argc, char **argv)
{
    if (argc != 3) {
        fprintf(stderr, "usage: img_lossless_dec compressed output\n");
        return 2;
    }
    char err[ERRLEN];
    size_t n;
    uint8_t *data = read_all(argv[1], &n, err);
    if (!data) {
        fprintf(stderr, "%s\n", err);
        return 1;
    }
    Image im;
    uint32_t crc_expected, crc;
    int rc = decode_image(data, n, &im, &crc_expected, err);
    free(data);
    if (rc) {
        fprintf(stderr, "%s\n", err);
        return 1;
    }
    rc = img_write(argv[2], &im, &crc, err);
    img_free(&im);
    if (rc) {
        fprintf(stderr, "%s\n", err);
        return 1;
    }
    if (crc != crc_expected) {
        fprintf(stderr, "%s: CRC mismatch, the compressed file is damaged\n", argv[1]);
        remove(argv[2]);
        return 1;
    }
    return 0;
}
