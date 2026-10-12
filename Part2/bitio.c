/* Round-trip test for bitio.h: random fixed-width fields and unary codes. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "bitio.h"

#define N 2000000

typedef struct { uint8_t kind; uint8_t n; uint32_t v; } Item;

static uint64_t s = 88172645463325252ull;
static uint32_t rnd(void) { s ^= s << 13; s ^= s >> 7; s ^= s << 17; return (uint32_t)(s >> 11); }

int main(void)
{
    Item *items = malloc(sizeof(Item) * N);
    for (int i = 0; i < N; i++) {
        if (rnd() % 3 == 0) {                       /* unary, mostly short, some long */
            items[i].kind = 1;
            items[i].v = (rnd() % 50 == 0) ? rnd() % 300 : rnd() % 12;
        } else {                                    /* fixed width, 0..32 bits */
            items[i].kind = 0;
            items[i].n = rnd() % 33;
            uint32_t v = rnd() ^ (rnd() << 11);
            items[i].v = items[i].n == 32 ? v : (v & ((1u << items[i].n) - 1u));
        }
    }

    FILE *f = fopen("bitio_test.bin", "wb");
    BitWriter *w = malloc(sizeof *w);
    bw_init(w, f);
    for (int i = 0; i < N; i++) {
        if (items[i].kind) bw_put_unary(w, items[i].v);
        else bw_put(w, items[i].v, items[i].n);
    }
    if (bw_flush(w)) { puts("FAIL: write error"); return 1; }
    printf("wrote %llu bytes\n", (unsigned long long)w->total_bytes);
    fclose(f);

    f = fopen("bitio_test.bin", "rb");
    BitReader *r = malloc(sizeof *r);
    br_init(r, f);
    for (int i = 0; i < N; i++) {
        uint32_t got = items[i].kind ? br_get_unary(r) : br_get(r, items[i].n);
        if (r->err || got != items[i].v) {
            printf("FAIL at item %d (kind=%d n=%d expected=%u got=%u err=%d)\n",
                   i, items[i].kind, items[i].n, items[i].v, got, r->err);
            return 1;
        }
    }
    /* reading past the end must flag an error */
    br_get(r, 32); br_get(r, 32);
    if (!r->err) { puts("FAIL: EOF not detected"); return 1; }

    puts("PASS: all items round-tripped, EOF detected");
    fclose(f); remove("bitio_test.bin");
    free(items); free(w); free(r);
    return 0;
}