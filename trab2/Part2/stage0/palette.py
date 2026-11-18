"""See README.md in this folder. Usage: python3 palette.py <ImageFiles folder> [results/stage0.csv]"""
import os, sys, glob, lzma, numpy as np, csv
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "..", "Part1")); sys.path.insert(0, HERE)
from imgio import read_image
import stage0 as s0
I=os.path.join(sys.argv[1], "imgs-ppm") + "/"
rows={r['file'].split('/')[-1]:r for r in csv.DictReader(open(sys.argv[2] if len(sys.argv) > 2 else os.path.join(HERE, 'results', 'stage0.csv')))}

def adaptive_bits(sym, ctx, nsym, a):
    """Exact cost of an adaptive frequency model per context: p = (n(ctx,s)+a)/(n(ctx)+a*nsym)."""
    def rank(key):              # number of earlier occurrences of the same key
        o = np.argsort(key, kind="stable"); k = key[o]
        start = np.r_[0, np.flatnonzero(k[1:] != k[:-1]) + 1]
        cnt = np.arange(len(k)) - np.repeat(start, np.diff(np.r_[start, len(k)]))
        r = np.empty_like(cnt); r[o] = cnt; return r
    n_cs = rank(ctx * nsym + sym); n_c = rank(ctx)
    return -np.log2((n_cs + a) / (n_c + a * nsym)).sum()

out = {}
files = sorted(glob.glob(I+"*.ppm")) + sorted(glob.glob(I+"kodak/*.gz"))
for f in files:
    im = read_image(f); d = im.data.reshape(-1, 3)
    pal, idx = np.unique(d, axis=0, return_inverse=True)
    K = len(pal); name = f.split('/')[-1]
    if K > 256: continue
    npx = im.rows*im.cols; idx = idx.reshape(im.rows, im.cols).astype(np.int64)
    pal_bits = K*24
    res = {"K": K}
    res["idx+xz"] = (len(lzma.compress(idx.astype(np.uint8).tobytes(), preset=9))*8 + pal_bits)/npx
    # sort palette by luma, then predictive coding of the index (stage 0 model)
    luma = pal @ np.array([299, 587, 114]); order = np.argsort(luma, kind="stable")
    rank_of = np.empty(K, np.int64); rank_of[order] = np.arange(K)
    sidx = rank_of[idx]
    for pred in ("MED", "WGT"):
        r, c = s0.plane_residual(sidx, 0, K-1, pred, 8)
        res[f"sorted+{pred}"] = (s0.pixel_costs(r, c).sum() + pal_bits)/npx
    nb = s0.neighbours(idx)
    for nm, ctx in (("adapt ctx=W", nb["W"]), ("adapt ctx=W,N", nb["W"]*K + nb["N"]),
                    ("adapt ctx=W,N,NE", (nb["W"]*K + nb["N"])*K + nb["NE"])):
        best = min(adaptive_bits(idx.ravel(), ctx.ravel(), K, a) for a in (0.02, 0.05, 0.1, 0.3))
        res[nm] = (best + pal_bits)/npx
    r0 = rows[name]
    res.update({k: float(r0[k]) for k in ("best_tile/MED", "JPEG-LS", "JPEG-XL", "xz-9")})
    out[name] = res
    print(name, " ".join(f"{k}={v:.2f}" if isinstance(v, float) else f"{k}={v}" for k, v in res.items()), flush=True)

keys = [k for k in next(iter(out.values())) if k != "K"]
kod = [v for n, v in out.items() if n[0].isdigit()]
print("\nKodak mean (all 768x512):", " ".join(f"{k}={np.mean([v[k] for v in kod]):.3f}" for k in keys))
print("arial:", " ".join(f"{k}={out['arial.ppm'][k]:.3f}" for k in keys))
