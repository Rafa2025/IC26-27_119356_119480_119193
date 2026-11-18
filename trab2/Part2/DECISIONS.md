# Part 2 – Lossless image codec: decision log

Every decision, the options considered, the evidence and the reason.
Status: **decided** (agreed), **proposed** (waiting for agreement), **open**.
Numbers are bits per pixel (bpp) = total bits / total pixels of the group; for PPM
a pixel is the three channels together. Stage 0 numbers are estimates (see
`stage0/README.md`), checked to be within 0.2% of a real adaptive coder.

Image groups: `pgm8` (7 grayscale 8-bit), `raw16` (4 grayscale 16-bit RAW),
`ppm natural` (11 colour photos), `palette` (23 Kodak `.256` + `arial.ppm`:
255–256 colours each).

---

## Decided (2026-11-17; D6–D9 accepted after Stage 0, D10–D12 after Stage 0b)

| # | Decision | Reason |
|---|---|---|
| D1 | **Goal**: clearly beat JPEG-LS; anything that costs speed must earn it in ratio | Ratio, speed and memory are all graded |
| D2 | **Prototype in Python first** (Stage 0), then C | Choose from data before writing C |
| D3 | **Entropy coder**: multi-symbol adaptive (rANS or adaptive arithmetic), residual split into token + raw extra bits | Faster decode than a binary coder; small alphabet also works for 16-bit |
| D4 | **Independent tiles + OpenMP threads** | Parallel encode/decode, low memory |
| D5 | **Colour transform chosen from Stage 0 data**, not fixed in advance | Measured below |
| D6 | **Predictor: WGT**, rewritten with integer maths for C (was P1) | F1 |
| D7 | **Colour: encoder chooses none / RCT / YCoCg-R per image** (from a subsample), 1 byte in the header; subG and ICP dropped (was P2) | F2 |
| D8 | **Palette mode** when an image has ≤ 256 colours, index coded with context = W index (was P3) | F3 |
| D9 | **No value packing** for 16-bit for now (was P4) | F4 |
| D10 | **Coding contexts = activity (8 levels) × WGT error (8 levels)** = 64 per plane (was P5) | F5 |
| D11 | ~~Bias correction (365 gradient contexts) only for images deeper than 8 bits~~ — **reversed by D24** (too slow for its gain) | F6 |
| D12 | **Colour planes 2 and 3: error context includes the earlier planes' errors at the same pixel** (was P7) | F7 |

## Stage 0 findings (2026-11-17)

### F1 – Predictor: the weighted blend (WGT) is best on every group

WGT = 6 sub-predictors (W, N, NE, NW, W+N−NW, MED), each weighted by
1/(its error at W, N, NW, NE)².

| bpp | WGT | GAP | MED | JPEG-LS | JPEG-XL |
|---|---:|---:|---:|---:|---:|
| pgm8 | **3.944** | 4.011 | 4.037 | 4.000 | 3.870 |
| raw16 | **7.740** | 7.828 | 8.000 | 7.870 | 7.891 |
| ppm natural (best colour transform per image) | **12.624** | 12.859 | 13.111 | 13.596 | 12.387 |

(adaptive-coder costs, `results/adaptive.txt`)

- vs JPEG-LS: −1.4% (pgm8), −1.7% (raw16), −7.1% (ppm natural)
- vs JPEG-XL: +1.9% (pgm8), −1.9% (raw16), +1.9% (ppm natural)

### F2 – Colour: choosing the transform per image gains 3.3%; per tile adds nothing

WGT, ppm natural (static estimate, `results/stage0.csv`):

| none | subG | RCT | YCoCg-R | ICP | best per image | best per 256×256 tile |
|---:|---:|---:|---:|---:|---:|---:|
| 13.217 | 13.220 | **13.031** | 13.036 | 13.492 | **12.598** | 12.592 |

- Winner per image: RCT 6, none 4, YCoCg-R 1. No single transform is right for all images.
- subG is never better than RCT; the simple inter-channel prediction (ICP: R and B
  residuals minus G's residual) is the worst option.
- Per-tile choice gains only 0.05% more.

### F3 – Palette images need their own mode (the biggest finding)

All 23 Kodak `.256` images and `arial.ppm` have at most 256 colours. Predictive
coding fails on them (the colours of neighbouring pixels are not close numbers):

| bpp | palette + adaptive model, context = W index | palette + xz | WGT (best transform per tile) | JPEG-LS | JPEG-XL |
|---|---:|---:|---:|---:|---:|
| Kodak (23) | **3.650** | 4.076 | 11.006 | 12.848 | 4.201 |
| arial | **4.996** | 5.178 | 14.19 | 17.180 | 5.764 |

Palette mode = list of colours (24 bits each) + one index per pixel, the index coded
with an adaptive model whose context is the index of W. It beats JPEG-XL by 13%.
Larger contexts (W,N: 3.852; W,N,NE: 4.725) are worse: too many contexts to learn.
Sorting the palette by brightness and predicting the index is also worse (4.68).

### F4 – Value packing for 16-bit: almost no gain (my earlier guess was wrong)

The 16-bit images use few distinct values (1,620–34,157 of 65,536), so I expected
remapping them to 0..K−1 to help a lot. Measured: 7.739 → 7.725 bpp (−0.2%) with WGT.

## Stage 0b findings (2026-11-17)

Simulated pixel by pixel like the future C codec (`stage0/stage0b.py`): WGT with
integer maths, adaptive frequency tables per context (+32 per symbol, halved past
2^16), exact cost of that model. Config A (Stage 0's 12 activity contexts) is the
starting point. Full table: `stage0/results/stage0b.txt`.

### F5 – Contexts: activity × error (8 × 8 = 64 contexts) is the best

Activity = |W−NW| + |N−NW| + |NE−N|; error = sum of WGT's own |error| at W, N, NW, NE.

| bpp | A: activity 12 | error 12 | activity 6 × error 6 | **J: activity 8 × error 8** |
|---|---:|---:|---:|---:|
| pgm8 | 3.929 | 3.908 | 3.910 | **3.880** (−1.3%) |
| raw16 | 7.714 | 7.715 | 7.721 | **7.711** (−0.0%) |
| ppm natural | 12.576 | 12.600 | 12.606 | **12.514** (−0.5%) |

Finer error levels (16, 24 half-octave steps) and the best sub-predictor's error as
context were in between; none beat J.

### F6 – Bias correction helps only 16-bit images

Bias correction = per context, the mean of recent prediction errors is added to the
prediction (JPEG-LS 365 gradient contexts, halved every 128 pixels).

| bpp | J | J + bias (365 ctx) |
|---|---:|---:|
| pgm8 | **3.880** | 3.881 |
| raw16 | 7.711 | **7.690** (−0.3%) |
| ppm natural | **12.514** | 12.525 |

Using the coding context itself for the bias (instead of the 365 gradient contexts)
was worse everywhere (on A: +0.3% pgm8, +0.1% raw16, +0.7% ppm natural).

### F7 – Colour: the error already seen in the earlier planes is a good context

For the 2nd and 3rd colour planes, adding to the error context the |error| of the
planes already coded at the same pixel:

| bpp, ppm natural | J | first plane's error | **all earlier planes' errors (Q)** |
|---|---:|---:|---:|
| | 12.514 | 12.481 | **12.478** (−0.3%) |

### Where Stage 0b leaves us

| bpp | best option | vs JPEG-LS | vs JPEG-XL |
|---|---:|---:|---:|
| pgm8 | 3.880 (J) | −3.0% | +0.3% |
| raw16 | 7.690 (J + bias; 7.711 without, see D24) | −2.3% | −2.5% |
| ppm natural | 12.478 (Q) | −8.2% | +0.7% |
| palette (Kodak) | 3.650 (F3) | −71.6% | −13.1% |

## Stage 1 – C skeleton (2026-11-17)

| # | Decision | Reason |
|---|---|---|
| D13 | Default tile size 256×256 for now (`-t`, 16..16384); tuned in Stage 3 (now 512, D23) | O2: only measurable once tiles are compressed |
| D14 | Container **ICL2** version 1, little-endian, layout in `container.h`; a table of tile sizes lets each thread find its tile | Parallel decode |
| D15 | Original PNM header and any bytes after the pixels stored **as bytes**, not rebuilt | Byte-exact output (e.g. `tulips.ppm` has a comment in its header) |
| D16 | **CRC-32 of the original file** stored; the decoder checks it and deletes the output on a mismatch | Damaged files are detected, never silently wrong |
| D17 | Only binary P5/P6; ASCII P2/P3 and `.gz` rejected with a clear message | The guide's formats; the benchmark unzips Kodak first |
| D18 | Unknown flags / non-zero reserved byte are rejected | A damaged header can never decode "successfully" with the wrong settings |

Results: 46 images + 22 edge cases (1×1, 1×N, N×1, odd sizes, maxval 1 and 4095,
values above maxval, 16-bit RAW in both byte orders, header comments with CRLF,
trailing bytes, tile sizes 16/100/16384) all byte-identical; 168 bad-input cases
(unsupported/truncated images, cut and bit-flipped compressed files) all fail
cleanly with exit code 1 and no output. Same results under address and
undefined-behaviour sanitizers. The test script was checked by planting a bug
(swapped bytes in 16-bit tiles): it reported 9 failures.

Largest image (4200×2154, 16-bit): encode 0.33 s, decode 0.08 s, encoder peak
memory 78 MB (about 4× the file: whole file + samples + tile buffers + output).

## Stage 3, step 1 – adaptive range coder (2026-11-17)

| # | Decision | Reason |
|---|---|---|
| D19 | **Adaptive range coder** (LZMA-style carry handling, 32-bit range, one division per symbol), not rANS (closes O3) | With adaptive tables the totals are not powers of two, so rANS also needs a division per symbol, and it must encode in reverse (store every symbol of a tile first). The range coder streams forward, with no extra pass or memory. (Not benchmarked against rANS.) |
| D20 | Model exactly as simulated in Stage 0b: 64 tokens, every count starts at 1, +32 per coded symbol, all halved when the total passes 2^16 | The Stage 0b estimates then carry over to C |
| D21 | Residual = token (u < 16 directly; else exponent + 1 mantissa bit) + raw extra bits; handles \|r\| < 2^24 | Small alphabet for 8- and 16-bit images |
| D22 | Makefile made generic (every `.c` except `*_main.c`/`test_*.c` is codec code; every `test_*.c` is a unit test) | New files need no Makefile change |

Unit tests (`test_rc.c`, also under sanitizers), all passed:
- 8 M residuals from Laplacian distributions in 64 contexts (8.4 bits/residual):
  **overhead +0.005%** over the ideal cost of the same model;
  encode 21–22 M residuals/s, decode 30 M residuals/s (this stream is about twice as
  wide as real image residuals; real speed is measured in step 2)
- 8 M zeros: 2,205 bytes (0.0022 bits/residual)
- residuals up to ±2^24 (largest token 57 of 64), raw bits of every width 0–16
- 200 random byte strings decoded without crashing or reading out of bounds
- writing output bytes inline instead of by function call: +22% encode speed

## Stage 3, step 2 – WGT + 64 contexts for grayscale (2026-11-17)

`plane.c`: one function codes a plane for both encoder and decoder, so they cannot
drift apart. Tiles fall back to "stored" if coding does not make them smaller.

**The C codec matches the Stage 0b simulation.** With one tile per image
(`-t 16384`), whole files including the container:

| bpp | C codec | Stage 0b estimate (J, no bias) |
|---|---:|---:|
| pgm8 | 3.881 | 3.880 |
| raw16 | 7.712 | 7.711 |

**Tile size (O2)** – each tile starts with empty models and loses its neighbours at
the border. 2 threads (this machine has 2 cores), best of 2 runs, process start and
file I/O included:

| tile | pgm8 bpp | raw16 bpp | pgm8 enc/dec MB/s | raw16 enc/dec MB/s |
|---:|---:|---:|---:|---:|
| 64 | 4.266 | 7.811 | – | – |
| 128 | 4.028 | 7.739 | – | – |
| 256 | 3.931 | 7.712 | 29.2 / 29.8 | 47.0 / 52.1 |
| **512** | **3.893** | **7.703** | **24.7 / 24.4** | **43.2 / 48.6** |
| 1024 | 3.884 | 7.701 | 21.1 / 21.0 | 40.0 / 47.9 |
| 16384 (1 tile) | 3.881 | 7.712 | – | – |

(64/128/16384 rows from a single run without repeats; their speeds are not comparable.)
16-bit images lose almost nothing to tiles (the models even adapt better); 8-bit
images, which are small (most 512×512), pay +1.3% at 256 but +0.3% at 512.

**Speed** – weights 2^40/s² taken from a table for s < 4096 instead of dividing:
same output, single-thread speed pgm8 13.3 → 16.4 MB/s encode, 12.3 → 17.5 decode;
raw16 23.9 → 27.0 / 26.5 → 30.4 (tile 256).

For context (single thread, inside Python via imagecodecs, so without process
start and file I/O – not a fair final comparison, that is Stage 5):

| MB/s enc / dec | ours (1 thread, tile 256) | JPEG-LS (CharLS) | JPEG-XL effort 7 |
|---|---:|---:|---:|
| pgm8 | 16.4 / 17.5 | 39.7 / 49.2 | 2.1 / 11.2 |
| raw16 | 27.0 / 30.4 | 75.8 / 79.9 | 3.8 / 18.1 |

All tests pass (unit, 46 images, 22 edge cases, 168 bad inputs), also with sanitizers.

| # | Decision (after step 2) | Reason |
|---|---|---|
| D23 | **Default tile size 512** (was P8; closes O2) | +0.2% bpp over 1024 on pgm8, 17% faster with 2 cores |

## Stage 3, step 3 – bias correction for 16-bit (2026-11-17): dropped

Implemented exactly as simulated (365 gradient contexts, mean of recent errors).
With one tile it matches Stage 0b: raw16 7.691 bpp (estimate 7.690). But it costs
much more speed than its 0.2% gain:

| raw16, tile 512, best of 3 | bpp | enc / dec s, 1 thread | enc / dec MB/s, 2 threads |
|---|---:|---:|---:|
| no bias correction | 7.703 | 1.22 / 1.08 | 46.1 / 51.8 |
| bias, as simulated (64-bit division) | 7.686 (−0.22%) | 1.36 / 1.27 | 37.8 / 43.3 |
| bias, 32-bit division | 7.686 | 1.39 / 1.28 | 38.1 / 44.2 |
| bias, JPEG-LS division-free update | 7.684 (−0.25%) | 1.34 / 1.24 | 37.5 / 41.1 |
| bias, branch-free gradient quantizer | 7.686 | 1.43 / 1.26 | 39.3 / 45.4 |

So the division is not the cost (the three cheaper versions are no faster); the
whole extra per-pixel work is: 3 gradients, the sign merge, and the update.
About −0.2% size for +11–18% time breaks D1 ("anything that costs speed must earn it").

| # | Decision | Reason |
|---|---|---|
| D24 | **No bias correction at all** (reverses D11; P9's optional flag also rejected, to keep the code simple). Header flag bit 0 stays unused. | Table above |

## Stage 3, step 4 – colour images (2026-11-17)

Colour tiles: transform each pixel (none → G,R,B; RCT → Y,U,V; YCoCg-R → Y,Co,Cg),
then code the 3 planes one after the other in one range-coder stream with
`plane_code()`; planes 2 and 3 add the earlier planes' |errors| at the same pixel to
their error context (D12). The transform is in header flag bits 1-2.

**The C codec matches the Stage 0b simulation** (11 natural photos, one tile, best
transform per image): **12.478 bpp**, estimate 12.478.

| ppm natural (11), tile 512 | bpp | enc s | dec s | enc / dec MB/s (2 threads) |
|---|---:|---:|---:|---:|
| `-c none` for all | 13.041 | 0.57 | 0.57 | 18.2 / 18.5 |
| `-c rct` for all | 12.917 | 0.58 | 0.57 | 18.0 / 18.4 |
| `-c ycocg` for all | 12.918 | 0.58 | 0.56 | 18.0 / 18.8 |
| **`-c auto`** (fast estimate) | **12.497** | 0.62 | 0.54 | 16.9 / 19.2 |
| `-c best` (code with all 3, keep smallest) | 12.497 | 1.57 | 0.55 | 6.7 / 18.9 |

- Choosing per image saves 3.3% over the best single transform (as in Stage 0, F2).
- The fast estimate (MED residual bit lengths of the 3 transforms on every 8th
  pair of rows) picks **the same transform as `best` on all 12 PPM images**, for
  +7% encode time instead of +170%.
- Winners: RCT 5, none 4, YCoCg-R 2 of the 11 natural photos (arial: YCoCg-R).
- vs references: 12.497 is −8.1% vs JPEG-LS (13.596), +0.9% vs JPEG-XL (12.387).
- Palette images still go through prediction for now: Kodak 10.787 bpp (step 5 target ~3.65).

| # | Decision | Reason |
|---|---|---|
| D25 | Encoder default `-c auto` (fast estimate); `-c best` and forced transforms kept as options | Same choice as `best` on every test image, at 7% of the cost |

All tests pass (unit, 46 images, 33 edge cases incl. every combination of extreme
colour values under each transform and values above maxval, 168 bad inputs), also
with sanitizers.

## Stage 3, step 5 – palette mode (2026-11-17)

Colour images with at most 256 colours: the colours (sorted) are stored once in the
container header; each tile codes the colour index of every pixel with an adaptive
model chosen by the index of the pixel on the left (K models × K symbols, counts start
at 1, +increment per coded symbol, halved past 2^16). Header type 2 = palette.

Model increment (how fast the palette models adapt), tile 512:

| increment | 4 | 8 | 16 | 24 | **32** | 64 | 128 | 256 | 512 |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| Kodak (23) bpp | 3.765 | 3.699 | 3.670 | 3.664 | **3.662** | 3.659 | 3.657 | 3.663 | 3.695 |
| arial bpp | – | 5.073 | 5.082 | – | **5.122** | 5.168 | 5.200 | – | – |

Over the 24 palette images together, 32, 64 and 128 are equal to within 0.03%.

| # | Decision | Reason |
|---|---|---|
| D26 | Palette mode for every colour image with ≤ 256 colours (no cost comparison with normal coding); `-nopal` turns it off | Stage 0 F3: much better on all 24 such images |
| D27 | Palette model increment **32** | Flat optimum on the table above; same value as the main model (D20) |

Results (tile 512): **Kodak 3.662 bpp** (Stage 0 estimate 3.650 for whole images;
one tile here gives 3.652), **arial 5.122** (estimate 4.996 with the best rate per
image). Kodak speed 50 MB/s encode, 44 MB/s decode (2 threads).
Tests: 8 more edge cases (1, 2, 256 and 257 colours, 16-bit palette, tile 16, 1×1,
`-nopal`); the step-4 colour-transform cases now force `-nopal` so they still test
the transforms. All pass, also with sanitizers.

## Where Stage 3 leaves us (real C codec, defaults, tile 512)

| bpp (lower is better) | original | **ours** | JPEG-LS | JPEG-XL | xz -9 | zstd -19 | vs JPEG-LS | vs JPEG-XL |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| 8-bit gray (7) | 8 | **3.893** | 4.000 | 3.870 | 4.845 | 5.398 | −2.7% | +0.6% |
| 16-bit RAW (4) | 16 | **7.703** | 7.870 | 7.891 | 8.409 | 8.969 | −2.1% | −2.4% |
| colour photos (11) | 24 | **12.497** | 13.596 | 12.387 | 17.136 | 18.965 | −8.1% | +0.9% |
| palette: Kodak (23) | 24 | **3.662** | 12.848 | 4.201 | 4.747 | 4.917 | −71.5% | −12.8% |
| palette: arial (1) | 24 | **5.122** | 17.180 | 5.764 | 6.138 | 6.341 | −70.2% | −11.1% |

Compression ratio over all 46 images: **ours 2.738**, JPEG-LS 1.917, JPEG-XL 2.648,
xz 2.296, zstd 2.136 (ours −30.0% bits vs JPEG-LS, −3.3% vs JPEG-XL).
Speed with 2 threads (process start + file I/O included): 8-bit gray 24/24 MB/s,
16-bit 45/51, colour photos 17/18, Kodak 50/44 (encode/decode).

## Stage 5 – fair benchmark against other codecs (2026-11-18)

`benchmark/compare.py`: every codec runs as a program on the same 46 files; bpp,
encode/decode time (best of 3; one run if over 2 s), peak memory per process
(through `memrun.c`), and every decoded image checked to be identical. JPEG-LS =
CharLS 2.4.2 through our small `jls_tool`; JPEG-XL = cjxl/djxl 0.7.0; zstd 1.5.5;
xz; ImageMagick PNG. Machine: 2 cores, Ubuntu 24.04. Full tables in
`benchmark/results/compare.md` and `benchmark/results_1thread/compare.md`.

**Bits per pixel** (lower is better):

| | ours | JPEG-LS | JPEG-XL e3 | JPEG-XL e7 | PNG | xz -9 | zstd -19 |
|---|---:|---:|---:|---:|---:|---:|---:|
| 8-bit gray (7) | 3.893 | 4.001 | 3.897 | **3.842** | 4.378 | 4.844 | 5.398 |
| 16-bit RAW (4) | 7.703 | 7.870 | 7.844 | **7.699** | 9.576 | 8.409 | 8.966 |
| colour photos (11) | **12.497** | 13.596 | 13.250 | 12.657 | 14.724 | 17.136 | 18.970 |
| palette: Kodak (23) | **3.662** | 12.848 | 10.407 | 4.301 | 10.830 | 4.748 | 4.918 |
| palette: arial (1) | **5.122** | 17.180 | 12.876 | 6.010 | 15.689 | 6.139 | 6.341 |
| **ratio, all 46** | **2.738** | 1.917 | 2.088 | 2.662 | 1.835 | 2.296 | 2.136 |

**Speed, MB/s encode / decode** (process start and file I/O included):

| | ours | JPEG-LS | JPEG-XL e3 | JPEG-XL e7 |
|---|---:|---:|---:|---:|
| all 46, 2 threads | **41.6 / 43.7** | 28.9 / 34.8 | 21.8 / 28.4 | 2.1 / 23.7 |
| all 46, 1 thread | 28.2 / 29.7 | **28.7 / 33.9** | 15.7 / 20.9 | 1.9 / 15.8 |
| 8-bit gray, 1 thread | 18.8 / 19.2 | **23.8 / 27.6** | 11.8 / 15.0 | 1.2 / 9.7 |
| 16-bit RAW, 1 thread | 28.4 / 33.6 | **44.6 / 51.1** | 21.6 / 31.9 | 1.8 / 17.8 |
| colour photos, 1 thread | 15.7 / 17.7 | **19.0 / 22.6** | 12.8 / 16.4 | 1.2 / 9.7 |
| Kodak, 1 thread | **45.8 / 38.4** | 24.5 / 29.6 | 13.6 / 17.3 | 3.0 / 20.3 |

(PNG decodes at ~70 MB/s and zstd at ~290 MB/s, but compress far worse.)

**Peak memory, MB, largest file of each group (enc / dec)**: ours 12/10 (8-bit),
43/28 (16-bit, 18 MB file), 16/17 (colour), 7/6 (Kodak); JPEG-LS 12/8, 47/29, 11/8,
8/6; JPEG-XL e7 135/44, 758/176, 127/26, 64/22.

Findings:
- Best compression of all codecs over the 46 images (ratio 2.738 vs 2.662 for
  JPEG-XL e7), clearly on colour photos (−1.3% vs JPEG-XL e7) and palette images
  (−15% vs JPEG-XL e7); within 0.05% of JPEG-XL e7 on 16-bit; 1.3% behind it on 8-bit.
- With 2 threads, the fastest encoder and decoder of the image codecs. With one
  thread, about as fast as JPEG-LS overall, but 20–40% slower than JPEG-LS on
  grayscale and colour photos; 15–20× faster than JPEG-XL e7 at encoding.
- Memory as low as JPEG-LS, 5–17× lower than JPEG-XL.
- **Tool versions matter**: Stage 0 used libjxl 0.13 (via imagecodecs), the CLI here is
  0.7. For JPEG-XL e7: 8-bit 3.870 (0.13) vs 3.842 (0.7); 16-bit 7.891 vs 7.699;
  colour photos 12.387 vs 12.657. The report must name the versions.

---

## Proposed (waiting for agreement)

None right now.

## Open

| # | Question | Plan |
|---|---|---|
| O1 | Remaining gap to JPEG-XL: +0.3% pgm8, +0.7% ppm natural (was +1.9%) | Mostly closed by Stage 0b; further tuning (WGT sub-predictors, weights) in C if time allows |
| O5 | Cheaper tile starts: models begin empty in every tile | Try a better starting state for the models (e.g. typical counts per context), which would make small tiles (more threads) cheaper |
| O4 | Encoder memory: 43 MB for the 18 MB 16-bit file (~2.4×) | Measured in Stage 5; already as low as JPEG-LS, so low priority |
