#!/usr/bin/env python3
"""
unify_results.py - merge the results.csv files written by benchmark.py (several runs,
several folders) and write ONE Markdown overview of the whole file set.

It produces everything the Part II evaluation of the report uses:
  1. overview of the files (per group: count, size, duration)
  2. totals per codec over all files (the main comparison table)
  3. compression ratio per codec and per group
  4. block-size sweep of our codec (total and per group)
  5. derived numbers quoted in the text (gaps, speed multiples, memory growth)
  6. integrity check (every round trip lossless, no failed runs, no duplicates)

Usage:
    python3 unify_results.py --dirs bench_20_main bench_20_rest bench_7_main bench_7_rest \
        --blocks bench_block_{N} bench_7_block_{N} --out lossless_overview.md

    --dirs     folders (each with a results.csv) with the comparison runs; rows are merged
    --blocks   folder name templates for the block-size sweep; {N} is the block size
    --sizes    block sizes of the sweep (default 256 512 1024 2048 4096 8192)
    --out      Markdown file to write (default lossless_overview.md)

Groups are decided from the file path: a folder named TECH3253 -> "SQAM", DS100 or
DSD100 -> "DSD100", anything else -> "course". All files are assumed to be 16-bit stereo
at 44.1 kHz (used only for the durations).
"""

import argparse
import csv
import os
import sys
from collections import defaultdict

RATE, CHANNELS = 44100, 2
GROUPS = ["course", "DSD100", "SQAM"]


def group_of(path):
    parts = path.replace("\\", "/").split("/")
    if "TECH3253" in parts:
        return "SQAM"
    if "DS100" in parts or "DSD100" in parts:
        return "DSD100"
    return "course"


def read_rows(folders):
    """All good rows of all results.csv in `folders`, plus a count of failed rows."""
    rows, failed, missing = [], 0, []
    for d in folders:
        p = os.path.join(d, "results.csv")
        if not os.path.isfile(p):
            missing.append(d)
            continue
        with open(p, newline="") as f:
            for r in csv.DictReader(f):
                if r["comp_bytes"] == "" or r["error"]:
                    failed += 1
                    continue
                rows.append({
                    "file": r["file"], "name": os.path.basename(r["file"]),
                    "group": group_of(r["file"]), "codec": r["codec"],
                    "orig": int(r["orig_bytes"]), "comp": int(r["comp_bytes"]),
                    "samples": int(r["samples"]) if r["samples"] else 0,
                    "enc_s": float(r["enc_s"]), "dec_s": float(r["dec_s"]),
                    "enc_kib": int(r["enc_peak_kib"]), "dec_kib": int(r["dec_peak_kib"]),
                    "ok": r["lossless"] == "True",
                })
    return rows, failed, missing


def dedupe(rows):
    """Keep the last row for each (file name, codec); return rows and the duplicates."""
    seen, dups = {}, []
    for r in rows:
        k = (r["name"], r["group"], r["codec"])
        if k in seen:
            dups.append(k)
        seen[k] = r
    return list(seen.values()), dups


def totals(rows):
    o = sum(r["orig"] for r in rows)
    c = sum(r["comp"] for r in rows)
    et = sum(r["enc_s"] for r in rows)
    dt = sum(r["dec_s"] for r in rows)
    ns = sum(r["samples"] for r in rows)
    return {
        "n": len(rows), "ratio": o / c,
        "bps": c * 8 / ns if ns and all(r["samples"] for r in rows) else None,
        "enc_mbs": o / 1e6 / max(et, 1e-9), "dec_mbs": o / 1e6 / max(dt, 1e-9),
        "enc_s": et, "dec_s": dt,
        "enc_mib": max(r["enc_kib"] for r in rows) / 1024,
        "dec_mib": max(r["dec_kib"] for r in rows) / 1024,
        "ok": all(r["ok"] for r in rows),
    }


def slope(xs, ys):
    """Least-squares line y = a + b*x; returns (b, a)."""
    n = len(xs)
    mx, my = sum(xs) / n, sum(ys) / n
    sxx = sum((x - mx) ** 2 for x in xs)
    if sxx == 0:
        return float("nan"), my
    b = sum((x - mx) * (y - my) for x, y in zip(xs, ys)) / sxx
    return b, my - b * mx


def fmt_dur(sec):
    return f"{int(sec // 60)}:{sec % 60:04.1f}"


def main():
    ap = argparse.ArgumentParser(description="Merge benchmark.py results into one Markdown file")
    ap.add_argument("--dirs", nargs="+", required=True)
    ap.add_argument("--blocks", nargs="*", default=[])
    ap.add_argument("--sizes", nargs="+", type=int, default=[256, 512, 1024, 2048, 4096, 8192])
    ap.add_argument("--out", default="lossless_overview.md")
    args = ap.parse_args()

    rows, failed, missing = read_rows(args.dirs)
    if missing:
        sys.exit("no results.csv in: " + ", ".join(missing))
    rows, dups = dedupe(rows)
    if not rows:
        sys.exit("no usable rows found")

    by_codec = defaultdict(list)
    for r in rows:
        by_codec[r["codec"]].append(r)
    files = {}
    for r in rows:
        files[(r["name"], r["group"])] = r        # one entry per file
    all_files = sorted(files.values(), key=lambda r: (GROUPS.index(r["group"]), r["name"]))

    out = []
    w = out.append

    # ---- 1. overview of the files ------------------------------------------
    w("# Lossless codec: results over all files\n")
    w("## 1. The files\n")
    w("| Group | Files | Total size (MB) | Duration (min:s) | Largest file (MB) |")
    w("|---|---:|---:|---:|---:|")
    for g in GROUPS + ["all"]:
        fs = [f for f in all_files if g == "all" or f["group"] == g]
        if not fs:
            continue
        mb = sum(f["orig"] for f in fs) / 1e6
        dur = sum(f["samples"] for f in fs) / CHANNELS / RATE
        big = max(f["orig"] for f in fs) / 1e6
        w(f"| {g} | {len(fs)} | {mb:.1f} | {fmt_dur(dur)} | {big:.1f} |")
    w("")

    # ---- 2. totals per codec -----------------------------------------------
    w("## 2. Totals per codec\n")
    n_files = len(all_files)
    w(f"Totals over {n_files} files. Ratio = total original / total compressed bytes. "
      "Times are sums of per-file medians, speed = total MB / total time, peak memory is the "
      "maximum over files.\n")
    w("| Codec | Files | Ratio | Bits/sample | Enc MB/s | Dec MB/s | Enc peak MiB | Dec peak MiB | Lossless |")
    w("|---|---:|---:|---:|---:|---:|---:|---:|:---:|")
    tot = {c: totals(rs) for c, rs in by_codec.items()}
    for c, t in sorted(tot.items(), key=lambda kv: -kv[1]["ratio"]):
        bps = "-" if t["bps"] is None else f"{t['bps']:.2f}"
        w(f"| {c} | {t['n']} | {t['ratio']:.3f} | {bps} | {t['enc_mbs']:.1f} | {t['dec_mbs']:.1f} | "
          f"{t['enc_mib']:.1f} | {t['dec_mib']:.1f} | {'yes' if t['ok'] else '**NO**'} |")
    w("")

    # ---- 3. ratio per group ------------------------------------------------
    w("## 3. Compression ratio per group\n")
    w("| Codec | " + " | ".join(GROUPS) + " | all |")
    w("|---|" + "---:|" * (len(GROUPS) + 1))
    for c, _ in sorted(tot.items(), key=lambda kv: -kv[1]["ratio"]):
        cells = []
        for g in GROUPS:
            rs = [r for r in by_codec[c] if r["group"] == g]
            cells.append(f"{totals(rs)['ratio']:.3f}" if rs else "-")
        w(f"| {c} | " + " | ".join(cells) + f" | {tot[c]['ratio']:.3f} |")
    w("")

    # ---- 4. block-size sweep -----------------------------------------------
    block_ratio = {}
    if args.blocks:
        w("## 4. Block size of our codec\n")
        w("| Block size | Ratio (all) | " + " | ".join(GROUPS) + " | Enc time (s) | Dec time (s) | Files |")
        w("|---:|---:|" + "---:|" * len(GROUPS) + "---:|---:|---:|")
        for n in args.sizes:
            dirs = [t.replace("{N}", str(n)) for t in args.blocks]
            brs, bfail, bmiss = read_rows([d for d in dirs if os.path.isdir(d)])
            brs = [r for r in brs if r["codec"] == "ours"]
            brs, _ = dedupe(brs)
            if not brs:
                w(f"| {n} | no data | | | | | | | |")
                continue
            t = totals(brs)
            block_ratio[n] = t["ratio"]
            per = []
            for g in GROUPS:
                gs = [r for r in brs if r["group"] == g]
                per.append(f"{totals(gs)['ratio']:.3f}" if gs else "-")
            w(f"| {n} | {t['ratio']:.3f} | " + " | ".join(per) +
              f" | {t['enc_s']:.3f} | {t['dec_s']:.3f} | {t['n']} |")
        w("")
        if "ours" in tot and block_ratio:
            match = [n for n, v in block_ratio.items() if abs(v - tot["ours"]["ratio"]) < 5e-4]
            if match:
                w(f"Check: `ours` in section 2 (ratio {tot['ours']['ratio']:.3f}) matches block size "
                  f"{', '.join(map(str, match))} of the sweep.\n")
            else:
                w(f"**WARNING:** `ours` in section 2 (ratio {tot['ours']['ratio']:.3f}) matches no row of "
                  "the sweep: the main run used a different block size or other files.\n")

    # ---- 5. derived numbers -----------------------------------------------
    w("## 5. Numbers quoted in the text\n")

    def have(*names):
        return all(n in tot for n in names)

    if have("ours", "flac -8"):
        o, f8 = tot["ours"], tot["flac -8"]
        w(f"- Our files vs `flac -8`: {100 * (f8['ratio'] / o['ratio'] - 1):.1f}% larger "
          f"(ratio {o['ratio']:.3f} vs {f8['ratio']:.3f}).")
        w(f"- Our encoder speed vs `flac -8`: {o['enc_mbs'] / f8['enc_mbs']:.1f}x; "
          f"decoder: {o['dec_mbs'] / f8['dec_mbs']:.2f}x.")
    if have("ours", "flac -5"):
        w(f"- Our encoder speed vs `flac -5`: {tot['ours']['enc_mbs'] / tot['flac -5']['enc_mbs']:.1f}x; "
          f"decoder: {tot['ours']['dec_mbs'] / tot['flac -5']['dec_mbs']:.2f}x.")
    if have("zstd -19", "zstd -3"):
        z19, z3 = tot["zstd -19"], tot["zstd -3"]
        w(f"- `zstd -19` vs `zstd -3`: {100 * (z19['ratio'] / z3['ratio'] - 1):.1f}% better ratio, "
          f"{z3['enc_mbs'] / z19['enc_mbs']:.0f}x slower to encode.")
    gp = {c: t for c, t in tot.items() if not c.startswith(("ours", "flac"))}
    if gp:
        best = max(gp, key=lambda c: gp[c]["ratio"])
        w(f"- Best general-purpose tool: `{best}`, ratio {gp[best]['ratio']:.3f}; "
          f"the audio codecs reach {tot['ours']['ratio']:.3f} (ours)"
          + (f" and {tot['flac -8']['ratio']:.3f} (`flac -8`)." if "flac -8" in tot else "."))
        w("- Peak memory of the general-purpose tools (encoder, MiB): "
          + ", ".join(f"`{c}` {t['enc_mib']:.0f}" for c, t in sorted(gp.items(), key=lambda kv: -kv[1]['enc_mib'])[:4]) + ".")
    w("")
    w("Peak memory against file size (least-squares line, peak = a + b * size, both in MiB):\n")
    w("| Codec | Enc b | Enc a | Dec b | Dec a | Largest file (MiB) | Enc peak there | Dec peak there |")
    w("|---|---:|---:|---:|---:|---:|---:|---:|")
    for c in ("ours", "flac -8", "xz -9e"):
        rs = by_codec.get(c)
        if not rs or len(rs) < 2:
            continue
        xs = [r["orig"] / 2 ** 20 for r in rs]
        be, ae = slope(xs, [r["enc_kib"] / 1024 for r in rs])
        bd, ad = slope(xs, [r["dec_kib"] / 1024 for r in rs])
        big = max(rs, key=lambda r: r["orig"])
        w(f"| {c} | {be:.2f} | {ae:.1f} | {bd:.2f} | {ad:.1f} | {big['orig'] / 2 ** 20:.1f} | "
          f"{big['enc_kib'] / 1024:.1f} | {big['dec_kib'] / 1024:.1f} |")
    w("")

    # ---- 6. integrity ------------------------------------------------------
    w("## 6. Integrity\n")
    bad = [(r["name"], r["codec"]) for r in rows if not r["ok"]]
    w(f"- Rows used: {len(rows)} ({len(by_codec)} codecs, {n_files} files).")
    w(f"- Round trips not byte-identical: {len(bad)}" + (f" -> {bad[:10]}" if bad else "."))
    w(f"- Failed runs skipped: {failed}.")
    w(f"- Duplicate (file, codec) rows, last one kept: {len(dups)}" + (f" -> {dups[:5]}" if dups else "."))
    incomplete = {c: n_files - len(rs) for c, rs in by_codec.items() if len(rs) != n_files}
    w("- Codecs missing some files: " + (", ".join(f"`{c}` ({m} missing)" for c, m in incomplete.items())
                                       if incomplete else "none."))
    w("")

    with open(args.out, "w") as f:
        f.write("\n".join(out) + "\n")
    print("\n".join(out))
    print(f"wrote {args.out}")


if __name__ == "__main__":
    main()
