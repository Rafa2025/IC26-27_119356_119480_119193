/*
 * codec_lossy.c - the lossy codec: chains all modules together.
 * The file format is documented in codec_lossy.h, the design in CODEC_LOSSY.md.
 *
 * ENCODER                                   DECODER
 * -------                                   -------
 * read whole input, wav_parse               read whole compressed file
 * for every block (Analyser):               read and check the header
 *     read its samples, mid/side,           for every block:
 *     forward transform                         for every channel:
 * --step: quantize and code each block              cc_decode_block -> q
 *         as soon as it is transformed          Synth: dequantize, inverse
 * --snr:  keep all coefficients (float),               transform, overlap-add,
 *         ratectl finds the step (its real             L/R, round, clip
 *         check runs the decoder's Synth),      write the frames
 *         then quantize and code
 *
 * Block layout (frames F, block size N):
 *   dct:  ceil(F/N) blocks,     block b = frames bN .. bN+N-1
 *   mdct: ceil(F/N) + 1 blocks, block b = frames (b-1)N .. (b+1)N-1
 * Frames outside 0 .. F-1 are zeros. In the MDCT, block b completes (after
 * overlap-add) the frames (b-1)N .. bN-1, so block 0 produces no output.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "codec_lossy.h"
#include "coefcoder.h"
#include "quant.h"
#include "rangecoder.h"
#include "ratectl.h"
#include "transform.h"
#include "wavfile.h"

#define HEADER_SIZE 28
static const uint8_t MAGIC[4] = {'I', 'C', 'D', '1'};

/* ------------------------------------------------------------------ */
/* Small helpers                                                       */
/* ------------------------------------------------------------------ */

static void put32(uint8_t *p, uint32_t v)
{
    for (int i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (8 * i));
}
static uint32_t get32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
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

/* ------------------------------------------------------------------ */
/* Block layout                                                        */
/* ------------------------------------------------------------------ */

typedef struct {
    int      channels, bits, n, mdct, ms;
    uint32_t rate;
    uint64_t frames, nblocks;
} Layout;

static void layout_finish(Layout *L)
{
    L->nblocks = (L->frames + (uint64_t)L->n - 1) / (uint64_t)L->n + (L->mdct ? 1 : 0);
}

/* First frame covered by block b (negative for the first MDCT block). */
static int64_t block_start(const Layout *L, uint64_t b)
{
    return L->mdct ? ((int64_t)b - 1) * L->n : (int64_t)b * L->n;
}

/*
 * Original samples of frames start .. start+count-1 into dst[c][0 .. count-1],
 * zeros for frames outside the file. Reuses wav_deinterleave on a sub-range.
 */
static void read_span(const uint8_t *file, const WavInfo *info,
                      int64_t start, int count, int32_t *dst[2])
{
    int64_t first = start < 0 ? 0 : start;
    int64_t end = start + count;
    if (end > (int64_t)info->nframes) end = (int64_t)info->nframes;

    for (int c = 0; c < info->channels; c++) memset(dst[c], 0, (size_t)count * sizeof(int32_t));
    if (end <= first) return;

    WavInfo sub = *info;
    sub.data_off += (uint64_t)first * (uint64_t)(info->channels * info->bits / 8);
    sub.nframes = (uint64_t)(end - first);
    int32_t *p[2] = {dst[0] + (first - start), info->channels == 2 ? dst[1] + (first - start) : NULL};
    wav_deinterleave(file, &sub, p);
}

/* Sum of squared original samples, all channels (8-bit centred on 0). */
static double signal_energy(const uint8_t *file, const WavInfo *info, int32_t *tmp[2], int chunk)
{
    double es = 0;
    for (uint64_t s = 0; s < info->nframes; s += (uint64_t)chunk) {
        int count = info->nframes - s < (uint64_t)chunk ? (int)(info->nframes - s) : chunk;
        read_span(file, info, (int64_t)s, count, tmp);
        for (int c = 0; c < info->channels; c++)
            for (int i = 0; i < count; i++) es += (double)tmp[c][i] * tmp[c][i];
    }
    return es;
}

/* ------------------------------------------------------------------ */
/* Analysis: samples -> coefficients (encoder)                         */
/* ------------------------------------------------------------------ */

typedef struct {
    const Layout *L;
    Transform     tr;
    int32_t      *raw[2];   /* tr_span samples */
    double       *in;       /* tr_span */
    double       *coef[2];  /* N */
} Analyser;

static int analyser_init(Analyser *a, const Layout *L)
{
    memset(a, 0, sizeof *a);
    a->L = L;
    if (tr_init(&a->tr, L->n, L->mdct ? TR_MDCT : TR_DCT)) return 1;
    int span = tr_span(&a->tr);
    a->in = malloc(span * sizeof(double));
    for (int c = 0; c < 2; c++) {
        a->raw[c]  = malloc(span * sizeof(int32_t));
        a->coef[c] = malloc(L->n * sizeof(double));
        if (!a->raw[c] || !a->coef[c]) return 1;
    }
    return a->in == NULL;
}

static void analyser_free(Analyser *a)
{
    tr_free(&a->tr);
    free(a->in);
    for (int c = 0; c < 2; c++) { free(a->raw[c]); free(a->coef[c]); }
}

/* Block b -> a->coef[c] for every channel (channel 0/1 = M/S if ms). */
static void analyse_block(Analyser *a, const uint8_t *file, const WavInfo *info, uint64_t b)
{
    const Layout *L = a->L;
    const int span = tr_span(&a->tr);
    read_span(file, info, block_start(L, b), span, a->raw);

    for (int c = 0; c < L->channels; c++) {
        const int32_t *x0 = a->raw[0], *x1 = a->raw[1];
        if (L->ms) {
            if (c == 0) for (int i = 0; i < span; i++) a->in[i] = 0.5 * ((double)x0[i] + x1[i]);
            else        for (int i = 0; i < span; i++) a->in[i] = 0.5 * ((double)x0[i] - x1[i]);
        } else {
            for (int i = 0; i < span; i++) a->in[i] = a->raw[c][i];
        }
        tr_forward(&a->tr, a->in, a->coef[c]);
    }
}

/* ------------------------------------------------------------------ */
/* Synthesis: q -> samples (decoder, and the encoder's real check)     */
/* ------------------------------------------------------------------ */

typedef struct {
    const Layout *L;
    Transform     tr;
    double        step;
    double       *coef;     /* N */
    double       *time;     /* tr_span */
    double       *olap[2];  /* N: second half of the previous MDCT block */
    double       *rec[2];   /* N: finished samples of the current block */
    int32_t      *out[2];   /* N: rounded and clipped */
} Synth;

static int synth_init(Synth *s, const Layout *L)
{
    memset(s, 0, sizeof *s);
    s->L = L;
    if (tr_init(&s->tr, L->n, L->mdct ? TR_MDCT : TR_DCT)) return 1;
    s->coef = malloc(L->n * sizeof(double));
    s->time = malloc(tr_span(&s->tr) * sizeof(double));
    if (!s->coef || !s->time) return 1;
    for (int c = 0; c < 2; c++) {
        s->olap[c] = calloc(L->n, sizeof(double));
        s->rec[c]  = malloc(L->n * sizeof(double));
        s->out[c]  = malloc(L->n * sizeof(int32_t));
        if (!s->olap[c] || !s->rec[c] || !s->out[c]) return 1;
    }
    return 0;
}

static void synth_reset(Synth *s, double step)
{
    s->step = step;
    for (int c = 0; c < 2; c++) memset(s->olap[c], 0, s->L->n * sizeof(double));
}

static void synth_free(Synth *s)
{
    tr_free(&s->tr);
    free(s->coef);
    free(s->time);
    for (int c = 0; c < 2; c++) { free(s->olap[c]); free(s->rec[c]); free(s->out[c]); }
}

/*
 * Reconstruct block b from its quantized values. Puts the finished frames in
 * s->out[c][0 .. count-1], sets *first to the index of the first one and
 * returns count (0 if the block finishes no frame of the file).
 */
static int synth_block(Synth *s, uint64_t b, int32_t *const q[2], int64_t *first)
{
    const Layout *L = s->L;
    const int n = L->n;

    for (int c = 0; c < L->channels; c++) {
        dequant_block(q[c], s->coef, n, s->step);
        tr_inverse(&s->tr, s->coef, s->time);
        if (L->mdct) {
            for (int i = 0; i < n; i++) s->rec[c][i] = s->olap[c][i] + s->time[i];
            memcpy(s->olap[c], s->time + n, n * sizeof(double));
        } else {
            memcpy(s->rec[c], s->time, n * sizeof(double));
        }
    }

    int64_t start = L->mdct ? ((int64_t)b - 1) * n : (int64_t)b * n;
    if (start < 0 || (uint64_t)start >= L->frames) return 0;
    int count = L->frames - (uint64_t)start < (uint64_t)n ? (int)(L->frames - (uint64_t)start) : n;

    /* Clip in double first: fmin/fmax also turn a NaN from damaged data
       into a limit, and lround then never sees an out-of-range value. */
    const double lo = -ldexp(1.0, L->bits - 1), hi = ldexp(1.0, L->bits - 1) - 1;
    for (int c = 0; c < L->channels; c++) {
        const double *m = s->rec[0], *sd = s->rec[1];
        for (int i = 0; i < count; i++) {
            double v = !L->ms ? s->rec[c][i] : c == 0 ? m[i] + sd[i] : m[i] - sd[i];
            s->out[c][i] = (int32_t)lround(fmin(fmax(v, lo), hi));
        }
    }
    *first = start;
    return count;
}

/* Squared error of synthesized frames against the original. */
static double frame_noise(Synth *s, const uint8_t *file, const WavInfo *info,
                          int64_t first, int count, int32_t *orig[2])
{
    double en = 0;
    read_span(file, info, first, count, orig);
    for (int c = 0; c < info->channels; c++)
        for (int i = 0; i < count; i++) {
            double e = (double)orig[c][i] - s->out[c][i];
            en += e * e;
        }
    return en;
}

/* ------------------------------------------------------------------ */
/* Encoder                                                             */
/* ------------------------------------------------------------------ */

/* Context of the real SNR check that ratectl calls back. */
typedef struct {
    const Layout  *L;
    const uint8_t *file;
    const WavInfo *info;
    const float   *coefs;   /* block-major: (b * channels + c) * N */
    Synth         *syn;
    double         round, signal;
    double        *tmp;     /* N */
    int32_t       *q[2], *orig[2];
} CheckCtx;

static void load_coefs(const float *src, double *dst, int n)
{
    for (int k = 0; k < n; k++) dst[k] = src[k];
}

static double real_snr(void *vctx, double step)
{
    CheckCtx *x = vctx;
    const Layout *L = x->L;
    double noise = 0;

    synth_reset(x->syn, step);
    for (uint64_t b = 0; b < L->nblocks; b++) {
        for (int c = 0; c < L->channels; c++) {
            load_coefs(x->coefs + (b * L->channels + c) * (uint64_t)L->n, x->tmp, L->n);
            quant_block(x->tmp, x->q[c], L->n, step, x->round);
        }
        int64_t first;
        int count = synth_block(x->syn, b, x->q, &first);
        if (count) noise += frame_noise(x->syn, x->file, x->info, first, count, x->orig);
    }
    return snr_db(x->signal, noise);
}

int lossy_encode(const char *in_path, const char *out_path, const LossyParams *p)
{
    int       rc = 1;
    uint64_t  len = 0;
    uint8_t  *file = NULL;
    float    *coefs = NULL;
    FILE     *fo = NULL;
    RcEnc    *enc = NULL;
    CoefCoder *cc = NULL;
    Analyser  an;
    Synth     syn;
    int32_t  *q[2] = {NULL, NULL}, *orig[2] = {NULL, NULL};
    WavInfo   info;
    Layout    L;

    memset(&an, 0, sizeof an);
    memset(&syn, 0, sizeof syn);

    file = read_file(in_path, &len);
    if (!file) return 1;
    if (!wav_parse(file, len, &info)) {
        fprintf(stderr, "%s: not a supported WAV file (PCM, 8/16/24 bit, 1 or 2 channels)\n", in_path);
        goto done;
    }
    if (info.nframes * (uint64_t)info.channels > ((uint64_t)1 << 31)) {
        fprintf(stderr, "%s: too long\n", in_path);
        goto done;
    }

    L.channels = info.channels;
    L.bits     = info.bits;
    L.rate     = info.rate;
    L.n        = p->n;
    L.mdct     = p->transform == TR_MDCT;
    L.ms       = p->ms && info.channels == 2;
    L.frames   = info.nframes;
    layout_finish(&L);

    const double min_step = fmax(0.01, quant_min_step(L.bits, L.n));
    const double max_step = ldexp(1.0, L.bits);
    const uint64_t nsamples = L.frames * (uint64_t)L.channels;

    cc  = calloc(1, sizeof *cc);
    enc = malloc(sizeof *enc);
    for (int c = 0; c < 2; c++) {
        q[c]    = malloc(L.n * sizeof(int32_t));
        orig[c] = malloc(L.n * sizeof(int32_t));
    }
    if (!cc || !enc || !q[0] || !q[1] || !orig[0] || !orig[1] ||
        cc_init(cc, L.n) || analyser_init(&an, &L) || synth_init(&syn, &L)) {
        fprintf(stderr, "out of memory\n");
        goto done;
    }

    double step, signal = 0, achieved = NAN;

    if (p->use_step) {
        step = p->step;
        if (step < min_step) {
            fprintf(stderr, "warning: step %g is below the minimum %g for this input, using the minimum\n",
                    step, min_step);
            step = min_step;
        }
        step = (float)step;
        if (step < min_step) step = nextafterf((float)min_step, INFINITY);
        if (p->verbose) signal = signal_energy(file, &info, orig, L.n);
    } else {
        /* Transform everything once and keep it for the search. */
        size_t ncoef = (size_t)L.nblocks * (size_t)L.channels * (size_t)L.n;
        coefs = malloc(ncoef ? ncoef * sizeof(float) : 1);
        if (!coefs) { fprintf(stderr, "out of memory\n"); goto done; }
        for (uint64_t b = 0; b < L.nblocks; b++) {
            analyse_block(&an, file, &info, b);
            for (int c = 0; c < L.channels; c++) {
                float *dst = coefs + (b * L.channels + c) * (uint64_t)L.n;
                for (int k = 0; k < L.n; k++) dst[k] = (float)an.coef[c][k];
            }
        }
        signal = signal_energy(file, &info, orig, L.n);

        CheckCtx ctx = {&L, file, &info, coefs, &syn, p->round, signal,
                        an.coef[0], {q[0], q[1]}, {orig[0], orig[1]}};
        RateCtl r = {coefs, ncoef, L.ms ? 2.0 : 1.0, signal, nsamples,
                     min_step, max_step, p->round, p->target_snr, real_snr, &ctx};
        step = ratectl_find_step(&r, &achieved);
    }

    /* ---- header ---- */
    uint8_t hdr[HEADER_SIZE];
    float   fstep = (float)step;
    uint32_t ustep;
    memcpy(&ustep, &fstep, 4);
    memcpy(hdr, MAGIC, 4);
    hdr[4] = (uint8_t)L.channels;
    hdr[5] = (uint8_t)L.bits;
    hdr[6] = (uint8_t)L.mdct;
    hdr[7] = (uint8_t)L.ms;
    put32(hdr + 8, L.rate);
    put32(hdr + 12, (uint32_t)L.frames);
    put32(hdr + 16, (uint32_t)(L.frames >> 32));
    put32(hdr + 20, (uint32_t)L.n);
    put32(hdr + 24, ustep);

    fo = fopen(out_path, "wb");
    if (!fo || fwrite(hdr, 1, HEADER_SIZE, fo) != HEADER_SIZE) {
        fprintf(stderr, "cannot write %s\n", out_path);
        goto done;
    }

    /* ---- payload ---- */
    double noise = 0;
    rc_enc_init(enc, fo);
    synth_reset(&syn, step);
    for (uint64_t b = 0; b < L.nblocks; b++) {
        if (p->use_step) analyse_block(&an, file, &info, b);
        for (int c = 0; c < L.channels; c++) {
            if (!p->use_step)
                load_coefs(coefs + (b * L.channels + c) * (uint64_t)L.n, an.coef[c], L.n);
            quant_block(an.coef[c], q[c], L.n, step, p->round);
            cc_encode_block(cc, enc, c, q[c]);
        }
        if (p->use_step && p->verbose) {          /* measure the SNR on the way */
            int64_t first;
            int count = synth_block(&syn, b, q, &first);
            if (count) noise += frame_noise(&syn, file, &info, first, count, orig);
        }
    }
    if (rc_enc_finish(enc)) { fprintf(stderr, "write error on %s\n", out_path); goto done; }
    if (p->use_step && p->verbose) achieved = snr_db(signal, noise);
    rc = 0;

    if (p->verbose) {
        uint64_t total = HEADER_SIZE + rc_enc_bytes(enc);
        double secs = L.rate ? (double)L.frames / L.rate : 0;
        fprintf(stderr, "%d ch, %d bit, %u Hz, %llu frames\n", L.channels, L.bits, L.rate,
                (unsigned long long)L.frames);
        fprintf(stderr, "transform %s, N %d, stereo %s, round %.3f (CTX_MODE %d, PROB_SHIFT %d)\n",
                L.mdct ? "mdct" : "dct", L.n, L.channels == 1 ? "mono" : L.ms ? "ms" : "lr",
                p->round, CTX_MODE, PROB_SHIFT);
        if (p->use_step) fprintf(stderr, "step %.6g\n", step);
        else             fprintf(stderr, "step %.6g (target %.2f dB)\n", step, p->target_snr);
        fprintf(stderr, "SNR %.3f dB\n", achieved);
        fprintf(stderr, "%llu -> %llu bytes, %.4f bits/sample, %.2f kbps\n",
                (unsigned long long)len, (unsigned long long)total,
                nsamples ? 8.0 * total / nsamples : 0.0,
                secs > 0 ? 8.0 * total / secs / 1000 : 0.0);
    }

done:
    if (fo && fclose(fo) != 0) rc = 1;
    if (rc && fo) remove(out_path);
    if (cc) cc_free(cc);
    analyser_free(&an);
    synth_free(&syn);
    for (int c = 0; c < 2; c++) { free(q[c]); free(orig[c]); }
    free(cc);
    free(enc);
    free(coefs);
    free(file);
    return rc;
}

/* ------------------------------------------------------------------ */
/* Decoder                                                             */
/* ------------------------------------------------------------------ */

int lossy_decode(const char *in_path, const char *out_path)
{
    int        rc = 1;
    uint64_t   len = 0;
    uint8_t   *buf = NULL;
    FILE      *fo = NULL;
    CoefCoder *cc = NULL;
    Synth      syn;
    int32_t   *q[2] = {NULL, NULL};
    Layout     L;

    memset(&syn, 0, sizeof syn);
    buf = read_file(in_path, &len);
    if (!buf) return 1;

    if (len < HEADER_SIZE || memcmp(buf, MAGIC, 4) != 0) {
        fprintf(stderr, "%s is not a file made by this encoder\n", in_path);
        goto done;
    }

    /* ---- header: reject anything the encoder can never produce ---- */
    L.channels = buf[4];
    L.bits     = buf[5];
    L.mdct     = buf[6];
    L.ms       = buf[7];
    L.rate     = get32(buf + 8);
    L.frames   = get32(buf + 12) | ((uint64_t)get32(buf + 16) << 32);
    uint32_t n = get32(buf + 20);
    uint32_t ustep = get32(buf + 24);
    float    fstep;
    memcpy(&fstep, &ustep, 4);

    int bytes = L.bits / 8;
    if ((L.channels != 1 && L.channels != 2) ||
        (L.bits != 8 && L.bits != 16 && L.bits != 24) ||
        L.mdct > 1 || L.ms > 1 || (L.ms && L.channels != 2) ||
        n < LOSSY_MIN_N || n > LOSSY_MAX_N || (n & (n - 1)) != 0 ||
        !isfinite(fstep) || !(fstep > 0) ||
        L.frames > ((uint64_t)1 << 31) / (uint64_t)L.channels ||
        L.frames * (uint64_t)(L.channels * bytes) > 0xFFFFFFFFull - 36) {
        fprintf(stderr, "%s: corrupt header\n", in_path);
        goto done;
    }
    L.n = (int)n;
    layout_finish(&L);

    cc = calloc(1, sizeof *cc);
    for (int c = 0; c < 2; c++) q[c] = malloc(L.n * sizeof(int32_t));
    if (!cc || !q[0] || !q[1] || cc_init(cc, L.n) || synth_init(&syn, &L)) {
        fprintf(stderr, "out of memory\n");
        goto done;
    }
    synth_reset(&syn, fstep);

    fo = fopen(out_path, "wb");
    uint8_t wh[WAV_HEADER_SIZE];
    wav_make_header(wh, L.channels, L.bits, L.rate, L.frames);
    if (!fo || fwrite(wh, 1, WAV_HEADER_SIZE, fo) != WAV_HEADER_SIZE) {
        fprintf(stderr, "cannot write %s\n", out_path);
        goto done;
    }

    /* ---- blocks: same order as the encoder wrote them ---- */
    RcDec d;
    rc_dec_init(&d, buf + HEADER_SIZE, (size_t)(len - HEADER_SIZE));
    for (uint64_t b = 0; b < L.nblocks; b++) {
        for (int c = 0; c < L.channels; c++) {
            if (cc_decode_block(cc, &d, c, q[c]) != 0 || d.err) {
                fprintf(stderr, "%s: corrupt or truncated data\n", in_path);
                goto done;
            }
        }
        int64_t first;
        int count = synth_block(&syn, b, q, &first);
        if (count && wav_rebuild(fo, NULL, 0, syn.out, L.channels, L.bits,
                                 (uint64_t)count, NULL, 0)) {
            fprintf(stderr, "write error on %s\n", out_path);
            goto done;
        }
    }
    if (rc_dec_finish(&d)) {
        fprintf(stderr, "%s: corrupt or truncated data\n", in_path);
        goto done;
    }
    rc = 0;

done:
    if (fo && fclose(fo) != 0) rc = 1;
    if (rc && fo) remove(out_path);          /* no half-written output */
    if (cc) cc_free(cc);
    synth_free(&syn);
    for (int c = 0; c < 2; c++) free(q[c]);
    free(cc);
    free(buf);
    return rc;
}
