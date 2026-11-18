/*
 * test_rc.c - unit tests for the adaptive range coder (rc.h).
 *
 *   ./test_rc            run all tests, print overhead and speed
 *
 * 1. round trip of residuals drawn from Laplacian-like distributions whose
 *    width changes with the context (like real prediction residuals)
 * 2. coder overhead: actual bytes vs the ideal cost of the same adaptive model
 * 3. edge cases: no symbols, one symbol, a constant stream, the largest
 *    residuals, raw bits of every width
 * 4. garbage input: decoding random bytes must not crash or read out of bounds
 */
#define _POSIX_C_SOURCE 199309L
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include "rc.h"

static int failures = 0;
#define CHECK(c, ...)                                                                              \
    do {                                                                                           \
        if (!(c)) {                                                                                \
            printf("  FAIL: " __VA_ARGS__);                                                        \
            printf("\n");                                                                          \
            failures++;                                                                            \
        }                                                                                          \
    } while (0)

static uint64_t rng_state = 88172645463325252ull;
static uint32_t rnd(void)
{
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 7;
    rng_state ^= rng_state << 17;
    return (uint32_t)(rng_state >> 11);
}
static double urand(void) { return (rnd() + 0.5) / 2097152.0 / 2048.0; }

/* Laplacian residual with scale b, rounded. */
static int32_t laplace(double b)
{
    double u = urand() - 0.5;
    double x = -b * (u < 0 ? -1 : 1) * log(1 - 2 * fabs(u));
    double lim = 131071;
    if (x > lim) x = lim;
    if (x < -lim) x = -lim;
    return (int32_t)lrint(x);
}

static double now(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec * 1e-9;
}

/* ideal cost of one residual under model m (bits), without updating it */
static double ideal_bits(const Model *m, int32_t r)
{
    uint32_t u = r >= 0 ? 2u * (uint32_t)r : 2u * (uint32_t)(-(r + 1)) + 1u;
    int t = (int)u, eb = 0;
    if (u >= 16) {
        int n = 31 - __builtin_clz(u);
        t = 16 + 2 * (n - 4) + (int)((u >> (n - 1)) & 1);
        eb = n - 1;
    }
    return log2((double)m->tot) - log2((double)m->f[t]) + eb;
}

static int token_of(int32_t r)
{
    uint32_t u = r >= 0 ? 2u * (uint32_t)r : 2u * (uint32_t)(-(r + 1)) + 1u;
    if (u < 16) return (int)u;
    int n = 31 - __builtin_clz(u);
    return 16 + 2 * (n - 4) + (int)((u >> (n - 1)) & 1);
}

/* encode n residuals in nctx contexts, decode, compare; returns bytes */
static size_t roundtrip(const int32_t *r, const int *ctx, size_t n, int nctx, double *ideal,
                        double *tenc, double *tdec, const char *name)
{
    Model *me = malloc(nctx * sizeof *me), *md = malloc(nctx * sizeof *md);
    for (int i = 0; i < nctx; i++) {
        model_init(&me[i]);
        model_init(&md[i]);
    }
    Buf b = {0};
    RcEnc e;
    rce_init(&e, &b);
    double t0 = now(), id = 0;
    for (size_t i = 0; i < n; i++) {
        if (ideal) id += ideal_bits(&me[ctx[i]], r[i]);
        rce_residual(&e, &me[ctx[i]], r[i]);
    }
    rce_flush(&e);
    double t1 = now();
    RcDec d;
    rcd_init(&d, b.p, b.n);
    size_t bad = 0;
    for (size_t i = 0; i < n; i++)
        bad += rcd_residual(&d, &md[ctx[i]]) != r[i];
    double t2 = now();
    CHECK(bad == 0, "%s: %zu of %zu residuals decoded wrong", name, bad, n);
    CHECK(!d.overrun, "%s: decoder read past the end", name);
    CHECK(!b.oom, "%s: out of memory", name);
    if (ideal) *ideal = id;
    if (tenc) *tenc = t1 - t0;
    if (tdec) *tdec = t2 - t1;
    size_t bytes = b.n;
    buf_free(&b);
    free(me);
    free(md);
    return bytes;
}

int main(void)
{
    /* 1 + 2: realistic stream */
    size_t n = 8000000;
    int nctx = 64;
    int32_t *r = malloc(n * sizeof *r);
    int *ctx = malloc(n * sizeof *ctx);
    for (size_t i = 0; i < n; i++) {
        ctx[i] = (int)(rnd() % nctx);
        double b = 0.3 * pow(1.25, ctx[i] % 32) * ((ctx[i] >= 32) ? 40 : 1);  /* 8- and 16-bit-like */
        r[i] = laplace(b);
    }
    double ideal, te, td;
    size_t bytes = roundtrip(r, ctx, n, nctx, &ideal, &te, &td, "Laplacian stream");
    double over = 100.0 * (bytes * 8.0 / ideal - 1);
    printf("1-2. %zu residuals, 64 contexts: %zu bytes, ideal %.0f bytes, overhead %+.4f%%\n", n, bytes,
           ideal / 8, over);
    printf("     encode %.1f M residuals/s, decode %.1f M residuals/s\n", n / te / 1e6, n / td / 1e6);
    CHECK(over < 0.1, "overhead %.4f%% is above 0.1%%", over);

    /* 3: edge cases */
    int zero_ctx[4] = {0, 0, 0, 0};
    roundtrip(NULL, zero_ctx, 0, 1, NULL, NULL, NULL, "no symbols");
    int32_t one = -5;
    roundtrip(&one, zero_ctx, 1, 1, NULL, NULL, NULL, "one symbol");
    for (size_t i = 0; i < n; i++) {
        r[i] = 0;
        ctx[i] = 0;
    }
    size_t cbytes = roundtrip(r, ctx, n, 1, NULL, NULL, NULL, "constant stream");
    printf("3.   constant stream of %zu zeros: %zu bytes (%.5f bits/residual)\n", n, cbytes,
           cbytes * 8.0 / n);
    size_t m = 0;
    int32_t ext[] = {0, -1, 1, 7, -8, 8, 15, 16, -17, 255, -256, 65535, -65536, 131071, -131072,
                     (1 << 23) - 1, -(1 << 23), (1 << 24) - 1, -(1 << 24)};
    int ne = (int)(sizeof ext / sizeof ext[0]);
    for (int rep = 0; rep < 1000; rep++)
        for (int k = 0; k < ne; k++) {
            r[m] = ext[k];
            ctx[m++] = k % 3;
        }
    roundtrip(r, ctx, m, 3, NULL, NULL, NULL, "largest residuals");
    int maxtok = 0;
    for (int k = 0; k < ne; k++)
        if (token_of(ext[k]) > maxtok) maxtok = token_of(ext[k]);
    CHECK(maxtok < NSYM, "token %d out of range", maxtok);
    printf("     largest residuals (|r| up to 2^24): ok, largest token %d of %d\n", maxtok, NSYM);
    /* raw bits of every width */
    {
        Buf b = {0};
        RcEnc e;
        rce_init(&e, &b);
        uint32_t vals[17 * 50];
        for (int w = 0, k = 0; w <= 16; w++)
            for (int j = 0; j < 50; j++, k++) {
                vals[k] = w ? rnd() & ((1u << w) - 1) : 0;
                rce_bits(&e, vals[k], w);
            }
        rce_flush(&e);
        RcDec d;
        rcd_init(&d, b.p, b.n);
        int bad = 0;
        for (int w = 0, k = 0; w <= 16; w++)
            for (int j = 0; j < 50; j++, k++)
                bad += rcd_bits(&d, w) != vals[k];
        CHECK(bad == 0 && !d.overrun, "raw bits: %d wrong", bad);
        printf("     raw bits, widths 0..16: ok\n");
        buf_free(&b);
    }

    /* 4: garbage input */
    {
        size_t len = 4096;
        uint8_t *g = malloc(len);
        Model mm[4];
        long sum = 0;
        for (int trial = 0; trial < 200; trial++) {
            for (size_t i = 0; i < len; i++)
                g[i] = (uint8_t)rnd();
            for (int i = 0; i < 4; i++)
                model_init(&mm[i]);
            RcDec d;
            rcd_init(&d, g, trial % 2 ? len : (size_t)(rnd() % 16));
            for (int i = 0; i < 20000; i++)
                sum += rcd_residual(&d, &mm[i & 3]);
        }
        printf("4.   200 random inputs decoded without crashing (checksum %ld)\n", sum & 0xFFFF);
        free(g);
    }

    free(r);
    free(ctx);
    printf(failures ? "\n%d FAILED\n" : "\nALL PASSED\n", failures);
    return failures != 0;
}
