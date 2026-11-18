"""See README.md in this folder. Usage: python3 adaptive.py <ImageFiles folder> [results/stage0.csv]"""
import os, sys, glob, numpy as np, csv
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "..", "Part1")); sys.path.insert(0, HERE)
from imgio import read_image
import stage0 as s0
I=sys.argv[1].rstrip("/") + "/"
rows={r['file'].split('/')[-1]:r for r in csv.DictReader(open(sys.argv[2] if len(sys.argv) > 2 else os.path.join(HERE, 'results', 'stage0.csv')))}

def rank(key):
    o = np.argsort(key, kind="stable"); k = key[o]
    start = np.r_[0, np.flatnonzero(k[1:] != k[:-1]) + 1]
    cnt = np.arange(len(k)) - np.repeat(start, np.diff(np.r_[start, len(k)]))
    r = np.empty_like(cnt); r[o] = cnt; return r

def adaptive(r, ctx, a=0.5):
    t, e = s0.tokens(r.ravel()); c = ctx.ravel(); nt = 64   # fixed alphabet the coder would use
    return (-np.log2((rank(c*nt+t)+a)/(rank(c)+a*nt))).sum() + e.sum()

def static(r, ctx): return s0.pixel_costs(r, ctx).sum()

acc = {}
def add(group, key, bits, npx):
    g = acc.setdefault(group, {}); b, n = g.get(key, (0, 0)); g[key] = (b+bits, n+npx)

for f in sorted(glob.glob(I+"imgs-pgm8/*"))+sorted(glob.glob(I+"imgs-raw16/*")):
    im = read_image(f); x = im.data.astype(np.int64); npx = x.size
    depth = im.bpp if im.fmt == "raw" else int(im.maxval).bit_length()
    grp = "raw16" if im.fmt == "raw" else "pgm8"
    for pred in ("MED", "GAP", "WGT"):
        r, c = s0.plane_residual(x, 0, im.maxval, pred, depth)
        add(grp, f"{pred} static", static(r, c), npx); add(grp, f"{pred} adaptive", adaptive(r, c), npx)
    for k in ("JPEG-LS", "JPEG-XL"): add(grp, k, float(rows[f.split('/')[-1]][k])*npx, npx)

for f in sorted(glob.glob(I+"imgs-ppm/*.ppm")):
    if f.endswith("arial.ppm"): continue          # palette image, handled separately
    im = read_image(f); npx = im.rows*im.cols
    for pred in ("MED", "GAP", "WGT"):
        best_s = best_a = None
        for tf in ("none", "subG", "RCT", "YCoCg-R"):
            bs = ba = 0
            for p, lo, hi in s0.colour_planes(im.data, im.maxval, tf):
                r, c = s0.plane_residual(p, lo, hi, pred, 8)
                bs += static(r, c); ba += adaptive(r, c)
            best_s = bs if best_s is None else min(best_s, bs); best_a = ba if best_a is None else min(best_a, ba)
        add("ppm natural (11)", f"{pred} static (best tf/img)", best_s, npx)
        add("ppm natural (11)", f"{pred} adaptive (best tf/img)", best_a, npx)
    for k in ("JPEG-LS", "JPEG-XL"): add("ppm natural (11)", k, float(rows[f.split('/')[-1]][k])*npx, npx)

for g, d in acc.items():
    print(f"\n{g}")
    for k, (b, n) in sorted(d.items(), key=lambda kv: kv[1][0]):
        print(f"  {k:32s} {b/n:7.3f}")
