# Lossless image codec (Part 2)
#
#   make            build img_lossless_enc and img_lossless_dec
#   make test       unit tests + round-trip tests on all images (needs ../../ImageFiles)
#   make san        build with address + undefined-behaviour sanitizers (san/ folder)
#   make test-san   the same tests with the sanitizer build (needs libasan + libubsan)
#   make clean
#
# Generic: every .c file except *_main.c and test_*.c is part of the codec, and
# every test_*.c is a unit-test program, so new files need no change here.

CC      ?= gcc
CFLAGS  ?= -O2 -Wall -Wextra -std=c11
OMP     ?= -fopenmp
LDLIBS  = -lm
SANFLAGS = -O1 -g -Wall -Wextra -std=c11 -fsanitize=address,undefined -fno-omit-frame-pointer

SRC     = $(filter-out %_main.c test_%.c,$(wildcard *.c))
HDR     = $(wildcard *.h)
UNITS   = $(patsubst %.c,%,$(wildcard test_*.c))
BIN     = img_lossless_enc img_lossless_dec

all: $(BIN)

img_lossless_%: %_main.c $(SRC) $(HDR)
	$(CC) $(CFLAGS) $(OMP) -o $@ $< $(SRC) $(LDLIBS)

test_%: test_%.c $(SRC) $(HDR)
	$(CC) $(CFLAGS) $(OMP) -o $@ $< $(SRC) $(LDLIBS)

san: san/img_lossless_enc san/img_lossless_dec $(addprefix san/,$(UNITS))

san/img_lossless_%: %_main.c $(SRC) $(HDR)
	@mkdir -p san
	$(CC) $(SANFLAGS) $(OMP) -o $@ $< $(SRC) $(LDLIBS)

san/test_%: test_%.c $(SRC) $(HDR)
	@mkdir -p san
	$(CC) $(SANFLAGS) $(OMP) -o $@ $< $(SRC) $(LDLIBS)

test: all $(UNITS)
	@for t in $(UNITS); do echo "== $$t"; ./$$t || exit 1; done
	python3 test_roundtrip.py ../../ImageFiles

test-san: san
	@for t in $(UNITS); do echo "== san/$$t"; ./san/$$t || exit 1; done
	python3 test_roundtrip.py ../../ImageFiles --bin san --quick

clean:
	rm -rf $(BIN) $(UNITS) san

.PHONY: all san test test-san clean