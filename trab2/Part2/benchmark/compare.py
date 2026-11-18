#!/usr/bin/env python3
"""
compare.py - fair comparison of our codec with other lossless codecs.

Every codec is run as a separate program on the same files, and measured the same way:
  - bits per pixel: compressed file size x 8 / pixels
  - time: wall clock of the encoder / decoder process (best of --repeat runs,
    or a single run if it takes more than 2 s)
  - peak memory: maximum resident set size of that process (measured by memrun.c,
    a tiny launcher, so Python's own memory is not counted)
  - lossless check: decoded pixels must equal the original pixels

Codecs (each used only if its programs are found):
  ours       ../img_lossless_enc / ../img_lossless_dec
  JPEG-LS    CharLS through jls_tool (built here from jls_tool.c if needed)
  JPEG-XL    cjxl -d 0 (lossless), efforts 3 and 7 (7 = cjxl's default); djxl
  PNG        ImageMagick convert, compression level 9
  xz -9      on the original file
  zstd -19   on the original file

Inputs that a tool cannot read directly are converted first (not timed): Kodak .gz
files are unzipped, RAW files are turned into 16-bit PGM for cjxl and convert.

  python3 compare.py ../../../ImageFiles                   # all cores
  python3 compare.py ../../../ImageFiles --threads 1       # one thread for everyone
  python3 compare.py ../../../ImageFiles --groups pgm8 --codecs ours JPEG-LS
Writes results/compare.csv (one row per file and codec) and results/compare.md.
"""
import argparse
import csv
import gzip
import os
import re
import shutil
import subprocess
import sys
import tempfile

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "..", "Part1"))
from imgio import read_image  # noqa: E402

GROUPS = {"pgm8": "imgs-pgm8", "raw16": "imgs-raw16", "ppm": "imgs-ppm", "kodak": "imgs-ppm/kodak"}
GROUP_NAMES = {"pgm8": "8-bit grayscale", "raw16": "16-bit RAW", "ppm": "colour (12, incl. arial)",
               "kodak": "palette: Kodak"}


# ------------------------------------------------------------------ measuring
MEMRUN = os.path.join(HERE, "memrun")


def run(cmd, env=None):
    """Run cmd through memrun; return (wall seconds, peak RSS in MB) of the command
    itself (a process forked from Python would report Python's own memory). Exits on
    failure."""
    res = os.path.join(tempfile.gettempdir(), f"memrun_{os.getpid()}.txt")
    p = subprocess.run([MEMRUN, res, *cmd], stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, env=env)
    if p.returncode != 0:
        sys.exit(f"failed ({p.returncode}): {' '.join(cmd)}\n{p.stderr.decode(errors='replace')[-500:]}")
    t, kb = open(res).read().split()
    return float(t), int(kb) / 1024


def best(cmd, repeat, env=None):
    t, m = run(cmd, env)
    if t < 2:
        for _ in range(repeat - 1):
            t2, m2 = run(cmd, env)
            t, m = min(t, t2), max(m, m2)
    return t, m


# ------------------------------------------------------------------ codecs
class Codec:
    """enc(src, info, out) / dec(comp, out) return command lists; check(info, out) -> bool."""

    def __init__(self, name, ext, enc, dec, check="pixels", needs_pnm=False, available=True):
        self.name, self.ext, self.enc, self.dec = name, ext, enc, dec
        self.check, self.needs_pnm, self.available = check, needs_pnm, available


def raw_args(path):
    m = re.search(r"u(8|16)(be|le)?-1x(\d+)x(\d+)", os.path.basename(path))
    if not m:
        return None
    a = ["-rows", m[3], "-cols", m[4]]
    if m[1] == "16":
        a += ["-bpp", "16", f"-{m[2] or 'be'}"]
    return a


def make_codecs(threads, ours_bin):
    T = str(threads) if threads else None
    if not os.path.exists(MEMRUN):
        subprocess.run(["cc", "-O2", "-o", MEMRUN, os.path.join(HERE, "memrun.c")], check=True)
    jls = os.path.join(HERE, "jls_tool")
    if not os.path.exists(jls) and shutil.which("cc"):
        subprocess.run(["cc", "-O2", "-o", jls, os.path.join(HERE, "jls_tool.c"),
                        os.path.join(HERE, "..", "imgfile.c"), "-lcharls"], capture_output=True)
    enc = os.path.join(ours_bin, "img_lossless_enc")
    dec = os.path.join(ours_bin, "img_lossless_dec")
    jxl_t = ["--num_threads", T] if T else []
    codecs = [
        Codec("ours", ".icl", lambda s, i, o: [enc, *i["raw"], s, o], lambda c, o: [dec, c, o],
              check="bytes", available=os.path.exists(enc)),
        Codec("JPEG-LS (CharLS)", ".jls", lambda s, i, o: [jls, "enc", *i["raw"], s, o],
              lambda c, o: [jls, "dec", c, o], available=os.path.exists(jls)),
        Codec("JPEG-XL e3", ".jxl", lambda s, i, o: ["cjxl", s, o, "-d", "0", "-e", "3", "--quiet", *jxl_t],
              lambda c, o: ["djxl", c, o, "--quiet", *jxl_t], needs_pnm=True,
              available=bool(shutil.which("cjxl"))),
        Codec("JPEG-XL e7", ".jxl", lambda s, i, o: ["cjxl", s, o, "-d", "0", "-e", "7", "--quiet", *jxl_t],
              lambda c, o: ["djxl", c, o, "--quiet", *jxl_t], needs_pnm=True,
              available=bool(shutil.which("cjxl"))),
        Codec("PNG (ImageMagick)", ".png",
              lambda s, i, o: ["convert", s, "-define", "png:compression-level=9", o],
              lambda c, o: ["convert", c, o], needs_pnm=True, available=bool(shutil.which("convert"))),
        Codec("xz -9", ".xz", lambda s, i, o: ["sh", "-c", f"xz -9 -c {T and '-T' + T or ''} '{s}' > '{o}'"],
              lambda c, o: ["sh", "-c", f"xz -dc '{c}' > '{o}'"], check="bytes",
              available=bool(shutil.which("xz"))),
        Codec("zstd -19", ".zst", lambda s, i, o: ["zstd", "-19", "-q", "-f", *(["-T" + T] if T else []), s, "-o", o],
              lambda c, o: ["zstd", "-d", "-q", "-f", c, "-o", o], check="bytes",
              available=bool(shutil.which("zstd"))),
    ]
    return codecs


def same_pixels(orig, info, out):
    a = info["pixels"]
    b = read_image(out).data
    return a.shape == b.shape and np.array_equal(a.astype(np.uint16), b.astype(np.uint16))


# ------------------------------------------------------------------ main
def main():
    ap = argparse.ArgumentParser(description="Compare lossless image codecs.")
    ap.add_argument("folder", help="ImageFiles folder")
    ap.add_argument("--groups", nargs="+", default=list(GROUPS))
    ap.add_argument("--codecs", nargs="+", help="names (prefixes) of codecs to run")
    ap.add_argument("--threads", type=int, default=0, help="threads for codecs that use them (0 = default)")
    ap.add_argument("--repeat", type=int, default=3)
    ap.add_argument("--bin", default=os.path.join(HERE, ".."), help="folder with our binaries")
    ap.add_argument("-o", "--outdir", default=os.path.join(HERE, "results"))
    args = ap.parse_args()

    env = dict(os.environ)
    if args.threads:
        env["OMP_NUM_THREADS"] = str(args.threads)
    codecs = [c for c in make_codecs(args.threads, args.bin) if c.available and
              (not args.codecs or any(c.name.startswith(x) for x in args.codecs))]
    missing = [c.name for c in make_codecs(args.threads, args.bin) if not c.available]
    if missing:
        print("not found, skipped:", ", ".join(missing))
    os.makedirs(args.outdir, exist_ok=True)
    rows = []
    with tempfile.TemporaryDirectory() as tmp:
        for g in args.groups:
            d = os.path.join(args.folder, GROUPS[g])
            files = sorted(os.path.join(d, f) for f in os.listdir(d) if os.path.isfile(os.path.join(d, f)))
            for f in files:
                src = f
                if f.endswith(".gz"):
                    src = os.path.join(tmp, os.path.basename(f)[:-3])
                    with gzip.open(f) as gz, open(src, "wb") as o:
                        o.write(gz.read())
                raw = raw_args(src) if src.endswith(".raw") else []
                img = read_image(src)                       # reads RAW sizes from the name
                info = {"raw": raw, "pixels": img.data, "npx": img.rows * img.cols,
                        "ext": ".ppm" if img.channels == 3 else ".pgm"}
                pnm = src
                if raw:                                     # PGM copy for tools that need PNM
                    pnm = os.path.join(tmp, "as16.pgm")
                    with open(pnm, "wb") as o:
                        o.write(f"P5\n{img.cols} {img.rows}\n65535\n".encode())
                        o.write(img.data.astype(">u2").tobytes())
                size = os.path.getsize(src)
                for c in codecs:
                    s = pnm if c.needs_pnm else src
                    comp = os.path.join(tmp, "c" + c.ext)
                    out = os.path.join(tmp, "out" + (info["ext"] if c.check == "pixels" else ""))
                    te, me = best(c.enc(s, info, comp), args.repeat, env)
                    td, md = best(c.dec(comp, out), args.repeat, env)
                    if c.check == "bytes":
                        ok = open(src, "rb").read() == open(out, "rb").read()
                    else:
                        ok = same_pixels(src, info, out)
                    if not ok:
                        sys.exit(f"NOT LOSSLESS: {c.name} on {f}")
                    rows.append(dict(group=g, file=os.path.basename(f), codec=c.name, pixels=info["npx"],
                                     orig_bytes=size, comp_bytes=os.path.getsize(comp),
                                     enc_s=round(te, 4), dec_s=round(td, 4),
                                     enc_mb=round(me, 1), dec_mb=round(md, 1)))
                print(f"{g:6s} {os.path.basename(f)}", flush=True)

    with open(os.path.join(args.outdir, "compare.csv"), "w", newline="") as fh:
        w = csv.DictWriter(fh, fieldnames=list(rows[0]))
        w.writeheader()
        w.writerows(rows)

    names = [c.name for c in codecs]
    lines = [f"# Lossless codec comparison ({'threads: ' + str(args.threads) if args.threads else 'default threads'})",
             "", "bpp = total compressed bits / total pixels (lower is better); speed = total input bytes /",
             "total time; memory = peak resident memory of one process (largest file of the group).", ""]
    for g in args.groups:
        R = [r for r in rows if r["group"] == g]
        if not R:
            continue
        lines += [f"## {GROUP_NAMES[g]} ({len({r['file'] for r in R})} images)", "",
                  "| codec | bpp | ratio | enc MB/s | dec MB/s | enc peak MB | dec peak MB |",
                  "|---|---:|---:|---:|---:|---:|---:|"]
        for n in names:
            S = [r for r in R if r["codec"] == n]
            px = sum(r["pixels"] for r in S)
            bits = sum(r["comp_bytes"] * 8 for r in S)
            ob = sum(r["orig_bytes"] for r in S)
            lines.append(f"| {n} | {bits / px:.3f} | {ob * 8 / bits:.3f} | "
                         f"{ob / sum(r['enc_s'] for r in S) / 1e6:.1f} | {ob / sum(r['dec_s'] for r in S) / 1e6:.1f} | "
                         f"{max(r['enc_mb'] for r in S):.0f} | {max(r['dec_mb'] for r in S):.0f} |")
        lines.append("")
    lines += ["## All images", "", "| codec | ratio (original bytes / compressed bytes) | enc MB/s | dec MB/s |",
              "|---|---:|---:|---:|"]
    for n in names:
        S = [r for r in rows if r["codec"] == n]
        ob = sum(r["orig_bytes"] for r in S)
        lines.append(f"| {n} | {ob / sum(r['comp_bytes'] for r in S):.3f} | "
                     f"{ob / sum(r['enc_s'] for r in S) / 1e6:.1f} | {ob / sum(r['dec_s'] for r in S) / 1e6:.1f} |")
    md = "\n".join(lines) + "\n"
    open(os.path.join(args.outdir, "compare.md"), "w").write(md)
    print(md)


if __name__ == "__main__":
    main()
