# Part 2 – Lossless image codec: approaches

Graded on **compression ratio + speed + memory**, compared with JPEG-LS, JPEG-XL, zstd, lzma.
Decoder must rebuild the file **byte-for-byte** (header, maxval, 16-bit endianness included).

Common pipeline:

```
pixels → [colour transform] → [prediction] → residuals → [context modelling] → [entropy coder] → bits
```

Neighbours used (only already-coded pixels, so the decoder can repeat everything):

```
      NN  NNE
 NW   N   NE
 WW  W   x
```

---

## A. JPEG-LS (LOCO-I)

- Gradients `NE−N`, `N−NW`, `NW−W` → quantized to 9 levels each → **365 contexts**
- **MED predictor**: `min(W,N)` / `max(W,N)` at edges, `W+N−NW` in smooth areas
- Per-context **bias correction**
- **Golomb-Rice** coding, parameter `k` adapted per context (from average |error|)
- **Run mode** for flat areas (code run length instead of pixels)
- Very fast, simple; ceiling = JPEG-LS itself

## B. CALIC

- Horizontal/vertical edge strength `dh`, `dv`
- **GAP predictor**: follows edge direction (W, N or blends)
- Context from **error energy** (`dh + dv + |previous error|`, 8 levels) + **texture pattern** (8 bits)
- **Error feedback**: add mean error of the context to the prediction
- **Adaptive arithmetic coding**, one model per energy level
- **Binary mode** for areas with only 1–2 values (text/graphics)
- A few % better than JPEG-LS, 2–3× slower

## C. Blended predictors + adaptive rANS

- Several simple predictors (W, N, W+N−NW, MED, N+(NE−NW)/2…)
- Each weighted by its **recent local error** → weighted average (integer maths)
- Context = quantized local error (~16 buckets)
- Residual split into **token + extra raw bits** (small alphabet, works for 16 bpp)
- **rANS** coder with adaptive frequency tables (note: encodes in reverse order)
- Good ratio, still fast

## D. Least-squares linear predictor

- Encoder solves the best linear predictor (order 6–12) **per tile**, stores the coefficients
- Decoder only applies them (cheap decode)
- **Online variant**: re-solved per pixel on both sides, no coefficients sent → best ratio, very slow
- Can use the other colour channel as an extra input (inter-channel)
- Strong on smooth / 16-bit images

## E. Context mixing (paq-style)

- Code bit by bit
- Many models (hashed contexts) each give a probability
- Small neural **mixer** combines them, learns online
- **Binary arithmetic coder**
- Best ratio, 10–100× slower → loses speed/memory marks

| | Prediction | Context | Coder | Speed |
|---|---|---|---|---|
| A | MED | 365 gradient ctx | Golomb-Rice | ★★★ |
| B | GAP + error feedback | energy × texture | arithmetic | ★★☆ |
| C | weighted mix | error buckets | rANS | ★★☆ |
| D | least squares | like C | rANS | ★★☆ (online ★☆☆) |
| E | learned mixing | hashed ctx | binary arithmetic | ★☆☆ |

---

## Extra ideas (combine with any of the above)

- **Mode search in the encoder**: try a few setups per image/tile, store the winner in 1 byte; decoder stays fast
- **Histogram packing**: if only a few values are used (e.g. unused low bits in 16-bit), remap to 0..K−1
- **Tiles/strips + threads (OpenMP)**: parallel encode/decode, low memory, small ratio loss
- **LZ fallback** for synthetic images (text, graphics)
- **Colour**: `G, R−G, B−G` or YCoCg-R (reversible), or predict R/B from G's residual
- **rANS instead of arithmetic coding**: same ratio, faster decode
- **Never expand**: store a tile raw if coding makes it bigger
