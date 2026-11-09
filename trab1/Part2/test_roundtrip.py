#!/usr/bin/env python3
"""
Round-trip tests for the lossless codec.

For every test file: encode, decode, and require the result to be
BYTE-IDENTICAL to the input. Also feeds damaged compressed files to the
decoder and requires that it never crashes.

Usage:
    python3 test_roundtrip.py            # uses ./wav_lossless_enc / _dec
    SUFFIX=_asan python3 test_roundtrip.py   # use the sanitizer builds
"""
import math
import os
import random
import struct
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
SUFFIX = os.environ.get("SUFFIX", "")
ENC = os.path.join(HERE, "wav_lossless_enc" + SUFFIX)
DEC = os.path.join(HERE, "wav_lossless_dec" + SUFFIX)
RATE = 44100
random.seed(1234)


# ---------------------------------------------------------------- builders
def pack_samples(vals, bits):
    """Flat list of signed ints -> little-endian PCM bytes."""
    if bits == 8:
        return bytes(v + 128 for v in vals)
    if bits == 16:
        return struct.pack("<%dh" % len(vals), *vals)
    out = bytearray()
    for v in vals:
        out += (v & 0xFFFFFF).to_bytes(3, "little")
    return bytes(out)


def chunk(cid, body, declared=None):
    size = len(body) if declared is None else declared
    data = cid + struct.pack("<I", size) + body
    if len(body) % 2:
        data += b"\x00"            # RIFF chunks are padded to an even size
    return data


def make_wav(vals, channels=2, bits=16, before=b"", after=b"",
             data_declared=None, tail=b"", tag=1):
    """Build a WAV file. `vals` is the flat interleaved sample list."""
    align = channels * bits // 8
    fmt = struct.pack("<HHIIHH", tag, channels, RATE, RATE * align, align, bits)
    payload = pack_samples(vals, bits) + tail     # tail = partial last frame
    body = b"WAVE" + chunk(b"fmt ", fmt) + before + \
        chunk(b"data", payload, data_declared) + after
    return b"RIFF" + struct.pack("<I", len(body)) + body


def music(frames, channels=2, bits=16):
    """Tones + a little noise: smooth like real audio, left/right similar."""
    top = 2 ** (bits - 1) - 1
    vals = []
    for i in range(frames):
        t = i / RATE
        base = (0.5 * math.sin(2 * math.pi * 220 * t) +
                0.25 * math.sin(2 * math.pi * 660 * t) +
                0.1 * math.sin(2 * math.pi * 1760 * t))
        for c in range(channels):
            s = base * (1.0 if c == 0 else 0.8) + random.gauss(0, 0.004)
            vals.append(max(-top - 1, min(top, int(s * top * 0.9))))
    return vals


def noise(frames, channels, bits):
    lo, hi = -2 ** (bits - 1), 2 ** (bits - 1) - 1
    return [random.randint(lo, hi) for _ in range(frames * channels)]


# ---------------------------------------------------------------- cases
def cases():
    c = []
    add = lambda name, data, flags=(): c.append((name, data, list(flags)))

    m = music(40000)
    add("stereo16 music", make_wav(m))
    add("mono16 music", make_wav(music(40000, 1), channels=1))
    add("stereo8 music", make_wav(music(30000, 2, 8), bits=8))
    add("mono8 music", make_wav(music(30000, 1, 8), channels=1, bits=8))
    add("stereo24 music", make_wav(music(30000, 2, 24), bits=24))
    add("mono24 music", make_wav(music(30000, 1, 24), channels=1, bits=24))

    add("stereo16 silence", make_wav([0] * 20000))
    add("stereo16 white noise", make_wav(noise(20000, 2, 16)))
    add("stereo24 white noise", make_wav(noise(10000, 2, 24), bits=24))
    add("stereo8 white noise", make_wav(noise(10000, 2, 8), bits=8))

    # Worst cases for overflow: full-scale square waves, L and R opposite.
    sq16 = []
    for i in range(6000):
        v = 32767 if (i // 3) % 2 else -32768
        sq16 += [v, -v - 1]
    add("stereo16 full-scale square", make_wav(sq16))
    sq24 = []
    for i in range(6000):
        v = 8388607 if (i // 3) % 2 else -8388608
        sq24 += [v, -v - 1]
    add("stereo24 full-scale square (SIDE 25 bit)", make_wav(sq24, bits=24))
    add("mono24 alternating extremes",
        make_wav([8388607, -8388608] * 3000, channels=1, bits=24))

    # Tiny and odd sizes.
    add("0 frames", make_wav([]))
    for n in (1, 2, 3, 5, 17):
        add("%d frame(s) stereo16" % n, make_wav(music(n)))
    add("1 frame mono8", make_wav([5], channels=1, bits=8))

    # Weird containers: everything outside the audio must survive unchanged.
    add("partial last frame", make_wav(m[:2000], tail=b"\x01\x02\x03"))
    add("LIST chunk before data",
        make_wav(m[:2000], before=chunk(b"LIST", b"INFOabcdefgh")))
    add("odd-sized chunk before data (pad byte)",
        make_wav(m[:2000], before=chunk(b"junk", b"xyz")))
    add("LIST chunk after data",
        make_wav(m[:2000], after=chunk(b"LIST", b"INFO" + bytes(range(20)))))
    add("odd data size + trailing chunk",
        make_wav(m[:2000], tail=b"\x07", after=chunk(b"LIST", b"zz")))
    add("data size field too large (truncated file)",
        make_wav(m[:2000], data_declared=10_000_000))
    add("data size field 0xFFFFFFFF",
        make_wav(m[:2000], data_declared=0xFFFFFFFF))
    add("data size field 0", make_wav(m[:2000], data_declared=0))

    # Inputs the codec cannot model: stored raw, must still round-trip.
    add("empty file", b"")
    add("random bytes", bytes(random.getrandbits(8) for _ in range(5000)))
    add("RIFF header only", b"RIFF\x04\x00\x00\x00WAVE")
    full = make_wav(m[:2000])
    add("file cut inside the fmt chunk", full[:30])
    add("file cut inside the audio", full[:len(full) // 2])
    add("32-bit PCM (unsupported)",
        make_wav([random.randint(-2**31, 2**31 - 1) for _ in range(2000)],
                 bits=32).replace(b"\x00", b"\x00"))
    add("3 channels (unsupported)", make_wav(m[:3000], channels=3))
    add("float format tag (unsupported)", make_wav(m[:2000], tag=3))

    # Block sizes, including degenerate ones.
    for b in (1, 2, 7, 64, 1000, 100000):
        add("block size %d" % b, make_wav(m[:30000]), ["-b", str(b)])
    return c


# ---------------------------------------------------------------- running
def run(cmd):
    return subprocess.run(cmd, capture_output=True)


def roundtrip(tmp, name, data, flags):
    src = os.path.join(tmp, "in.wav")
    cmp_ = os.path.join(tmp, "c.bin")
    out = os.path.join(tmp, "out.wav")
    with open(src, "wb") as f:
        f.write(data)
    for p in (cmp_, out):
        if os.path.exists(p):
            os.remove(p)

    e = run([ENC] + flags + [src, cmp_])
    if e.returncode != 0:
        return False, "encoder failed: %r" % e.stderr[-200:], 0
    d = run([DEC, cmp_, out])
    if d.returncode != 0:
        return False, "decoder failed: %r" % d.stderr[-200:], 0
    with open(out, "rb") as f:
        got = f.read()
    csize = os.path.getsize(cmp_)
    if got != data:
        return False, "output differs (%d vs %d bytes)" % (len(got), len(data)), csize
    return True, "", csize


def corruption_tests(tmp):
    """Damaged compressed files: the decoder may fail, but must not crash."""
    data = make_wav(music(20000))
    src = os.path.join(tmp, "in.wav")
    cmp_ = os.path.join(tmp, "c.bin")
    bad = os.path.join(tmp, "bad.bin")
    out = os.path.join(tmp, "out.wav")
    open(src, "wb").write(data)
    assert run([ENC, src, cmp_]).returncode == 0
    good = open(cmp_, "rb").read()

    variants = []
    for cut in (0, 1, 4, 5, 20, 40, 60, len(good) // 2, len(good) - 1):
        variants.append(("truncated to %d bytes" % cut, good[:cut]))
    rnd = random.Random(7)
    for i in range(40):
        b = bytearray(good)
        for _ in range(rnd.choice((1, 3, 20))):
            b[rnd.randrange(len(b))] ^= 1 << rnd.randrange(8)
        variants.append(("bit flips #%d" % i, bytes(b)))
    for i in range(10):
        b = bytearray(good)
        b[rnd.randrange(4, 40)] = rnd.randrange(256)   # damage the header
        variants.append(("header byte #%d" % i, bytes(b)))
    variants.append(("random garbage", bytes(rnd.getrandbits(8) for _ in range(3000))))

    crashed = 0
    for name, blob in variants:
        open(bad, "wb").write(blob)
        r = run([DEC, bad, out])
        sanitizer = b"Sanitizer" in r.stderr or b"runtime error" in r.stderr
        if r.returncode < 0 or sanitizer:        # killed by a signal, or UB found
            crashed += 1
            print("  CRASH on %-24s rc=%d %r" % (name, r.returncode, r.stderr[:160]))
    return len(variants), crashed


def main():
    for p in (ENC, DEC):
        if not os.path.exists(p):
            sys.exit("missing %s - run `make` first" % p)

    failures = 0
    print("%-46s %9s %9s %7s  %s" % ("case", "input", "packed", "ratio", "result"))
    with tempfile.TemporaryDirectory() as tmp:
        for name, data, flags in cases():
            ok, why, csize = roundtrip(tmp, name, data, flags)
            ratio = "%.2f" % (len(data) / csize) if csize else "-"
            print("%-46s %9d %9d %7s  %s" % (name, len(data), csize, ratio,
                                              "ok" if ok else "FAIL " + why))
            failures += not ok

        print("\ncorrupted compressed files:")
        total, crashed = corruption_tests(tmp)
        print("  %d damaged files tried, %d crashes" % (total, crashed))
        failures += crashed

    print("\n%s" % ("ALL PASSED" if failures == 0 else "%d FAILURE(S)" % failures))
    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()
