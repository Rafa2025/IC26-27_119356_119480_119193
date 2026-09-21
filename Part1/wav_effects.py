#!/usr/bin/env python3
"""
wav_effects.py - apply audio effects to a WAV file.
Feed-forward (single-reflection) design: echoes are delayed copies of
the INPUT, not fed back from the output. Output is normalized to the
original sample range only if it would otherwise clip.

Usage:
    python3 wav_effects.py input.wav output.wav echo --delay 0.3 --gain 0.5
    python3 wav_effects.py input.wav output.wav multiecho --taps 0.2:0.6 0.4:0.3 0.6:0.15
    python3 wav_effects.py input.wav output.wav am --depth 0.5 --freq 5
    python3 wav_effects.py input.wav output.wav vdelay --base 0.02 --depth 0.01 --lfo 2
"""

import argparse
import array
import math
import sys
import wave


FORMATS = {1: ("b", 8, True), 2: ("h", 16, False), 4: ("i", 32, False)}


def read_wav(path):
    with wave.open(path, "rb") as wf:
        params = wf.getparams()
        sampwidth = wf.getsampwidth()
        n_channels = wf.getnchannels()
        raw = wf.readframes(wf.getnframes())

    if sampwidth not in FORMATS:
        sys.exit(f"unsupported sample width: {sampwidth} bytes")
    typecode, bits, unsigned = FORMATS[sampwidth]

    if unsigned:
        flat = [b - 128 for b in raw]
    else:
        a = array.array(typecode)
        a.frombytes(raw)
        flat = list(a)

    channels = [flat[i::n_channels] for i in range(n_channels)]
    return channels, params, bits, unsigned


def write_wav(path, channels, params, bits, unsigned):
    n = len(channels[0])
    n_channels = len(channels)
    interleaved = [0] * (n * n_channels)
    for ch_idx, ch in enumerate(channels):
        interleaved[ch_idx::n_channels] = ch

    if unsigned:
        out_bytes = bytearray(((v + 128) & 0xFF) for v in interleaved)
    else:
        typecode = FORMATS[params.sampwidth][0]
        out_bytes = array.array(typecode, interleaved).tobytes()

    with wave.open(path, "wb") as wf:
        wf.setparams(params)
        wf.writeframes(out_bytes)


def normalize(channels, bits):
    """Scale down only if the effect pushed samples past the valid range."""
    limit = 2 ** (bits - 1) - 1
    peak = max(abs(v) for ch in channels for v in ch) or 1
    if peak <= limit:
        return channels
    scale = limit / peak
    return [[int(v * scale) for v in ch] for ch in channels]


# ---- effects (each takes one channel: list of ints; fs = sample rate) ----

def fx_echo(x, fs, delay_s, gain):
    D = int(delay_s * fs)
    y = list(x)
    for n in range(D, len(x)):
        y[n] += gain * x[n - D]
    return [int(v) for v in y]


def fx_multiecho(x, fs, taps):
    """taps: list of (delay_seconds, gain) pairs, applied to the input."""
    y = list(x)
    for delay_s, gain in taps:
        D = int(delay_s * fs)
        for n in range(D, len(x)):
            y[n] += gain * x[n - D]
    return [int(v) for v in y]


def fx_am(x, fs, depth, freq):
    y = []
    for n, s in enumerate(x):
        mod = 1 + depth * math.cos(2 * math.pi * freq * n / fs)
        y.append(int(s * mod))
    return y


def fx_vdelay(x, fs, base_s, depth_s, lfo_freq):
    """Time-varying delay with linear interpolation between integer taps."""
    N = len(x)
    y = []
    for n in range(N):
        d = (base_s + depth_s * math.sin(2 * math.pi * lfo_freq * n / fs)) * fs
        idx = n - d
        i0 = int(math.floor(idx))
        frac = idx - i0
        s0 = x[i0] if 0 <= i0 < N else 0
        s1 = x[i0 + 1] if 0 <= i0 + 1 < N else 0
        y.append(int(x[n] + ((1 - frac) * s0 + frac * s1)))
    return y


def parse_taps(items):
    taps = []
    for item in items:
        d, g = item.split(":")
        taps.append((float(d), float(g)))
    return taps


def main():
    ap = argparse.ArgumentParser(description="Audio effects for WAV files")
    ap.add_argument("input")
    ap.add_argument("output")
    sub = ap.add_subparsers(dest="effect", required=True)

    p = sub.add_parser("echo")
    p.add_argument("--delay", type=float, required=True, help="seconds")
    p.add_argument("--gain", type=float, required=True)

    p = sub.add_parser("multiecho")
    p.add_argument("--taps", nargs="+", required=True,
                    help="delay:gain pairs, e.g. 0.2:0.6 0.4:0.3")

    p = sub.add_parser("am")
    p.add_argument("--depth", type=float, required=True, help="0..1")
    p.add_argument("--freq", type=float, required=True, help="Hz")

    p = sub.add_parser("vdelay")
    p.add_argument("--base", type=float, required=True, help="base delay, seconds")
    p.add_argument("--depth", type=float, required=True, help="delay swing, seconds")
    p.add_argument("--lfo", type=float, required=True, help="LFO rate, Hz")

    args = ap.parse_args()

    channels, params, bits, unsigned = read_wav(args.input)
    fs = params.framerate

    if args.effect == "echo":
        out = [fx_echo(ch, fs, args.delay, args.gain) for ch in channels]
    elif args.effect == "multiecho":
        taps = parse_taps(args.taps)
        out = [fx_multiecho(ch, fs, taps) for ch in channels]
    elif args.effect == "am":
        out = [fx_am(ch, fs, args.depth, args.freq) for ch in channels]
    elif args.effect == "vdelay":
        out = [fx_vdelay(ch, fs, args.base, args.depth, args.lfo) for ch in channels]

    out = normalize(out, bits)
    write_wav(args.output, out, params, bits, unsigned)
    print(f"wrote {args.output} ({args.effect})")


if __name__ == "__main__":
    main()