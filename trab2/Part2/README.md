# Part 2 – Lossless image codec

C codec for RAW, PGM (P5) and PPM (P6) images, 8 or 16 bits per sample.
Design decisions and the measurements behind them: [DECISIONS.md](DECISIONS.md).
The Python measurements that chose the design: [stage0/](stage0/README.md).

**Status: Stage 3 done** – grayscale (8/16-bit) and colour images are compressed with
WGT prediction + 64 contexts + the adaptive range coder; colour images first get a
colour transform chosen per image; colour images with ≤ 256 colours use palette mode.

## Build

Needs gcc (or any C11 compiler with OpenMP) and make.

```
cd trab2/Part2
make            # img_lossless_enc, img_lossless_dec
make san        # same, with address/undefined-behaviour sanitizers, in san/
make test       # unit tests + round-trip tests on ../../ImageFiles
make test-san   # the same tests with the sanitizer build (needs libasan + libubsan)
```

## Run

```
# PGM / PPM
./img_lossless_enc ../../ImageFiles/imgs-ppm/tulips.ppm tulips.icl
./img_lossless_dec tulips.icl tulips_out.ppm

# 8-bit RAW
./img_lossless_enc -rows 512 -cols 512 image.raw image.icl

# 16-bit RAW (big- or little-endian)
./img_lossless_enc -rows 1124 -cols 1124 -bpp 16 -be image.raw image.icl
./img_lossless_dec image.icl image_out.raw
```

Options: `-t N` tile size in pixels (16..16384, default 512); `-c auto|best|none|rct|ycocg`
colour transform (default `auto`: fast estimate; `best` tries all three, 3× slower
encoding); `-nopal` turns palette mode off. The decoder needs no
options: everything is in the compressed file. The output is byte-for-byte the
original file (PNM header, comments and any trailing bytes included); a CRC-32 of
the original is stored and checked.

## Measure

```
python3 bench.py ../../ImageFiles                      # all groups, default tile size
python3 bench.py ../../ImageFiles --groups pgm8 raw16 -t 256 512 1024 --threads 1
python3 bench.py ../../ImageFiles --groups ppm --exclude arial --enc-args "-c best"
```

## Compare with other codecs

See [benchmark/README.md](benchmark/README.md): `python3 benchmark/compare.py ../../ImageFiles`.

## Files

| File | What |
|---|---|
| `imgfile.c/.h` | exact reading/writing of RAW, PGM, PPM; CRC-32 |
| `container.c/.h` | the ICL2 compressed format (layout documented in `container.h`) |
| `rc.h` | adaptive range coder: 64-token adaptive model, raw bits, residual coding |
| `plane.c/.h` | WGT prediction + 64 contexts for one plane of one tile (shared by encoder and decoder) |
| `palette.c/.h` | palette mode: colour list + index coding with context = left index |
| `codec.c/.h` | tiles, OpenMP, per-tile coding (stage 1: stored) |
| `enc_main.c`, `dec_main.c` | the two programs |
| `test_roundtrip.py` | all images + 41 edge cases + 168 bad-input cases |
| `bench.py` | bits per pixel, time and speed per image group and tile size |
| `test_rc.c` | unit tests of the coder: round trip, overhead, speed, edge cases, garbage input |
