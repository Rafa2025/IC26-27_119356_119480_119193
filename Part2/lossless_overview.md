# Lossless codec: results over all files

## 1. The files

| Group | Files | Total size (MB) | Duration (min:s) | Largest file (MB) |
|---|---:|---:|---:|---:|
| course | 7 | 25.3 | 2:23.4 | 5.2 |
| DSD100 | 10 | 52.9 | 5:00.0 | 5.3 |
| SQAM | 10 | 72.1 | 6:49.0 | 13.1 |
| all | 27 | 150.4 | 14:12.4 | 13.1 |

## 2. Totals per codec

Totals over 27 files. Ratio = total original / total compressed bytes. Times are sums of per-file medians, speed = total MB / total time, peak memory is the maximum over files.

| Codec | Files | Ratio | Bits/sample | Enc MB/s | Dec MB/s | Enc peak MiB | Dec peak MiB | Lossless |
|---|---:|---:|---:|---:|---:|---:|---:|:---:|
| flac -8 | 27 | 2.391 | 6.69 | 48.4 | 161.7 | 3.5 | 3.0 | yes |
| flac -5 | 27 | 2.364 | 6.77 | 100.1 | 165.3 | 3.5 | 3.0 | yes |
| ours | 27 | 2.164 | 7.39 | 136.5 | 154.5 | 38.6 | 26.2 | yes |
| xz -9e | 27 | 1.822 | 8.78 | 5.7 | 31.4 | 178.8 | 14.5 | yes |
| xz -6 | 27 | 1.822 | 8.78 | 5.7 | 31.7 | 95.1 | 10.2 | yes |
| zstd -19 | 27 | 1.646 | 9.72 | 9.9 | 401.1 | 101.2 | 12.5 | yes |
| bzip2 -9 | 27 | 1.428 | 11.20 | 13.0 | 24.5 | 8.5 | 4.8 | yes |
| zstd -3 | 27 | 1.421 | 11.26 | 203.6 | 441.7 | 23.2 | 6.6 | yes |
| gzip -9 | 27 | 1.313 | 12.18 | 24.0 | 133.2 | 2.0 | 1.8 | yes |
| gzip -6 | 27 | 1.313 | 12.18 | 31.8 | 130.9 | 2.0 | 1.8 | yes |

## 3. Compression ratio per group

| Codec | course | DSD100 | SQAM | all |
|---|---:|---:|---:|---:|
| flac -8 | 1.786 | 1.926 | 3.398 | 2.391 |
| flac -5 | 1.776 | 1.909 | 3.336 | 2.364 |
| ours | 1.678 | 1.770 | 2.945 | 2.164 |
| xz -9e | 1.200 | 1.306 | 3.449 | 1.822 |
| xz -6 | 1.199 | 1.307 | 3.448 | 1.822 |
| zstd -19 | 1.118 | 1.189 | 2.981 | 1.646 |
| bzip2 -9 | 1.172 | 1.265 | 1.725 | 1.428 |
| zstd -3 | 1.111 | 1.177 | 1.893 | 1.421 |
| gzip -9 | 1.115 | 1.181 | 1.535 | 1.313 |
| gzip -6 | 1.115 | 1.181 | 1.535 | 1.313 |

## 4. Block size of our codec

| Block size | Ratio (all) | course | DSD100 | SQAM | Enc time (s) | Dec time (s) | Files |
|---:|---:|---:|---:|---:|---:|---:|---:|
| 256 | 2.164 | 1.677 | 1.771 | 2.941 | 1.130 | 1.009 | 27 |
| 512 | 2.164 | 1.678 | 1.770 | 2.945 | 1.102 | 0.973 | 27 |
| 1024 | 2.160 | 1.677 | 1.765 | 2.941 | 1.102 | 0.971 | 27 |
| 2048 | 2.150 | 1.673 | 1.752 | 2.930 | 1.092 | 1.028 | 27 |
| 4096 | 2.129 | 1.665 | 1.729 | 2.904 | 1.060 | 1.018 | 27 |
| 8192 | 2.101 | 1.652 | 1.703 | 2.865 | 1.094 | 1.058 | 27 |

Check: `ours` in section 2 (ratio 2.164) matches block size 256, 512 of the sweep.

## 5. Numbers quoted in the text

- Our files vs `flac -8`: 10.5% larger (ratio 2.164 vs 2.391).
- Our encoder speed vs `flac -8`: 2.8x; decoder: 0.96x.
- Our encoder speed vs `flac -5`: 1.4x; decoder: 0.93x.
- `zstd -19` vs `zstd -3`: 15.9% better ratio, 21x slower to encode.
- Best general-purpose tool: `xz -9e`, ratio 1.822; the audio codecs reach 2.164 (ours) and 2.391 (`flac -8`).
- Peak memory of the general-purpose tools (encoder, MiB): `xz -9e` 179, `zstd -19` 101, `xz -6` 95, `zstd -3` 23.

Peak memory against file size (least-squares line, peak = a + b * size, both in MiB):

| Codec | Enc b | Enc a | Dec b | Dec a | Largest file (MiB) | Enc peak there | Dec peak there |
|---|---:|---:|---:|---:|---:|---:|---:|
| ours | 3.00 | 1.3 | 2.00 | 1.3 | 12.4 | 38.6 | 26.2 |
| flac -8 | 0.00 | 3.4 | -0.00 | 3.0 | 12.4 | 3.5 | 3.0 |
| xz -9e | 9.00 | 66.7 | 1.00 | 2.1 | 12.4 | 178.8 | 14.5 |

## 6. Integrity

- Rows used: 270 (10 codecs, 27 files).
- Round trips not byte-identical: 0.
- Failed runs skipped: 0.
- Duplicate (file, codec) rows, last one kept: 27 -> [('052_mix.wav', 'DSD100', 'ours'), ('055_mix.wav', 'DSD100', 'ours'), ('062_mix.wav', 'DSD100', 'ours'), ('067_mix.wav', 'DSD100', 'ours'), ('073_mix.wav', 'DSD100', 'ours')]
- Codecs missing some files: none.

