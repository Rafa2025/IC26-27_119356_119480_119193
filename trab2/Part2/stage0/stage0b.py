#!/usr/bin/env python3
"""
stage0b.py - Stage 0b: bias correction and context design for the WGT predictor.

Unlike stage0.py (vectorised, static model), this simulates the future C codec
pixel by pixel, exactly in the order the decoder will see the data:
  - WGT predictor with integer maths (the version the C code will use)
  - optional bias correction per context (mean of recent errors, halved every 128)
  - token + extra bits, token coded with an adaptive frequency table per context
    (counts +32 per symbol, halved when the total passes 2^16, like a real coder)
The cost is the exact number of bits that adaptive model would spend.

Needs numba (pip install numba) for speed.

Usage:
  python3 stage0b.py ../../../ImageFiles [results/stage0.csv] > results/stage0b.txt
"""
import csv
import glob
import os
import sys
import time

import numpy as np
from numba import njit

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "..", "Part1"))
sys.path.insert(0, HERE)
from imgio import read_image  # noqa: E402
import stage0 as s0  # noqa: E402

NT = 64                 # token alphabet (enough for 16-bit residuals)
INC, LIMIT = 32, 1 << 16

# context modes
CTX_ACT, CTX_ERR, CTX_ERRMIN, CTX_COMB = 0, 1, 2, 3
# bias modes
BIAS_NONE, BIAS_GRAD, BIAS_CTX = 0, 1, 2


@njit(cache=True)
def bitlen(v):
    n = 0
    while v > 0:
        n += 1
        v >>= 1
    return n


@njit(cache=True)
def qlev(v, nlev, half):
    """Log-scale level of v >= 0. half=True uses half-octave steps (finer)."""
    if half:
        n = bitlen(v)
        q = 2 * n - 1 if n >= 2 and (v >> (n - 2)) & 1 else 2 * n
        if n < 2:
            q = n
    else:
        q = bitlen(v)
    return min(q, nlev - 1)


@njit(cache=True)
def qgrad(d, t1, t2, t3):
    if d <= -t3:
        return -4
    if d <= -t2:
        return -3
    if d <= -t1:
        return -2
    if d < 0:
        return -1
    if d == 0:
        return 0
    if d < t1:
        return 1
    if d < t2:
        return 2
    if d < t3:
        return 3
    return 4


@njit(cache=True)
def get(a, r, c, R, C):
    """Causal neighbour value with the same border fallbacks as stage0.py."""
    if r < 0 and c < 0:
        return 0
    if r < 0:                       # above the image: use W of the first row
        return a[0, c - 1] if c - 1 >= 0 and c - 1 < C else 0
    if c < 0:                       # left of the image: use N
        return a[r - 1, 0] if r - 1 >= 0 else 0
    if c >= C:                      # right of the image: use N
        return a[r, C - 1]
    return a[r, c]


@njit(cache=True)
def egot(e, r, c, R, C):
    """Stored error at (r, c), 0 outside the image."""
    if r < 0 or c < 0 or c >= C:
        return 0
    return e[r, c]


@njit(cache=True)
def code_plane(x, lo, hi, depth, ctx_mode, nlev, nlev2, half, bias_mode, prev_err):
    """prev_err: |error| of the colour planes already coded, at the same pixel (zeros for the first plane).
    Returns (bits, |error| of this plane)."""
    R, C = x.shape
    shift = max(0, depth - 8)
    scale = max(1, (hi - lo + 1) // 256)
    t1, t2, t3 = 3 * scale, 7 * scale, 21 * scale
    nctx = nlev * nlev2 if ctx_mode == CTX_COMB else nlev
    freq = np.ones((nctx, NT), np.int64)
    tot = np.full(nctx, NT, np.int64)
    nb_ctx = 365 if bias_mode == BIAS_GRAD else nctx
    bS = np.zeros(nb_ctx, np.int64)
    bN = np.zeros(nb_ctx, np.int64)
    esub = np.zeros((6, R, C), np.int64)       # |error| of each sub-predictor
    efin = np.zeros((R, C), np.int64)          # |error| of the final prediction
    bits = 0.0
    subs = np.zeros(6, np.int64)
    for r in range(R):
        for c in range(C):
            v = x[r, c]
            # neighbours (first row: N = W ; first column: W = N)
            if r == 0:
                W = x[r, c - 1] if c > 0 else 0
                N = W
                NW = W
                NE = W
            else:
                N = x[r - 1, c]
                W = x[r, c - 1] if c > 0 else N
                NW = x[r - 1, c - 1] if c > 0 else N
                NE = x[r - 1, c + 1] if c + 1 < C else N
            mx = max(W, N)
            mn = min(W, N)
            med = mn if NW >= mx else (mx if NW <= mn else W + N - NW)
            subs[0] = W
            subs[1] = N
            subs[2] = NE
            subs[3] = NW
            subs[4] = W + N - NW
            subs[5] = med
            # WGT: integer weights 2^40 / s^2, s = 1 + error of the sub-predictor around
            num = 0
            den = 0
            smin = 1 << 62
            for i in range(6):
                s = 1 + egot(esub[i], r, c - 1, R, C) + egot(esub[i], r - 1, c, R, C) \
                      + egot(esub[i], r - 1, c - 1, R, C) + egot(esub[i], r - 1, c + 1, R, C)
                smin = min(smin, s - 1)
                w = (1 << 40) // (s * s)
                if w < 1:
                    w = 1
                num += w * subs[i]
                den += w
            pred = (num + den // 2) // den
            pred = min(max(pred, lo), hi)

            # coding context
            act = (abs(W - NW) + abs(N - NW) + abs(NE - N)) >> shift
            eE = (egot(efin, r, c - 1, R, C) + egot(efin, r - 1, c, R, C)
                  + egot(efin, r - 1, c - 1, R, C) + egot(efin, r - 1, c + 1, R, C)
                  + prev_err[r, c]) >> shift
            if ctx_mode == CTX_ACT:
                ctx = qlev(act, nlev, half)
            elif ctx_mode == CTX_ERR:
                ctx = qlev(eE, nlev, half)
            elif ctx_mode == CTX_ERRMIN:
                ctx = qlev(smin >> shift, nlev, half)
            else:
                ctx = qlev(act, nlev, half) * nlev2 + qlev(eE, nlev2, half)

            # bias correction
            sign = 1
            bc = 0
            pred0 = pred
            if bias_mode != BIAS_NONE:
                if bias_mode == BIAS_GRAD:
                    q1 = qgrad(NE - N, t1, t2, t3)
                    q2 = qgrad(N - NW, t1, t2, t3)
                    q3 = qgrad(NW - W, t1, t2, t3)
                    if q1 < 0 or (q1 == 0 and (q2 < 0 or (q2 == 0 and q3 < 0))):
                        q1, q2, q3 = -q1, -q2, -q3
                        sign = -1
                    bc = (q1 * 9 + q2) * 9 + q3          # 0..364 after sign merging
                else:
                    bc = ctx
                if bN[bc] > 0:
                    n2 = 2 * bN[bc]
                    corr = (2 * bS[bc] + bN[bc]) // n2
                    pred = min(max(pred + sign * corr, lo), hi)
            res = v - pred
            if bias_mode != BIAS_NONE:
                # accumulate the error of the uncorrected prediction: its mean is the bias
                bS[bc] += sign * (v - pred0)
                bN[bc] += 1
                if bN[bc] >= 128:
                    bS[bc] //= 2
                    bN[bc] //= 2
            res = sign * res

            # token + extra bits, adaptive frequency model per context
            u = 2 * res if res >= 0 else -2 * res - 1
            if u < 16:
                t = u
                eb = 0
            else:
                n = bitlen(u) - 1
                t = 16 + (n - 4) * 2 + ((u >> (n - 1)) & 1)
                eb = n - 1
            bits += np.log2(tot[ctx]) - np.log2(freq[ctx, t]) + eb
            freq[ctx, t] += INC
            tot[ctx] += INC
            if tot[ctx] > LIMIT:
                tt = 0
                for k in range(NT):
                    freq[ctx, k] = (freq[ctx, k] + 1) // 2
                    tt += freq[ctx, k]
                tot[ctx] = tt

            # store errors for the next pixels
            for i in range(6):
                esub[i, r, c] = abs(v - subs[i])
            efin[r, c] = abs(v - pred)
    return bits, efin


CONFIGS = [
    # name, ctx_mode, nlev, nlev2, half, bias, cross-plane error (0 no, 1 first plane, 2 all earlier)
    ("A  activity 12 (stage 0 model)", CTX_ACT, 12, 1, False, BIAS_NONE, 0),
    ("B  activity 12 + bias (365 gradient ctx)", CTX_ACT, 12, 1, False, BIAS_GRAD, 0),
    ("C  activity 12 + bias (coding ctx)", CTX_ACT, 12, 1, False, BIAS_CTX, 0),
    ("D  error 12", CTX_ERR, 12, 1, False, BIAS_NONE, 0),
    ("E  best-sub-error 12", CTX_ERRMIN, 12, 1, False, BIAS_NONE, 0),
    ("F  error 16 (half-octave)", CTX_ERR, 16, 1, True, BIAS_NONE, 0),
    ("G  error 24 (half-octave)", CTX_ERR, 24, 1, True, BIAS_NONE, 0),
    ("H  error 8", CTX_ERR, 8, 1, False, BIAS_NONE, 0),
    ("I  activity 6 x error 6", CTX_COMB, 6, 6, False, BIAS_NONE, 0),
    ("J  activity 8 x error 8", CTX_COMB, 8, 8, False, BIAS_NONE, 0),
    ("K  error 12 + bias (365 gradient ctx)", CTX_ERR, 12, 1, False, BIAS_GRAD, 0),
    ("L  activity 6 x error 6 + bias (365 gradient ctx)", CTX_COMB, 6, 6, False, BIAS_GRAD, 0),
    ("M  best-sub-error 12 + bias (365 gradient ctx)", CTX_ERRMIN, 12, 1, False, BIAS_GRAD, 0),
    ("N  activity 8 x error 8 + bias (365 gradient ctx)", CTX_COMB, 8, 8, False, BIAS_GRAD, 0),
    ("O  error 16 (half-octave) + bias (365 gradient ctx)", CTX_ERR, 16, 1, True, BIAS_GRAD, 0),
    ("P  activity 8 x error 8, error includes 1st colour plane", CTX_COMB, 8, 8, False, BIAS_NONE, 1),
    ("Q  activity 8 x error 8, error includes all earlier planes", CTX_COMB, 8, 8, False, BIAS_NONE, 2),
]


def planes_of(img, csvrow):
    if img.channels == 1:
        return [(img.data.astype(np.int64), 0, img.maxval)], "gray"
    tfs = ("none", "RCT", "YCoCg-R")
    tf = min(tfs, key=lambda k: float(csvrow[f"{k}/WGT"]))       # the per-image choice (D7)
    return [(np.ascontiguousarray(p), lo, hi) for p, lo, hi in s0.colour_planes(img.data, img.maxval, tf)], tf


def main():
    folder = sys.argv[1]
    csvpath = sys.argv[2] if len(sys.argv) > 2 else os.path.join(HERE, "results", "stage0.csv")
    rows = {r["file"].replace("\\", "/").split("/")[-1]: r for r in csv.DictReader(open(csvpath))}
    groups = {
        "pgm8": sorted(glob.glob(os.path.join(folder, "imgs-pgm8", "*.pgm"))),
        "raw16": sorted(glob.glob(os.path.join(folder, "imgs-raw16", "*.raw"))),
        "ppm natural": [f for f in sorted(glob.glob(os.path.join(folder, "imgs-ppm", "*.ppm")))
                        if not f.endswith("arial.ppm")],
    }
    for g, files in groups.items():
        colour = g.startswith("ppm")
        tot = {c[0]: 0.0 for c in CONFIGS if colour or not c[-1]}
        ref = {"JPEG-LS": 0.0, "JPEG-XL": 0.0}
        npx = 0
        t0 = time.time()
        for f in files:
            img = read_image(f)
            depth = img.bpp if img.fmt == "raw" else int(img.maxval).bit_length()
            row = rows[os.path.basename(f)]
            planes, _ = planes_of(img, row)
            n = img.rows * img.cols
            npx += n
            for name, cm, nl, nl2, half, bias, xc in CONFIGS:
                if xc and len(planes) == 1:
                    continue                         # cross-plane context: colour only
                prev = np.zeros(planes[0][0].shape, np.int64)
                for i, (p, lo, hi) in enumerate(planes):
                    b, e = code_plane(p, lo, hi, depth, cm, nl, nl2, half, bias, prev)
                    tot[name] += b
                    if xc == 1 and i == 0:           # first plane's error only
                        prev = e
                    elif xc == 2:                    # errors of all planes coded so far
                        prev = prev + e
            for k in ref:
                ref[k] += float(row[k]) * n
        print(f"\n## {g} ({len(files)} images, {time.time() - t0:.0f}s)\n")
        print("| option | bpp | vs A |")
        print("|---|---:|---:|")
        base = tot[CONFIGS[0][0]]
        for name, b in sorted(list(tot.items()) + list(ref.items()), key=lambda kv: kv[1]):
            print(f"| {name} | {b / npx:.3f} | {100 * (b / base - 1):+.1f}% |")
        sys.stdout.flush()


if __name__ == "__main__":
    main()
