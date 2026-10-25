#!/usr/bin/env python3
"""
plot_quant_waveform.py - plot an excerpt of a WAV file and its quantized version
on the same axes, to show the staircase that wav_quant creates.

Usage:
    python3 plot_quant_waveform.py original.wav quantized.wav
    python3 plot_quant_waveform.py original.wav quantized.wav --start 44100 --length 150 \
            --channel 0 --out quant_waveform.png

16-bit PCM only. --start and --length are in sample frames.
"""

import argparse
import sys
import wave

import numpy as np
import matplotlib.pyplot as plt


def read_channel(path, channel):
    """Return (samples of one channel as int array, sample rate)."""
    with wave.open(path, "rb") as wf:
        if wf.getsampwidth() != 2:
            sys.exit(f"{path}: only 16-bit WAV is supported by this script")
        n_ch = wf.getnchannels()
        if channel >= n_ch:
            sys.exit(f"{path}: has {n_ch} channel(s), no channel {channel}")
        rate = wf.getframerate()
        raw = wf.readframes(wf.getnframes())
    data = np.frombuffer(raw, dtype=np.int16).reshape(-1, n_ch)
    return data[:, channel].astype(np.int32), rate


def main():
    ap = argparse.ArgumentParser(description="Plot original vs quantized waveform")
    ap.add_argument("original")
    ap.add_argument("quantized")
    ap.add_argument("--start", type=int, default=44100,
                    help="first frame of the excerpt (default 44100, i.e. 1 s)")
    ap.add_argument("--length", type=int, default=150,
                    help="number of frames in the excerpt (default 150)")
    ap.add_argument("--channel", type=int, default=0, help="0 = left (default), 1 = right")
    ap.add_argument("--out", default="quant_waveform.png")
    args = ap.parse_args()

    x, rate = read_channel(args.original, args.channel)
    y, rate_q = read_channel(args.quantized, args.channel)
    if rate != rate_q or len(x) != len(y):
        sys.exit("the two files must have the same sample rate and length")
    if args.start + args.length > len(x):
        sys.exit(f"excerpt goes past the end of the file ({len(x)} frames)")

    sl = slice(args.start, args.start + args.length)
    n = np.arange(args.start, args.start + args.length)

    # 4 x 2.4 inches: fits half the text width of the report with readable text
    fig, ax = plt.subplots(figsize=(4, 2.4))
    ax.plot(n, x[sl], color="#1f77b4", linewidth=1.2, label="original")
    ax.step(n, y[sl], where="post", color="#d62728", linewidth=1.4, label="quantized")
    ax.set_xlabel("Sample index")
    ax.set_ylabel("Sample value")
    ax.legend(fontsize=8, loc="best")
    fig.tight_layout()
    fig.savefig(args.out, dpi=200)
    print(f"saved {args.out}")


if __name__ == "__main__":
    main()