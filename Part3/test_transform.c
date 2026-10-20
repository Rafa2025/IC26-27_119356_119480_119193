/*
 * test_transform.c - checks for transform.c.
 *
 *   ./test_transform           correctness tests
 *   ./test_transform --speed   also time FFT-based vs direct (experiment 7)
 *
 * Correctness, for every N = 64 .. 8192:
 *   1. FFT-based DCT-II, DCT-III, MDCT and IMDCT equal the direct O(N^2)
 *      versions on random blocks (max difference below 1e-9);
 *   2. DCT: inverse(forward(x)) == x;
 *   3. MDCT: a whole signal, padded and cut into overlapping blocks exactly
 *      as the codec does, comes back after overlap-add (TDAC), including the
 *      samples in the first and the last block;
 *   4. Parseval: total coefficient energy == signal energy, for both.
 */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "transform.h"

static int failures = 0;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; \
    fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
    fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); } } while (0)

static uint64_t rng = 0x9E3779B97F4A7C15ull;
static double urand(void)                       /* uniform in [-1, 1) */
{
    rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17;
    return (double)(rng >> 11) / (double)(1ull << 52) - 1.0;
}

static double maxdiff(const double *a, const double *b, int n)
{
    double m = 0;
    for (int i = 0; i < n; i++) if (fabs(a[i] - b[i]) > m) m = fabs(a[i] - b[i]);
    return m;
}

static void test_against_direct(int n, int type)
{
    Transform t;
    CHECK(tr_init(&t, n, type) == 0, "tr_init(%d)", n);
    int span = tr_span(&t);
    double *x = malloc(span * sizeof *x), *y1 = malloc(span * sizeof *y1),
           *y2 = malloc(span * sizeof *y2), *X1 = malloc(n * sizeof *X1),
           *X2 = malloc(n * sizeof *X2);
    const char *name = type == TR_MDCT ? "MDCT" : "DCT";
    int reps = n <= 1024 ? 3 : 1;               /* the direct versions are slow */

    for (int r = 0; r < reps; r++) {
        for (int i = 0; i < span; i++) x[i] = urand();
        tr_forward(&t, x, X1);
        if (type == TR_MDCT) mdct_direct(x, X2, n); else dct2_direct(x, X2, n);
        double d = maxdiff(X1, X2, n);
        CHECK(d < 1e-9, "%s N=%d forward: fast vs direct differ by %g", name, n, d);

        for (int i = 0; i < n; i++) X1[i] = urand();
        tr_inverse(&t, X1, y1);
        if (type == TR_MDCT) imdct_direct(X1, y2, n); else dct3_direct(X1, y2, n);
        d = maxdiff(y1, y2, span);
        CHECK(d < 1e-9, "%s N=%d inverse: fast vs direct differ by %g", name, n, d);

        if (type == TR_DCT) {                   /* DCT: perfect reconstruction per block */
            tr_forward(&t, x, X1);
            tr_inverse(&t, X1, y1);
            d = maxdiff(x, y1, n);
            CHECK(d < 1e-9, "DCT N=%d: inverse(forward(x)) differs by %g", n, d);
        }
    }
    free(x); free(y1); free(y2); free(X1); free(X2);
    tr_free(&t);
}

/*
 * Whole-signal round trip with the codec's block layout:
 *   DCT:  ceil(F/N) blocks, block b = samples bN .. bN+N-1
 *   MDCT: ceil(F/N)+1 blocks, block b = samples (b-1)N .. (b+1)N-1
 * with zeros outside 0 .. F-1. Checks reconstruction and Parseval.
 */
static void test_signal(int n, int type, int frames)
{
    Transform t;
    tr_init(&t, n, type);
    int span = tr_span(&t);
    int mdct = type == TR_MDCT;
    int nblocks = (frames + n - 1) / n + (mdct ? 1 : 0);

    double *x   = malloc((frames + 1) * sizeof *x);
    double *out = calloc((size_t)(nblocks + 1) * n, sizeof *out);  /* out[i] = sample i */
    double *in  = malloc(span * sizeof *in), *y = malloc(span * sizeof *y);
    double *X   = malloc(n * sizeof *X);
    double es = 0, ec = 0;
    for (int i = 0; i < frames; i++) { x[i] = urand(); es += x[i] * x[i]; }

    for (int b = 0; b < nblocks; b++) {
        long start = mdct ? (long)(b - 1) * n : (long)b * n;
        for (int i = 0; i < span; i++) {
            long p = start + i;
            in[i] = (p >= 0 && p < frames) ? x[p] : 0.0;
        }
        tr_forward(&t, in, X);
        for (int k = 0; k < n; k++) ec += X[k] * X[k];
        tr_inverse(&t, X, y);
        for (int i = 0; i < span; i++) {
            long p = start + i;
            if (p >= 0) out[p] += y[i];         /* overlap-add (MDCT), plain copy (DCT) */
        }
    }
    double d = maxdiff(x, out, frames);
    CHECK(d < 1e-9, "%s N=%d F=%d: reconstruction error %g",
          mdct ? "MDCT" : "DCT", n, frames, d);
    CHECK(fabs(es - ec) <= 1e-9 * (es + 1), "%s N=%d F=%d: Parseval, signal %.12g coefs %.12g",
          mdct ? "MDCT" : "DCT", n, frames, es, ec);
    free(x); free(out); free(in); free(y); free(X);
    tr_free(&t);
}

static double now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

/* Experiment 7: time per block of the forward transform, fast vs direct. */
static void speed(void)
{
    printf("\nforward transform, time per block (microseconds)\n");
    printf("%-5s %6s %12s %12s %10s\n", "type", "N", "fft", "direct", "speedup");
    for (int type = 0; type <= 1; type++) {
        for (int n = 64; n <= TR_MAX_N; n *= 2) {
            Transform t;
            tr_init(&t, n, type);
            int span = tr_span(&t);
            double *x = malloc(span * sizeof *x), *X = malloc(n * sizeof *X);
            for (int i = 0; i < span; i++) x[i] = urand();

            int reps = 2000000 / n;
            double t0 = now();
            for (int r = 0; r < reps; r++) tr_forward(&t, x, X);
            double fast = (now() - t0) / reps;

            int dreps = n <= 512 ? 20 : n <= 2048 ? 3 : 1;
            t0 = now();
            for (int r = 0; r < dreps; r++) {
                if (type == TR_MDCT) mdct_direct(x, X, n); else dct2_direct(x, X, n);
            }
            double direct = (now() - t0) / dreps;

            printf("%-5s %6d %12.2f %12.1f %9.0fx\n", type ? "mdct" : "dct", n,
                   fast * 1e6, direct * 1e6, direct / fast);
            free(x); free(X);
            tr_free(&t);
        }
    }
}

int main(int argc, char **argv)
{
    Transform t;
    CHECK(tr_init(&t, 48, TR_DCT) != 0, "N=48 accepted");
    CHECK(tr_init(&t, 2 * TR_MAX_N, TR_DCT) != 0, "N too large accepted");

    for (int n = 64; n <= TR_MAX_N; n *= 2) {
        test_against_direct(n, TR_DCT);
        test_against_direct(n, TR_MDCT);
        int lens[] = {0, 1, n - 1, n, n + 1, 3 * n + 17};   /* frames F */
        for (int i = 0; i < 6; i++) {
            test_signal(n, TR_DCT, lens[i]);
            test_signal(n, TR_MDCT, lens[i]);
        }
    }

    if (argc > 1 && strcmp(argv[1], "--speed") == 0) speed();

    if (failures) { printf("test_transform: %d FAILED\n", failures); return 1; }
    printf("test_transform: all passed\n");
    return 0;
}
