#!/usr/bin/env python3
"""
test_lossy.py - end-to-end tests of wav_lossy_enc / wav_lossy_dec.

    python3 test_lossy.py                       # all tests, default binaries
    python3 test_lossy.py --enc X --dec Y       # other binaries (e.g. sanitizer build)
    python3 test_lossy.py --quick               # fewer cases (used by `make asan`)

Checks:
  - every bit depth (8/16/24), mono and stereo, sine / noise / silence,
    both transforms and both stereo modes: decoded file has the same rate,
    channels, bit depth and length, and with --snr X the SNR is at least X;
  - tiny files: 0, 1, N-1, N, N+1 frames;
  - every block size, several SNR targets, --step, --round;
  - real recordings from the course samples, if they are present;
  - damaged input: truncated, extended and bit-flipped compressed files are
    rejected (or at least decoded) without crashing; bad options and
    unsupported WAV files are refused.
The SNR is computed here, independently of the C code.
"""
import argparse
import math
import os
import random
import struct
import subprocess
import sys
import tempfile
import wave

HERE = os.path.dirname(os.path.abspath(__file__))
SAMPLES = os.path.join(HERE, "..", "Audio files (WAV format)-20261020")

failures = 0
runs = 0


def fail(msg):
    global failures
    failures += 1
    print("FAIL:", msg)


# ---------------------------------------------------------------- WAV I/O

def write_wav(path, chans, bits, rate=44100, extra_chunk=False):
    """chans: list of lists of ints (same length). 8-bit values are signed here."""
    n = len(chans[0]) if chans else 0
    nc = len(chans)
    out = bytearray()
    for i in range(n):
        for c in range(nc):
            v = chans[c][i]
            if bits == 8:
                out.append(v + 128)
            elif bits == 16:
                out += struct.pack("<h", v)
            else:
                out += struct.pack("<i", v)[:3]
    if not extra_chunk:
        with wave.open(path, "wb") as w:
            w.setnchannels(nc)
            w.setsampwidth(bits // 8)
            w.setframerate(rate)
            w.writeframes(bytes(out))
        return
    # Hand-made file with a LIST chunk between fmt and data.
    align = nc * bits // 8
    fmt = struct.pack("<HHIIHH", 1, nc, rate, rate * align, align, bits)
    lst = b"INFOISFT" + struct.pack("<I", 6) + b"test\0\0"
    body = b"WAVE" + b"fmt " + struct.pack("<I", 16) + fmt
    body += b"LIST" + struct.pack("<I", len(lst)) + lst
    body += b"data" + struct.pack("<I", len(out)) + bytes(out)
    with open(path, "wb") as f:
        f.write(b"RIFF" + struct.pack("<I", len(body)) + body)


def read_wav(path):
    """Returns (channels, bits, rate, list of per-channel int lists, header size)."""
    with wave.open(path, "rb") as w:
        nc, sw, rate, n = w.getnchannels(), w.getsampwidth(), w.getframerate(), w.getnframes()
        raw = w.readframes(n)
    bits = sw * 8
    chans = [[] for _ in range(nc)]
    if bits == 8:
        vals = [b - 128 for b in raw]
    elif bits == 16:
        vals = list(struct.unpack("<%dh" % (len(raw) // 2), raw))
    else:
        vals = [int.from_bytes(raw[i:i + 3], "little", signed=True) for i in range(0, len(raw), 3)]
    for i, v in enumerate(vals):
        chans[i % nc].append(v)
    return nc, bits, rate, chans, os.path.getsize(path) - len(raw)


def snr(a, b):
    es = sum(v * v for ch in a for v in ch)
    en = sum((x - y) ** 2 for ca, cb in zip(a, b) for x, y in zip(ca, cb))
    if en == 0:
        return math.inf
    if es == 0:
        return -math.inf
    return 10 * math.log10(es / en)


# ---------------------------------------------------------------- signals

def make_signal(kind, n, nc, bits, seed=1):
    rnd = random.Random(seed)
    amp = (1 << (bits - 1)) - 1
    chans = []
    for c in range(nc):
        if kind == "silence":
            ch = [0] * n
        elif kind == "sine":
            f1, f2 = 440.0 * (1 + 0.5 * c), 3150.0
            ch = [int(round(amp * (0.6 * math.sin(2 * math.pi * f1 * i / 44100)
                                   + 0.3 * math.sin(2 * math.pi * f2 * i / 44100 + c))))
                  for i in range(n)]
        elif kind == "noise":
            ch = [rnd.randint(-amp // 2, amp // 2) for _ in range(n)]
        elif kind == "full":          # full-scale square wave: exercises clipping
            ch = [amp if (i // 50) % 2 else -amp - 1 for i in range(n)]
        else:
            raise ValueError(kind)
        chans.append(ch)
    return chans


# ---------------------------------------------------------------- running

def run(cmd):
    global runs
    runs += 1
    p = subprocess.run(cmd, capture_output=True, text=True)
    err = p.stderr
    if p.returncode < 0 or "Sanitizer" in err or "runtime error" in err:
        fail("crash or sanitizer report: %s\n%s" % (" ".join(cmd), err[-2000:]))
    return p.returncode, err


def roundtrip(tmp, name, chans, bits, opts, target=None, extra_chunk=False, rate=44100):
    """Encode + decode; check format, length and SNR. Returns (snr, compressed size)."""
    src = os.path.join(tmp, name + ".wav")
    cmp_ = os.path.join(tmp, name + ".icd")
    dst = os.path.join(tmp, name + ".out.wav")
    write_wav(src, chans, bits, rate, extra_chunk)
    rc, err = run([ENC] + opts + [src, cmp_])
    if rc != 0:
        fail("%s: encoder failed (%d): %s" % (name, rc, err.strip()))
        return None, None
    rc, err = run([DEC, cmp_, dst])
    if rc != 0:
        fail("%s: decoder failed (%d): %s" % (name, rc, err.strip()))
        return None, None
    nc, b, r, out, hdr = read_wav(dst)
    n = len(chans[0])
    if (nc, b, r) != (len(chans), bits, rate):
        fail("%s: format %s, expected %s" % (name, (nc, b, r), (len(chans), bits, rate)))
    if any(len(ch) != n for ch in out):
        fail("%s: %s frames, expected %d" % (name, [len(ch) for ch in out], n))
        return None, None
    if hdr != 44:
        fail("%s: output header is %d bytes, expected 44" % (name, hdr))
    s = snr(chans, out)
    if target is not None and s < target - 1e-9:
        fail("%s: SNR %.3f dB below target %.3f" % (name, s, target))
    if all(v == 0 for ch in chans for v in ch) and s != math.inf:
        fail("%s: silence did not decode to silence" % name)
    return s, os.path.getsize(cmp_)


# ---------------------------------------------------------------- tests

def test_formats(tmp, quick):
    kinds = ["sine", "noise", "silence", "full"]
    for bits in (8, 16, 24):
        for nc in (1, 2):
            for kind in kinds:
                for tr in ("dct", "mdct"):
                    for st in (("lr", "ms") if nc == 2 else ("ms",)):
                        if quick and (kind == "full" or st == "lr"):
                            continue
                        sig = make_signal(kind, 3000, nc, bits)
                        target = 25 if bits == 8 else 30
                        name = "fmt_%d_%d_%s_%s_%s" % (bits, nc, kind, tr, st)
                        roundtrip(tmp, name, sig, bits,
                                  ["-b", "256", "--transform", tr, "--stereo", st,
                                   "--snr", str(target)], target)


def test_tiny(tmp, quick):
    for n_block in (64, 2048):
        for frames in sorted({0, 1, n_block - 1, n_block, n_block + 1}):
            for tr in ("dct", "mdct"):
                for nc in ((1, 2) if not quick else (2,)):
                    sig = make_signal("sine", frames, nc, 16)
                    target = 30 if frames else None
                    roundtrip(tmp, "tiny_%d_%d_%s_%d" % (n_block, frames, tr, nc), sig, 16,
                              ["-b", str(n_block), "--transform", tr, "--snr", "30"], target)


def test_block_sizes(tmp, quick):
    sig = make_signal("sine", 20000, 2, 16)
    sizes = [64, 128, 256, 512, 1024, 2048, 4096, 8192]
    for n_block in (sizes if not quick else [64, 8192]):
        for tr in ("dct", "mdct"):
            roundtrip(tmp, "bs_%d_%s" % (n_block, tr), sig, 16,
                      ["-b", str(n_block), "--transform", tr, "--snr", "35"], 35)


def test_targets(tmp, quick):
    sig = make_signal("sine", 30000, 2, 16, seed=3)
    noise = make_signal("noise", 30000, 2, 16, seed=4)
    sig = [[(a + b // 8) for a, b in zip(c1, c2)] for c1, c2 in zip(sig, noise)]
    sig = [[max(-32768, min(32767, v)) for v in ch] for ch in sig]
    prev_size = 0
    for target in ((5, 10, 20, 40, 60, 80) if not quick else (20, 60)):
        s, size = roundtrip(tmp, "target_%d" % target, sig, 16, ["--snr", str(target)], target)
        if size is not None and size < prev_size:
            fail("target %d: smaller file (%d) than a lower target (%d)" % (target, size, prev_size))
        prev_size = size or prev_size
    # A target that integer rounding cannot reach: warning, still decodes.
    roundtrip(tmp, "target_unreachable", make_signal("sine", 3000, 1, 8), 8, ["--snr", "200"])
    # Loud tonal input, low target: the step limit must not stop the encoder
    # from going down to the target (it used to stop around 20 dB).
    loud = make_signal("sine", 20000, 2, 16, seed=5)
    for target in (5, 10):
        s, _ = roundtrip(tmp, "loud_low_%d" % target, loud, 16, ["--snr", str(target)], target)
        if s is not None and s > target + 1.5:
            fail("loud tonal input, target %d dB: got %.2f dB (step limited)" % (target, s))
    # High targets on quiet input, close to the 16-bit limit, must be met
    # without a warning (the output rounding is part of the estimate).
    # It must not overshoot by much either (that would waste bits).
    quiet = [[v // 30 for v in ch] for ch in sig]
    for target in (60, 66):
        src = os.path.join(tmp, "high.wav")
        write_wav(src, quiet, 16)
        rc, err = run([ENC, "--snr", str(target), src, os.path.join(tmp, "high.icd")])
        if rc != 0 or "warning" in err:
            fail("target %d dB: %s" % (target, err.strip() or "encoder failed"))
        s, _ = roundtrip(tmp, "high_%d" % target, quiet, 16, ["--snr", str(target)], target)
        if s is not None and s > target + 0.25:
            fail("quiet input, target %d dB: got %.2f dB (overshoot)" % (target, s))
    # Fixed step: bigger step, lower SNR.
    s1, _ = roundtrip(tmp, "step_small", sig, 16, ["--step", "4"])
    s2, _ = roundtrip(tmp, "step_large", sig, 16, ["--step", "400"])
    if s1 is not None and s2 is not None and not s1 > s2:
        fail("--step 4 gave %.2f dB, --step 400 gave %.2f dB" % (s1, s2))
    # Rounding offset below 0.5 still meets the target (search adapts the step).
    roundtrip(tmp, "round_035", sig, 16, ["--snr", "30", "--round", "0.35"], 30)
    # Extra chunks in the input: output still has a plain 44-byte header.
    roundtrip(tmp, "extra_chunk", sig, 16, ["--snr", "30"], 30, extra_chunk=True)
    # Unusual rate is kept.
    roundtrip(tmp, "rate_8000", make_signal("sine", 4000, 1, 16), 16, ["--snr", "30"], 30, rate=8000)


def test_real(tmp, quick):
    if not os.path.isdir(SAMPLES):
        print("  (course samples not found, skipping real recordings)")
        return
    files = sorted(f for f in os.listdir(SAMPLES) if f.endswith(".wav"))[:1 if quick else 2]
    for f in files:
        _, bits, _, chans, _ = read_wav(os.path.join(SAMPLES, f))
        # Two seconds are enough and keep the Python SNR fast.
        chans = [ch[44100 * 5:44100 * 7] for ch in chans]
        for tr in ("dct", "mdct"):
            for target in (20, 40):
                s, size = roundtrip(tmp, "real_%s_%s_%d" % (f[:-4], tr, target), chans, bits,
                                    ["--transform", tr, "--snr", str(target)], target)
                if s is not None:
                    kbps = size * 8 / 2.0 / 1000
                    print("  %s %-4s target %2d dB: %.2f dB, %.0f kbps" % (f, tr, target, s, kbps))


def test_damaged(tmp, quick):
    src = os.path.join(tmp, "dmg.wav")
    good = os.path.join(tmp, "dmg.icd")
    bad = os.path.join(tmp, "dmg_bad.icd")
    out = os.path.join(tmp, "dmg_out.wav")
    write_wav(src, make_signal("sine", 20000, 2, 16), 16)
    rc, _ = run([ENC, "--snr", "40", src, good])
    data = open(good, "rb").read()

    def decode(blob, expect_fail, what):
        with open(bad, "wb") as f:
            f.write(blob)
        if os.path.exists(out):
            os.remove(out)
        rc, err = run([DEC, bad, out])
        if expect_fail and rc == 0:
            fail("%s: decoder accepted it" % what)
        if rc != 0 and os.path.exists(out):
            fail("%s: decoder failed but left %s behind" % (what, out))
        return rc

    # Truncations: anywhere is an error.
    cuts = [0, 3, 4, 27, 28, 29, 32, len(data) // 2, len(data) - 5, len(data) - 1]
    for cut in cuts:
        decode(data[:cut], True, "truncated to %d of %d bytes" % (cut, len(data)))
    decode(data + b"\0", True, "one extra byte")
    decode(b"RIFF" + data[4:], True, "wrong magic")

    # Header fields with impossible values.
    def patched(off, fmt, val):
        b = bytearray(data)
        struct.pack_into(fmt, b, off, val)
        return bytes(b)
    for off, fmt, val, what in [
        (4, "<B", 3, "channels 3"), (4, "<B", 0, "channels 0"),
        (5, "<B", 12, "bits 12"), (6, "<B", 2, "transform 2"), (7, "<B", 2, "stereo 2"),
        (20, "<I", 100, "N 100"), (20, "<I", 32, "N 32"), (20, "<I", 16384, "N 16384"),
        (24, "<f", 0.0, "step 0"), (24, "<f", -1.0, "step -1"), (24, "<f", math.inf, "step inf"),
        (24, "<I", 0x7FC00000, "step NaN"), (12, "<Q", 1 << 40, "frames 2^40"),
    ]:
        decode(patched(off, fmt, val), True, "header " + what)
    # Mono file claiming mid/side.
    b = bytearray(data)
    b[4] = 1
    decode(bytes(b), True, "header channels 1 with stereo ms")

    # Random damage in the payload: must not crash; usually rejected.
    rnd = random.Random(7)
    rejected = 0
    trials = 10 if quick else 60
    for t in range(trials):
        b = bytearray(data)
        for _ in range(1 + t % 4):
            i = rnd.randrange(28, len(b))
            b[i] ^= 1 << rnd.randrange(8)
        rejected += decode(bytes(b), False, "bit flips") != 0
    print("  payload bit flips: %d/%d rejected, none crashed" % (rejected, trials))


def test_cli(tmp, quick):
    src = os.path.join(tmp, "cli.wav")
    dst = os.path.join(tmp, "cli.icd")
    write_wav(src, make_signal("sine", 1000, 1, 16), 16)
    for opts in (["--step", "1", "--snr", "30"], ["-b", "100"], ["-b", "32"], ["-b", "16384"],
                 ["--transform", "fft"], ["--stereo", "xy"], ["--step", "0"], ["--step", "-2"],
                 ["--snr", "0"], ["--snr", "abc"], ["--round", "0.6"], ["--round", "0.2"],
                 ["--bogus"], ["-b"]):
        rc, _ = run([ENC] + opts + [src, dst])
        if rc == 0:
            fail("encoder accepted options %s" % opts)
    rc, _ = run([ENC, src])
    if rc == 0:
        fail("encoder accepted one file name")
    # Not a WAV, and a 32-bit float WAV: refused.
    junk = os.path.join(tmp, "junk.wav")
    with open(junk, "wb") as f:
        f.write(b"this is not a wav file at all" * 10)
    rc, _ = run([ENC, junk, dst])
    if rc == 0:
        fail("encoder accepted a non-WAV file")
    fl = os.path.join(tmp, "float.wav")
    fmt = struct.pack("<HHIIHH", 3, 1, 44100, 44100 * 4, 4, 32)
    body = b"WAVEfmt " + struct.pack("<I", 16) + fmt + b"data" + struct.pack("<I", 400) + bytes(400)
    with open(fl, "wb") as f:
        f.write(b"RIFF" + struct.pack("<I", len(body)) + body)
    rc, _ = run([ENC, fl, dst])
    if rc == 0:
        fail("encoder accepted a float WAV")
    rc, _ = run([ENC, os.path.join(tmp, "missing.wav"), dst])
    if rc == 0:
        fail("encoder accepted a missing file")
    rc, _ = run([DEC, os.path.join(tmp, "missing.icd"), dst])
    if rc == 0:
        fail("decoder accepted a missing file")


def main():
    global ENC, DEC
    ap = argparse.ArgumentParser()
    ap.add_argument("--enc", default=os.path.join(HERE, "wav_lossy_enc"))
    ap.add_argument("--dec", default=os.path.join(HERE, "wav_lossy_dec"))
    ap.add_argument("--quick", action="store_true")
    a = ap.parse_args()
    ENC, DEC = os.path.abspath(a.enc), os.path.abspath(a.dec)

    with tempfile.TemporaryDirectory() as tmp:
        for t in (test_formats, test_tiny, test_block_sizes, test_targets,
                  test_real, test_damaged, test_cli):
            print(t.__name__)
            t(tmp, a.quick)

    if failures:
        print("test_lossy: %d FAILED (%d runs)" % (failures, runs))
        return 1
    print("test_lossy: all passed (%d runs)" % runs)
    return 0


if __name__ == "__main__":
    sys.exit(main())
