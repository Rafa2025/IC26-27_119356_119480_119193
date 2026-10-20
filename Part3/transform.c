/* transform.c - FFT-based DCT-II and MDCT, and their direct references. */
#include <math.h>
#include <stdlib.h>
#include "transform.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* ------------------------------------------------------------------ */
/* Radix-2 complex FFT (forward, e^{-2 pi i nk/m}), in place           */
/* ------------------------------------------------------------------ */
static void fft(const Transform *t, double *re, double *im)
{
    const int m = t->m;

    for (int i = 0; i < m; i++) {               /* bit-reversed order */
        int j = t->rev[i];
        if (j > i) {
            double a = re[i]; re[i] = re[j]; re[j] = a;
            a = im[i]; im[i] = im[j]; im[j] = a;
        }
    }
    for (int len = 2; len <= m; len <<= 1) {     /* butterflies */
        int half = len >> 1, step = m / len;
        for (int i = 0; i < m; i += len) {
            for (int j = 0; j < half; j++) {
                double wr = t->tw[2 * j * step], wi = -t->tw[2 * j * step + 1];
                int a = i + j, b = a + half;
                double xr = re[b] * wr - im[b] * wi;
                double xi = re[b] * wi + im[b] * wr;
                re[b] = re[a] - xr; im[b] = im[a] - xi;
                re[a] += xr;        im[a] += xi;
            }
        }
    }
}

/* ------------------------------------------------------------------ */
/* Setup                                                               */
/* ------------------------------------------------------------------ */
int tr_init(Transform *t, int n, int type)
{
    *t = (Transform){0};
    if (n < 4 || n > TR_MAX_N || (n & (n - 1)) != 0) return 1;
    t->n = n;
    t->type = type;
    t->m = type == TR_MDCT ? n / 2 : n;
    const int m = t->m;

    t->rev = malloc(m * sizeof *t->rev);
    t->tw  = malloc(m * sizeof *t->tw);           /* m/2 complex values */
    t->rot = malloc(2 * n * sizeof *t->rot);      /* n complex values */
    t->re  = malloc(m * sizeof *t->re);
    t->im  = malloc(m * sizeof *t->im);
    t->tmp = malloc(n * sizeof *t->tmp);
    if (type == TR_MDCT) t->win = malloc(2 * n * sizeof *t->win);
    if (!t->rev || !t->tw || !t->rot || !t->re || !t->im || !t->tmp ||
        (type == TR_MDCT && !t->win)) {
        tr_free(t);
        return 1;
    }

    int bits = 0;
    while ((1 << bits) < m) bits++;
    for (int i = 0; i < m; i++) {
        int r = 0;
        for (int b = 0; b < bits; b++) r |= ((i >> b) & 1) << (bits - 1 - b);
        t->rev[i] = r;
    }
    for (int j = 0; j < m / 2; j++) {
        t->tw[2 * j]     = cos(2 * M_PI * j / m);
        t->tw[2 * j + 1] = sin(2 * M_PI * j / m);
    }

    if (type == TR_DCT) {
        for (int k = 0; k < n; k++) {             /* exp(-i pi k / 2N) */
            t->rot[2 * k]     = cos(M_PI * k / (2.0 * n));
            t->rot[2 * k + 1] = -sin(M_PI * k / (2.0 * n));
        }
    } else {
        for (int j = 0; j < m; j++) {
            /* pre-rotation exp(-i pi (j + 1/4) / N) in rot[0 .. N-1] */
            t->rot[2 * j]     = cos(M_PI * (j + 0.25) / n);
            t->rot[2 * j + 1] = -sin(M_PI * (j + 0.25) / n);
            /* post-rotation exp(-i pi j / N) in rot[N .. 2N-1] */
            t->rot[n + 2 * j]     = cos(M_PI * j / n);
            t->rot[n + 2 * j + 1] = -sin(M_PI * j / n);
        }
        for (int i = 0; i < 2 * n; i++) t->win[i] = sin(M_PI * (i + 0.5) / (2.0 * n));
    }
    return 0;
}

void tr_free(Transform *t)
{
    free(t->rev); free(t->tw); free(t->rot); free(t->win);
    free(t->re);  free(t->im); free(t->tmp);
    *t = (Transform){0};
}

/* ------------------------------------------------------------------ */
/* DCT-II / DCT-III through an N-point FFT                             */
/* ------------------------------------------------------------------ */
/*
 * Reorder v = [x0 x2 x4 ... x5 x3 x1]; then
 *     sum_n x[n] cos(pi (2n+1) k / 2N) = Re( exp(-i pi k / 2N) * FFT(v)[k] ).
 */
static void dct2_fast(Transform *t, const double *x, double *X)
{
    const int n = t->n;
    double *re = t->re, *im = t->im;
    for (int i = 0; i < n / 2; i++) {
        re[i]         = x[2 * i];
        re[n - 1 - i] = x[2 * i + 1];
    }
    for (int i = 0; i < n; i++) im[i] = 0;
    fft(t, re, im);

    const double s = sqrt(2.0 / n);
    for (int k = 0; k < n; k++)
        X[k] = s * (re[k] * t->rot[2 * k] - im[k] * t->rot[2 * k + 1]);
    X[0] *= M_SQRT1_2;
}

/*
 * Inverse of the above. With X'[k] the unscaled DCT-II and X'[N] = 0,
 *     FFT(v)[k] = exp(+i pi k / 2N) * (X'[k] - i X'[N-k]),
 * and v = IFFT, computed as conj(FFT(conj(.))) / N; only the real part is used.
 */
static void dct3_fast(Transform *t, const double *X, double *x)
{
    const int n = t->n;
    double *re = t->re, *im = t->im;
    const double s = sqrt(n / 2.0) / n;           /* undo sqrt(2/N), then the 1/N of the IFFT */

    for (int k = 0; k < n; k++) {
        double zr = X[k] * (k == 0 ? M_SQRT2 : 1.0);
        double zi = k == 0 ? 0.0 : -X[n - k];
        double c = t->rot[2 * k], sn = -t->rot[2 * k + 1];   /* exp(+i pi k / 2N) */
        re[k] = s * (c * zr - sn * zi);
        im[k] = -s * (sn * zr + c * zi);                     /* conjugate */
    }
    fft(t, re, im);
    for (int i = 0; i < n / 2; i++) {
        x[2 * i]     = re[i];
        x[2 * i + 1] = re[n - 1 - i];
    }
}

/* ------------------------------------------------------------------ */
/* DCT-IV through an N/2-point FFT (orthonormal, its own inverse)      */
/* ------------------------------------------------------------------ */
/*
 * t[j] = (u[2j] + i u[N-1-2j]) exp(-i pi (j + 1/4) / N), T = FFT(t),
 * Y[k] = T[k] exp(-i pi k / N); then X[2k] = Re Y[k], X[N-1-2k] = -Im Y[k].
 */
static void dct4_fast(Transform *t, const double *u, double *X)
{
    const int n = t->n, m = t->m;
    const double *pre = t->rot, *post = t->rot + n;
    double *re = t->re, *im = t->im;

    for (int j = 0; j < m; j++) {
        double a = u[2 * j], b = u[n - 1 - 2 * j];
        re[j] = a * pre[2 * j] - b * pre[2 * j + 1];
        im[j] = a * pre[2 * j + 1] + b * pre[2 * j];
    }
    fft(t, re, im);
    const double s = sqrt(2.0 / n);
    for (int k = 0; k < m; k++) {
        double yr = re[k] * post[2 * k] - im[k] * post[2 * k + 1];
        double yi = re[k] * post[2 * k + 1] + im[k] * post[2 * k];
        X[2 * k]         = s * yr;
        X[n - 1 - 2 * k] = -s * yi;
    }
}

/* ------------------------------------------------------------------ */
/* MDCT = window, fold 2N -> N, DCT-IV                                 */
/* ------------------------------------------------------------------ */
/*
 * With z = w.x split in quarters [a b c d] of N/2 samples each, the MDCT is
 * the DCT-IV of  u = [ -c_r - d ,  a - b_r ]   (_r = reversed).
 * The inverse is the same DCT-IV followed by the opposite unfolding
 * (y = [ u2, -u2_r, -u1_r, -u1 ] with u = [u1 u2]) and the window.
 */
static void mdct_fast(Transform *t, const double *x, double *X)
{
    const int n = t->n, h = n / 2;
    const double *w = t->win;
    double *u = t->tmp;
    for (int j = 0; j < h; j++) {
        int p = 3 * h - 1 - j, q = 3 * h + j;
        u[j]     = -w[p] * x[p] - w[q] * x[q];
        u[h + j] =  w[j] * x[j] - w[n - 1 - j] * x[n - 1 - j];
    }
    dct4_fast(t, u, X);
}

static void imdct_fast(Transform *t, const double *X, double *y)
{
    const int n = t->n, h = n / 2;
    const double *w = t->win;
    double *u = t->tmp;
    dct4_fast(t, X, u);
    for (int j = 0; j < h; j++) {
        y[j]             =  w[j] * u[h + j];
        y[n - 1 - j]     = -w[n - 1 - j] * u[h + j];
        y[3 * h - 1 - j] = -w[3 * h - 1 - j] * u[j];
        y[3 * h + j]     = -w[3 * h + j] * u[j];
    }
}

void tr_forward(Transform *t, const double *in, double *out)
{
    if (t->type == TR_MDCT) mdct_fast(t, in, out);
    else                    dct2_fast(t, in, out);
}

void tr_inverse(Transform *t, const double *in, double *out)
{
    if (t->type == TR_MDCT) imdct_fast(t, in, out);
    else                    dct3_fast(t, in, out);
}

/* ------------------------------------------------------------------ */
/* Direct O(N^2) references                                            */
/* ------------------------------------------------------------------ */
/*
 * The cosine arguments are reduced exactly with integers before calling
 * cos(), so the references stay accurate for large N:
 *   DCT:  pi (2n+1) k / (2N)            -> index mod 4N into cos(pi j / 2N)
 *   MDCT: pi (2n+1+N)(2k+1) / (4N)      -> index mod 8N into cos(pi j / 4N)
 */
static double *cos_table(int len, double div)
{
    double *c = malloc(len * sizeof *c);
    if (!c) abort();
    for (int j = 0; j < len; j++) c[j] = cos(M_PI * j / div);
    return c;
}

void dct2_direct(const double *x, double *X, int n)
{
    double *c = cos_table(4 * n, 2.0 * n);
    for (int k = 0; k < n; k++) {
        double s = 0;
        for (int i = 0; i < n; i++) s += x[i] * c[((long long)(2 * i + 1) * k) % (4 * n)];
        X[k] = sqrt(2.0 / n) * (k == 0 ? M_SQRT1_2 : 1.0) * s;
    }
    free(c);
}

void dct3_direct(const double *X, double *x, int n)
{
    double *c = cos_table(4 * n, 2.0 * n);
    for (int i = 0; i < n; i++) {
        double s = 0;
        for (int k = 0; k < n; k++)
            s += (k == 0 ? M_SQRT1_2 : 1.0) * X[k] * c[((long long)(2 * i + 1) * k) % (4 * n)];
        x[i] = sqrt(2.0 / n) * s;
    }
    free(c);
}

static double sine_window(int i, int n) { return sin(M_PI * (i + 0.5) / (2.0 * n)); }

void mdct_direct(const double *x, double *X, int n)
{
    double *c = cos_table(8 * n, 4.0 * n);
    double *z = malloc(2 * n * sizeof *z);
    if (!z) abort();
    for (int i = 0; i < 2 * n; i++) z[i] = sine_window(i, n) * x[i];
    for (int k = 0; k < n; k++) {
        double s = 0;
        for (int i = 0; i < 2 * n; i++)
            s += z[i] * c[((long long)(2 * i + 1 + n) * (2 * k + 1)) % (8 * n)];
        X[k] = sqrt(2.0 / n) * s;
    }
    free(z);
    free(c);
}

void imdct_direct(const double *X, double *y, int n)
{
    double *c = cos_table(8 * n, 4.0 * n);
    for (int i = 0; i < 2 * n; i++) {
        double s = 0;
        for (int k = 0; k < n; k++)
            s += X[k] * c[((long long)(2 * i + 1 + n) * (2 * k + 1)) % (8 * n)];
        y[i] = sine_window(i, n) * sqrt(2.0 / n) * s;
    }
    free(c);
}
