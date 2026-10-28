/* The `main` of an x86-64 release build. The program is compiled three times,
 * for plain x86-64, x86-64-v3 (AVX2, FMA, BMI) and x86-64-v4 (AVX-512); this
 * picks the newest one the CPU and the OS support. BRINE_CPU=base or v3 caps
 * the choice (to test the other versions on a new machine).
 *
 * Floating-point results are the same in all three: C11 mode keeps gcc and
 * clang from fusing multiplies and adds (-ffp-contract=off). */
#include <cpuid.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

int brine_main_base(int argc, char **argv);
int brine_main_v3(int argc, char **argv);
int brine_main_v4(int argc, char **argv);

/* the register state the OS saves on a thread switch (XCR0) */
static uint64_t brine_xcr0(void) {
    uint32_t a, d;
    __asm__ volatile("xgetbv" : "=a"(a), "=d"(d) : "c"(0));
    return ((uint64_t)d << 32) | a;
}

static int brine_cpu_level(void) {
    int cap = 4;
    const char *e = getenv("BRINE_CPU");
    if (e && strcmp(e, "base") == 0) cap = 1;
    if (e && strcmp(e, "v3") == 0) cap = 3;
    unsigned a, b, c, d;
    if (cap < 3 || !__get_cpuid(1, &a, &b, &c, &d)) return 1;
    /* SSE3, SSSE3, FMA, CMPXCHG16B, SSE4.1, SSE4.2, MOVBE, POPCNT, OSXSAVE, AVX, F16C */
    const unsigned l1 = 1u << 0 | 1u << 9 | 1u << 12 | 1u << 13 | 1u << 19 | 1u << 20 | 1u << 22 |
                        1u << 23 | 1u << 27 | 1u << 28 | 1u << 29;
    if ((c & l1) != l1) return 1;
    /* the OS saves the SSE and AVX registers */
    if ((brine_xcr0() & 6) != 6) return 1;
    /* LAHF in 64-bit mode, LZCNT */
    if (!__get_cpuid(0x80000001, &a, &b, &c, &d) || (c & 0x21) != 0x21) return 1;
    /* BMI1, AVX2, BMI2 */
    if (!__get_cpuid_count(7, 0, &a, &b, &c, &d)) return 1;
    const unsigned l7 = 1u << 3 | 1u << 5 | 1u << 8;
    if ((b & l7) != l7) return 1;
    /* AVX-512 F, DQ, CD, BW, VL, and the OS saves their registers */
    const unsigned v4 = 1u << 16 | 1u << 17 | 1u << 28 | 1u << 30 | 1u << 31;
    if (cap >= 4 && (b & v4) == v4 && (brine_xcr0() & 0xe6) == 0xe6) return 4;
    return 3;
}

int main(int argc, char **argv) {
    switch (brine_cpu_level()) {
    case 4:
        return brine_main_v4(argc, argv);
    case 3:
        return brine_main_v3(argc, argv);
    default:
        return brine_main_base(argc, argv);
    }
}
