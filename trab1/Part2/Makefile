# Build the lossless codec.
#
#   make          optimised build: wav_lossless_enc and wav_lossless_dec
#   make test     build, then run the round-trip tests (needs python3)
#   make bench    build, then compare with flac/zstd/xz/... (WAVS=dir or files)
#   make asan     same tools built with address/undefined-behaviour sanitizers
#   make clean

CC      ?= gcc
CFLAGS  ?= -O2
CFLAGS  += -std=gnu11 -Wall -Wextra

SRC  = wavfile.c stereo.c predictor.c rice.c codec.c
HDR  = bitio.h wavfile.h stereo.h predictor.h rice.h codec.h

all: wav_lossless_enc wav_lossless_dec memrun

wav_lossless_enc: enc_main.c $(SRC) $(HDR)
	$(CC) $(CFLAGS) -o $@ enc_main.c $(SRC)

wav_lossless_dec: dec_main.c $(SRC) $(HDR)
	$(CC) $(CFLAGS) -o $@ dec_main.c $(SRC)

memrun: memrun.c
	$(CC) $(CFLAGS) -o $@ memrun.c

ASAN = -std=gnu11 -O1 -g -Wall -Wextra -fsanitize=address,undefined -fno-omit-frame-pointer

asan: enc_main.c dec_main.c $(SRC) $(HDR)
	$(CC) $(ASAN) -o wav_lossless_enc_asan enc_main.c $(SRC)
	$(CC) $(ASAN) -o wav_lossless_dec_asan dec_main.c $(SRC)

test_bitio: test_bitio.c bitio.h
	$(CC) $(CFLAGS) -o $@ test_bitio.c
	./test_bitio

test: all
	python3 test_roundtrip.py

bench: all
	@test -n "$(WAVS)" || { echo 'usage: make bench WAVS="dir_or_files"' >&2; exit 2; }
	python3 benchmark.py $(WAVS)

clean:
	rm -f wav_lossless_enc wav_lossless_dec memrun wav_lossless_enc_asan wav_lossless_dec_asan test_bitio

.PHONY: all asan test test_bitio bench clean
