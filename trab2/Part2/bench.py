#!/usr/bin/env python3
"""
bench.py - bits per pixel, time and peak memory of our codec, per image group.

  python3 bench.py ../../ImageFiles                 # default tile size
  python3 bench.py ../../ImageFiles -t 64 256 1024  # compare tile sizes
  python3 bench.py ../../ImageFiles --groups pgm8 raw16 --threads 1

bpp = compressed file size (whole ICL2 file) x 8 / pixels; group values are
total bits / total pixels. Times are wall-clock, best of --repeat runs.
Every run is checked: the decoded file must be identical to the original.
"""
import argparse
import gzip
import os
import re
import resource
import subprocess
import sys
import tempfile
import time

GROUPS = {"pgm8": "imgs-pgm8", "raw16": "imgs-raw16", "ppm": "imgs-ppm", "kodak": "imgs-ppm/kodak"}


def raw_args(path):
    m = re.search(r"u(8|16)(be|le)?-1x(\d+)x(\d+)", os.path.basename(path))
    a = ["-rows", m[3], "-cols", m[4]]
    if m[1] == "16":
        a += ["-bpp", "16", f"-{m[2] or 'be'}"]
    return a, int(m[3]) * int(m[4])


def pnm_pixels(path):
    with open(path, "rb") as f:
        head = f.read(4096)
    tok = re.sub(rb"#[^\n]*", b" ", head).split()
    return int(tok[1]) * int(tok[2])


def timed(cmd, env):
    """Run cmd; return (seconds, peak RSS in MB of this child)."""
    before = resource.getrusage(resource.RUSAGE_CHILDREN)
    t0 = time.perf_counter()
    p = subprocess.run(cmd, capture_output=True, env=env)
    dt = time.perf_counter() - t0
    if p.returncode:
        sys.exit(f"failed: {' '.join(cmd)}\n{p.stderr.decode()}")
    after = resource.getrusage(resource.RUSAGE_CHILDREN)
    return dt, after.ru_maxrss / 1024 if after.ru_maxrss > before.ru_maxrss else None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("folder")
    ap.add_argument("-t", "--tiles", nargs="+", type=int, default=[512])
    ap.add_argument("--groups", nargs="+", default=list(GROUPS))
    ap.add_argument("--threads", type=int, default=0, help="OMP_NUM_THREADS (0 = all cores)")
    ap.add_argument("--repeat", type=int, default=1)
    ap.add_argument("--bin", default=".")
    ap.add_argument("--exclude", nargs="*", default=[], help="skip files whose name contains these")
    ap.add_argument("--enc-args", default="", help="extra encoder options, e.g. '-c best'")
    ap.add_argument("--label", default="", help="text for the first column")
    args = ap.parse_args()
    env = dict(os.environ)
    if args.threads:
        env["OMP_NUM_THREADS"] = str(args.threads)
    enc, dec = (os.path.join(args.bin, f"img_lossless_{k}") for k in ("enc", "dec"))

    with tempfile.TemporaryDirectory() as tmp:
        print("| group | tile | images | bpp | enc s | dec s | enc MB/s | dec MB/s |")
        print("|---|---:|---:|---:|---:|---:|---:|---:|")
        for g in args.groups:
            d = os.path.join(args.folder, GROUPS[g])
            files = sorted(os.path.join(d, f) for f in os.listdir(d)
                           if os.path.isfile(os.path.join(d, f)) and not any(x in f for x in args.exclude))
            for tile in args.tiles:
                bits = px = size = 0
                te = td = 0.0
                for f in files:
                    src = f
                    if f.endswith(".gz"):
                        src = os.path.join(tmp, os.path.basename(f)[:-3])
                        with gzip.open(f) as gz, open(src, "wb") as o:
                            o.write(gz.read())
                    if src.endswith(".raw"):
                        extra, n = raw_args(src)
                    else:
                        extra, n = [], pnm_pixels(src)
                    comp, out = os.path.join(tmp, "c.icl"), os.path.join(tmp, "out")
                    best_e = best_d = 1e9
                    for _ in range(args.repeat):
                        best_e = min(best_e, timed([enc, "-t", str(tile), *args.enc_args.split(), *extra, src, comp], env)[0])
                        best_d = min(best_d, timed([dec, comp, out], env)[0])
                    if open(src, "rb").read() != open(out, "rb").read():
                        sys.exit(f"NOT LOSSLESS: {f}")
                    bits += os.path.getsize(comp) * 8
                    px += n
                    size += os.path.getsize(src)
                    te += best_e
                    td += best_d
                print(f"| {g}{' ' + args.label if args.label else ''} | {tile} | {len(files)} | {bits / px:.3f} | {te:.2f} | {td:.2f} | "
                      f"{size / te / 1e6:.1f} | {size / td / 1e6:.1f} |", flush=True)


if __name__ == "__main__":
    main()
