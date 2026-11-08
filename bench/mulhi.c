/* The high halves of 64 x 64-bit products in C, the same as mulhi.br. */
#include <stdint.h>
#include <stdio.h>

static inline uint64_t mulhi(uint64_t a, uint64_t b) { return (uint64_t)(((unsigned __int128)a * b) >> 64); }

int main(void) {
    uint64_t acc = 0, x = 0x9E3779B97F4A7C15ull;
    for (int64_t i = 0; i < 200000000; i++) {
        x = x * 6364136223846793005ull + 1442695040888963407ull;
        acc ^= mulhi(x, x ^ (uint64_t)i);
        acc += (uint64_t)((unsigned __int128)acc * x);
    }
    printf("%llu\n", (unsigned long long)acc);
    return 0;
}
