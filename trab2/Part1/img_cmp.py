#!/usr/bin/env python3
"""
img_cmp.py - compare an image with its original version.

Prints, per channel (R, G, B for PPM) and globally:
  - maximum absolute error   max |orig - test|
  - MSE
  - PSNR = 10 * log10(MAX^2 / MSE), MAX = 2^bpp - 1  (inf if the images are equal)

Examples:
  python3 img_cmp.py ../ImageFiles/imgs-pgm8/lena.pgm lena_q4.pgm
  python3 img_cmp.py orig.raw test.raw -rows 1124 -cols 1124 -bpp 16 -be
"""
import argparse
import sys

import numpy as np

from imgio import Image, add_raw_args, read_from_args


def parse_args():
    """Build the argparse parser (original, test + RAW options) and return the args.
    Both files use the same RAW options (they must have the same size anyway)."""
    ap = argparse.ArgumentParser(description="Max absolute error and PSNR of an image vs its original.")
    ap.add_argument("original", help="original image (RAW, PGM or PPM; .gz allowed)")
    ap.add_argument("test", help="image to compare (same size and bpp)")
    add_raw_args(ap)
    return ap.parse_args()


def load_pair(orig_path, test_path, args):
    """Read the original and the test image. Returns (orig, test) as Image objects."""
    try:
        orig = read_from_args(orig_path, args)
        test = read_from_args(test_path, args)
    except (OSError, ValueError) as e:
        sys.exit(f"error: {e}")
    return orig, test


def check_compatible(orig, test):
    """Exit with an error message if the images cannot be compared:
    different shape (rows, cols, channels), different bpp or different maxval."""
    if orig.data.shape != test.data.shape:
        sys.exit(f"error: different sizes: original {orig.data.shape}, test {test.data.shape}")
    if orig.bpp != test.bpp:
        sys.exit(f"error: different bpp: original {orig.bpp}, test {test.bpp}")
    if orig.maxval != test.maxval:
        sys.exit(f"error: different maxval: original {orig.maxval}, test {test.maxval} "
                 f"(was the test image written with img_quant --raw-levels?)")


def max_abs_error(x, y):
    """Max absolute difference between two arrays (any shape). Returns an int.
    Note: convert to int64 before subtracting (uint8/uint16 would wrap around)."""
    return int(np.abs(x.astype(np.int64) - y.astype(np.int64)).max())


def mse(x, y):
    """Mean squared error between two arrays (any shape). Returns a float."""
    d = x.astype(np.float64) - y.astype(np.float64)
    return float(np.mean(d * d))


def psnr(mse_value, peak):
    """PSNR in dB from an MSE and the peak value (2^bpp - 1).
    Returns float('inf') when mse_value == 0."""
    if mse_value == 0:
        return float("inf")
    return float(10 * np.log10(peak * peak / mse_value))


def compare(orig, test):
    """Compute the metrics for each channel and for all samples together.
    Returns a list of tuples: (name, max_err, mse, psnr)
      - grayscale: one row ("Gray")
      - PPM:       rows "R", "G", "B" and "Global"
    """
    peak = (1 << orig.bpp) - 1
    rows = []
    for i, name in enumerate(orig.channel_names()):
        x, y = orig.channel(i), test.channel(i)
        m = mse(x, y)
        rows.append((name, max_abs_error(x, y), m, psnr(m, peak)))
    if orig.channels > 1:
        # global: one MSE over all samples (not the average of the channel PSNRs)
        m = mse(orig.data, test.data)
        rows.append(("Global", max_abs_error(orig.data, test.data), m, psnr(m, peak)))
    return rows


def print_report(orig_path, test_path, orig, rows):
    """Print a small header (files, format, size, bpp) and a table with the rows from compare()."""
    print(f"original: {orig_path}")
    print(f"test:     {test_path}")
    print(f"{orig.fmt.upper()} {orig.cols}x{orig.rows}, {orig.bpp} bpp, "
          f"{orig.channels} channel(s), peak = {(1 << orig.bpp) - 1}")
    print(f"{'channel':<8}{'max abs err':>13}{'MSE':>14}{'PSNR (dB)':>12}")
    for name, err, m, p in rows:
        ps = "inf" if p == float("inf") else f"{p:.4f}"
        print(f"{name:<8}{err:>13}{m:>14.4f}{ps:>12}")


def main():
    args = parse_args()
    orig, test = load_pair(args.original, args.test, args)
    check_compatible(orig, test)
    rows = compare(orig, test)
    print_report(args.original, args.test, orig, rows)


if __name__ == "__main__":
    main()