/*
 * img_lossless_enc - lossless image encoder.
 *
 *   img_lossless_enc [-t tile] [-c auto|best|none|rct|ycocg] [-nopal] input.pgm|input.ppm compressed
 *   img_lossless_enc [-t tile] -rows R -cols C [-bpp 8|16] [-be|-le] input.raw compressed
 *
 * A file is read as RAW when -rows/-cols are given, otherwise as PGM/PPM.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "codec.h"

static void usage(void)
{
    fprintf(stderr,
            "usage: img_lossless_enc [-t tile] [-c transform] [-nopal] input.pgm|input.ppm compressed\n"
            "       img_lossless_enc [-t tile] -rows R -cols C [-bpp 8|16] [-be|-le] input.raw compressed\n"
            "  -t tile       tile size in pixels (%d..%d, default %d)\n"
            "  -c transform  colour images: auto (default, fast estimate), best (try all,\n"
            "                3x slower), none, rct or ycocg\n"
            "  -nopal        do not use palette mode for images with <= 256 colours\n",
            TILE_MIN, TILE_MAX, TILE_DEFAULT);
    exit(2);
}

static long num(const char *s)
{
    char *end;
    long v = strtol(s, &end, 10);
    if (*s == '\0' || *end != '\0' || v <= 0)
        usage();
    return v;
}

int main(int argc, char **argv)
{
    RawParams raw = {0, 0, 8, 1};
    int raw_given = 0, endian_given = 0;
    long tile = TILE_DEFAULT;
    int ctf = TF_AUTO, use_palette = 1;
    const char *in = NULL, *out = NULL;

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "-t") && i + 1 < argc)
            tile = num(argv[++i]);
        else if (!strcmp(a, "-nopal"))
            use_palette = 0;
        else if (!strcmp(a, "-c") && i + 1 < argc) {
            const char *v = argv[++i];
            ctf = !strcmp(v, "auto") ? TF_AUTO : !strcmp(v, "best") ? TF_BEST : !strcmp(v, "none") ? TF_NONE
                : !strcmp(v, "rct") ? TF_RCT : !strcmp(v, "ycocg") ? TF_YCOCG : -99;
            if (ctf == -99)
                usage();
        }
        else if (!strcmp(a, "-rows") && i + 1 < argc)
            raw.rows = (uint32_t)num(argv[++i]), raw_given = 1;
        else if (!strcmp(a, "-cols") && i + 1 < argc)
            raw.cols = (uint32_t)num(argv[++i]), raw_given = 1;
        else if (!strcmp(a, "-bpp") && i + 1 < argc)
            raw.bpp = (int)num(argv[++i]), raw_given = 1;
        else if (!strcmp(a, "-be"))
            raw.big_endian = 1, raw_given = endian_given = 1;
        else if (!strcmp(a, "-le"))
            raw.big_endian = 0, raw_given = endian_given = 1;
        else if (a[0] == '-' && a[1] != '\0')
            usage();
        else if (!in)
            in = a;
        else if (!out)
            out = a;
        else
            usage();
    }
    if (!in || !out || tile < TILE_MIN || tile > TILE_MAX)
        usage();
    if (endian_given && raw.bpp != 16)
        fprintf(stderr, "warning: -be/-le only matter for 16 bpp RAW images\n");

    char err[ERRLEN];
    Image im;
    uint32_t crc;
    if (img_read(in, raw_given ? &raw : NULL, &im, &crc, err)) {
        fprintf(stderr, "%s\n", err);
        return 1;
    }
    Buf b = {0};
    int rc = encode_image(&im, crc, (uint32_t)tile, ctf, use_palette, &b, err);
    img_free(&im);
    if (rc) {
        fprintf(stderr, "%s\n", err);
        buf_free(&b);
        return 1;
    }
    FILE *f = fopen(out, "wb");
    int bad = !f || fwrite(b.p, 1, b.n, f) != b.n;
    if (f && fclose(f) != 0)
        bad = 1;
    buf_free(&b);
    if (bad) {
        fprintf(stderr, "%s: write error\n", out);
        remove(out);
        return 1;
    }
    return 0;
}
