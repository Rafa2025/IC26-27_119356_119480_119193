/*
 * dec_main.c - command line of the decoder.
 *
 *   wav_lossless_dec compressed_file output.wav
 */
#include <stdio.h>
#include "codec.h"

int main(int argc, char **argv)
{
    if (argc != 3) {
        fprintf(stderr, "usage: wav_lossless_dec compressed_file output.wav\n");
        return 2;
    }
    return codec_decode(argv[1], argv[2]);
}
