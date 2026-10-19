#!/usr/bin/env python3
"""
benchmark.py - compare our lossless audio codec with other lossless compressors.

Competitors: flac (audio specific) and the general purpose zstd, xz (lzma),
bzip2, gzip, brotli and lz4, each at its default and at its maximum level.

For every input WAV and every codec, measures:
    * compressed size and compression ratio (original / compressed)
    * bits per sample (compressed bits / total samples over all channels)
    * compression and decompression wall time (median of --runs runs)
    * peak memory (max resident set size) of the compressor and decompressor
    * whether the decoded file is byte-identical to the original

All tools run single-threaded so the comparison with our (single-threaded)
codec is fair. Each command runs through ./memrun (built by `make`), which
reports its wall time and peak RSS; see memrun.c for why Python can't
measure the memory itself. Linux only (ru_maxrss is in KiB there).

Usage:
    python3 benchmark.py [options] input.wav|directory [...]

    --runs N       timed repetitions per measurement (default 3)
    --block N      block size passed to wav_lossless_enc (default: codec default)
    --only a,b     run only the codecs whose name starts with one of these
    --out DIR      where to write results.csv and summary.md (default: bench_out)
"""

import argparse
import csv
import filecmp
import os
import shutil
import statistics
import subprocess
import sys
import tempfile
import wave

HERE = os.path.dirname(os.path.abspath(__file__))


def codec_table(enc, dec, block):
    """
    One entry per codec: (name, compress argv, decompress argv).
    In the argv templates, {in}, {out} are file paths. A template ending in
    the marker ">" sends stdout to {out} (for tools without an -o flag).
    """
    ours_enc = [enc] + (["-b", str(block)] if block else []) + ["{in}", "{out}"]
    t = [("ours", ours_enc, [dec, "{in}", "{out}"])]

    # flac keeps the non-audio WAV chunks so the output can be byte-identical.
    for lvl in ("5", "8"):
        t.append((f"flac -{lvl}",
                  ["flac", f"-{lvl}", "--keep-foreign-metadata", "-s", "-f", "-o", "{out}", "{in}"],
                  ["flac", "-d", "--keep-foreign-metadata", "-s", "-f", "-o", "{out}", "{in}"]))

    for lvl in ("3", "19"):
        t.append((f"zstd -{lvl}",
                  ["zstd", f"-{lvl}", "-T1", "-q", "-f", "-o", "{out}", "{in}"],
                  ["zstd", "-d", "-q", "-f", "-o", "{out}", "{in}"]))

    for lvl in ("6", "9e"):
        t.append((f"xz -{lvl}",
                  ["xz", f"-{lvl}", "-T1", "-c", "{in}", ">"],
                  ["xz", "-d", "-T1", "-c", "{in}", ">"]))

    t.append(("bzip2 -9", ["bzip2", "-9", "-c", "{in}", ">"],
              ["bzip2", "-d", "-c", "{in}", ">"]))

    for lvl in ("6", "9"):
        t.append((f"gzip -{lvl}", ["gzip", f"-{lvl}", "-c", "{in}", ">"],
                  ["gzip", "-d", "-c", "{in}", ">"]))

    for lvl in ("6", "11"):
        t.append((f"brotli -q{lvl}",
                  ["brotli", "-q", lvl, "-f", "-o", "{out}", "{in}"],
                  ["brotli", "-d", "-f", "-o", "{out}", "{in}"]))

    for lvl in ("1", "9"):
        t.append((f"lz4 -{lvl}",
                  ["lz4", f"-{lvl}", "-T1", "-q", "-f", "{in}", "{out}"],
                  ["lz4", "-d", "-q", "-f", "{in}", "{out}"]))
    return t


def run(template, src, dst, memrun):
    """
    Run one command through memrun. Returns (wall seconds, peak RSS in KiB).
    Raises RuntimeError if the command fails.
    """
    to_stdout = template[-1] == ">"
    argv = [a.replace("{in}", src).replace("{out}", dst)
            for a in (template[:-1] if to_stdout else template)]
    stats = dst + ".stats"
    out = open(dst, "wb") if to_stdout else subprocess.DEVNULL
    try:
        p = subprocess.run([memrun, stats] + argv, stdout=out, stderr=subprocess.PIPE)
    finally:
        if to_stdout:
            out.close()
    if p.returncode != 0:
        err = p.stderr.decode(errors="replace").strip()
        raise RuntimeError(f"{' '.join(argv)} exited with {p.returncode}: {err}")
    with open(stats) as f:
        wall, rss = f.read().split()
    return float(wall), int(rss)


def measure(template, src, dst, runs, memrun):
    """Median wall time and max peak memory over `runs` executions."""
    times, mems = [], []
    for _ in range(runs):
        t, m = run(template, src, dst, memrun)
        times.append(t)
        mems.append(m)
    return statistics.median(times), max(mems)


def total_samples(path):
    """frames * channels, or None if Python's wave module can't read the file."""
    try:
        with wave.open(path, "rb") as w:
            return w.getnframes() * w.getnchannels()
    except (wave.Error, EOFError):
        return None


def collect_inputs(paths):
    files = []
    for p in paths:
        if os.path.isdir(p):
            for root, _, names in os.walk(p):
                files += [os.path.join(root, n) for n in names if n.lower().endswith(".wav")]
        elif os.path.isfile(p):
            files.append(p)
        else:
            sys.exit(f"error: {p} not found")
    if not files:
        sys.exit(f"error: no .wav files found in {' '.join(paths)}")
    return sorted(files)


def summarise(rows, codecs, path):
    """Aggregate over all files and write a Markdown table, best ratio first."""
    lines = ["| Codec | Ratio | Bits/sample | Enc MB/s | Dec MB/s | "
             "Enc time (s) | Dec time (s) | Enc peak MiB | Dec peak MiB | Lossless |",
             "|---|---:|---:|---:|---:|---:|---:|---:|---:|:---:|"]
    agg = []
    for name in codecs:
        r = [x for x in rows if x["codec"] == name and x["comp_bytes"] != ""]
        if not r:
            continue
        orig = sum(x["orig_bytes"] for x in r)
        comp = sum(x["comp_bytes"] for x in r)
        enc_t = sum(x["enc_s"] for x in r)
        dec_t = sum(x["dec_s"] for x in r)
        nsamp = [x["samples"] for x in r]
        bps = comp * 8 / sum(nsamp) if all(nsamp) else None
        agg.append((orig / comp, name, bps, orig / 1e6 / max(enc_t, 1e-9), orig / 1e6 / max(dec_t, 1e-9),
                    enc_t, dec_t,
                    max(x["enc_peak_kib"] for x in r) / 1024,
                    max(x["dec_peak_kib"] for x in r) / 1024,
                    all(x["lossless"] for x in r)))
    for ratio, name, bps, emb, dmb, et, dt, em, dm, ok in sorted(agg, reverse=True):
        lines.append(f"| {name} | {ratio:.3f} | {'-' if bps is None else f'{bps:.2f}'} | "
                     f"{emb:.1f} | {dmb:.1f} | {et:.3f} | {dt:.3f} | {em:.1f} | {dm:.1f} | "
                     f"{'yes' if ok else '**NO**'} |")
    nfiles = len({x["file"] for x in rows})
    text = (f"Totals over {nfiles} file(s). Ratio = total original / total compressed bytes. "
            f"Times are summed per-file medians; peak memory is the max over files.\n\n"
            + "\n".join(lines) + "\n")
    with open(path, "w") as f:
        f.write(text)
    return text


def main():
    ap = argparse.ArgumentParser(description="Benchmark lossless compressors on WAV files.")
    ap.add_argument("inputs", nargs="+", help="WAV files and/or directories")
    ap.add_argument("--runs", type=int, default=3)
    ap.add_argument("--block", type=int, default=0)
    ap.add_argument("--only", default="", help="comma-separated codec name prefixes")
    ap.add_argument("--out", default="bench_out")
    ap.add_argument("--enc", default=os.path.join(HERE, "wav_lossless_enc"))
    ap.add_argument("--dec", default=os.path.join(HERE, "wav_lossless_dec"))
    ap.add_argument("--memrun", default=os.path.join(HERE, "memrun"))
    args = ap.parse_args()

    for exe in (args.enc, args.dec, args.memrun):
        if not os.access(exe, os.X_OK):
            sys.exit(f"error: {exe} not found; run `make` in {HERE} first")

    files = collect_inputs(args.inputs)
    codecs = codec_table(args.enc, args.dec, args.block)
    if args.only:
        prefixes = tuple(p.strip() for p in args.only.split(","))
        codecs = [c for c in codecs if c[0].startswith(prefixes)]
    available = []
    for c in codecs:
        if os.path.isabs(c[1][0]) or shutil.which(c[1][0]):
            available.append(c)
        else:
            print(f"warning: {c[1][0]} not installed, skipping {c[0]}", file=sys.stderr)

    os.makedirs(args.out, exist_ok=True)
    fields = ["file", "codec", "orig_bytes", "comp_bytes", "ratio", "samples", "bits_per_sample",
              "enc_s", "dec_s", "enc_peak_kib", "dec_peak_kib", "lossless", "error"]
    rows = []
    with tempfile.TemporaryDirectory(prefix="bench_") as tmp:
        comp, rec = os.path.join(tmp, "comp"), os.path.join(tmp, "rec.wav")
        for path in files:
            orig = os.path.getsize(path)
            nsamp = total_samples(path)
            print(f"{path} ({orig / 1e6:.2f} MB)")
            for name, enc_cmd, dec_cmd in available:
                row = dict.fromkeys(fields, "")
                row.update(file=path, codec=name, orig_bytes=orig, samples=nsamp or "")
                try:
                    row["enc_s"], row["enc_peak_kib"] = measure(enc_cmd, path, comp, args.runs, args.memrun)
                    size = os.path.getsize(comp)
                    row["dec_s"], row["dec_peak_kib"] = measure(dec_cmd, comp, rec, args.runs, args.memrun)
                    row.update(comp_bytes=size, ratio=orig / size,
                               lossless=filecmp.cmp(path, rec, shallow=False))
                    if nsamp:
                        row["bits_per_sample"] = size * 8 / nsamp
                    print(f"  {name:<12} ratio {row['ratio']:6.3f}  enc {row['enc_s']:7.3f}s"
                          f"  dec {row['dec_s']:7.3f}s"
                          f"  mem {row['enc_peak_kib'] / 1024:6.1f}/{row['dec_peak_kib'] / 1024:6.1f} MiB"
                          f"{'' if row['lossless'] else '  NOT LOSSLESS'}")
                except RuntimeError as e:
                    row["error"] = str(e)
                    print(f"  {name:<12} FAILED: {e}", file=sys.stderr)
                rows.append(row)

    csv_path = os.path.join(args.out, "results.csv")
    with open(csv_path, "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=fields)
        w.writeheader()
        w.writerows(rows)
    md_path = os.path.join(args.out, "summary.md")
    print()
    print(summarise(rows, [c[0] for c in available], md_path))
    print(f"wrote {csv_path} and {md_path}")


if __name__ == "__main__":
    main()
