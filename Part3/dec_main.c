/*
 * dec_main.c - command line of the lossy decoder.
 *
 *   wav_lossy_dec compressed_file output.wav
 */
#include <stdio.h>
#include "codec_lossy.h"

int main(int argc, char **argv)
{
    if (argc != 3) {
        fprintf(stderr, "usage: wav_lossy_dec compressed_file output.wav\n");
        return 2;
    }
    return lossy_decode(argv[1], argv[2]);
}
