/*
 * test_coefcoder.c - round trip of quantized blocks through coefcoder.c.
 *
 * For every N = 64 .. 8192, a sequence of blocks on two interleaved channels
 * (as in the codec): all zero, mostly zero with an audio-like decay, large
 * values, escape values up to QUANT_MAX, and only the last coefficient set.
 * The decoder must return every value exactly and consume the stream exactly.
 * Then random bytes are decoded: this must never crash (run it with the
 * sanitizer build) and must end with an error or a mismatch.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "coefcoder.h"
#include "quant.h"

static int failures = 0;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; \
    fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
    fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); } } while (0)

static uint64_t rng = 0x2545F4914F6CDD1Dull;
static uint32_t rnd(void)
{
    rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17;
    return (uint32_t)(rng >> 16);
}
static int32_t rsign(int32_t v) { return (rnd() & 1) ? -v : v; }

enum { ZERO, SPARSE, LARGE, ESCAPE, LAST_ONLY, NKINDS };

static void fill(int32_t *q, int n, int kind)
{
    memset(q, 0, n * sizeof *q);
    switch (kind) {
    case ZERO:
        break;
    case SPARSE:                     /* like audio: big at low k, rare above */
        for (int k = 0; k < n; k++) {
            int scale = 1 + 40 * 64 / (k + 64);
            if (rnd() % (1 + k / 8) == 0) q[k] = rsign((int32_t)(rnd() % scale));
        }
        break;
    case LARGE:
        for (int k = 0; k < n; k++) q[k] = rsign((int32_t)(rnd() % 40));
        break;
    case ESCAPE:                     /* around the escape threshold and huge */
        for (int k = 0; k < n; k += 1 + rnd() % 5) {
            int r = rnd() % 4;
            q[k] = r == 0 ? 14 + (int32_t)(rnd() % 3)
                 : r == 1 ? (int32_t)(rnd() % 100000)
                 : r == 2 ? QUANT_MAX - (int32_t)(rnd() % 3)
                 : (int32_t)(rnd() % 1000);
            q[k] = rsign(q[k]);
        }
        break;
    case LAST_ONLY:
        q[n - 1] = rsign(1 + (int32_t)(rnd() % 20));
        break;
    }
}

static uint8_t *slurp(FILE *f, size_t *len)
{
    long n = ftell(f);
    uint8_t *b = malloc(n > 0 ? (size_t)n : 1);
    rewind(f);
    if (fread(b, 1, (size_t)n, f) != (size_t)n) { perror("fread"); exit(1); }
    *len = (size_t)n;
    return b;
}

static void test_n(int n)
{
    const int nblocks = 60;
    int32_t *all = malloc((size_t)nblocks * 2 * n * sizeof *all);
    int32_t *got = malloc(n * sizeof *got);
    for (int b = 0; b < nblocks; b++)
        for (int c = 0; c < 2; c++)
            fill(all + ((size_t)b * 2 + c) * n, n, (b / 3 + c) % NKINDS);

    CoefCoder *cc = malloc(sizeof *cc);
    RcEnc *e = malloc(sizeof *e);
    FILE *f = tmpfile();
    CHECK(cc_init(cc, n) == 0, "cc_init(%d)", n);
    rc_enc_init(e, f);
    for (int b = 0; b < nblocks; b++)
        for (int c = 0; c < 2; c++)
            cc_encode_block(cc, e, c, all + ((size_t)b * 2 + c) * n);
    rc_enc_finish(e);
    cc_free(cc);

    size_t len;
    uint8_t *buf = slurp(f, &len);
    RcDec d;
    rc_dec_init(&d, buf, len);
    cc_init(cc, n);
    int bad = 0;
    for (int b = 0; b < nblocks && !bad; b++) {
        for (int c = 0; c < 2; c++) {
            int r = cc_decode_block(cc, &d, c, got);
            const int32_t *want = all + ((size_t)b * 2 + c) * n;
            if (r != 0 || d.err || memcmp(got, want, n * sizeof *got) != 0) {
                bad = 1;
                CHECK(0, "N=%d block %d ch %d differs", n, b, c);
                break;
            }
        }
    }
    CHECK(rc_dec_finish(&d) == 0, "N=%d: stream not consumed exactly", n);

    /* Garbage in: must not crash (that is what the sanitizer build checks).
       Most garbage streams are also caught; count how many. */
    int caught = 0;
    for (int trial = 0; trial < 20; trial++) {
        for (size_t i = 5; i < len; i++) buf[i] = (uint8_t)rnd();
        cc_free(cc);
        cc_init(cc, n);
        rc_dec_init(&d, buf, len);
        int failed = 0;
        for (int b = 0; b < nblocks && !failed; b++)
            for (int c = 0; c < 2 && !failed; c++)
                failed = cc_decode_block(cc, &d, c, got) != 0 || d.err;
        caught += failed || rc_dec_finish(&d) != 0;
    }
    printf("  N=%5d: %zu bytes for %d blocks, %d/20 garbage streams rejected\n",
           n, len, nblocks * 2, caught);

    cc_free(cc);
    free(buf); free(e); free(cc); fclose(f); free(all); free(got);
}

int main(void)
{
    CoefCoder cc;
    CHECK(cc_init(&cc, 32) != 0, "N=32 accepted");
    CHECK(cc_init(&cc, 100) != 0, "N=100 accepted");

    for (int n = 64; n <= 8192; n *= 2) test_n(n);

    if (failures) { printf("test_coefcoder: %d FAILED\n", failures); return 1; }
    printf("test_coefcoder: all passed (CTX_MODE %d, PROB_SHIFT %d)\n", CTX_MODE, PROB_SHIFT);
    return 0;
}
