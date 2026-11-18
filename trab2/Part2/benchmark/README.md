# Benchmark – our codec against other lossless codecs

`compare.py` runs every codec as a separate program on the same files and measures
them the same way: bits per pixel, encode/decode time (best of 3) and peak memory of
each process, and checks that every decoded image is identical to the original.

| Codec | Program | Notes |
|---|---|---|
| ours | `../img_lossless_enc`, `../img_lossless_dec` | build with `make` in `..` |
| JPEG-LS | `jls_tool` (this folder) on top of the CharLS library | built automatically if `cc` and CharLS are installed |
| JPEG-XL | `cjxl -d 0 -e 3` and `-e 7` (7 = default), `djxl` | lossless mode |
| PNG | ImageMagick `convert`, compression level 9 | |
| xz, zstd | `xz -9`, `zstd -19` on the original file | general-purpose |

Codecs whose programs are missing are skipped. Install them with e.g.
`sudo dnf install libjxl-utils zstd ImageMagick charls-devel` (Fedora) or
`sudo apt install libjxl-tools zstd imagemagick libcharls-dev` (Ubuntu).

```
python3 compare.py ../../../ImageFiles                     # default threads -> results/
python3 compare.py ../../../ImageFiles --threads 1 -o results_1thread
python3 compare.py ../../../ImageFiles --groups pgm8 --codecs ours JPEG-LS
```

`memrun.c` is a tiny launcher that reports a command's time and peak memory: a
process started directly from Python reports Python's own memory, not the codec's.

Note: compression depends on the version of each tool (e.g. libjxl 0.7 and 0.13 give
different JPEG-XL sizes), so the report must state the versions used.
