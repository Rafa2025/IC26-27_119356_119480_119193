#!/usr/bin/env python3
"""
wav_quant.py - reduce the effective number of bits per sample (uniform
scalar quantization, mid-tread, shift-truncate). Output stays in the
same WAV container width as the input; only the value resolution drops.

Usage:
    python3 wav_quant.py in.wav out.wav --bits 8
    python3 wav_quant.py in.wav out.wav --levels 1000
"""

import argparse
import array
import sys
import wave


# sample width in bytes -> (array typecode, bits, is_unsigned_in_file)
FORMATS = {
    1: ("b", 8, True),   # 8-bit WAV is unsigned in the file
    2: ("h", 16, False),
    4: ("i", 32, False),
}


def read_samples(path):
    with wave.open(path, "rb") as wf:
        params = wf.getparams()
        sampwidth = wf.getsampwidth()
        raw = wf.readframes(wf.getnframes())

    if sampwidth == 3:
        sys.exit("24-bit WAV not supported by this script")
    if sampwidth not in FORMATS:
        sys.exit(f"unsupported sample width: {sampwidth} bytes")

    typecode, bits, unsigned_in_file = FORMATS[sampwidth]

    if unsigned_in_file:
        # 8-bit: bytes are 0..255 in the file -> shift to signed -128..127
        samples = array.array("b", [b - 128 for b in raw])
    else:
        samples = array.array(typecode)
        samples.frombytes(raw)

    return samples, params, bits, unsigned_in_file


def quantize_by_bits(samples, bits, target_bits):
    """
    Mid-tread uniform scalar quantization via shift-truncate.
    Only valid when the level count is a power of two (2^target_bits),
    since bit-shifting IS division by a power of two.
    shift = bits - target_bits
    y = (x >> shift) << shift
    Truncates toward -inf (Python's >> on negative ints does this
    correctly), zero always maps to zero (mid-tread property), no
    overflow risk since truncation only removes low-order bits.
    """
    if target_bits <= 0 or target_bits >= bits:
        sys.exit(f"--bits must be between 1 and {bits - 1} for this file")
    shift = bits - target_bits
    return array.array(samples.typecode, [(x >> shift) << shift for x in samples])


def quantize_by_levels(samples, bits, levels):
    """
    Mid-tread uniform scalar quantization for an ARBITRARY level count
    (not restricted to powers of two). Step size derived directly from
    the requested number of levels, so no bit-shift trick applies here.

    delta = full_range / levels
    y = (x // delta) * delta

    0 // delta == 0 always, so zero maps to zero (mid-tread), and floor
    division toward -inf can't push y outside the original signed range
    -- no clipping needed, same guarantee as the bits-based path.
    """
    if levels <= 1:
        sys.exit("--levels must be >= 2")
    full_range = 2 ** bits  # e.g. 65536 for 16-bit
    if levels >= full_range:
        sys.exit(f"--levels must be less than {full_range} for this file")
    delta = full_range // levels
    if delta < 1:
        delta = 1
    return array.array(samples.typecode, [(x // delta) * delta for x in samples])


def write_samples(path, samples, params, unsigned_in_file):
    if unsigned_in_file:
        raw_samples = array.array("b", [x + 128 for x in samples])
        # pack back as unsigned bytes
        out_bytes = bytearray((v & 0xFF) for v in raw_samples)
    else:
        out_bytes = samples.tobytes()

    with wave.open(path, "wb") as wf:
        wf.setparams(params)
        wf.writeframes(out_bytes)


def main():
    ap = argparse.ArgumentParser(description="Uniform scalar quantizer for WAV files")
    ap.add_argument("input", help="input .wav file")
    ap.add_argument("output", help="output .wav file (quantized)")
    group = ap.add_mutually_exclusive_group(required=True)
    group.add_argument("--bits", type=int,
                        help="target effective bits per sample -> 2^bits levels "
                             "(power-of-two shortcut, e.g. 8 for 256 levels)")
    group.add_argument("--levels", type=int,
                        help="target number of quantization levels, any integer "
                             "(not restricted to a power of two)")
    args = ap.parse_args()

    samples, params, bits, unsigned_in_file = read_samples(args.input)

    if args.bits is not None:
        print(f"{args.input}: {bits}-bit container, {params.nchannels} channel(s), "
              f"quantizing to {args.bits} effective bits (2**{args.bits}="
              f"{2**args.bits} levels)")
        quantized = quantize_by_bits(samples, bits, args.bits)
    else:
        print(f"{args.input}: {bits}-bit container, {params.nchannels} channel(s), "
              f"quantizing to {args.levels} levels")
        quantized = quantize_by_levels(samples, bits, args.levels)

    write_samples(args.output, quantized, params, unsigned_in_file)
    print(f"wrote {args.output}")


if __name__ == "__main__":
    main()