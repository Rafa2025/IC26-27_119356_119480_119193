/*
 * transform.h - orthonormal block transforms: DCT-II and MDCT.
 *
 * Both are orthonormal, so the squared error of the coefficients equals the
 * squared error of the samples (Parseval); see CODEC_LOSSY.md.
 *
 *   DCT-II   N samples  -> N coefficients   X[k] = sqrt(2/N) c_k sum x[n] cos(pi/N (n+1/2) k)
 *            inverse: DCT-III, N coefficients -> N samples.
 *
 *   MDCT     2N samples -> N coefficients   X[k] = sqrt(2/N) sum w[n] x[n] cos(pi/N (n+1/2+N/2)(k+1/2))
 *            inverse: N coefficients -> 2N samples, already multiplied by the
 *            window; the caller overlap-adds the first half with the second
 *            half of the previous block (TDAC).
 *            w[n] = sin(pi (n+1/2) / (2N)) is the sine window.
 *
 * FAST VERSIONS (tr_forward / tr_inverse), O(N log N):
 *   DCT-II  = N-point complex FFT of the reordered input [x0 x2 x4 ... x5 x3 x1],
 *             then a rotation by exp(-i pi k / 2N).
 *   MDCT    = fold the windowed 2N samples to N, then a DCT-IV computed with
 *             an N/2-point complex FFT (pre- and post-rotation).
 * DIRECT VERSIONS (*_direct), O(N^2): straight from the formulas above.
 * They are the reference for the tests and the speed comparison.
 *
 * N must be a power of two, 4 <= N <= TR_MAX_N.
 */
#ifndef TRANSFORM_H
#define TRANSFORM_H

#define TR_DCT  0
#define TR_MDCT 1
#define TR_MAX_N 8192

typedef struct {
    int     n;        /* coefficients per block */
    int     type;     /* TR_DCT or TR_MDCT */
    int     m;        /* FFT size: N for the DCT, N/2 for the MDCT */
    int    *rev;      /* bit-reversal permutation, m entries */
    double *tw;       /* FFT twiddles: cos and sin of 2 pi j / m, j < m/2 (interleaved) */
    double *rot;      /* DCT: exp(-i pi k / 2N), k < N
                         MDCT: pre-rotation exp(-i pi (n + 1/4) / N), n < N/2 (interleaved) */
    double *win;      /* MDCT only: sine window, 2N values */
    double *re, *im;  /* FFT work arrays, m values each */
    double *tmp;      /* N values */
} Transform;

/* Returns 0 on success, nonzero on a bad size or out of memory. */
int  tr_init(Transform *t, int n, int type);
void tr_free(Transform *t);

/* Number of input samples per block: N (DCT) or 2N (MDCT). */
static inline int tr_span(const Transform *t) { return t->type == TR_MDCT ? 2 * t->n : t->n; }

/* in: tr_span samples, out: N coefficients. in and out must not overlap. */
void tr_forward(Transform *t, const double *in, double *out);

/* in: N coefficients, out: tr_span samples (MDCT: windowed, to overlap-add). */
void tr_inverse(Transform *t, const double *in, double *out);

/* O(N^2) references with the same conventions. */
void dct2_direct(const double *x, double *X, int n);
void dct3_direct(const double *X, double *x, int n);
void mdct_direct(const double *x, double *X, int n);   /* x has 2N samples */
void imdct_direct(const double *X, double *y, int n);  /* y has 2N samples */

#endif /* TRANSFORM_H */
