# Lossless codec comparison (threads: 1)

bpp = total compressed bits / total pixels (lower is better); speed = total input bytes /
total time; memory = peak resident memory of one process (largest file of the group).

## 8-bit grayscale (7 images)

| codec | bpp | ratio | enc MB/s | dec MB/s | enc peak MB | dec peak MB |
|---|---:|---:|---:|---:|---:|---:|
| ours | 3.893 | 2.055 | 14.4 | 13.3 | 10 | 9 |
| JPEG-LS (CharLS) | 4.001 | 2.000 | 33.5 | 41.9 | 12 | 8 |
| JPEG-XL e3 | 3.897 | 2.053 | 11.5 | 15.5 | 76 | 15 |
| JPEG-XL e7 | 3.837 | 2.085 | 1.0 | 11.5 | 85 | 15 |
| PNG (ImageMagick) | 4.380 | 1.827 | 5.8 | 35.4 | 59 | 32 |
| xz -9 | 4.844 | 1.652 | 2.8 | 38.9 | 88 | 5 |
| zstd -19 | 5.398 | 1.482 | 3.7 | 221.3 | 57 | 6 |

## 16-bit RAW (4 images)

| codec | bpp | ratio | enc MB/s | dec MB/s | enc peak MB | dec peak MB |
|---|---:|---:|---:|---:|---:|---:|
| ours | 7.703 | 2.077 | 20.0 | 21.5 | 37 | 27 |
| JPEG-LS (CharLS) | 7.870 | 2.033 | 54.6 | 62.9 | 46 | 29 |
| JPEG-XL e3 | 7.844 | 2.040 | 21.4 | 31.2 | 174 | 52 |
| JPEG-XL e7 | 7.832 | 2.043 | 1.8 | 20.0 | 177 | 52 |
| PNG (ImageMagick) | 9.576 | 1.671 | 6.5 | 95.0 | 149 | 56 |
| xz -9 | 8.409 | 1.903 | 1.8 | 45.8 | 219 | 20 |
| zstd -19 | 8.969 | 1.784 | 2.0 | 377.1 | 110 | 12 |

## colour (12, incl. arial) (12 images)

| codec | bpp | ratio | enc MB/s | dec MB/s | enc peak MB | dec peak MB |
|---|---:|---:|---:|---:|---:|---:|
| ours | 11.802 | 2.034 | 12.1 | 11.2 | 12 | 12 |
| JPEG-LS (CharLS) | 13.933 | 1.723 | 29.2 | 34.8 | 12 | 8 |
| JPEG-XL e3 | 12.577 | 1.908 | 9.6 | 13.7 | 52 | 15 |
| JPEG-XL e7 | 12.007 | 1.999 | 0.9 | 9.4 | 77 | 16 |
| PNG (ImageMagick) | 13.876 | 1.730 | 8.5 | 52.6 | 29 | 21 |
| xz -9 | 16.100 | 1.491 | 2.8 | 31.7 | 85 | 4 |
| zstd -19 | 17.779 | 1.350 | 4.3 | 194.9 | 56 | 6 |

## palette: Kodak (23 images)

| codec | bpp | ratio | enc MB/s | dec MB/s | enc peak MB | dec peak MB |
|---|---:|---:|---:|---:|---:|---:|
| ours | 3.662 | 6.555 | 31.9 | 26.0 | 6 | 6 |
| JPEG-LS (CharLS) | 12.848 | 1.868 | 32.4 | 41.6 | 8 | 6 |
| JPEG-XL e3 | 4.949 | 4.849 | 11.3 | 30.4 | 23 | 16 |
| JPEG-XL e7 | 4.303 | 5.577 | 2.3 | 21.0 | 45 | 16 |
| PNG (ImageMagick) | 4.632 | 5.182 | 17.7 | 68.7 | 29 | 18 |
| xz -9 | 4.748 | 5.055 | 2.3 | 95.8 | 74 | 4 |
| zstd -19 | 4.918 | 4.880 | 2.7 | 296.5 | 38 | 4 |

## All images

| codec | ratio (original bytes / compressed bytes) | enc MB/s | dec MB/s |
|---|---:|---:|---:|
| ours | 2.738 | 20.1 | 19.2 |
| JPEG-LS (CharLS) | 1.917 | 38.4 | 46.9 |
| JPEG-XL e3 | 2.544 | 13.6 | 24.5 |
| JPEG-XL e7 | 2.639 | 1.6 | 16.6 |
| PNG (ImageMagick) | 2.242 | 8.7 | 68.9 |
| xz -9 | 2.296 | 2.1 | 51.3 |
| zstd -19 | 2.136 | 2.5 | 292.4 |
