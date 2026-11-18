# Lossless codec comparison (default threads)

bpp = total compressed bits / total pixels (lower is better); speed = total input bytes /
total time; memory = peak resident memory of one process (largest file of the group).

## 8-bit grayscale (7 images)

| codec | bpp | ratio | enc MB/s | dec MB/s | enc peak MB | dec peak MB |
|---|---:|---:|---:|---:|---:|---:|
| ours | 3.893 | 2.055 | 27.1 | 22.6 | 16 | 13 |
| JPEG-LS (CharLS) | 4.001 | 2.000 | 29.4 | 37.9 | 12 | 8 |
| JPEG-XL e3 | 3.897 | 2.053 | 21.7 | 34.9 | 82 | 22 |
| JPEG-XL e7 | 3.837 | 2.085 | 1.0 | 29.4 | 85 | 22 |
| PNG (ImageMagick) | 4.380 | 1.827 | 5.0 | 28.8 | 59 | 32 |
| xz -9 | 4.844 | 1.652 | 2.5 | 32.5 | 92 | 9 |
| zstd -19 | 5.398 | 1.482 | 3.4 | 211.6 | 57 | 6 |

## 16-bit RAW (4 images)

| codec | bpp | ratio | enc MB/s | dec MB/s | enc peak MB | dec peak MB |
|---|---:|---:|---:|---:|---:|---:|
| ours | 7.703 | 2.077 | 55.8 | 49.0 | 43 | 35 |
| JPEG-LS (CharLS) | 7.870 | 2.033 | 45.2 | 50.6 | 46 | 29 |
| JPEG-XL e3 | 7.844 | 2.040 | 41.5 | 66.3 | 184 | 61 |
| JPEG-XL e7 | 7.832 | 2.043 | 2.6 | 50.0 | 191 | 61 |
| PNG (ImageMagick) | 9.576 | 1.671 | 5.6 | 80.4 | 149 | 57 |
| xz -9 | 8.409 | 1.903 | 1.7 | 40.4 | 244 | 45 |
| zstd -19 | 8.969 | 1.784 | 2.0 | 335.8 | 110 | 12 |

## colour (12, incl. arial) (12 images)

| codec | bpp | ratio | enc MB/s | dec MB/s | enc peak MB | dec peak MB |
|---|---:|---:|---:|---:|---:|---:|
| ours | 11.802 | 2.034 | 13.1 | 12.3 | 23 | 17 |
| JPEG-LS (CharLS) | 13.933 | 1.723 | 26.3 | 32.9 | 11 | 8 |
| JPEG-XL e3 | 12.577 | 1.908 | 18.3 | 29.6 | 58 | 28 |
| JPEG-XL e7 | 12.007 | 1.999 | 1.0 | 22.3 | 100 | 26 |
| PNG (ImageMagick) | 13.876 | 1.730 | 8.2 | 48.9 | 30 | 22 |
| xz -9 | 16.100 | 1.491 | 3.0 | 30.3 | 89 | 8 |
| zstd -19 | 17.779 | 1.350 | 4.8 | 224.9 | 56 | 6 |

## palette: Kodak (23 images)

| codec | bpp | ratio | enc MB/s | dec MB/s | enc peak MB | dec peak MB |
|---|---:|---:|---:|---:|---:|---:|
| ours | 3.662 | 6.555 | 35.3 | 28.9 | 6 | 6 |
| JPEG-LS (CharLS) | 12.848 | 1.868 | 32.0 | 40.1 | 8 | 6 |
| JPEG-XL e3 | 4.949 | 4.849 | 14.1 | 66.9 | 27 | 22 |
| JPEG-XL e7 | 4.303 | 5.577 | 2.5 | 51.2 | 45 | 22 |
| PNG (ImageMagick) | 4.632 | 5.182 | 17.8 | 68.0 | 29 | 19 |
| xz -9 | 4.748 | 5.055 | 2.3 | 90.6 | 75 | 5 |
| zstd -19 | 4.918 | 4.880 | 2.7 | 311.2 | 38 | 4 |

## All images

| codec | ratio (original bytes / compressed bytes) | enc MB/s | dec MB/s |
|---|---:|---:|---:|
| ours | 2.738 | 31.3 | 27.4 |
| JPEG-LS (CharLS) | 1.917 | 34.9 | 42.2 |
| JPEG-XL e3 | 2.544 | 21.2 | 53.1 |
| JPEG-XL e7 | 2.639 | 1.9 | 40.6 |
| PNG (ImageMagick) | 2.242 | 7.8 | 62.3 |
| xz -9 | 2.296 | 2.1 | 46.5 |
| zstd -19 | 2.136 | 2.6 | 293.3 |
