#!/usr/bin/env python3
"""
wav_cmp.py - compare a (processed) WAV file against its original version.

For each channel, and for the average of the channels, prints:
    * the mean squared error (MSE, L2 norm)
    * the maximum per-sample absolute error (L-infinity norm)
    * the signal-to-noise ratio (SNR, in dB) of the file w.r.t. the original

Usage:
    python3 wav_cmp.py original.wav processed.wav
"""

import argparse
import os
import sys

import numpy as np

from wav_hist import read_wav


def compare(orig, proc):
    """
    Compute MSE, max absolute error and SNR (dB) between two 1-D integer
    signals of the same length. Computations are done in float64 so that
    squaring 24/32-bit samples cannot overflow.
    """
    x = orig.astype(np.float64)
    err = x - proc.astype(np.float64)

    mse = np.mean(err ** 2)
    max_abs = np.max(np.abs(err))

    signal_energy = np.sum(x ** 2)
    noise_energy = np.sum(err ** 2)
    if noise_energy == 0:
        snr = float("inf")           # identical signals
    elif signal_energy == 0:
        snr = float("-inf")          # silent original but non-zero error
    else:
        snr = 10.0 * np.log10(signal_energy / noise_energy)

    return mse, max_abs, snr


def main():
    ap = argparse.ArgumentParser(description="Compare a WAV file with its original")
    ap.add_argument("original", help="original .wav file")
    ap.add_argument("processed", help="processed .wav file to compare against the original")
    args = ap.parse_args()

    for path in (args.original, args.processed):
        if not os.path.isfile(path):
            sys.exit(f"file not found: {path}")

    orig, rate_o, width_o, nch_o = read_wav(args.original)
    proc, rate_p, width_p, nch_p = read_wav(args.processed)

    if nch_o != nch_p:
        sys.exit(f"channel count mismatch: {nch_o} vs {nch_p}")
    if rate_o != rate_p:
        print(f"warning: sample rate differs ({rate_o} Hz vs {rate_p} Hz)", file=sys.stderr)
    if width_o != width_p:
        print(f"warning: bit depth differs ({width_o*8}-bit vs {width_p*8}-bit)", file=sys.stderr)
    if orig.shape[0] != proc.shape[0]:
        n = min(orig.shape[0], proc.shape[0])
        print(f"warning: length differs ({orig.shape[0]} vs {proc.shape[0]} frames), "
              f"comparing the first {n}", file=sys.stderr)
        orig, proc = orig[:n], proc[:n]

    print(f"original : {args.original} ({rate_o} Hz, {width_o*8}-bit, {nch_o} ch, {orig.shape[0]} frames)")
    print(f"processed: {args.processed} ({rate_p} Hz, {width_p*8}-bit, {nch_p} ch, {proc.shape[0]} frames)")
    print()

    # signals to compare: each channel, then the average of the channels
    signals = []
    if nch_o == 1:
        signals.append(("mono", orig[:, 0], proc[:, 0]))
    else:
        names = ["L", "R"] if nch_o == 2 else [f"ch{i}" for i in range(nch_o)]
        for i, name in enumerate(names):
            signals.append((name, orig[:, i], proc[:, i]))
        # average of the channels with integer division (same as MID in wav_hist)
        avg_o = orig.astype(np.int64).sum(axis=1) // nch_o
        avg_p = proc.astype(np.int64).sum(axis=1) // nch_p
        signals.append(("avg", avg_o, avg_p))

    print(f"{'channel':<8} {'MSE (L2)':>16} {'max |err| (Linf)':>18} {'SNR (dB)':>12}")
    for name, a, b in signals:
        mse, max_abs, snr = compare(a, b)
        print(f"{name:<8} {mse:>16.4f} {int(max_abs):>18d} {snr:>12.4f}")


if __name__ == "__main__":
    main()
