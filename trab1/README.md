# Information and Coding 2026/27 — Lab work 1

Audio tools, a lossless audio codec and a lossy (DCT-based) audio codec.

| Part | Folder | Contents | Language |
|---|---|---|---|
| I | [`Part1/`](Part1) | `wav_hist`, `wav_quant`, `wav_cmp`, `wav_effects` | Python 3 |
| II | [`Part2/`](Part2) | lossless codec `wav_lossless_enc` / `wav_lossless_dec` + benchmark | C (benchmark in Python) |
| III | [`Part3/`](Part3) | lossy codec `wav_lossy_enc` / `wav_lossy_dec` + benchmark | C (tools in Python and Bash) |

Everything was developed and tested on Linux (Ubuntu). The codecs are written in C
because running time and memory are part of the assessment; the small tools are in
Python, where speed does not matter.

## Requirements

| What | Needed for | Ubuntu / Debian |
|---|---|---|
| `gcc`, `make` | building the codecs (Parts II and III) | `sudo apt install build-essential` |
| Python 3.8+ | Part I, all tests | preinstalled |
| `numpy`, `matplotlib` | Part I only | `sudo apt install python3-numpy python3-matplotlib` |
| `ffmpeg` (with libmp3lame) | Part III benchmark only (MP3/AAC) | `sudo apt install ffmpeg` |
| `flac`, `zstd`, `xz`, `bzip2`, `gzip`, `brotli`, `lz4` | Part II benchmark only | `sudo apt install flac zstd xz-utils bzip2 gzip brotli lz4` |
| GNU `time` (`/usr/bin/time`) | Part III benchmark only | `sudo apt install time` |

Part III's own tools (`test_lossy.py`, `snr_tool.py`) use only the Python standard library.

If you prefer not to install the Python packages system-wide:

```bash
python3 -m venv .venv && . .venv/bin/activate && pip install numpy matplotlib
```

## Installation

```bash
git clone git@github.com:Rafa2025/IC26-27_119356_119480_119193.git
cd IC26-27_119356_119480_119193
make -C Part2          # wav_lossless_enc, wav_lossless_dec
make -C Part3          # wav_lossy_enc, wav_lossy_dec
```

The programs are built inside their folder (`Part2/wav_lossless_enc`, ...). Part III
compiles `Part2/wavfile.c`, so keep the folders side by side.

## Part I — audio tools

```bash
cd Part1
python3 wav_hist.py input.wav                       # histograms of L, R, MID, SIDE -> hist_out/
python3 wav_hist.py input.wav --k 0 2 4 --outdir plots   # also with bins of 2^k values
python3 wav_quant.py input.wav out.wav --bits 8     # keep 8 bits per sample
python3 wav_quant.py input.wav out.wav --levels 1000
python3 wav_cmp.py original.wav processed.wav       # MSE, max abs error, SNR per channel and average
python3 wav_effects.py input.wav out.wav echo --delay 0.3 --gain 0.5
python3 wav_effects.py input.wav out.wav multiecho --taps 0.2:0.6 0.4:0.3 0.6:0.15
python3 wav_effects.py input.wav out.wav am --depth 0.5 --freq 5
python3 wav_effects.py input.wav out.wav vdelay --base 0.02 --depth 0.01 --lfo 2
```

## Part II — lossless codec

```bash
cd Part2
./wav_lossless_enc [-b block_size] [-v] input.wav compressed_file
./wav_lossless_dec compressed_file output.wav       # output.wav is identical to input.wav
```

| Option | Meaning |
|---|---|
| `-b N` | samples per block and channel (default 4096) |
| `-v` | print statistics (sizes, ratio, predictor orders used) |

```bash
make test                          # round-trip tests
make asan                          # sanitizer build (then: SUFFIX=_asan python3 test_roundtrip.py)
make bench WAVS="dir_or_files"     # compare with flac, zstd, xz, bzip2, gzip, brotli, lz4 -> bench_out/
```

The design is described in [`Part2/codec.md`](Part2/codec.md).

## Part III — lossy codec

```bash
cd Part3
./wav_lossy_enc [options] input.wav compressed_file
./wav_lossy_dec compressed_file output.wav
```

The decoder needs no options. The decoded WAV has the same sample rate, channels and
bit depth as the original (16-bit stereo, but also 8/24-bit and mono).

| Option | Values | Default | Meaning |
|---|---|---|---|
| `--snr DB` | real > 0 | 30 | target SNR; the encoder finds the cheapest file that reaches it |
| `--step S` | real > 0 | | fixed quantization step instead (excludes `--snr`) |
| `-b N` | power of two, 64–8192 | 2048 | transform size |
| `--transform` | `dct`, `mdct` | `mdct` | block DCT-II, or MDCT (windowed, 50% overlap) |
| `--stereo` | `lr`, `ms` | `ms` | left/right or mid/side (ignored for mono) |
| `-v` | | | print step, SNR reached, bits per sample and kbps |

Examples:

```bash
./wav_lossy_enc input.wav song.icd                    # 30 dB SNR (default)
./wav_lossy_enc -v --snr 40 input.wav song.icd        # higher quality, prints the rate
./wav_lossy_enc --step 200 --transform dct input.wav song.icd
./wav_lossy_dec song.icd decoded.wav
python3 snr_tool.py snr input.wav decoded.wav         # measure the SNR independently
```

Tests and benchmarks:

```bash
make test           # C unit tests + end-to-end tests (test_lossy.py)
make asan           # the same with address/undefined-behaviour sanitizers
make bench WAVS="dir_or_files"         # vs MP3 and AAC (needs ffmpeg) -> bench_out/compare.*
make experiments WAVS="dir_or_files"   # the design experiments -> bench_out/exp*.*
make speed          # FFT-based vs direct transforms
```

`bench_lossy.sh` (behind `make bench` / `make experiments`) writes, for each mode, a CSV
with one row per run (size, compression ratio, kbps, SNR, times, peak memory), a
Markdown table (bitrate and compression ratio at equal SNR, averaged over the files)
and an SVG plot of the rate-distortion curves. Run `./bench_lossy.sh -h` for all modes
and options.

`snr_tool.py diff original.wav decoded.wav diff.wav --gain 20` writes the difference
signal, to listen to what the codec removed.

The design is described in [`Part3/CODEC_LOSSY.md`](Part3/CODEC_LOSSY.md).

## Test data

The WAV files are not in the repository (`*.wav` is ignored). Put the course datasets
anywhere and pass their folder, for example:

```bash
make -C Part3 bench WAVS="../Audio_Datasets/DS100 ../Audio_Datasets/TECH3253"
```
