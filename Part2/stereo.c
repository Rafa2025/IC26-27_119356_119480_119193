/* stereo.c - see stereo.h for the maths. */
#include "stereo.h"

void ms_forward(int32_t *a, int32_t *b, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        int32_t l = a[i], r = b[i];
        a[i] = (l + r) >> 1;     /* MID: arithmetic shift = floor(sum / 2), also for negatives */
        b[i] = l - r;            /* SIDE: full difference */
    }
}

void ms_inverse(int32_t *a, int32_t *b, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        int32_t mid = a[i], side = b[i];
        /* Rebuild the exact sum L+R. We use *2 and + instead of << and | so
           that negative values are handled without undefined behaviour:
           (side & 1) is the parity of SIDE in two's complement, also for negatives. */
        int32_t sum = mid * 2 + (side & 1);
        a[i] = (sum + side) >> 1;   /* L = (L+R + L-R) / 2 */
        b[i] = (sum - side) >> 1;   /* R = (L+R - (L-R)) / 2 */
    }
}
