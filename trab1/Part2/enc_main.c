/*
 * enc_main.c - command line of the encoder.
 *
 *   wav_lossless_enc [-b block_size] [-v] input.wav compressed_file
 *
 *   -b N   samples per block and channel (default 4096)
 *   -v     print statistics (sizes, ratio, predictor orders used)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "codec.h"

static int usage(void)
{
    fprintf(stderr,
            "usage: wav_lossless_enc [-b block_size] [-v] input.wav compressed_file\n"
            "  -b N   samples per block and channel (1..%u, default %u)\n"
            "  -v     print statistics\n",
            CODEC_MAX_BLOCK, CODEC_DEFAULT_BLOCK);
    return 2;
}

int main(int argc, char **argv)
{
    uint32_t block_size = CODEC_DEFAULT_BLOCK;
    int      verbose = 0;
    int      i = 1;

    /* Flags come first, then the two file names. */
    for (; i < argc && argv[i][0] == '-' && argv[i][1] != '\0'; i++) {
        if (strcmp(argv[i], "-v") == 0) {
            verbose = 1;
        } else if (strcmp(argv[i], "-b") == 0 && i + 1 < argc) {
            char *end;
            unsigned long v = strtoul(argv[++i], &end, 10);
            if (*end != '\0' || v < 1 || v > CODEC_MAX_BLOCK) return usage();
            block_size = (uint32_t)v;
        } else {
            return usage();
        }
    }
    if (argc - i != 2) return usage();

    return codec_encode(argv[i], argv[i + 1], block_size, verbose);
}
