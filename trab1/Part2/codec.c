/*
 * codec.c - the lossless codec: chains all modules together.
 * The file format is documented in codec.h.
 *
 * ENCODER                                  DECODER (exact reverse)
 * -------                                  -----------------------
 * read whole input file                    read header
 * wav_parse -> prefix / audio / suffix     read prefix and suffix bytes
 * write header, prefix, suffix             for every block, every channel:
 * wav_deinterleave -> one array/channel        read order and k
 * stereo: ms_forward  (L,R -> MID,SIDE)        rice_decode_block
 * for every block, every channel:              unzigzag
 *     predictor_choose_order                   predictor_restore
 *     predictor_residuals                  stereo: ms_inverse (MID,SIDE -> L,R)
 *     zigzag                               wav_rebuild: prefix + audio + suffix
 *     rice_choose_k
 *     write order, k, rice_encode_block
 *
 * If the input is not a WAV we can handle, it is stored unchanged (mode RAW),
 * so ANY file round-trips exactly.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "codec.h"
#include "bitio.h"
#include "wavfile.h"
#include "stereo.h"
#include "predictor.h"
#include "rice.h"

#define MAGIC      0x49434C31u   /* "ICL1" */
#define MODE_RAW   0
#define MODE_PCM   1
#define ORDER_BITS 3             /* predictor order 0..4 fits in 3 bits */
#define K_BITS     5             /* Rice parameter 0..30 fits in 5 bits */

/* ------------------------------------------------------------------ */
/* Small helpers                                                       */
/* ------------------------------------------------------------------ */

/* bw_put / br_get handle at most 32 bits, so 64-bit fields use two calls. */
static void put64(BitWriter *w, uint64_t v)
{
    bw_put(w, (uint32_t)(v >> 32), 32);
    bw_put(w, (uint32_t)v, 32);
}

static uint64_t get64(BitReader *r)
{
    uint64_t hi = br_get(r, 32);
    uint64_t lo = br_get(r, 32);
    return (hi << 32) | lo;
}

static void put_bytes(BitWriter *w, const uint8_t *p, uint64_t n)
{
    for (uint64_t i = 0; i < n; i++) bw_put(w, p[i], 8);
}

/* Stops early if the data runs out; the caller checks r->err. */
static void get_bytes(BitReader *r, uint8_t *p, uint64_t n)
{
    for (uint64_t i = 0; i < n; i++) {
        p[i] = (uint8_t)br_get(r, 8);
        if (r->err) return;
    }
}

/* Load a whole file into memory. Returns NULL (and prints why) on failure. */
static uint8_t *read_file(const char *path, uint64_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "cannot open %s\n", path); return NULL; }

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size < 0) { fprintf(stderr, "cannot size %s\n", path); fclose(f); return NULL; }

    uint8_t *buf = malloc(size > 0 ? (size_t)size : 1);   /* never malloc(0) */
    if (!buf || fread(buf, 1, (size_t)size, f) != (size_t)size) {
        fprintf(stderr, "cannot read %s\n", path);
        free(buf);
        fclose(f);
        return NULL;
    }
    fclose(f);
    *len = (uint64_t)size;
    return buf;
}

/*
 * One array per channel, with PRED_MAX_ORDER zeros in front (see predictor.h):
 * base[c] is what we malloc, chan[c] = base[c] + PRED_MAX_ORDER is where sample
 * 0 lives. The zeros are the "history" of the very first block.
 */
static int alloc_channels(int32_t *base[2], int32_t *chan[2],
                          int channels, uint64_t nframes)
{
    if (nframes > ((uint64_t)1 << 40)) return 1;          /* absurd size: refuse */
    for (int c = 0; c < channels; c++) {
        base[c] = calloc((size_t)nframes + PRED_MAX_ORDER, sizeof(int32_t));
        if (!base[c]) return 1;
        chan[c] = base[c] + PRED_MAX_ORDER;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Encoder                                                             */
/* ------------------------------------------------------------------ */

/* Compress the audio part. Frees *file as soon as it is no longer needed. */
static int encode_pcm(BitWriter *bw, uint8_t **file, uint64_t len,
                      const WavInfo *info, uint32_t block_size,
                      unsigned long long order_hist[PRED_MAX_ORDER + 1])
{
    int       rc = 1;
    const int channels = info->channels;
    const uint64_t nframes = info->nframes;
    int32_t  *base[2] = {NULL, NULL}, *chan[2] = {NULL, NULL};
    int32_t  *res = NULL;
    uint32_t *u = NULL;

    /* Where the three byte ranges of the input are. */
    uint64_t audio_bytes = nframes * (uint64_t)(channels * info->bits / 8);
    uint64_t prefix_len  = info->data_off;
    uint64_t suffix_off  = prefix_len + audio_bytes;
    uint64_t suffix_len  = len - suffix_off;

    /* ---- header ---- */
    bw_put(bw, MODE_PCM, 8);
    bw_put(bw, (uint32_t)channels, 8);
    bw_put(bw, (uint32_t)info->bits, 8);
    bw_put(bw, block_size, 32);
    put64(bw, nframes);
    put64(bw, prefix_len);
    put64(bw, suffix_len);
    put_bytes(bw, *file, prefix_len);               /* stored verbatim */
    put_bytes(bw, *file + suffix_off, suffix_len);  /* stored verbatim */

    /* ---- samples into per-channel int32 arrays, then free the file ---- */
    if (alloc_channels(base, chan, channels, nframes)) {
        fprintf(stderr, "out of memory\n");
        goto done;
    }
    wav_deinterleave(*file, info, chan);
    free(*file);
    *file = NULL;

    /* Stereo: from now on channel 0 is MID and channel 1 is SIDE. */
    if (channels == 2) ms_forward(chan[0], chan[1], (size_t)nframes);

    /* Scratch buffers, one block long. */
    size_t cap = nframes < block_size ? (size_t)nframes : block_size;
    if (cap == 0) cap = 1;
    res = malloc(cap * sizeof(int32_t));
    u   = malloc(cap * sizeof(uint32_t));
    if (!res || !u) { fprintf(stderr, "out of memory\n"); goto done; }

    /* ---- blocks ---- */
    for (uint64_t start = 0; start < nframes; start += block_size) {
        size_t n = (size_t)((nframes - start < block_size) ? nframes - start : block_size);

        for (int c = 0; c < channels; c++) {
            const int32_t *x = chan[c] + start;     /* x[-1..-4] = history */

            /* 1. predict: pick the best order and compute the residuals */
            int order = predictor_choose_order(x, n);
            predictor_residuals(x, n, order, res);

            /* 2. make them unsigned and choose the Rice parameter */
            for (size_t i = 0; i < n; i++) u[i] = zigzag(res[i]);
            int k = rice_choose_k(u, n);

            /* 3. write: order, k, then the coded residuals */
            bw_put(bw, (uint32_t)order, ORDER_BITS);
            bw_put(bw, (uint32_t)k, K_BITS);
            rice_encode_block(bw, u, n, k);

            order_hist[order]++;
        }
    }
    rc = 0;

done:
    free(res);
    free(u);
    for (int c = 0; c < 2; c++) free(base[c]);
    return rc;
}

int codec_encode(const char *in_path, const char *out_path,
                 uint32_t block_size, int verbose)
{
    int       rc = 1;
    uint8_t  *file = NULL;
    uint64_t  len = 0;
    FILE     *fo = NULL;
    BitWriter *bw = NULL;
    WavInfo   info;
    unsigned long long order_hist[PRED_MAX_ORDER + 1] = {0};
    int       pcm = 0;

    if (block_size == 0) block_size = CODEC_DEFAULT_BLOCK;
    if (block_size > CODEC_MAX_BLOCK) {
        fprintf(stderr, "block size must be between 1 and %u\n", CODEC_MAX_BLOCK);
        return 1;
    }

    file = read_file(in_path, &len);
    if (!file) return 1;

    fo = fopen(out_path, "wb");
    bw = malloc(sizeof *bw);                         /* ~64 KiB, keep it off the stack */
    if (!fo || !bw) { fprintf(stderr, "cannot create %s\n", out_path); goto done; }
    bw_init(bw, fo);

    bw_put(bw, MAGIC, 32);

    pcm = wav_parse(file, len, &info);
    if (pcm) {
        if (encode_pcm(bw, &file, len, &info, block_size, order_hist)) goto done;
    } else {
        /* Not a WAV we can model: keep the bytes as they are. */
        bw_put(bw, MODE_RAW, 8);
        put64(bw, len);
        put_bytes(bw, file, len);
    }

    if (bw_flush(bw)) { fprintf(stderr, "write error on %s\n", out_path); goto done; }
    rc = 0;

    if (verbose) {
        uint64_t out_size = bw->total_bytes;
        if (pcm) {
            fprintf(stderr, "%d ch, %d bit, %llu frames, block %u\n",
                    info.channels, info.bits,
                    (unsigned long long)info.nframes, block_size);
            fprintf(stderr, "predictor orders used (0..4):");
            for (int o = 0; o <= PRED_MAX_ORDER; o++)
                fprintf(stderr, " %llu", order_hist[o]);
            fprintf(stderr, "\n");
        } else {
            fprintf(stderr, "not a supported PCM WAV: stored unchanged\n");
        }
        fprintf(stderr, "%llu -> %llu bytes (ratio %.3f)\n",
                (unsigned long long)len, (unsigned long long)out_size,
                out_size ? (double)len / (double)out_size : 0.0);
    }

done:
    free(file);
    free(bw);
    if (fo && fclose(fo) != 0) rc = 1;
    return rc;
}

/* ------------------------------------------------------------------ */
/* Decoder                                                             */
/* ------------------------------------------------------------------ */

static int decode_pcm(BitReader *br, FILE *fo, uint64_t comp_size)
{
    int       rc = 1;
    uint8_t  *prefix = NULL, *suffix = NULL;
    int32_t  *base[2] = {NULL, NULL}, *chan[2] = {NULL, NULL};
    int32_t  *res = NULL;
    uint32_t *u = NULL;

    /* ---- header ---- */
    int      channels   = (int)br_get(br, 8);
    int      bits       = (int)br_get(br, 8);
    uint32_t block_size = br_get(br, 32);
    uint64_t nframes    = get64(br);
    uint64_t prefix_len = get64(br);
    uint64_t suffix_len = get64(br);
    if (br->err) { fprintf(stderr, "truncated header\n"); return 1; }

    /* Do not trust the file: reject values the encoder can never produce.
     * Size checks use the real compressed file size, so a damaged header can
     * never make us allocate gigabytes:
     *   - prefix and suffix are stored verbatim, so they must fit in the file;
     *   - every sample of every channel costs at least 1 bit (a Rice code is
     *     always >= 1 bit), so nframes * channels cannot exceed the file's bits. */
    if ((channels != 1 && channels != 2) ||
        (bits != 8 && bits != 16 && bits != 24) ||
        block_size < 1 || block_size > CODEC_MAX_BLOCK ||
        prefix_len > comp_size || suffix_len > comp_size ||
        nframes > comp_size * 8 / (uint64_t)channels) {
        fprintf(stderr, "corrupt header\n");
        return 1;
    }

    prefix = malloc(prefix_len ? (size_t)prefix_len : 1);
    suffix = malloc(suffix_len ? (size_t)suffix_len : 1);
    if (!prefix || !suffix) { fprintf(stderr, "out of memory\n"); goto done; }
    get_bytes(br, prefix, prefix_len);
    get_bytes(br, suffix, suffix_len);
    if (br->err) { fprintf(stderr, "truncated file\n"); goto done; }

    if (alloc_channels(base, chan, channels, nframes)) {
        fprintf(stderr, "out of memory\n");
        goto done;
    }
    size_t cap = nframes < block_size ? (size_t)nframes : block_size;
    if (cap == 0) cap = 1;
    res = malloc(cap * sizeof(int32_t));
    u   = malloc(cap * sizeof(uint32_t));
    if (!res || !u) { fprintf(stderr, "out of memory\n"); goto done; }

    /* ---- blocks: same order as the encoder wrote them ---- */
    for (uint64_t start = 0; start < nframes; start += block_size) {
        size_t n = (size_t)((nframes - start < block_size) ? nframes - start : block_size);

        for (int c = 0; c < channels; c++) {
            int order = (int)br_get(br, ORDER_BITS);
            int k     = (int)br_get(br, K_BITS);
            if (br->err || order > PRED_MAX_ORDER || k > RICE_MAX_K) {
                fprintf(stderr, "corrupt or truncated data\n");
                goto done;
            }

            rice_decode_block(br, u, n, k);
            if (br->err) { fprintf(stderr, "corrupt or truncated data\n"); goto done; }

            for (size_t i = 0; i < n; i++) res[i] = unzigzag(u[i]);

            /* Needs the previous samples in chan[c] - they are already decoded. */
            predictor_restore(chan[c] + start, n, order, res);
        }
    }

    /* Undo mid/side on the whole signal at once. */
    if (channels == 2) ms_inverse(chan[0], chan[1], (size_t)nframes);

    /* prefix + audio + suffix */
    if (wav_rebuild(fo, prefix, prefix_len, chan, channels, bits, nframes,
                    suffix, suffix_len)) {
        fprintf(stderr, "write error\n");
        goto done;
    }
    rc = 0;

done:
    free(prefix);
    free(suffix);
    free(res);
    free(u);
    for (int c = 0; c < 2; c++) free(base[c]);
    return rc;
}

int codec_decode(const char *in_path, const char *out_path)
{
    int        rc = 1;
    FILE      *fi = fopen(in_path, "rb");
    FILE      *fo = NULL;
    BitReader *br = malloc(sizeof *br);
    uint64_t   comp_size = 0;

    if (!fi || !br) { fprintf(stderr, "cannot open %s\n", in_path); goto done; }

    /* Size of the compressed file, used to sanity-check the header. */
    fseek(fi, 0, SEEK_END);
    long fsize = ftell(fi);
    fseek(fi, 0, SEEK_SET);
    if (fsize < 0) { fprintf(stderr, "cannot size %s\n", in_path); goto done; }
    comp_size = (uint64_t)fsize;

    br_init(br, fi);

    if (br_get(br, 32) != MAGIC || br->err) {
        fprintf(stderr, "%s is not a file made by this encoder\n", in_path);
        goto done;
    }
    int mode = (int)br_get(br, 8);

    fo = fopen(out_path, "wb");      /* opened only after the magic checked out */
    if (!fo) { fprintf(stderr, "cannot create %s\n", out_path); goto done; }

    if (mode == MODE_RAW) {
        uint64_t remaining = get64(br);
        uint8_t  tmp[65536];
        while (remaining > 0 && !br->err) {
            size_t m = remaining < sizeof tmp ? (size_t)remaining : sizeof tmp;
            get_bytes(br, tmp, m);
            if (br->err) break;
            if (fwrite(tmp, 1, m, fo) != m) { fprintf(stderr, "write error\n"); goto done; }
            remaining -= m;
        }
        if (br->err) { fprintf(stderr, "truncated file\n"); goto done; }
        rc = 0;
    } else if (mode == MODE_PCM) {
        rc = decode_pcm(br, fo, comp_size);
    } else {
        fprintf(stderr, "unknown mode %d\n", mode);
    }

done:
    free(br);
    if (fi) fclose(fi);
    if (fo && fclose(fo) != 0) rc = 1;
    return rc;
}
