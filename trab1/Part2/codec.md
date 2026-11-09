# Lossless audio codec: design

C implementation. Commands required by the assignment:

```
wav_lossless_enc [flags] input.wav compressed_file
wav_lossless_dec compressed_file output.wav
```

The decoded file must be byte-identical to the original WAV.

## Scope

| Item | Decision |
|---|---|
| Language | C |
| Sample widths | 8, 16, 24-bit (held as `int32` internally) |
| Channels | mono, stereo (more than 2 channels or 32-bit: rejected with an error) |
| Block size | configurable by CLI flag (default 4096 samples) |
| Exact output | non-audio WAV bytes (header, extra chunks) stored verbatim |
| Predictor | fixed polynomial, orders 0 to 4, best per block |
| Entropy coder | Rice coding, `k` chosen per block |
| Stretch goal | adaptive LPC (same architecture, swaps the predictor) |

## Encoder flow

```mermaid
flowchart TD
    A[input.wav] --> B[Parse WAV]
    B -->|header and extra chunks| H[Stored verbatim]
    B -->|audio samples| C{Stereo?}
    C -->|yes| D["Mid/side<br/>MID = (L+R)//2<br/>SIDE = L-R"]
    C -->|no| E[Single channel]
    D --> F
    E --> F
    subgraph F[Per block and per channel]
        direction LR
        P["Predictor<br/>try orders 0-4<br/>keep lowest sum of abs residuals"] --> Z["Zigzag<br/>e>=0: 2e<br/>e<0: -2e-1"] --> R["Rice coder<br/>pick k from mean<br/>q unary, r in k bits"]
    end
    F --> W[Bit writer]
    H --> O
    W --> O[compressed file]
```

## Decoder flow

```mermaid
flowchart TD
    A[compressed file] --> B[Bit reader]
    B -->|header and extra chunks| H[Stored bytes]
    B -->|block records| F
    subgraph F[Per block and per channel]
        direction LR
        R["Rice decode<br/>count zeros = q<br/>read k bits = r<br/>u = q*2^k + r"] --> Z["Un-zigzag<br/>u even: u/2<br/>u odd: -(u+1)/2"] --> P["Un-predict<br/>x[n] = pred[n] + e[n]<br/>start from warm-up"]
    end
    F --> M{Stereo?}
    M -->|yes| I["Inverse mid/side<br/>sum = (MID*2) OR (SIDE AND 1)<br/>L = (sum+SIDE)/2<br/>R = (sum-SIDE)/2"]
    M -->|no| S[Single channel]
    I --> W[Rebuild WAV]
    S --> W
    H --> W
    W --> O[output.wav]
```

## Compressed file layout

```mermaid
flowchart LR
    G["Global header<br/>rate, channels, bit depth,<br/>sample count, block size"] --> X["Stored non-audio bytes<br/>WAV header + extra chunks"] --> B1["Block 1 record"] --> B2["Block 2 record"] --> BN["... Block N record"]
```

Each block record, per channel:

```mermaid
flowchart LR
    O["Predictor order<br/>3 bits"] --> K["Rice k<br/>5 bits"] --> WU["Warm-up samples<br/>raw, order count"] --> RS["Rice-coded residuals<br/>block size minus order"]
```


## Predictors

| Order | Prediction `pred[n]` | Warm-up samples |
|---|---|---|
| 0 | `0` | 0 |
| 1 | `x[n-1]` | 1 |
| 2 | `2x[n-1] - x[n-2]` | 2 |
| 3 | `3x[n-1] - 3x[n-2] + x[n-3]` | 3 |
| 4 | `4x[n-1] - 6x[n-2] + 4x[n-3] - x[n-4]` | 4 |

Residual: `e[n] = x[n] - pred[n]`. Selection cost: sum of `|e[n]|` over the block.

## Rice coding

For a non-negative value `u` and parameter `k`:

| Symbol | Meaning |
|---|---|
| `q = u >> k` | quotient, written in unary (`q` zeros then a `1`) |
| `r = u & (2^k - 1)` | remainder, written in `k` bits |
| cost | `q + 1 + k` bits |

Choosing `k`: roughly `log2(mean(u))` per block. Too small gives long unary runs, too large wastes remainder bits.

### Worked example (k = 2)

| u | q | r | code |
|---|---|---|---|
| 3 | 0 | 3 | `1 11` |
| 6 | 1 | 2 | `01 10` |
| 9 | 2 | 1 | `001 01` |

## Worked example: mid/side

| n | 0 | 1 | 2 | 3 |
|---|---|---|---|---|
| L | 100 | 102 | 105 | 103 |
| R | 98 | 101 | 104 | 102 |
| MID | 99 | 101 | 104 | 102 |
| SIDE | 2 | 1 | 1 | 1 |

SIDE is nearly constant, so its residuals are close to zero and cost very few bits.

## Code modules

Which source file provides which stage of the codec.

```mermaid
flowchart TD
    subgraph enc[Encoder pipeline]
        direction LR
        E1[Parse WAV] --> E2[Mid/side] --> E3[Predictor] --> E4[Zigzag] --> E5[Rice coder] --> E6[Bit writer]
    end
    subgraph dec[Decoder pipeline]
        direction LR
        D1[Bit reader] --> D2[Rice decode] --> D3[Un-zigzag] --> D4[Un-predict] --> D5[Inverse mid/side] --> D6[Rebuild WAV]
    end
    E1 -.- M1["wavfile.c<br/>wav_parse()"]
    D6 -.- M2["wavfile.c<br/>wav_rebuild()"]
    E2 -.- M3["stereo.c<br/>ms_forward()"]
    D5 -.- M4["stereo.c<br/>ms_inverse()"]
    E3 -.- M5["predictor.c<br/>choose_order()<br/>residuals()"]
    D4 -.- M6["predictor.c<br/>restore()"]
    E4 -.- M7["rice.c<br/>zigzag()"]
    D3 -.- M8["rice.c<br/>unzigzag()"]
    E5 -.- M9["rice.c<br/>choose_k()<br/>rice_encode()"]
    D2 -.- M10["rice.c<br/>rice_decode()"]
    E6 -.- M11["bitio.h<br/>bw_put()<br/>bw_put_unary()<br/>bw_flush()"]
    D1 -.- M12["bitio.h<br/>br_get()<br/>br_get_unary()"]
```

### Module responsibilities

| File | Provides | Used by |
|---|---|---|
| `bitio.h` | Buffered MSB-first bit writer and reader (`bw_put`, `bw_put_unary`, `bw_flush`, `br_get`, `br_get_unary`), fixed 64 KiB buffers | `rice.c`, `codec.c` |
| `wavfile.c` | `wav_parse()` splits a WAV into stored non-audio bytes plus `int32` samples. `wav_rebuild()` writes the byte-identical WAV back. Handles 8, 16, 24-bit | `codec.c` |
| `stereo.c` | `ms_forward()` and `ms_inverse()`: the exact mid/side transform | `codec.c` |
| `predictor.c` | Fixed predictors of order 0 to 4, `choose_order()` per block, `residuals()` on encode, `restore()` on decode | `codec.c` |
| `rice.c` | `zigzag()` / `unzigzag()`, `choose_k()`, `rice_encode()` / `rice_decode()` | `codec.c` |
| `codec.c` | `encode_block()` / `decode_block()` chain the stages for one block. Writes and reads the global header | `enc_main.c`, `dec_main.c` |
| `enc_main.c` | `wav_lossless_enc` CLI: flags (block size), file handling | none |
| `dec_main.c` | `wav_lossless_dec` CLI: file handling | none |

### Dependencies

```mermaid
flowchart BT
    bitio["bitio.h"]
    wavfile["wavfile.c"]
    stereo["stereo.c"]
    predictor["predictor.c"]
    rice["rice.c"]
    codec["codec.c"]
    enc["enc_main.c"]
    dec["dec_main.c"]
    rice --> bitio
    codec --> rice
    codec --> predictor
    codec --> stereo
    codec --> wavfile
    codec --> bitio
    enc --> codec
    dec --> codec
```

### Data passed between modules

```mermaid
flowchart LR
    A["wav_parse<br/>int32 samples per channel<br/>+ stored bytes"] --> B["ms_forward<br/>MID, SIDE arrays"]
    B --> C["residuals<br/>order + warm-up<br/>+ int32 residuals"]
    C --> D["zigzag<br/>uint32 values"]
    D --> E["rice_encode<br/>k + bits"]
    E --> F["bitio<br/>bytes in file"]
```