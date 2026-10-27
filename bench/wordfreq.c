/* Word frequencies in C, the same algorithm as wordfreq.br: an open-addressing
 * hash table of malloc'd strings. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    char *key;
    long long count;
} Slot;

static Slot *slots;
static long long cap = 1024, used = 0;

static uint64_t hash(const char *s) {
    uint64_t h = 1469598103934665603ULL;
    for (; *s; s++) h = (h ^ (unsigned char)*s) * 1099511628211ULL;
    return h;
}

static void grow(void) {
    Slot *old = slots;
    long long oc = cap;
    cap *= 2;
    slots = calloc(cap, sizeof(Slot));
    for (long long i = 0; i < oc; i++)
        if (old[i].key) {
            long long j = hash(old[i].key) & (cap - 1);
            while (slots[j].key) j = (j + 1) & (cap - 1);
            slots[j] = old[i];
        }
    free(old);
}

static void add(const char *w) {
    long long j = hash(w) & (cap - 1);
    while (slots[j].key) {
        if (strcmp(slots[j].key, w) == 0) {
            slots[j].count++;
            return;
        }
        j = (j + 1) & (cap - 1);
    }
    slots[j].key = strdup(w);
    slots[j].count = 1;
    if (++used * 2 > cap) grow();
}

int main(void) {
    const char *letters = "abcdefgh";
    slots = calloc(cap, sizeof(Slot));
    long long seed = 42;
    char w[16];
    for (long long k = 0; k < 3000000; k++) {
        seed = (seed * 1103515245 + 12345) % 2147483648LL;
        long long n = 3 + seed % 4;
        for (long long i = 0; i < n; i++) {
            seed = (seed * 1103515245 + 12345) % 2147483648LL;
            w[i] = letters[seed / 65536 % 8];
        }
        w[n] = 0;
        add(w);
    }
    long long squares = 0, top = 0;
    for (long long i = 0; i < cap; i++)
        if (slots[i].key) {
            long long c = slots[i].count;
            squares += c * c;
            if (c > top) top = c;
        }
    printf("%lld %lld %lld\n", used, squares, top);
    return 0;
}
