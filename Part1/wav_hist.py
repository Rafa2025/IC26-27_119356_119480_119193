#!/usr/bin/env python3
"""
wav_hist.py - compute and plot histograms of WAV audio files.

Usage:
    python3 wav_hist.py input.wav
    python3 wav_hist.py input.wav --k 0 1 2 3        # coarser bins (2^k grouping)
    python3 wav_hist.py input.wav --outdir plots
"""

import argparse
import os
import sys
import wave

import numpy as np
import matplotlib
matplotlib.use("Agg")  # no GUI backend, just save to files
import matplotlib.pyplot as plt


def read_wav(path):
    """
    Read a WAV file and return (samples, sample_rate, sampwidth, n_channels).
    samples: np.ndarray of shape (n_samples, n_channels), dtype = signed int
             matching the file's bit depth.
    Only PCM integer WAVs (8/16/24/32-bit) are handled, which covers the
    typical case for this assignment. 8-bit WAV is unsigned in the file
    format, so it is shifted to signed range for consistency.
    """
    with wave.open(path, "rb") as wf:
        n_channels = wf.getnchannels()
        sampwidth = wf.getsampwidth()      # bytes per sample (1, 2, 3, 4)
        framerate = wf.getframerate()
        n_frames = wf.getnframes()
        raw = wf.readframes(n_frames)

    if sampwidth == 1:
        # 8-bit WAV is stored as unsigned 0..255 -> center to signed -128..127
        data = np.frombuffer(raw, dtype=np.uint8).astype(np.int16) - 128
    elif sampwidth == 2:
        data = np.frombuffer(raw, dtype=np.int16)
    elif sampwidth == 3:
        # 24-bit has no native numpy dtype: unpack 3 bytes -> int32 manually
        b = np.frombuffer(raw, dtype=np.uint8).reshape(-1, 3)
        data = (b[:, 0].astype(np.int32)
                | (b[:, 1].astype(np.int32) << 8)
                | (b[:, 2].astype(np.int32) << 16))
        # sign-extend 24-bit values
        data = np.where(data >= (1 << 23), data - (1 << 24), data)
    elif sampwidth == 4:
        data = np.frombuffer(raw, dtype=np.int32)
    else:
        raise ValueError(f"Unsupported sample width: {sampwidth} bytes")

    data = data.reshape(-1, n_channels)
    return data, framerate, sampwidth, n_channels


def coarsen(samples, k):
    """
    Group sample values into bins of width 2^k using integer division.
    Returns the coarsened (still integer) values, ready for np.histogram
    with unit-width bins, or you can just count unique coarsened values.
    """
    if k == 0:
        return samples
    return samples // (2 ** k)


def plot_hist(values, title, outpath):
    plt.figure(figsize=(8, 4))
    lo, hi = int(values.min()), int(values.max())
    bins = np.arange(lo, hi + 2) - 0.5  # one bin per integer value
    plt.hist(values, bins=bins)
    plt.title(title)
    plt.xlabel("Sample value")
    plt.ylabel("Count")
    plt.tight_layout()
    plt.savefig(outpath)
    plt.close()
    print(f"saved {outpath}")


def main():
    ap = argparse.ArgumentParser(description="Histogram tool for WAV files")
    ap.add_argument("input", help="input .wav file")
    ap.add_argument("--k", type=int, nargs="+", default=[0],
                     help="exponents k for coarser bins of width 2^k (default: 0, no coarsening)")
    ap.add_argument("--outdir", default="hist_out", help="output directory for plots")
    args = ap.parse_args()

    if not os.path.isfile(args.input):
        sys.exit(f"file not found: {args.input}")
    os.makedirs(args.outdir, exist_ok=True)

    samples, rate, sampwidth, n_channels = read_wav(args.input)
    base = os.path.splitext(os.path.basename(args.input))[0]
    print(f"{args.input}: {rate} Hz, {sampwidth*8}-bit, {n_channels} channel(s), "
          f"{samples.shape[0]} frames")

    # per-channel signals to analyze
    channels = {}
    if n_channels == 1:
        channels["mono"] = samples[:, 0]
    else:
        L = samples[:, 0].astype(np.int64)
        R = samples[:, 1].astype(np.int64)
        channels["L"] = L
        channels["R"] = R
        channels["MID"] = (L + R) // 2          # integer division, as required
        channels["SIDE"] = (L - R) // 2          # integer division, as required

    for k in args.k:
        for name, sig in channels.items():
            vals = coarsen(sig, k)
            tag = f"{name}_k{k}"
            title = f"{base} - {name}" + (f" (bin width 2^{k})" if k else "")
            outpath = os.path.join(args.outdir, f"{base}_{tag}.png")
            plot_hist(vals, title, outpath)


if __name__ == "__main__":
    main()