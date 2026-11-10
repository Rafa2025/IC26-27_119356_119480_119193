#!/usr/bin/env python3
"""
img_quant.py - uniform scalar quantization of RAW/PGM/PPM images.

Each sample (pixel, or R/G/B channel for PPM) goes from B bits to k bits:

    s = B - k                  (number of low bits thrown away, step = 2^s)
    q = x >> s                 (quantization index, 0 .. 2^k - 1)

B is the real bit depth of the image: bpp for RAW, bit_length(maxval) for
PGM/PPM (e.g. maxval 4095 -> 12 bits, even though it is stored in 16).

For PPM, -k takes one value for all channels or three values R,G,B
(e.g. -k 5,6,5, the RGB565 format).

Output (always an image file):
  default        reconstructed image at the original depth, so it can be
                 compared with the original using img_cmp:
                   --recon mid  (default)  y = (q << s) + 2^(s-1)   middle of the step
                   --recon low             y =  q << s              bottom of the step
  --raw-levels   the k-bit indexes q themselves (maxval 2^k - 1;
                 a RAW output is written as 8 bpp if k <= 8, else 16 bpp)

Examples:
  python3 img_quant.py ../../ImageFiles/imgs-pgm8/lena.pgm lena_q4.pgm -k 4
  python3 img_quant.py ../../ImageFiles/imgs-ppm/tulips.ppm tulips_565.ppm -k 5,6,5
  python3 img_quant.py in.raw out.raw -k 10 -rows 1124 -cols 1124 -bpp 16 -be
  python3 img_quant.py ../../ImageFiles/imgs-pgm8/lena.pgm lena_idx.pgm -k 4 --raw-levels
"""
import argparse
import sys

import numpy as np

from imgio import Image, add_raw_args, read_from_args, write_image


def bit_depth(img):
    """Real number of bits per sample of the image."""
    return img.bpp if img.fmt == "raw" else max(1, int(img.maxval).bit_length())


def quantize(data, depth, k, recon="mid", maxval=None):
    """Return (indexes q, reconstruction y), both with the dtype of data.
    No wider type is needed: every value stays below 2^depth."""
    s = depth - k
    q = data >> s
    y = q << s
    if recon == "mid" and s > 0:
        y = y + (1 << (s - 1))
    if maxval is not None:
        y = np.minimum(y, maxval)          # only matters if maxval is not 2^B - 1
    return q, y


def parse_keep(text, channels, depth):
    """'5' -> [5]*channels ; '5,6,5' -> [5, 6, 5] (PPM only)."""
    try:
        ks = [int(v) for v in text.split(",")]
    except ValueError:
        sys.exit(f"-k must be a number or R,G,B numbers (got {text!r})")
    if len(ks) == 1:
        ks *= channels
    if len(ks) != channels:
        need = "1 value" if channels == 1 else f"1 or {channels} values"
        sys.exit(f"-k needs {need} for this image (got {len(ks)})")
    if not all(1 <= k <= depth for k in ks):
        sys.exit(f"-k values must be between 1 and {depth} for this image")
    return ks


def main():
    ap = argparse.ArgumentParser(description="Uniform scalar quantization of RAW/PGM/PPM images.")
    ap.add_argument("input", help="input image (RAW, PGM or PPM; .gz allowed)")
    ap.add_argument("output", help="output image")
    ap.add_argument("-k", "--keep", required=True, metavar="K[,K,K]",
                    help="bits per sample to keep (PPM: one value or R,G,B)")
    ap.add_argument("--recon", choices=("mid", "low"), default="mid",
                    help="reconstruction value inside each step (default: mid)")
    ap.add_argument("--raw-levels", action="store_true",
                    help="write the k-bit indexes instead of the reconstructed image")
    ap.add_argument("-f", "--format", choices=("raw", "pgm", "ppm"),
                    help="output format (default: same as input)")
    add_raw_args(ap)
    args = ap.parse_args()

    img = read_from_args(args.input, args)
    depth = bit_depth(img)
    ks = parse_keep(args.keep, img.channels, depth)

    fmt = args.format or img.fmt
    if fmt == "ppm" and img.channels != 3:
        sys.exit("PPM output needs a color input")
    if fmt in ("raw", "pgm") and img.channels != 1:
        sys.exit("PPM input can only be written as PPM")
    if args.raw_levels and len(set(ks)) > 1:
        sys.exit("--raw-levels needs the same k for all channels (PPM has a single maxval)")

    q = np.empty_like(img.data)
    y = np.empty_like(img.data)
    for i, k in enumerate(ks):
        qi, yi = quantize(img.channel(i), depth, k, args.recon, img.maxval)
        if img.channels == 1:
            q[...], y[...] = qi, yi
        else:
            q[:, :, i], y[:, :, i] = qi, yi

    if args.raw_levels:
        k = ks[0]
        out_bpp = 8 if k <= 8 else 16
        dtype = np.uint8 if out_bpp == 8 else np.uint16
        out = Image(q.astype(dtype), fmt, out_bpp, (1 << k) - 1, img.endian)
    else:
        out = Image(y, fmt, img.bpp, img.maxval, img.endian)
    write_image(args.output, out, fmt=fmt)

    mode = "indexes" if args.raw_levels else f"reconstruction ({args.recon})"
    kdesc = ",".join(map(str, ks)) if len(set(ks)) > 1 else str(ks[0])
    steps = ",".join(str(1 << (depth - k)) for k in ks) if len(set(ks)) > 1 else str(1 << (depth - ks[0]))
    print(f"{args.input}: {img.fmt.upper()} {img.cols}x{img.rows}, {img.channels} channel(s), "
          f"{depth} -> {kdesc} bits (step {steps})")
    print(f"wrote {args.output} as {fmt.upper()}, {mode}, maxval {out.maxval}")
    if fmt == "raw" and args.raw_levels:
        endian = f" -{out.endian}" if out.bpp == 16 else ""
        print(f"read it back with: -rows {img.rows} -cols {img.cols} -bpp {out.bpp}{endian}")


if __name__ == "__main__":
    main()
