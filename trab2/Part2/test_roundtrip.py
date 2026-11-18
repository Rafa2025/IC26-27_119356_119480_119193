#!/usr/bin/env python3
"""
test_roundtrip.py - checks that img_lossless_enc / img_lossless_dec rebuild every
file byte for byte, and fail cleanly (exit code 1, no output, no crash) on bad input.

  python3 test_roundtrip.py ../../ImageFiles              # normal build
  python3 test_roundtrip.py ../../ImageFiles --bin san    # sanitizer build
  python3 test_roundtrip.py ../../ImageFiles --quick      # skip files over 3 MB

Three groups of tests:
  1. every image in the folder (.gz files are unzipped first; RAW sizes from the name)
  2. synthetic edge cases: tiny / odd sizes, maxval 1 and 4095, 16-bit RAW in both
     byte orders, header comments, extra bytes after the pixels, several tile sizes
  3. bad input: unsupported or truncated images, truncated / damaged compressed files
"""
import argparse
import gzip
import os
import random
import re
import subprocess
import sys
import tempfile
import time

SAN_ENV = dict(os.environ, ASAN_OPTIONS="exitcode=99:detect_leaks=1",
               UBSAN_OPTIONS="halt_on_error=1:exitcode=99:print_stacktrace=1")

fails = []


def run(cmd):
    t0 = time.time()
    p = subprocess.run(cmd, capture_output=True, env=SAN_ENV)
    return p.returncode, p.stderr.decode(errors="replace").strip(), time.time() - t0


def check(name, ok, detail=""):
    if not ok:
        fails.append(f"{name}: {detail}")
        print(f"  FAIL {name}  {detail}")


def roundtrip(b, src, tmp, args=(), label=None):
    label = label or os.path.basename(src)
    comp, out = os.path.join(tmp, "c.icl"), os.path.join(tmp, "out")
    for f in (comp, out):
        if os.path.exists(f):
            os.remove(f)
    rc, err, te = run([b["enc"], *args, src, comp])
    if rc != 0:
        check(label, False, f"encoder exit {rc}: {err}")
        return None
    rc, err, td = run([b["dec"], comp, out])
    if rc != 0:
        check(label, False, f"decoder exit {rc}: {err}")
        return None
    same = open(src, "rb").read() == open(out, "rb").read()
    check(label, same, "decoded file differs from the original")
    return os.path.getsize(src), os.path.getsize(comp), te, td


def expect_error(b, name, cmd, out):
    if os.path.exists(out):
        os.remove(out)
    rc, err, _ = run(cmd)
    ok = rc == 1 and not os.path.exists(out)
    check(name, ok, f"exit {rc}, output left: {os.path.exists(out)}, stderr: {err[-300:]}")
    return rc


# ------------------------------------------------------------------ 1. real images
def raw_args(path):
    m = re.search(r"u(8|16)(be|le)?-1x(\d+)x(\d+)", os.path.basename(path))
    if not m:
        return None
    bpp, endian, rows, cols = m[1], m[2] or "be", m[3], m[4]
    a = ["-rows", rows, "-cols", cols]
    if bpp == "16":
        a += ["-bpp", "16", f"-{endian}"]
    return a


def real_images(b, folder, tmp, quick):
    files = sorted(os.path.join(d, f) for d, _, fs in os.walk(folder) for f in fs
                   if f.endswith((".pgm", ".ppm", ".raw", ".gz")))
    print(f"1. {len(files)} images from {folder}")
    n = 0
    for f in files:
        if quick and os.path.getsize(f) > 3 << 20:
            continue
        src = f
        if f.endswith(".gz"):
            src = os.path.join(tmp, os.path.basename(f)[:-3])
            with gzip.open(f, "rb") as g, open(src, "wb") as o:
                o.write(g.read())
        args = raw_args(src) if src.endswith(".raw") else []
        if args is None:
            check(os.path.basename(f), False, "cannot get RAW size from the file name")
            continue
        r = roundtrip(b, src, tmp, args, os.path.basename(f))
        n += r is not None
    print(f"   {n} round trips done")


# ------------------------------------------------------------------ 2. synthetic
def pnm(magic, w, h, maxval, data, header=None, trailer=b""):
    head = header if header is not None else f"{magic}\n{w} {h}\n{maxval}\n".encode()
    return head + data + trailer


def samples(n, maxval, rng, big=True):
    vals = [rng.randint(0, maxval) for _ in range(n)]
    if maxval < 256:
        return bytes(vals)
    return b"".join(v.to_bytes(2, "big" if big else "little") for v in vals)


def synthetic(b, tmp):
    rng = random.Random(7)
    cases = []
    for w, h in ((1, 1), (1, 40), (40, 1), (257, 300), (300, 17)):
        cases.append((f"pgm {w}x{h}", pnm("P5", w, h, 255, samples(w * h, 255, rng)), []))
    cases += [
        ("pgm maxval 1", pnm("P5", 33, 21, 1, samples(33 * 21, 1, rng)), []),
        ("pgm maxval 4095 (16-bit)", pnm("P5", 70, 50, 4095, samples(70 * 50, 4095, rng)), []),
        ("pgm values above maxval", pnm("P5", 20, 20, 100, samples(400, 255, rng)), []),
        ("ppm 8-bit 129x77", pnm("P6", 129, 77, 255, samples(129 * 77 * 3, 255, rng)), []),
        ("ppm 16-bit", pnm("P6", 31, 45, 65535, samples(31 * 45 * 3, 65535, rng)), []),
        ("ppm values above maxval", pnm("P6", 16, 16, 200, samples(768, 255, rng)), ["-c", "ycocg"]),
        ("pgm header with comments, tabs, CRLF",
         pnm("P5", 9, 7, 255, samples(63, 255, rng),
             header=b"P5 # first\r\n#another comment\r\n\t9  7 # size\n255\n"), []),
        ("pgm extra bytes after the pixels", pnm("P5", 10, 10, 255, samples(100, 255, rng),
                                                 trailer=b"\nextra trailing bytes\n"), []),
        ("ppm extra bytes after the pixels", pnm("P6", 8, 8, 255, samples(192, 255, rng),
                                                 trailer=b"\x00\x01\x02"), []),
        ("raw 8-bit 50x60", samples(3000, 255, rng), ["-rows", "50", "-cols", "60"]),
        ("raw 16-bit big-endian", samples(1200, 65535, rng, True),
         ["-rows", "30", "-cols", "40", "-bpp", "16", "-be"]),
        ("raw 16-bit little-endian", samples(1200, 65535, rng, False),
         ["-rows", "30", "-cols", "40", "-bpp", "16", "-le"]),
        ("raw 16-bit, extra bytes", samples(1200, 65535, rng) + b"xyz",
         ["-rows", "30", "-cols", "40", "-bpp", "16"]),
        ("raw 1x1", b"\x07", ["-rows", "1", "-cols", "1"]),
        ("raw that starts with 'P5'", b"P5" + samples(98, 255, rng), ["-rows", "10", "-cols", "10"]),
    ]
    # colour transforms: every combination of extreme channel values, each transform forced
    for mv in (255, 65535):
        ext = [0, 1, mv // 2, mv - 1, mv]
        px = [(r, g, b) for r in ext for g in ext for b in ext] * 4          # 500 pixels
        data = b"".join(v.to_bytes(1 if mv < 256 else 2, "big") for p in px for v in p)
        for tf in ("none", "rct", "ycocg", "auto", "best"):
            cases.append((f"ppm extremes maxval {mv}, -c {tf}", pnm("P6", 25, 20, mv, data), ["-c", tf, "-nopal"]))
    # palette mode
    def pal_img(k, w, h, mv=255, seed=3):
        r2 = random.Random(seed)
        cols = set()
        while len(cols) < k:
            cols.add(tuple(r2.randint(0, mv) for _ in range(3)))
        cols = sorted(cols)
        px = [cols[i % k] for i in range(k)] + [r2.choice(cols) for _ in range(w * h - k)]
        r2.shuffle(px)
        nb = 1 if mv < 256 else 2
        return pnm("P6", w, h, mv, b"".join(v.to_bytes(nb, "big") for p in px for v in p))
    cases += [
        ("palette: 1 colour", pal_img(1, 40, 30), []),
        ("palette: 2 colours", pal_img(2, 64, 64), []),
        ("palette: exactly 256 colours", pal_img(256, 100, 90), []),
        ("palette: 257 colours (not palette)", pal_img(257, 100, 90), []),
        ("palette: 16-bit, 200 colours", pal_img(200, 60, 70, 65535), []),
        ("palette: 256 colours, tile 16", pal_img(256, 70, 50), ["-t", "16"]),
        ("palette: 1x1", pal_img(1, 1, 1), []),
        ("palette disabled (-nopal)", pal_img(50, 40, 40), ["-nopal"]),
    ]
    big = pnm("P5", 300, 200, 255, samples(60000, 255, rng))
    for t in ("16", "100", "16384"):
        cases.append((f"pgm 300x200, tile {t}", big, ["-t", t]))
    print(f"2. {len(cases)} synthetic edge cases")
    src = os.path.join(tmp, "synth")
    for name, data, args in cases:
        with open(src, "wb") as f:
            f.write(data)
        roundtrip(b, src, tmp, args, name)


# ------------------------------------------------------------------ 3. bad input
def bad_input(b, tmp):
    rng = random.Random(11)
    out = os.path.join(tmp, "out")
    comp = os.path.join(tmp, "c.icl")
    src = os.path.join(tmp, "bad")
    n = 0

    def enc_error(name, data, args=()):
        nonlocal n
        with open(src, "wb") as f:
            f.write(data)
        expect_error(b, name, [b["enc"], *args, src, comp], comp)
        n += 1

    enc_error("ascii PGM (P2)", b"P2\n2 2\n255\n1 2 3 4\n")
    enc_error("not an image, no -rows/-cols", b"hello world")
    enc_error("PGM shorter than its header says", pnm("P5", 10, 10, 255, bytes(50)))
    enc_error("PGM maxval 0", b"P5\n2 2\n0\n\x00\x00\x00\x00")
    enc_error("PGM maxval 70000", b"P5\n2 2\n70000\n" + bytes(8))
    enc_error("PGM without whitespace after maxval", b"P5\n2 2\n255")
    enc_error("RAW shorter than rows x cols", bytes(10), ["-rows", "4", "-cols", "4"])
    enc_error("RAW -bpp 12", bytes(32), ["-rows", "4", "-cols", "4", "-bpp", "12"])
    expect_error(b, "missing input file", [b["enc"], os.path.join(tmp, "nope.pgm"), comp], comp)
    n += 1

    # a valid compressed file to damage
    good = pnm("P6", 70, 40, 255, samples(70 * 40 * 3, 255, rng))
    with open(src, "wb") as f:
        f.write(good)
    rc, err, _ = run([b["enc"], "-t", "32", src, comp])
    check("encode reference for damage tests", rc == 0, err)
    data = open(comp, "rb").read()
    dmg = os.path.join(tmp, "dmg.icl")

    def dec_error(name, blob):
        nonlocal n
        with open(dmg, "wb") as f:
            f.write(blob)
        expect_error(b, name, [b["dec"], dmg, out], out)
        n += 1

    dec_error("empty compressed file", b"")
    for cut in (3, 10, 27, 40, 80, len(data) // 2, len(data) - 1):
        dec_error(f"compressed file cut at {cut}", data[:cut])
    dec_error("compressed file with extra bytes", data + b"\x00")
    for i in range(150):
        pos = rng.randrange(len(data))
        blob = bytearray(data)
        blob[pos] ^= 1 << rng.randrange(8)
        dec_error(f"bit flip at byte {pos}", bytes(blob))
    print(f"3. {n} bad-input cases")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("folder", help="ImageFiles folder")
    ap.add_argument("--bin", default=".", help="folder with the binaries (default: .)")
    ap.add_argument("--quick", action="store_true", help="skip images over 3 MB")
    args = ap.parse_args()
    b = {k: os.path.join(args.bin, f"img_lossless_{k}") for k in ("enc", "dec")}
    with tempfile.TemporaryDirectory() as tmp:
        real_images(b, args.folder, tmp, args.quick)
        synthetic(b, tmp)
        bad_input(b, tmp)
    print("\nALL PASSED" if not fails else f"\n{len(fails)} FAILED")
    sys.exit(1 if fails else 0)


if __name__ == "__main__":
    main()
