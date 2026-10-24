#!/usr/bin/env python3
"""
snr_tool.py - SNR measurement and rate-distortion tables for the lossy codec
benchmark. Standard library only.

  snr_tool.py snr ORIGINAL.wav DECODED.wav [--max-lag N]
      Prints "<snr_db> <lag>". SNR = 10 log10(sum x^2 / sum (x - y)^2) over
      all channels and samples, as integers (8-bit centred on 0).
      MP3 and AAC decoders can add a delay at the start: with --max-lag N
      (default 0) the delay 0..N frames is found first, and that many frames
      are dropped from the start of the decoded file. The candidates are
      lag 0 and the 5 highest peaks of the cross-correlation (FFT); the one
      with the best SNR wins. Checking the SNR matters for periodic signals,
      whose correlation also peaks at multiples of the period. Missing frames at the end count as
      zeros, extra frames are ignored.

  snr_tool.py diff ORIGINAL.wav DECODED.wav OUT.wav [--gain DB] [--max-lag N]
      Writes original - decoded (what the codec removed or added) as a WAV
      in the original's format, amplified by --gain dB (default 0) and
      clipped to the sample range. Prints the RMS level of the difference.

  snr_tool.py info FILE.wav
      Prints "<frames> <rate> <channels> <bits>".

  snr_tool.py table RESULTS.csv [--at-snr 20,30,40] [--at-kbps 96,128,192]
      For every curve (codec + config in the CSV), interpolates the bitrate
      at each SNR (and the SNR at each bitrate) on every file, then averages
      over the files that cover that point (their count is shown when it is
      not all of them). With a "ratio" column in the CSV, the compression
      ratio at each SNR is printed too: total original size / total
      compressed size (a plain mean of ratios would be dominated by
      near-silent files).
      Prints a Markdown table. Interpolation is linear in
      SNR vs log(kbps). Up to 0.5 dB (or 3% in kbps) outside a file's
      measured range the end segment is extended (our encoder lands just
      above each target, e.g. 20.02 dB for 20); further out the file does
      not count for that point.

  snr_tool.py plot RESULTS.csv OUT.svg [--title T]
      SVG plot of the rate-distortion curve of every config: one point per
      encoder setting (SNR target for ours, bitrate for MP3/AAC) at the mean
      kbps and mean SNR over the files; kbps on the x axis, SNR on the y axis.

CSV columns (written by bench_lossy.sh):
  file,codec,config,param,bytes,ratio,kbps,snr_db,lag,enc_s,enc_kb,dec_s,dec_kb
"""
import argparse
import array
import cmath
import csv
import math
import struct
import sys
from collections import defaultdict


# ---------------------------------------------------------------- WAV

def read_wav(path):
    """Returns (channels, bits, rate, interleaved samples as array of ints)."""
    with open(path, "rb") as f:
        data = f.read()
    if data[:4] != b"RIFF" or data[8:12] != b"WAVE":
        raise ValueError("%s: not a WAV file" % path)
    pos, fmt = 12, None
    while pos + 8 <= len(data):
        cid = data[pos:pos + 4]
        size = struct.unpack_from("<I", data, pos + 4)[0]
        body = pos + 8
        if cid == b"fmt ":
            tag, nc, rate = struct.unpack_from("<HHI", data, body)
            bits = struct.unpack_from("<H", data, body + 14)[0]
            if tag not in (1, 0xFFFE) or bits not in (8, 16, 24, 32):
                raise ValueError("%s: unsupported format (tag %d, %d bits)" % (path, tag, bits))
            fmt = (nc, bits, rate)
        elif cid == b"data":
            if fmt is None:
                raise ValueError("%s: data before fmt" % path)
            nc, bits, rate = fmt
            raw = data[body:body + min(size, len(data) - body)]
            raw = raw[:len(raw) - len(raw) % (nc * bits // 8)]
            return nc, bits, rate, decode_pcm(raw, bits)
        pos = body + size + (size & 1)
    raise ValueError("%s: no data chunk" % path)


def decode_pcm(raw, bits):
    if bits == 8:
        return array.array("h", (b - 128 for b in raw))
    if bits == 16:
        a = array.array("h")
        a.frombytes(raw)
    elif bits == 32:
        a = array.array("i")
        a.frombytes(raw)
    else:  # 24: widen to 32 bits, then shift the sign into place
        wide = bytearray(len(raw) // 3 * 4)
        wide[1::4], wide[2::4], wide[3::4] = raw[0::3], raw[1::3], raw[2::3]
        a = array.array("i")
        a.frombytes(bytes(wide))
        a = array.array("i", (v >> 8 for v in a))
    if sys.byteorder == "big":
        a.byteswap()
    return a


# ---------------------------------------------------------------- lag search

def fft(a, invert=False):
    """Iterative radix-2 FFT of a list of complex numbers (len = power of 2)."""
    n = len(a)
    a = list(a)
    j = 0
    for i in range(1, n):
        bit = n >> 1
        while j & bit:
            j ^= bit
            bit >>= 1
        j |= bit
        if i < j:
            a[i], a[j] = a[j], a[i]
    length = 2
    while length <= n:
        w = cmath.exp((2j if invert else -2j) * math.pi / length)
        half = length // 2
        ws = [1]
        for _ in range(half - 1):
            ws.append(ws[-1] * w)
        for i in range(0, n, length):
            for k in range(half):
                u = a[i + k]
                v = a[i + k + half] * ws[k]
                a[i + k] = u + v
                a[i + k + half] = u - v
        length <<= 1
    if invert:
        a = [v / n for v in a]
    return a


def lag_candidates(x, y, nc, max_lag, count=5):
    """Delays (in frames, 0..max_lag) of y relative to x worth checking:
    0 and the `count` best peaks of the cross-correlation of the channel sums
    over a 32768-frame window from the middle of the file."""
    if max_lag <= 0:
        return [0]
    win = 32768
    fx, fy = len(x) // nc, len(y) // nc
    if fx < 2 * max_lag + 16 or fy < 2 * max_lag + 16:
        win = max(16, min(fx, fy) - max_lag)
    start = max(0, min(fx // 2 - win // 2, fx - win, fy - win - max_lag))
    mono_x = [sum(x[(start + i) * nc + c] for c in range(nc)) for i in range(min(win, fx - start))]
    seg_len = min(win + max_lag, fy - start)
    mono_y = [sum(y[(start + i) * nc + c] for c in range(nc)) for i in range(seg_len)]
    n = 1
    while n < len(mono_x) + len(mono_y):
        n <<= 1
    X = fft(mono_x + [0] * (n - len(mono_x)))
    Y = fft(mono_y + [0] * (n - len(mono_y)))
    corr = fft([b * a.conjugate() for a, b in zip(X, Y)], invert=True)
    # corr[lag] = sum_i x[i] y[i + lag]
    lags = range(min(max_lag, len(mono_y) - 1) + 1)
    peaks = sorted(lags, key=lambda l: corr[l].real, reverse=True)[:count]
    return sorted(set([0] + peaks))


def best_lag(x, y, nc, max_lag):
    """(snr, lag) for the candidate lag with the best SNR."""
    return max((snr_db(x, y, nc, lag), lag) for lag in lag_candidates(x, y, nc, max_lag))


# ---------------------------------------------------------------- SNR

def snr_db(x, y, nc, lag=0):
    if lag:
        y = y[lag * nc:]
    n = len(x)
    if len(y) < n:                       # missing samples count as zeros
        y = y + array.array(y.typecode, bytes(y.itemsize * (n - len(y))))
    elif len(y) > n:
        y = y[:n]
    es = math.sumprod(x, x)
    d = array.array("q", map(int.__sub__, x, y))
    en = math.sumprod(d, d)
    if en == 0:
        return math.inf
    if es == 0:
        return -math.inf
    return 10 * math.log10(es / en)


def cmd_info(a):
    nc, bits, rate, x = read_wav(a.file)
    print(len(x) // nc, rate, nc, bits)


def write_wav(path, nc, bits, rate, samples):
    """samples: interleaved ints, already in range (8-bit centred on 0)."""
    import wave
    if bits == 8:
        raw = bytes(v + 128 for v in samples)
    elif bits == 16:
        a = array.array("h", samples)
        if sys.byteorder == "big":
            a.byteswap()
        raw = a.tobytes()
    else:
        a = array.array("i", samples)
        if sys.byteorder == "big":
            a.byteswap()
        b = a.tobytes()
        raw = b"".join(b[i:i + bits // 8] for i in range(0, len(b), 4)) if bits == 24 else b
    with wave.open(path, "wb") as w:
        w.setnchannels(nc)
        w.setsampwidth(bits // 8)
        w.setframerate(rate)
        w.writeframes(raw)


def cmd_diff(a):
    nc, bits, rate, x = read_wav(a.original)
    nc2, bits2, rate2, y = read_wav(a.decoded)
    if (nc, bits, rate) != (nc2, bits2, rate2):
        sys.exit("format mismatch: %s vs %s" % ((nc, bits, rate), (nc2, bits2, rate2)))
    _, lag = best_lag(x, y, nc, a.max_lag)
    y = y[lag * nc:lag * nc + len(x)]
    if len(y) < len(x):
        y = y + array.array(y.typecode, bytes(y.itemsize * (len(x) - len(y))))
    gain = 10 ** (a.gain / 20)
    lo, hi = -(1 << (bits - 1)), (1 << (bits - 1)) - 1
    d = array.array("q", map(int.__sub__, x, y))
    rms = math.sqrt(math.sumprod(d, d) / len(d)) if len(d) else 0.0
    out = [min(hi, max(lo, round(v * gain))) for v in d]
    clipped = sum(1 for v, o in zip(d, out) if round(v * gain) != o)
    write_wav(a.out, nc, bits, rate, out)
    full = 20 * math.log10(rms / (1 << (bits - 1))) if rms > 0 else -math.inf
    print("difference RMS %.1f (%.1f dBFS), gain %+.1f dB, %d samples clipped, lag %d"
          % (rms, full, a.gain, clipped, lag))


def cmd_snr(a):
    nc, bits, rate, x = read_wav(a.original)
    nc2, bits2, rate2, y = read_wav(a.decoded)
    if (nc, rate) != (nc2, rate2):
        sys.exit("format mismatch: %s vs %s" % ((nc, bits, rate), (nc2, bits2, rate2)))
    if bits != bits2:            # compare at the original's scale
        shift = bits - bits2
        y = array.array("q", (v << shift if shift > 0 else v >> -shift for v in y))
    s, lag = best_lag(x, y, nc, a.max_lag)
    print("%.4f %d" % (s, lag))


# ---------------------------------------------------------------- tables

def load_curves(path, ratio_k=None):
    """{(codec, config): {file: [(snr, kbps), ...] sorted by snr}}
    If ratio_k is a dict, it gets {file: ratio * kbps} (a constant per file:
    both are proportional to the compressed size), for CSVs with a ratio."""
    curves = defaultdict(lambda: defaultdict(list))
    with open(path, newline="") as f:
        for r in csv.DictReader(f):
            try:
                s, k = float(r["snr_db"]), float(r["kbps"])
            except (ValueError, KeyError):
                continue
            if math.isfinite(s) and k > 0:
                curves[(r["codec"], r["config"])][r["file"]].append((s, k))
                if ratio_k is not None and r.get("ratio"):
                    ratio_k[r["file"]] = float(r["ratio"]) * k
    for c in curves.values():
        for pts in c.values():
            pts.sort()
    return curves


def interp(pts, x, xi, yi, logx=False, logy=False):
    """Linear interpolation through pts (tuples) from coordinate xi to yi."""
    f = (lambda v: math.log(v)) if logx else (lambda v: v)
    g = (lambda v: math.log(v)) if logy else (lambda v: v)
    gi = (lambda v: math.exp(v)) if logy else (lambda v: v)
    pts = sorted(pts, key=lambda p: p[xi])
    for p, q in zip(pts, pts[1:]):
        if p[xi] <= x <= q[xi] and q[xi] > p[xi]:
            t = (f(x) - f(p[xi])) / (f(q[xi]) - f(p[xi]))
            return gi(g(p[yi]) + t * (g(q[yi]) - g(p[yi])))
    # Just outside the range: extend the end segment a little.
    if len(pts) >= 2:
        lo, hi = pts[0][xi], pts[-1][xi]
        tol = (lambda v: v * 0.03) if logx else (lambda v: 0.5)
        if lo - tol(lo) <= x < lo:
            p, q = pts[0], pts[1]
        elif hi < x <= hi + tol(hi):
            p, q = pts[-2], pts[-1]
        else:
            return None
        if q[xi] == p[xi]:
            return None
        t = (f(x) - f(p[xi])) / (f(q[xi]) - f(p[xi]))
        return gi(g(p[yi]) + t * (g(q[yi]) - g(p[yi])))
    if pts and pts[0][xi] == x:
        return pts[0][yi]
    return None


def partial_average_at(files, x, xi, yi, logx, logy):
    """(mean over the files whose curve covers x, how many files that is)."""
    vals = [interp(pts, x, xi, yi, logx, logy) for pts in files.values()]
    vals = [v for v in vals if v is not None]
    return (sum(vals) / len(vals) if vals else None), len(vals)


def ratio_at(files, snr, ratio_k):
    """Overall compression ratio at an SNR: total original size / total
    compressed size over the files that cover it (the harmonic mean of the
    per-file ratios). A plain mean would be dominated by near-silent files."""
    inv = []
    for f, pts in files.items():
        k = interp(pts, snr, 0, 1, False, True)
        if k is not None and f in ratio_k:
            inv.append(k / ratio_k[f])
    return (len(inv) / sum(inv) if inv else None), len(inv)


def cell(v, n, total, fmt):
    if v is None:
        return "-"
    return fmt % v + ("" if n == total else " (%d)" % n)


def cmd_table(a):
    ratio_k = {}
    curves = load_curves(a.csv, ratio_k)
    at_snr = [float(v) for v in a.at_snr.split(",") if v]
    at_kbps = [float(v) for v in a.at_kbps.split(",") if v]
    at_ratio = at_snr if ratio_k else []
    head = ["codec", "config", "files"]
    head += ["kbps @ %g dB" % s for s in at_snr] + ["ratio @ %g dB" % s for s in at_ratio]
    head += ["dB @ %g kbps" % k for k in at_kbps]
    print("| " + " | ".join(head) + " |")
    print("|" + "---|" * len(head))
    for (codec, config), files in sorted(curves.items()):
        total = len(files)
        row = [codec, config, str(total)]
        for s in at_snr:
            row.append(cell(*partial_average_at(files, s, 0, 1, False, True), total, "%.1f"))
        for s in at_ratio:
            row.append(cell(*ratio_at(files, s, ratio_k), total, "%.1f"))
        for k in at_kbps:
            row.append(cell(*partial_average_at(files, k, 1, 0, True, False), total, "%.2f"))
        print("| " + " | ".join(row) + " |")
    print("\nAverages over the files; \"(n)\" = only n files cover that point, the others are "
          "outside their measured range; '-' = none does.")
    if at_ratio:
        print("ratio = total original WAV size / total compressed size over those files.")


# ---------------------------------------------------------------- plot

COLORS = ["#2a6fdb", "#d1495b", "#2e933c", "#edae49", "#7b4fb3", "#00798c",
          "#8c564b", "#e377c2", "#555555", "#bcbd22", "#17becf", "#ff7f0e"]


def setting_points(path):
    """{(codec, config): [(mean kbps, mean snr), ...]}: one point per encoder
    setting (SNR target for ours, bitrate for MP3/AAC), averaged over the
    files, using only settings that every file of that curve has."""
    acc = defaultdict(lambda: defaultdict(list))
    files = defaultdict(set)
    with open(path, newline="") as f:
        for r in csv.DictReader(f):
            try:
                s, k = float(r["snr_db"]), float(r["kbps"])
            except (ValueError, KeyError):
                continue
            if math.isfinite(s) and k > 0:
                acc[(r["codec"], r["config"])][r["param"]].append((k, s))
                files[(r["codec"], r["config"])].add(r["file"])
    out = {}
    for key, params in acc.items():
        pts = [(sum(p[0] for p in v) / len(v), sum(p[1] for p in v) / len(v))
               for v in params.values() if len(v) == len(files[key])]
        out[key] = sorted(pts)
    return out


def cmd_plot(a):
    series = [("%s %s" % key, pts) for key, pts in sorted(setting_points(a.csv).items())
              if len(pts) >= 2]
    if not series:
        sys.exit("nothing to plot")

    W, H, L, R, T, B = 820, 520, 70, 210, 40, 60
    kmin = min(p[0] for _, pts in series for p in pts)
    kmax = max(p[0] for _, pts in series for p in pts)
    smin = min(p[1] for _, pts in series for p in pts)
    smax = max(p[1] for _, pts in series for p in pts)
    kmin, kmax = 2 ** math.floor(math.log2(kmin)), 2 ** math.ceil(math.log2(kmax))
    smin, smax = 5 * math.floor(smin / 5), 5 * math.ceil(smax / 5)

    def px(k):
        return L + (W - L - R) * math.log(k / kmin) / math.log(kmax / kmin)

    def py(s):
        return H - B - (H - T - B) * (s - smin) / (smax - smin)

    o = ['<svg xmlns="http://www.w3.org/2000/svg" width="%d" height="%d" '
         'font-family="sans-serif" font-size="12">' % (W, H),
         '<rect width="100%" height="100%" fill="white"/>',
         '<text x="%d" y="22" font-size="15">%s</text>' % (L, a.title)]
    k = kmin
    while k <= kmax * 1.0001:
        o.append('<line x1="%.1f" y1="%d" x2="%.1f" y2="%d" stroke="#ddd"/>' % (px(k), T, px(k), H - B))
        o.append('<text x="%.1f" y="%d" text-anchor="middle">%g</text>' % (px(k), H - B + 16, k))
        k *= 2
    for s in range(int(smin), int(smax) + 1, 5):
        o.append('<line x1="%d" y1="%.1f" x2="%d" y2="%.1f" stroke="#ddd"/>' % (L, py(s), W - R, py(s)))
        o.append('<text x="%d" y="%.1f" text-anchor="end">%d</text>' % (L - 6, py(s) + 4, s))
    o.append('<text x="%d" y="%d" text-anchor="middle">bitrate (kbps, log scale)</text>'
             % ((L + W - R) // 2, H - 18))
    o.append('<text transform="translate(18,%d) rotate(-90)" text-anchor="middle">SNR (dB)</text>'
             % ((T + H - B) // 2))
    for i, (name, pts) in enumerate(series):
        col = COLORS[i % len(COLORS)]
        path = " ".join("%.1f,%.1f" % (px(k), py(s)) for k, s in pts)
        o.append('<polyline fill="none" stroke="%s" stroke-width="2" points="%s"/>' % (col, path))
        y = T + 10 + 18 * i
        o.append('<line x1="%d" y1="%d" x2="%d" y2="%d" stroke="%s" stroke-width="3"/>'
                 % (W - R + 15, y, W - R + 35, y, col))
        o.append('<text x="%d" y="%d">%s</text>' % (W - R + 40, y + 4, name))
    o.append("</svg>")
    with open(a.out, "w") as f:
        f.write("\n".join(o) + "\n")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("snr")
    p.add_argument("original")
    p.add_argument("decoded")
    p.add_argument("--max-lag", type=int, default=0)
    p = sub.add_parser("diff")
    p.add_argument("original")
    p.add_argument("decoded")
    p.add_argument("out")
    p.add_argument("--gain", type=float, default=0.0)
    p.add_argument("--max-lag", type=int, default=0)
    p = sub.add_parser("info")
    p.add_argument("file")
    p = sub.add_parser("table")
    p.add_argument("csv")
    p.add_argument("--at-snr", default="20,30,40")
    p.add_argument("--at-kbps", default="")
    p = sub.add_parser("plot")
    p.add_argument("csv")
    p.add_argument("out")
    p.add_argument("--title", default="Rate-distortion")
    a = ap.parse_args()
    {"snr": cmd_snr, "diff": cmd_diff, "info": cmd_info, "table": cmd_table, "plot": cmd_plot}[a.cmd](a)


if __name__ == "__main__":
    main()
