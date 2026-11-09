#!/usr/bin/env python3
"""
img_hist.py - histograms and entropies of RAW/PGM/PPM images.

For each channel it prints the first-order entropy of:
  - the pixel values
  - the horizontal differences  x[r][c] - x[r][c-1]
  - the vertical differences    x[r][c] - x[r-1][c]
and saves PNG plots of the three histograms.

With --bits lo-hi, it uses only bits lo..hi of each sample: (x >> lo) & (2**n - 1).

Examples:
  python3 img_hist.py ../ImageFiles/imgs-pgm8/lena.pgm
  python3 img_hist.py ../ImageFiles/imgs-ppm/tulips.ppm --bits 0-5
  python3 img_hist.py img.raw -rows 1124 -cols 1124 -bpp 16 -be
"""
import argparse
import os
import sys

import numpy as np
import matplotlib
matplotlib.use("Agg")            # no window, just files
import matplotlib.pyplot as plt

from imgio import add_raw_args, read_from_args


def entropy(counts):
    """First-order entropy (bits/symbol) from a histogram."""
    p = counts[counts > 0] / counts.sum()
    return float(-(p * np.log2(p)).sum())


def parse_bits(text, bpp):
    try:
        lo, hi = (int(v) for v in text.split("-"))
    except ValueError:
        sys.exit(f"--bits must look like 'lo-hi', e.g. 0-5 (got {text!r})")
    if not (0 <= lo <= hi < bpp):
        sys.exit(f"--bits {text}: need 0 <= lo <= hi <= {bpp - 1}")
    return lo, hi


def analyse(ch, nbits):
    """ch: 2-D int64 array with values in [0, 2**nbits).
    Returns dict with histograms (values + offsets) and entropies."""
    size = 1 << nbits
    off = size - 1                               # diffs go from -(size-1) to +(size-1)
    pix = np.bincount(ch.ravel(), minlength=size)
    hd = np.bincount((np.diff(ch, axis=1) + off).ravel(), minlength=2 * size - 1)
    vd = np.bincount((np.diff(ch, axis=0) + off).ravel(), minlength=2 * size - 1)
    return {
        "pix": (pix, 0), "hdiff": (hd, -off), "vdiff": (vd, -off),
        "H_pix": entropy(pix), "H_hdiff": entropy(hd), "H_vdiff": entropy(vd),
    }


def plot_range(counts, kind, full):
    """Indexes [lo, hi] of the bins to show. Pixel plots show all non-zero bins.
    Difference plots are zoomed to the central 99.9% of the values (unless full=True),
    because a few big outliers would squash the useful part of the plot."""
    nz = np.nonzero(counts)[0]
    if len(nz) == 0:
        return 0, len(counts) - 1
    if kind == "pix" or full:
        return nz[0], nz[-1]
    cdf = np.cumsum(counts) / counts.sum()
    return int(np.searchsorted(cdf, 0.0005)), int(np.searchsorted(cdf, 0.9995))


def plot(results, names, kind, title, path, logy, full):
    fig, axes = plt.subplots(len(names), 1, figsize=(9, 3.2 * len(names)), squeeze=False)
    colors = {"Gray": "0.3", "R": "tab:red", "G": "tab:green", "B": "tab:blue"}
    for ax, name, res in zip(axes[:, 0], names, results):
        counts, first = res[kind]
        lo, hi = plot_range(counts, kind, full)
        x = np.arange(lo, hi + 2) + first        # bin edges
        ax.stairs(counts[lo:hi + 1], x - 0.5, fill=True, color=colors[name])
        ax.set_xlim(x[0] - 0.5, x[-1] - 0.5)
        if logy:
            ax.set_yscale("log")
        ax.set_title(f"{name}   H = {res['H_' + kind]:.4f} bits")
        ax.set_ylabel("count")
    axes[-1, 0].set_xlabel("value" if kind == "pix" else "difference")
    fig.suptitle(title)
    fig.tight_layout()
    fig.savefig(path, dpi=110)
    plt.close(fig)


def main():
    ap = argparse.ArgumentParser(description="Histograms and entropies of RAW/PGM/PPM images.")
    ap.add_argument("input", help="image file (RAW, PGM or PPM; .gz allowed)")
    ap.add_argument("--bits", metavar="LO-HI", help="only use bits LO..HI of each sample, e.g. 0-5")
    ap.add_argument("-o", "--outdir", default="hist_out", help="folder for the PNG plots (default: hist_out)")
    ap.add_argument("--log", action="store_true", help="log scale on the y axis")
    ap.add_argument("--full", action="store_true", help="do not zoom the difference plots")
    ap.add_argument("--no-plot", action="store_true", help="only print the entropies")
    add_raw_args(ap)
    args = ap.parse_args()

    img = read_from_args(args.input, args)
    names = img.channel_names()

    if args.bits:
        lo, hi = parse_bits(args.bits, img.bpp)
    else:
        lo, hi = 0, img.bpp - 1
    nbits = hi - lo + 1
    mask = (1 << nbits) - 1

    results = []
    for i in range(img.channels):
        ch = (img.channel(i).astype(np.int64) >> lo) & mask
        results.append(analyse(ch, nbits))

    # ---- print
    base = os.path.basename(args.input)
    for ext in (".gz", ".pgm", ".ppm", ".raw"):
        base = base[: -len(ext)] if base.endswith(ext) else base
    tag = f"bits {lo}-{hi}" if args.bits else "all bits"
    print(f"{args.input}: {img.fmt.upper()} {img.cols}x{img.rows}, {img.bpp} bpp, "
          f"{img.channels} channel(s), {tag} ({nbits} bits)")
    print(f"{'channel':<8}{'H(pixels)':>12}{'H(horiz diff)':>16}{'H(vert diff)':>15}")
    for name, r in zip(names, results):
        print(f"{name:<8}{r['H_pix']:>12.4f}{r['H_hdiff']:>16.4f}{r['H_vdiff']:>15.4f}")

    # ---- plots
    if not args.no_plot:
        os.makedirs(args.outdir, exist_ok=True)
        suffix = f"_b{lo}-{hi}" if args.bits else ""
        for kind, label in (("pix", "pixel values"), ("hdiff", "horizontal differences"),
                            ("vdiff", "vertical differences")):
            path = os.path.join(args.outdir, f"{base}{suffix}_{kind}.png")
            plot(results, names, kind, f"{base} - {label} ({tag})", path, args.log, args.full)
        print(f"plots saved in {args.outdir}/")


if __name__ == "__main__":
    main()