/*
 * jls_tool - minimal command-line JPEG-LS encoder/decoder on top of CharLS, so that
 * JPEG-LS is timed as a program, like the other codecs in the benchmark.
 *
 *   jls_tool enc [-rows R -cols C [-bpp 16] [-be|-le]] input compressed.jls
 *   jls_tool dec compressed.jls output.pnm
 *
 * Input is read with our own imgfile.c (RAW/PGM/PPM). Standard JPEG-LS (no colour
 * transform), colour images sample-interleaved. The decoder writes a PGM/PPM with
 * maxval 2^bits - 1 (the pixels, not the original file bytes).
 *
 * Build (needs the CharLS library, e.g. libcharls-dev / charls-devel):
 *   cc -O2 -o jls_tool jls_tool.c ../imgfile.c -lcharls
 */
#include <charls/charls.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../imgfile.h"

static int die(const char *msg)
{
    fprintf(stderr, "jls_tool: %s\n", msg);
    return 1;
}

static int encode(int argc, char **argv)
{
    RawParams raw = {0, 0, 8, 1};
    int israw = 0;
    const char *in = NULL, *out = NULL;
    for (int i = 0; i < argc; i++) {
        if (!strcmp(argv[i], "-rows") && i + 1 < argc) raw.rows = (uint32_t)atol(argv[++i]), israw = 1;
        else if (!strcmp(argv[i], "-cols") && i + 1 < argc) raw.cols = (uint32_t)atol(argv[++i]), israw = 1;
        else if (!strcmp(argv[i], "-bpp") && i + 1 < argc) raw.bpp = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-be")) raw.big_endian = 1;
        else if (!strcmp(argv[i], "-le")) raw.big_endian = 0;
        else if (!in) in = argv[i];
        else out = argv[i];
    }
    if (!in || !out)
        return die("usage: jls_tool enc [raw options] input output.jls");
    Image im;
    char err[ERRLEN];
    if (img_read(in, israw ? &raw : NULL, &im, NULL, err))
        return die(err);
    int bits = im.kind == KIND_RAW ? im.bpp : 32 - __builtin_clz(im.maxval);
    if (bits < 2)
        bits = 2;
    size_t ns = img_samples(&im), bytes = im.bpp == 8 ? ns : 2 * ns;
    void *src = malloc(bytes);
    if (im.bpp == 8)
        for (size_t i = 0; i < ns; i++) ((uint8_t *)src)[i] = (uint8_t)im.pix[i];
    else
        memcpy(src, im.pix, bytes);                         /* native byte order */
    charls_frame_info fi = {im.cols, im.rows, bits, im.channels};
    charls_jpegls_encoder *enc = charls_jpegls_encoder_create();
    size_t cap = 0;
    if (charls_jpegls_encoder_set_frame_info(enc, &fi) ||
        (im.channels == 3 && charls_jpegls_encoder_set_interleave_mode(enc, CHARLS_INTERLEAVE_MODE_SAMPLE)) ||
        charls_jpegls_encoder_get_estimated_destination_size(enc, &cap))
        return die("cannot set up the encoder");
    void *dst = malloc(cap);
    size_t written = 0;
    if (charls_jpegls_encoder_set_destination_buffer(enc, dst, cap) ||
        charls_jpegls_encoder_encode_from_buffer(enc, src, bytes, 0) ||
        charls_jpegls_encoder_get_bytes_written(enc, &written))
        return die("encoding failed");
    FILE *f = fopen(out, "wb");
    if (!f || fwrite(dst, 1, written, f) != written || fclose(f))
        return die("write error");
    charls_jpegls_encoder_destroy(enc);
    free(src);
    free(dst);
    img_free(&im);
    return 0;
}

static int decode(const char *in, const char *out)
{
    size_t n;
    char err[ERRLEN];
    uint8_t *data = read_all(in, &n, err);
    if (!data)
        return die(err);
    charls_jpegls_decoder *dec = charls_jpegls_decoder_create();
    charls_frame_info fi;
    size_t size = 0;
    if (charls_jpegls_decoder_set_source_buffer(dec, data, n) || charls_jpegls_decoder_read_header(dec) ||
        charls_jpegls_decoder_get_frame_info(dec, &fi) ||
        charls_jpegls_decoder_get_destination_size(dec, 0, &size))
        return die("bad JPEG-LS file");
    uint8_t *pix = malloc(size);
    if (charls_jpegls_decoder_decode_to_buffer(dec, pix, size, 0))
        return die("decoding failed");
    FILE *f = fopen(out, "wb");
    if (!f)
        return die("cannot create output");
    fprintf(f, "P%c\n%u %u\n%u\n", fi.component_count == 3 ? '6' : '5', fi.width, fi.height,
            (1u << fi.bits_per_sample) - 1);
    if (fi.bits_per_sample > 8)                              /* PNM is big-endian */
        for (size_t i = 0; i + 1 < size; i += 2) {
            uint8_t t = pix[i];
            pix[i] = pix[i + 1];
            pix[i + 1] = t;
        }
    if (fwrite(pix, 1, size, f) != size || fclose(f))
        return die("write error");
    charls_jpegls_decoder_destroy(dec);
    free(pix);
    free(data);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc >= 2 && !strcmp(argv[1], "enc"))
        return encode(argc - 2, argv + 2);
    if (argc == 4 && !strcmp(argv[1], "dec"))
        return decode(argv[2], argv[3]);
    return die("usage: jls_tool enc [raw options] input output.jls | jls_tool dec input.jls output");
}
