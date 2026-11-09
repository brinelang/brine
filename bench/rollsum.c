/* An rsync-style rolling checksum over 64 MB in C, the same as rollsum.br. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

typedef struct { uint32_t a, b; } Roll;

static Roll roll(Roll r, uint8_t out, uint8_t inp, uint32_t w) {
    uint32_t a = r.a - out + inp;
    uint32_t b = r.b - w * out + a;
    return (Roll){a, b};
}

int main(void) {
    long long n = 64000000, w = 64;
    uint8_t *data = malloc(n);
    uint32_t s = 12345;
    for (long long i = 0; i < n; i++) {
        s = s * 1103515245u + 12345u;
        data[i] = (uint8_t)(s >> 24);
    }
    long long cuts = 0;
    uint32_t h = 0;
    for (int k = 0; k < 4; k++) {
        Roll r = {0, 0};
        for (long long i = 0; i < w; i++) r = roll(r, 0, data[i], (uint32_t)w);
        for (long long i = w; i < n; i++) {
            r = roll(r, data[i - w], data[i], (uint32_t)w);
            if (((r.a ^ r.b) & 4095) == 0) {
                cuts++;
                h = ((h << 5) | (h >> 27)) ^ r.b;
            }
        }
    }
    printf("%lld %u\n", cuts, h);
    free(data);
    return 0;
}
