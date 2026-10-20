/*
 * enc_main.c - command line of the lossy encoder.
 *
 *   wav_lossy_enc [options] input.wav compressed_file
 *
 *   -b N                 transform size, power of two 64..8192 (default 2048)
 *   --transform dct|mdct (default mdct)
 *   --stereo lr|ms       (default ms; ignored for mono)
 *   --step S             fixed quantization step
 *   --snr DB             target SNR in dB (default 30); excludes --step
 *   -v                   print step, SNR, bits per sample, kbps
 *
 * Not in the help text (experiments only):
 *   --round R            quantizer rounding offset, 0.3..0.5 (default 0.5)
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "codec_lossy.h"
#include "quant.h"
#include "transform.h"

static int usage(void)
{
    fprintf(stderr,
            "usage: wav_lossy_enc [options] input.wav compressed_file\n"
            "  -b N                 transform size: power of two, %d..%d (default %d)\n"
            "  --transform dct|mdct block DCT-II or MDCT with 50%% overlap (default mdct)\n"
            "  --stereo lr|ms       left/right or mid/side (default ms; ignored for mono)\n"
            "  --step S             fixed quantization step\n"
            "  --snr DB             target SNR in dB (default %g); excludes --step\n"
            "  -v                   print step, SNR, bits per sample, kbps\n",
            LOSSY_MIN_N, LOSSY_MAX_N, LOSSY_DEFAULT_N, LOSSY_DEFAULT_SNR);
    return 2;
}

/* Parse a finite real number; 0 on success. */
static int parse_real(const char *s, double *v)
{
    char *end;
    *v = strtod(s, &end);
    return *end != '\0' || end == s || !isfinite(*v);
}

int main(int argc, char **argv)
{
    LossyParams p = {
        .n = LOSSY_DEFAULT_N, .transform = TR_MDCT, .ms = 1,
        .use_step = 0, .step = 0, .target_snr = LOSSY_DEFAULT_SNR,
        .round = QUANT_ROUND_DEFAULT, .verbose = 0,
    };
    int have_step = 0, have_snr = 0;
    int i = 1;

    /* Options come first, then the two file names. */
    for (; i < argc && argv[i][0] == '-' && argv[i][1] != '\0'; i++) {
        const char *o = argv[i];
        const char *arg = i + 1 < argc ? argv[i + 1] : NULL;
        double v;

        if (strcmp(o, "-v") == 0) {
            p.verbose = 1;
            continue;
        }
        if (strcmp(o, "-h") == 0 || strcmp(o, "--help") == 0) {
            usage();
            return 0;
        }
        if (!arg) return usage();                  /* every other option has a value */
        i++;

        if (strcmp(o, "-b") == 0) {
            char *end;
            long n = strtol(arg, &end, 10);
            if (*end || n < LOSSY_MIN_N || n > LOSSY_MAX_N || (n & (n - 1))) {
                fprintf(stderr, "-b: must be a power of two between %d and %d\n",
                        LOSSY_MIN_N, LOSSY_MAX_N);
                return 2;
            }
            p.n = (int)n;
        } else if (strcmp(o, "--transform") == 0) {
            if      (strcmp(arg, "dct") == 0)  p.transform = TR_DCT;
            else if (strcmp(arg, "mdct") == 0) p.transform = TR_MDCT;
            else return usage();
        } else if (strcmp(o, "--stereo") == 0) {
            if      (strcmp(arg, "lr") == 0) p.ms = 0;
            else if (strcmp(arg, "ms") == 0) p.ms = 1;
            else return usage();
        } else if (strcmp(o, "--step") == 0) {
            /* The file stores a float32: the step must fit in one. */
            if (parse_real(arg, &v) || !(v > 0) || v > 1e30) {
                fprintf(stderr, "--step: must be a number > 0\n");
                return 2;
            }
            p.step = v;
            have_step = 1;
        } else if (strcmp(o, "--snr") == 0) {
            if (parse_real(arg, &v) || !(v > 0)) {
                fprintf(stderr, "--snr: must be a number > 0\n");
                return 2;
            }
            p.target_snr = v;
            have_snr = 1;
        } else if (strcmp(o, "--round") == 0) {
            if (parse_real(arg, &v) || v < 0.3 || v > 0.5) {
                fprintf(stderr, "--round: must be between 0.3 and 0.5\n");
                return 2;
            }
            p.round = v;
        } else {
            return usage();
        }
    }
    if (have_step && have_snr) {
        fprintf(stderr, "--step and --snr exclude each other\n");
        return 2;
    }
    if (argc - i != 2) return usage();
    p.use_step = have_step;

    return lossy_encode(argv[i], argv[i + 1], &p);
}
