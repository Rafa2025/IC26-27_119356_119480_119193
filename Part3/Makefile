# Build the lossy codec.
#
#   make           optimised build: wav_lossy_enc and wav_lossy_dec
#   make test      build, then run the C unit tests and the end-to-end tests (needs python3)
#   make asan      encoder, decoder and C tests with address/undefined-behaviour
#                  sanitizers; runs the C tests and the end-to-end tests with them
#   make speed     time FFT-based vs direct transforms (experiment 7)
#   make variants  one encoder/decoder pair per CTX_MODE x PROB_SHIFT in build/ctx<M>_shift<S>/
#   make clean
#
# wavfile.c is shared with Part 2 and compiled from there.

CC      ?= gcc
CFLAGS  ?= -O2
CFLAGS  += -std=gnu11 -Wall -Wextra -I../Part2
LDLIBS  = -lm

WAV  = ../Part2/wavfile.c
SRC  = codec_lossy.c transform.c quant.c ratectl.c coefcoder.c rangecoder.c $(WAV)
HDR  = codec_lossy.h transform.h quant.h ratectl.h coefcoder.h rangecoder.h ../Part2/wavfile.h

TESTS = test_rangecoder test_transform test_coefcoder

all: wav_lossy_enc wav_lossy_dec

wav_lossy_enc: enc_main.c $(SRC) $(HDR)
	$(CC) $(CFLAGS) -o $@ enc_main.c $(SRC) $(LDLIBS)

wav_lossy_dec: dec_main.c $(SRC) $(HDR)
	$(CC) $(CFLAGS) -o $@ dec_main.c $(SRC) $(LDLIBS)

test_rangecoder: test_rangecoder.c rangecoder.c rangecoder.h
	$(CC) $(CFLAGS) -o $@ test_rangecoder.c rangecoder.c $(LDLIBS)

test_transform: test_transform.c transform.c transform.h
	$(CC) $(CFLAGS) -o $@ test_transform.c transform.c $(LDLIBS)

test_coefcoder: test_coefcoder.c coefcoder.c rangecoder.c quant.c coefcoder.h rangecoder.h quant.h
	$(CC) $(CFLAGS) -o $@ test_coefcoder.c coefcoder.c rangecoder.c quant.c $(LDLIBS)

test: all $(TESTS)
	./test_rangecoder
	./test_transform
	./test_coefcoder
	python3 test_lossy.py

speed: test_transform
	./test_transform --speed

ASAN = -std=gnu11 -O1 -g -Wall -Wextra -I../Part2 -fsanitize=address,undefined -fno-omit-frame-pointer

asan: enc_main.c dec_main.c test_rangecoder.c test_transform.c test_coefcoder.c $(SRC) $(HDR)
	$(CC) $(ASAN) -o wav_lossy_enc_asan enc_main.c $(SRC) $(LDLIBS)
	$(CC) $(ASAN) -o wav_lossy_dec_asan dec_main.c $(SRC) $(LDLIBS)
	$(CC) $(ASAN) -o test_rangecoder_asan test_rangecoder.c rangecoder.c $(LDLIBS)
	$(CC) $(ASAN) -o test_transform_asan test_transform.c transform.c $(LDLIBS)
	$(CC) $(ASAN) -o test_coefcoder_asan test_coefcoder.c coefcoder.c rangecoder.c quant.c $(LDLIBS)
	./test_rangecoder_asan
	./test_transform_asan
	./test_coefcoder_asan
	python3 test_lossy.py --enc ./wav_lossy_enc_asan --dec ./wav_lossy_dec_asan --quick

# Compile-time variants for experiments 4 and 5 (see "Build variants" in CODEC_LOSSY.md).
CTX_MODES   = 0 1 2 3
PROB_SHIFTS = 4 5 6

variants: enc_main.c dec_main.c $(SRC) $(HDR)
	@for m in $(CTX_MODES); do for s in $(PROB_SHIFTS); do \
	    d=build/ctx$${m}_shift$${s}; mkdir -p $$d; \
	    echo "$$d"; \
	    $(CC) $(CFLAGS) -DCTX_MODE=$$m -DPROB_SHIFT=$$s -o $$d/wav_lossy_enc enc_main.c $(SRC) $(LDLIBS) || exit 1; \
	    $(CC) $(CFLAGS) -DCTX_MODE=$$m -DPROB_SHIFT=$$s -o $$d/wav_lossy_dec dec_main.c $(SRC) $(LDLIBS) || exit 1; \
	done; done

clean:
	rm -f wav_lossy_enc wav_lossy_dec wav_lossy_enc_asan wav_lossy_dec_asan
	rm -f $(TESTS) $(addsuffix _asan,$(TESTS))
	rm -rf build

.PHONY: all test speed asan variants clean
