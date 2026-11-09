/* A bytecode interpreter's dispatch loop in C, the same program as dispatch.br. */
#include <stdint.h>
#include <stdio.h>

static uint64_t rotl(uint64_t x, int r) { return (x << r) | (x >> (64 - r)); }

int main(void) {
    enum { n = 256 };
    static long long code[n];
    uint64_t s = 12345;
    for (int i = 0; i < n; i++) {
        s = s * 6364136223846793005u + 1442695040888963407u;
        code[i] = (long long)(s >> 33) % 40;
    }
    uint64_t a = 1, b = 2;
    long long pc = 0;
    for (long long t = 0; t < 300000000; t++) {
        switch (code[pc]) {
        case 0: a += b ^ 3652509721u; break;
        case 1: b = rotl(b, 8) ^ a; break;
        case 2: a *= 3628718147u; break;
        case 3: b -= a >> 22; break;
        case 4: a ^= (b << 29) ^ 4u; break;
        case 5: a += b ^ 1445547138u; break;
        case 6: b = rotl(b, 43) ^ a; break;
        case 7: a *= 1421755565u; break;
        case 8: b -= a >> 57; break;
        case 9: a ^= (b << 3) ^ 9u; break;
        case 10: a += b ^ 3533551851u; break;
        case 11: b = rotl(b, 17) ^ a; break;
        case 12: a *= 3509760277u; break;
        case 13: b -= a >> 31; break;
        case 14: a ^= (b << 6) ^ 14u; break;
        case 15: a += b ^ 1326589268u; break;
        case 16: b = rotl(b, 52) ^ a; break;
        case 17: a *= 1302797695u; break;
        case 18: b -= a >> 5; break;
        case 19: a ^= (b << 12) ^ 19u; break;
        case 20: a += b ^ 3414593981u; break;
        case 21: b = rotl(b, 26) ^ a; break;
        case 22: a *= 3390802407u; break;
        case 23: b -= a >> 40; break;
        case 24: a ^= (b << 15) ^ 24u; break;
        case 25: a += b ^ 1207631398u; break;
        case 26: b = rotl(b, 61) ^ a; break;
        case 27: a *= 1183839825u; break;
        case 28: b -= a >> 14; break;
        case 29: a ^= (b << 21) ^ 29u; break;
        case 30: a += b ^ 3295636111u; break;
        case 31: b = rotl(b, 35) ^ a; break;
        case 32: a *= 3271844537u; break;
        case 33: b -= a >> 49; break;
        case 34: a ^= (b << 24) ^ 34u; break;
        case 35: a += b ^ 1088673528u; break;
        case 36: b = rotl(b, 9) ^ a; break;
        case 37: a *= 1064881955u; break;
        case 38: b -= a >> 23; break;
        case 39: a ^= (b << 30) ^ 39u; break;
        default: break;
        }
        pc = (pc + 1) & (n - 1);
    }
    printf("%llu %llu\n", (unsigned long long)a, (unsigned long long)b);
    return 0;
}
