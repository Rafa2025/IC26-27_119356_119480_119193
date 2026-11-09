/*
 * codec_lossy.h - the lossy (DCT/MDCT) codec: ties all modules together.
 * Design and file format: CODEC_LOSSY.md.
 *
 * COMPRESSED FILE (little-endian)
 *   offset size
 *    0     4   magic "ICD1"
 *    4     1   channels (1 or 2)
 *    5     1   bits per sample (8, 16, 24)
 *    6     1   transform (0 = dct, 1 = mdct)
 *    7     1   stereo (0 = lr, 1 = ms)
 *    8     4   sample rate
 *   12     8   number of frames
 *   20     4   N (block size)
 *   24     4   step (float32)
 *   28         range coder payload: for every block, for every channel,
 *              one coefficient block (coefcoder.h); then the coder flush.
 */
#ifndef CODEC_LOSSY_H
#define CODEC_LOSSY_H

#define LOSSY_DEFAULT_N   2048
#define LOSSY_MIN_N       64
#define LOSSY_MAX_N       8192
#define LOSSY_DEFAULT_SNR 30.0

typedef struct {
    int    n;          /* coefficients per block */
    int    transform;  /* TR_DCT or TR_MDCT */
    int    ms;         /* mid/side requested (ignored for mono) */
    int    use_step;   /* 1: fixed step; 0: search for target_snr */
    double step;
    double target_snr;
    double round;      /* quantizer rounding offset (hidden --round) */
    int    verbose;
} LossyParams;

/* Both return 0 on success, nonzero on error (message printed to stderr). */
int lossy_encode(const char *in_path, const char *out_path, const LossyParams *p);
int lossy_decode(const char *in_path, const char *out_path);

#endif /* CODEC_LOSSY_H */
