#!/usr/bin/env python3
"""
stage0.py - Stage 0 of the lossless image codec: estimate, in Python, how many
bits per pixel each design option would cost, before writing any C.

Model of the future coder (the same for every option, so they compare fairly):
  residual r = x - prediction
  u = zigzag(r)                       (0, -1, 1, -2, ... -> 0, 1, 2, 3, ...)
  u -> token + raw extra bits         (u < 16: token u; else exponent + 1 mantissa bit)
  token coded with an adaptive model per context, context = 12 levels of local
  activity |W-NW| + |N-NW| + |NE-N| (log scale)
  cost = H(token | context) * pixels + sum(extra bits)

This is a static estimate (the adaptive coder pays a little to learn its
tables, and gains a little by adapting inside the image), so it is good for
ranking options, not for predicting the final file size to the byte.

Options measured:
  predictors   W, N, GRAD (W+N-NW), AVG ((W+N)/2), MED (JPEG-LS), GAP (CALIC),
               WGT (6 sub-predictors weighted by their recent local error)
  colour (PPM) none, subG (G, R-G, B-G), RCT (JPEG 2000), YCoCg-R,
               ICP (inter-channel: R and B residuals minus G's residual)
  packing      remap the values actually used to 0..K-1 (grayscale only)
  selection    best colour transform per image, and per 256x256 tile
References: JPEG-LS (CharLS) and JPEG-XL lossless (libjxl, effort 7) through
the imagecodecs package, plus xz -9 and zstd -19 on the raw pixel bytes.

Usage:
  python3 stage0.py ../../../ImageFiles -o results          # all images
  python3 stage0.py ../../../ImageFiles --quick             # skip references
"""
import argparse
import csv
import glob
import lzma
import os
import sys
import time

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "..", "Part1"))
from imgio import read_image  # noqa: E402

N_CTX = 12
TILE = 256
PREDICTORS = ["W", "N", "GRAD", "AVG", "MED", "GAP", "WGT"]
TRANSFORMS = ["none", "subG", "RCT", "YCoCg-R", "ICP"]


# ------------------------------------------------------------------ neighbours
def _nb(x, dy, dx):
    """y[r,c] = x[r+dy, c+dx] where that pixel exists (dy <= 0), plus a validity mask."""
    R, C = x.shape
    out = np.zeros_like(x)
    ok = np.zeros(x.shape, bool)
    r0, c0, c1 = -dy, max(0, -dx), C - max(0, dx)
    if r0 < R and c0 < c1:
        out[r0:, c0:c1] = x[:R - r0, c0 + dx:c1 + dx]
        ok[r0:, c0:c1] = True
    return out, ok


def neighbours(x):
    """Causal neighbours with simple fallbacks at the borders (only already coded pixels)."""
    W, mW = _nb(x, 0, -1)
    N, mN = _nb(x, -1, 0)
    NW, mNW = _nb(x, -1, -1)
    NE, mNE = _nb(x, -1, 1)
    WW, mWW = _nb(x, 0, -2)
    NN, mNN = _nb(x, -2, 0)
    NNE, mNNE = _nb(x, -2, 1)
    N = np.where(mN, N, W)          # first row: use W
    W = np.where(mW, W, N)          # first column: use N
    NW = np.where(mNW, NW, N)
    NE = np.where(mNE, NE, N)
    WW = np.where(mWW, WW, W)
    NN = np.where(mNN, NN, N)
    NNE = np.where(mNNE, NNE, NE)
    return dict(W=W, N=N, NW=NW, NE=NE, WW=WW, NN=NN, NNE=NNE)


# ------------------------------------------------------------------ predictors
def med(W, N, NW):
    mx, mn = np.maximum(W, N), np.minimum(W, N)
    return np.where(NW >= mx, mn, np.where(NW <= mn, mx, W + N - NW))


def gap(nb, scale):
    W, N, NW, NE, WW, NN, NNE = (nb[k] for k in ("W", "N", "NW", "NE", "WW", "NN", "NNE"))
    dh = np.abs(W - WW) + np.abs(N - NW) + np.abs(N - NE)
    dv = np.abs(W - NW) + np.abs(N - NN) + np.abs(NE - NNE)
    d = dv - dh
    base = (2 * (W + N) + (NE - NW)) // 4
    p = base
    p = np.where(d > 8 * scale, (3 * base + W) // 4, p)
    p = np.where(d > 32 * scale, (base + W) // 2, p)
    p = np.where(d > 80 * scale, W, p)
    p = np.where(d < -8 * scale, (3 * base + N) // 4, p)
    p = np.where(d < -32 * scale, (base + N) // 2, p)
    p = np.where(d < -80 * scale, N, p)
    return p


def weighted(x, nb):
    """Blend of sub-predictors, each weighted by 1/(its error at W,N,NW,NE)^2."""
    W, N, NW, NE = nb["W"], nb["N"], nb["NW"], nb["NE"]
    subs = [W, N, NE, NW, W + N - NW, med(W, N, NW)]
    num = np.zeros(x.shape, np.float64)
    den = np.zeros(x.shape, np.float64)
    for p in subs:
        e = np.abs(x - p)
        s = 1.0
        for dy, dx in ((0, -1), (-1, 0), (-1, -1), (-1, 1)):
            s = s + _nb(e, dy, dx)[0]
        w = 1.0 / (s * s)
        num += w * p
        den += w
    return np.floor(num / den + 0.5).astype(np.int64)


def predict(x, name, lo, hi, nb, scale):
    W, N, NW = nb["W"], nb["N"], nb["NW"]
    if name == "W":
        p = W
    elif name == "N":
        p = N
    elif name == "GRAD":
        p = W + N - NW
    elif name == "AVG":
        p = (W + N) // 2
    elif name == "MED":
        p = med(W, N, NW)
    elif name == "GAP":
        p = gap(nb, scale)
    elif name == "WGT":
        p = weighted(x, nb)
    else:
        raise ValueError(name)
    return np.clip(p, lo, hi)


def context(nb, shift):
    a = np.abs(nb["W"] - nb["NW"]) + np.abs(nb["N"] - nb["NW"]) + np.abs(nb["NE"] - nb["N"])
    a = a >> shift
    b = np.zeros(a.shape, np.int64)
    nz = a > 0
    b[nz] = np.floor(np.log2(a[nz])).astype(np.int64) + 1
    return np.minimum(b, N_CTX - 1)


# ------------------------------------------------------------------ cost model
def tokens(r):
    u = np.where(r >= 0, 2 * r, -2 * r - 1)
    t = u.copy()
    e = np.zeros_like(u)
    big = u >= 16
    if big.any():
        ub = u[big]
        n = np.floor(np.log2(ub)).astype(np.int64)
        m = (ub >> (n - 1)) & 1
        t[big] = 16 + (n - 4) * 2 + m
        e[big] = n - 1
    return t, e


def pixel_costs(r, ctx):
    """Per-pixel cost (bits) under a static model of token|context, plus extra bits."""
    t, e = tokens(r.ravel())
    c = ctx.ravel()
    nt = int(t.max()) + 1
    key = c * nt + t
    joint = np.bincount(key, minlength=N_CTX * nt).reshape(N_CTX, nt).astype(np.float64)
    tot = joint.sum(axis=1, keepdims=True)
    with np.errstate(divide="ignore", invalid="ignore"):
        bits = -np.log2(joint / tot)
    return (bits.ravel()[key] + e).reshape(r.shape)


# ------------------------------------------------------------------ planes
def plane_residual(p, lo, hi, pred, depth):
    nb = neighbours(p)
    scale = max(1, (hi - lo + 1) // 256) if lo == 0 else max(1, (hi + 1) // 256)
    shift = max(0, depth - 8)
    return p - predict(p, pred, lo, hi, nb, scale), context(nb, shift)


def colour_planes(rgb, maxv, tf):
    R, G, B = (rgb[:, :, i].astype(np.int64) for i in range(3))
    if tf in ("none", "ICP"):
        return [(G, 0, maxv), (R, 0, maxv), (B, 0, maxv)]
    if tf == "subG":
        return [(G, 0, maxv), (R - G, -maxv, maxv), (B - G, -maxv, maxv)]
    if tf == "RCT":
        return [((R + 2 * G + B) >> 2, 0, maxv), (R - G, -maxv, maxv), (B - G, -maxv, maxv)]
    if tf == "YCoCg-R":
        Co = R - B
        t = B + (Co >> 1)
        Cg = G - t
        Y = t + (Cg >> 1)
        return [(Y, 0, maxv), (Co, -maxv, maxv), (Cg, -maxv, maxv)]
    raise ValueError(tf)


def check_transforms():
    """The C codec needs exact inverses; check them on random data."""
    rng = np.random.default_rng(1)
    rgb = rng.integers(0, 256, (64, 64, 3))
    R, G, B = (rgb[:, :, i] for i in range(3))
    (Y, _, _), (U, _, _), (V, _, _) = colour_planes(rgb, 255, "RCT")
    G2 = Y - ((U + V) >> 2)
    assert np.array_equal(G2, G) and np.array_equal(U + G2, R) and np.array_equal(V + G2, B)
    (Y, _, _), (Co, _, _), (Cg, _, _) = colour_planes(rgb, 255, "YCoCg-R")
    t = Y - (Cg >> 1)
    G2 = Cg + t
    B2 = t - (Co >> 1)
    assert np.array_equal(G2, G) and np.array_equal(B2, B) and np.array_equal(B2 + Co, R)


def tile_sum(cost, tile):
    R, C = cost.shape
    nr, nc = -(-R // tile), -(-C // tile)
    pad = np.zeros((nr * tile, nc * tile))
    pad[:R, :C] = cost
    return pad.reshape(nr, tile, nc, tile).sum(axis=(1, 3))


# ------------------------------------------------------------------ references
def references(img):
    import imagecodecs as ic
    d = img.data
    out = {}
    try:
        out["JPEG-LS"] = len(ic.jpegls_encode(d)) * 8
    except Exception as e:  # noqa: BLE001
        print("  jpegls failed:", e)
    try:
        out["JPEG-XL"] = len(ic.jpegxl_encode(d, lossless=True, effort=7)) * 8
    except Exception as e:  # noqa: BLE001
        print("  jpegxl failed:", e)
    raw = d.astype(">u2").tobytes() if img.bpp == 16 else d.tobytes()
    out["xz-9"] = len(lzma.compress(raw, preset=9)) * 8
    out["zstd-19"] = len(ic.zstd_encode(raw, level=19)) * 8
    return out


# ------------------------------------------------------------------ per image
def analyse(path, refs=True):
    img = read_image(path)
    depth = img.bpp if img.fmt == "raw" else int(img.maxval).bit_length()
    npx = img.rows * img.cols
    row = {"file": os.path.relpath(path), "fmt": img.fmt, "rows": img.rows, "cols": img.cols,
           "bpp_in": depth * img.channels}

    if img.channels == 1:
        x = img.data.astype(np.int64)
        vals = np.unique(x)
        K = len(vals)
        row["distinct"] = K
        for pred in PREDICTORS:
            r, ctx = plane_residual(x, 0, img.maxval, pred, depth)
            row[pred] = pixel_costs(r, ctx).sum() / npx
        # histogram packing: predict on the index of each value instead of the value
        if K < img.maxval + 1:
            idx = np.searchsorted(vals, x)
            pdepth = max(1, int(K - 1).bit_length())
            table = min(K * depth, img.maxval + 1)          # list of values or bitmap
            for pred in ("MED", "GAP", "WGT"):
                r, ctx = plane_residual(idx, 0, K - 1, pred, pdepth)
                row[f"pack_{pred}"] = (pixel_costs(r, ctx).sum() + table) / npx
        else:                                   # all values used: nothing to pack
            for pred in ("MED", "GAP", "WGT"):
                row[f"pack_{pred}"] = row[pred]
    else:
        rgb = img.data
        per_tf = {}
        for tf in TRANSFORMS:
            for pred in ("MED", "GAP", "WGT"):
                planes = colour_planes(rgb, img.maxval, tf)
                cost = np.zeros(rgb.shape[:2])
                res = []
                for p, lo, hi in planes:
                    r, ctx = plane_residual(p, lo, hi, pred, depth)
                    res.append((r, ctx))
                if tf == "ICP":                     # R and B minus G's residual
                    rg = res[0][0]
                    res = [res[0], (res[1][0] - rg, res[1][1]), (res[2][0] - rg, res[2][1])]
                for r, ctx in res:
                    cost += pixel_costs(r, ctx)
                row[f"{tf}/{pred}"] = cost.sum() / npx
                per_tf[(tf, pred)] = cost
        for pred in ("MED", "GAP", "WGT"):
            fixed = [per_tf[(tf, pred)] for tf in TRANSFORMS if tf != "ICP"]
            row[f"best_img/{pred}"] = (min(c.sum() for c in fixed) + 2) / npx
            tiles = np.stack([tile_sum(c, TILE) for c in fixed])
            row[f"best_tile/{pred}"] = (tiles.min(axis=0).sum() + 2 * tiles[0].size) / npx

    if refs:
        for k, bits in references(img).items():
            row[k] = bits / npx
    return row


def group_of(path):
    p = path.replace("\\", "/")
    if "/kodak/" in p:
        return "kodak"
    for g in ("imgs-pgm8", "imgs-ppm", "imgs-raw16"):
        if f"/{g}/" in p:
            return g.replace("imgs-", "")
    return "other"


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[1])
    ap.add_argument("folder", help="ImageFiles folder")
    ap.add_argument("-o", "--outdir", default="results")
    ap.add_argument("--quick", action="store_true", help="skip the reference codecs")
    args = ap.parse_args()

    check_transforms()
    files = sorted(f for f in glob.glob(os.path.join(args.folder, "**", "*"), recursive=True)
                   if f.endswith((".pgm", ".ppm", ".raw", ".ppm.gz", ".pgm.gz")))
    os.makedirs(args.outdir, exist_ok=True)
    rows = []
    for f in files:
        t0 = time.time()
        r = analyse(f, refs=not args.quick)
        r["group"] = group_of(f)
        rows.append(r)
        print(f"{time.time() - t0:6.1f}s  {os.path.basename(f)}", flush=True)

    keys = []
    for r in rows:
        keys += [k for k in r if k not in keys]
    with open(os.path.join(args.outdir, "stage0.csv"), "w", newline="") as fh:
        w = csv.DictWriter(fh, fieldnames=keys)
        w.writeheader()
        for r in rows:
            w.writerow({k: (f"{v:.4f}" if isinstance(v, float) else v) for k, v in r.items()})

    # summary: bits per pixel per group = total bits / total pixels
    lines = ["# Stage 0 results (bits per pixel, total bits / total pixels per group)", ""]
    for g in ("pgm8", "raw16", "ppm", "kodak"):
        grp = [r for r in rows if r["group"] == g]
        if not grp:
            continue
        cols = [k for k in keys if all(k in r for r in grp) and isinstance(grp[0][k], float)]
        npx = sum(r["rows"] * r["cols"] for r in grp)
        lines += [f"## {g} ({len(grp)} images)", "", "| option | bpp |", "|---|---:|"]
        for k in sorted(cols, key=lambda k: sum(r[k] * r["rows"] * r["cols"] for r in grp)):
            lines.append(f"| {k} | {sum(r[k] * r['rows'] * r['cols'] for r in grp) / npx:.3f} |")
        lines.append("")
    with open(os.path.join(args.outdir, "stage0_summary.md"), "w") as fh:
        fh.write("\n".join(lines))
    print("\n".join(lines))


if __name__ == "__main__":
    main()
