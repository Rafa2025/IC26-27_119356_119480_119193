# Stage 0 – choosing the design of the lossless codec in Python

Before writing the C codec, these scripts estimate how many bits per pixel (bpp)
each design option would cost, on all the images in `ImageFiles/`.

Needs Python 3, numpy and `imagecodecs` (`pip install imagecodecs`, used only for
the JPEG-LS / JPEG-XL / zstd reference numbers).

```
cd trab2/Part2/stage0
python3 stage0.py ../../../ImageFiles -o results        # ~4 min, writes results/stage0.csv + stage0_summary.md
python3 palette.py ../../../ImageFiles  > results/palette.txt    # palette images (<= 256 colours)
python3 adaptive.py ../../../ImageFiles > results/adaptive.txt   # static estimate vs real adaptive cost
python3 stage0b.py ../../../ImageFiles > results/stage0b.txt     # Stage 0b, needs numba (~1 min)
```

## How the cost is estimated

Every option uses the same model of the future coder, so they compare fairly:

1. residual `r = x - prediction`, `u = zigzag(r)`
2. `u` is split into a **token** (u itself if u < 16, otherwise exponent + 1 mantissa bit)
   and **raw extra bits**, so the alphabet stays small (~40 tokens) even for 16-bit images
3. the token is coded with a model per **context** (12 levels of local activity
   `|W-NW| + |N-NW| + |NE-N|`, log scale)
4. cost = H(token | context) × pixels + extra bits

`adaptive.py` checks this static estimate against the exact cost of an adaptive
frequency model that learns as it goes (what the C coder will do): they differ by
less than 0.2%, so the estimate is safe for ranking options.

## Files

| File | What |
|---|---|
| `stage0.py` | predictors, colour transforms, value packing, per-image / per-tile transform choice, reference codecs |
| `palette.py` | options for palette images: index + xz, sorted index + prediction, adaptive context models |
| `adaptive.py` | static estimate vs adaptive model cost, for the main options |
| `stage0b.py` | Stage 0b: pixel-by-pixel simulation of the C codec (integer WGT, adaptive tables); bias correction and context options |
| `results/` | outputs (`stage0.csv` has one row per image) |
