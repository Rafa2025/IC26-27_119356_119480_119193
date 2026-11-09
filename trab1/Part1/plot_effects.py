#!/usr/bin/env python3
"""
plot_effects.py - one stacked figure: an excerpt of a WAV file (top row) and the same
excerpt after each effect of wav_effects.py (echo, multi-echo, AM, variable delay).

Usage:
    python3 plot_effects.py input.wav
    python3 plot_effects.py input.wav --start 88200 --length 44100 --out effects.png

Needs wav_effects.py in the same folder. 16-bit WAV, one channel is plotted.
The effects are applied to the excerpt plus the history they need, so the echoes in
the excerpt are the real ones. The final normalization of wav_effects.py is NOT
applied: all rows share the same vertical scale, so a level change is visible.
Choose --start at a loud, short sound (a drum hit, a note onset), so the echoes
appear in a quiet gap.
"""

import argparse
import sys

import numpy as np
import matplotlib.pyplot as plt

import wav_effects as fx


def main():
    ap = argparse.ArgumentParser(description="Stacked waveform figure of the effects")
    ap.add_argument("input")
    ap.add_argument("--start", type=int, default=44100, help="first frame (default 44100)")
    ap.add_argument("--length", type=int, default=44100, help="frames to plot (default 44100)")
    ap.add_argument("--channel", type=int, default=0)
    ap.add_argument("--out", default="effects.png")
    # parameters, chosen so that each effect is visible on a 1 s excerpt
    ap.add_argument("--echo-delay", type=float, default=0.25)
    ap.add_argument("--echo-gain", type=float, default=0.6)
    ap.add_argument("--taps", nargs="+", default=["0.15:0.6", "0.30:0.4", "0.45:0.25"])
    ap.add_argument("--am-depth", type=float, default=0.8)
    ap.add_argument("--am-freq", type=float, default=4.0)
    ap.add_argument("--vd-base", type=float, default=0.01)
    ap.add_argument("--vd-depth", type=float, default=0.008)
    ap.add_argument("--vd-lfo", type=float, default=3.0)
    args = ap.parse_args()

    channels, params, bits, unsigned = fx.read_wav(args.input)
    if bits != 16:
        sys.exit("only 16-bit WAV is supported by this script")
    if args.channel >= len(channels):
        sys.exit(f"the file has {len(channels)} channel(s)")
    x_all = channels[args.channel]
    fs = params.framerate
    if args.start + args.length > len(x_all):
        sys.exit(f"excerpt goes past the end of the file ({len(x_all)} frames)")

    taps = fx.parse_taps(args.taps)
    history = int(max([args.echo_delay] + [d for d, _ in taps]) * fs)
    seg0 = max(0, args.start - history)
    seg = x_all[seg0:args.start + args.length]
    off = args.start - seg0

    rows = [
        ("original", seg),
        (f"echo\n{args.echo_delay:g} s, g={args.echo_gain:g}",
         fx.fx_echo(seg, fs, args.echo_delay, args.echo_gain)),
        ("multi-echo\n" + ", ".join(f"{d:g} s" for d, _ in taps),
         fx.fx_multiecho(seg, fs, taps)),
        (f"AM\n{args.am_depth:g}, {args.am_freq:g} Hz",
         fx.fx_am(seg, fs, args.am_depth, args.am_freq)),
        (f"variable delay\n{args.vd_base:g}±{args.vd_depth:g} s, {args.vd_lfo:g} Hz",
         fx.fx_vdelay(seg, fs, args.vd_base, args.vd_depth, args.vd_lfo)),
    ]

    t = np.arange(args.length) / fs
    top = 1.05 * max(abs(v) for _, y in rows for v in y[off:])
    fig, axes = plt.subplots(len(rows), 1, figsize=(7, 5), sharex=True, sharey=True)
    for ax, (label, y) in zip(axes, rows):
        ax.plot(t, seg[off:], color="#1f77b4", linewidth=0.5, alpha=0.55)
        if label != "original":
            ax.plot(t, y[off:], color="#d62728", linewidth=0.6)
        ax.set_ylim(-top, top)
        ax.set_yticks([])
        ax.set_ylabel(label, rotation=0, ha="right", va="center", fontsize=8)
    axes[-1].set_xlabel("Time (s)")
    fig.tight_layout()
    fig.savefig(args.out, dpi=200)
    print(f"saved {args.out}")


if __name__ == "__main__":
    main()