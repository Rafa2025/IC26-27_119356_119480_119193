/*
 * test_rangecoder.c - round-trip tests for the range coder.
 *
 *   1. random bits, each with its own random fixed probability
 *   2. adaptive models: skewed bits spread over a few models, mixed with
 *      direct bits; also checks that adaptation actually compresses
 *   3. carry stress: probabilities near the extremes
 *   4. truncated stream: reading past the end sets the error flag
 *   5. empty stream (nothing coded)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "rangecoder.h"

static int failures = 0;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; \
    fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
    fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); } } while (0)

/* Small deterministic PRNG so failures are reproducible. */
static uint64_t rng = 88172645463325252ull;
static uint32_t rnd(void)
{
    rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17;
    return (uint32_t)(rng >> 16);
}

/* Read a whole tmpfile back into memory. */
static uint8_t *slurp(FILE *f, size_t *len)
{
    long n = ftell(f);
    uint8_t *b = malloc(n > 0 ? (size_t)n : 1);
    rewind(f);
    if (fread(b, 1, (size_t)n, f) != (size_t)n) { perror("fread"); exit(1); }
    *len = (size_t)n;
    return b;
}

static void test_fixed(int n)
{
    uint32_t *p = malloc(n * sizeof *p);
    uint8_t  *bits = malloc(n);
    for (int i = 0; i < n; i++) {
        p[i] = 1 + rnd() % (RC_PROB_ONE - 1);              /* 1 .. 2047 */
        bits[i] = (rnd() % RC_PROB_ONE) >= p[i];           /* P(0) = p/2048 */
    }

    FILE *f = tmpfile();
    RcEnc *e = malloc(sizeof *e);
    rc_enc_init(e, f);
    for (int i = 0; i < n; i++) rc_enc_bit_fixed(e, p[i], bits[i]);
    CHECK(rc_enc_finish(e) == 0, "write error");
    CHECK(rc_enc_bytes(e) == (uint64_t)ftell(f), "byte count");

    size_t len;
    uint8_t *buf = slurp(f, &len);
    RcDec d;
    rc_dec_init(&d, buf, len);
    int bad = 0;
    for (int i = 0; i < n; i++) bad += rc_dec_bit_fixed(&d, p[i]) != bits[i];
    CHECK(bad == 0, "fixed: %d of %d bits wrong", bad, n);
    CHECK(rc_dec_finish(&d) == 0, "fixed: stream not consumed exactly (pos %zu len %zu)",
          d.pos, len);

    free(buf); free(e); fclose(f); free(p); free(bits);
}

/* Bits from NM sources with different skews, coded with one model each,
   interleaved with direct bits of random width. */
#define NM 8
static void test_adaptive(int n)
{
    static const int skew[NM] = {2, 50, 200, 500, 1024, 1500, 1900, 2040};
    int      *src = malloc(n * sizeof *src);
    uint8_t  *bits = malloc(n);
    uint32_t *dv = malloc(n * sizeof *dv);
    int      *dn = malloc(n * sizeof *dn);
    for (int i = 0; i < n; i++) {
        src[i]  = rnd() % NM;
        bits[i] = (int)(rnd() % RC_PROB_ONE) >= skew[src[i]];
        dn[i]   = (i % 7 == 0) ? (int)(rnd() % 33) : 0;    /* 0..32 direct bits */
        dv[i]   = rnd() ^ (rnd() << 16);
        if (dn[i] < 32) dv[i] &= (1u << dn[i]) - 1u;
    }

    FILE *f = tmpfile();
    RcEnc *e = malloc(sizeof *e);
    Prob pm[NM];
    rc_prob_init(pm, NM);
    rc_enc_init(e, f);
    for (int i = 0; i < n; i++) {
        rc_enc_bit(e, &pm[src[i]], bits[i]);
        rc_enc_direct(e, dv[i], dn[i]);
    }
    CHECK(rc_enc_finish(e) == 0, "write error");

    size_t len;
    uint8_t *buf = slurp(f, &len);
    RcDec d;
    rc_dec_init(&d, buf, len);
    rc_prob_init(pm, NM);
    int bad = 0;
    for (int i = 0; i < n; i++) {
        bad += rc_dec_bit(&d, &pm[src[i]]) != bits[i];
        bad += rc_dec_direct(&d, dn[i]) != dv[i];
    }
    CHECK(bad == 0, "adaptive: %d mismatches", bad);
    CHECK(rc_dec_finish(&d) == 0, "adaptive: stream not consumed exactly");

    /* Compression check: ideal cost = sum of entropies + direct bits.
       The adaptive coder must be within 5% of it. */
    double ideal = 0;
    for (int i = 0; i < n; i++) {
        double p0 = skew[src[i]] / (double)RC_PROB_ONE;
        ideal += bits[i] ? -log2(1 - p0) : -log2(p0);
        ideal += dn[i];
    }
    double got = 8.0 * (double)len;
    printf("  adaptive: %zu bytes, %.1f%% above the entropy\n", len, 100 * (got / ideal - 1));
    CHECK(got < ideal * 1.05, "adaptive: %.0f bits, entropy %.0f", got, ideal);

    /* Truncation: drop the last byte, or half the stream. */
    for (int cut = 1; cut <= 2; cut++) {
        size_t tlen = cut == 1 ? len - 1 : len / 2;
        rc_dec_init(&d, buf, tlen);
        rc_prob_init(pm, NM);
        for (int i = 0; i < n; i++) {
            rc_dec_bit(&d, &pm[src[i]]);
            rc_dec_direct(&d, dn[i]);
        }
        CHECK(d.err == 1, "truncated to %zu of %zu bytes: error flag not set", tlen, len);
        CHECK(rc_dec_finish(&d) != 0, "truncated: finish reported success");
    }

    /* Extra byte at the end is also reported. */
    uint8_t *longer = malloc(len + 1);
    memcpy(longer, buf, len);
    longer[len] = 0;
    rc_dec_init(&d, longer, len + 1);
    rc_prob_init(pm, NM);
    for (int i = 0; i < n; i++) { rc_dec_bit(&d, &pm[src[i]]); rc_dec_direct(&d, dn[i]); }
    CHECK(d.err == 0 && rc_dec_finish(&d) != 0, "trailing byte not reported");

    free(longer); free(buf); free(e); fclose(f);
    free(src); free(bits); free(dv); free(dn);
}

/* Long runs of very likely / very unlikely bits produce many 0xFF bytes and
   carries, which is where a range coder usually breaks. */
static void test_carry(int n)
{
    uint8_t *bits = malloc(n);
    uint32_t *p = malloc(n * sizeof *p);
    for (int i = 0; i < n; i++) {
        int phase = (i / 1000) % 4;
        p[i] = phase == 0 ? 1 : phase == 1 ? RC_PROB_ONE - 1 : phase == 2 ? 2 : 1 + rnd() % 2047;
        bits[i] = phase < 2 ? (phase == 0) : (int)(rnd() & 1);
    }
    FILE *f = tmpfile();
    RcEnc *e = malloc(sizeof *e);
    rc_enc_init(e, f);
    for (int i = 0; i < n; i++) rc_enc_bit_fixed(e, p[i], bits[i]);
    rc_enc_finish(e);
    size_t len;
    uint8_t *buf = slurp(f, &len);
    RcDec d;
    rc_dec_init(&d, buf, len);
    int bad = 0;
    for (int i = 0; i < n; i++) bad += rc_dec_bit_fixed(&d, p[i]) != bits[i];
    CHECK(bad == 0 && rc_dec_finish(&d) == 0, "carry: %d bits wrong", bad);
    free(buf); free(e); fclose(f); free(bits); free(p);
}

static void test_empty(void)
{
    FILE *f = tmpfile();
    RcEnc *e = malloc(sizeof *e);
    rc_enc_init(e, f);
    rc_enc_finish(e);
    size_t len;
    uint8_t *buf = slurp(f, &len);
    CHECK(len == 5, "empty stream is %zu bytes, expected 5", len);
    RcDec d;
    rc_dec_init(&d, buf, len);
    CHECK(rc_dec_finish(&d) == 0, "empty stream not accepted");
    rc_dec_init(&d, buf, 0);
    CHECK(rc_dec_finish(&d) != 0, "zero-byte stream accepted");
    free(buf); free(e); fclose(f);
}

int main(void)
{
    for (int r = 0; r < 20; r++) test_fixed(1 + rnd() % 200000);
    test_adaptive(1000000);
    test_carry(400000);
    test_empty();
    if (failures) { printf("test_rangecoder: %d FAILED\n", failures); return 1; }
    printf("test_rangecoder: all passed\n");
    return 0;
}
