/* The ChaCha20 block function in C, the same as chacha.br. */
#include <stdint.h>
#include <stdio.h>

static inline uint32_t rotl(uint32_t x, int n) { return (x << n) | (x >> (32 - n)); }

static inline void quarter(uint32_t *x, int a, int b, int c, int d) {
    x[a] += x[b];
    x[d] = rotl(x[d] ^ x[a], 16);
    x[c] += x[d];
    x[b] = rotl(x[b] ^ x[c], 12);
    x[a] += x[b];
    x[d] = rotl(x[d] ^ x[a], 8);
    x[c] += x[d];
    x[b] = rotl(x[b] ^ x[c], 7);
}

static void block(uint32_t *out, const uint32_t *st) {
    for (int i = 0; i < 16; i++) out[i] = st[i];
    for (int r = 0; r < 10; r++) {
        quarter(out, 0, 4, 8, 12);
        quarter(out, 1, 5, 9, 13);
        quarter(out, 2, 6, 10, 14);
        quarter(out, 3, 7, 11, 15);
        quarter(out, 0, 5, 10, 15);
        quarter(out, 1, 6, 11, 12);
        quarter(out, 2, 7, 8, 13);
        quarter(out, 3, 4, 9, 14);
    }
    for (int i = 0; i < 16; i++) out[i] += st[i];
}

int main(void) {
    uint32_t st[16], out[16];
    for (int i = 0; i < 16; i++) st[i] = (uint32_t)(i * 0x01010101 + 7);
    uint32_t acc = 0;
    for (int k = 0; k < 2000000; k++) {
        st[12] = (uint32_t)k;
        block(out, st);
        for (int i = 0; i < 16; i++) acc = rotl(acc ^ out[i], 1);
    }
    printf("%u\n", acc);
    return 0;
}
