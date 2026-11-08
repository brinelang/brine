/* A byte tokenizer over 64 MB of log lines in C, the same as scan.br. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static long long find_byte(const unsigned char *b, long long lo, long long hi, unsigned char c) {
    if (lo >= hi) return hi;
    for (long long i = lo; i < hi; i++)
        if (b[i] == c) return i;
    return hi;
}

int main(void) {
    const char *words[7] = {"GET", "/api/v1/users/12345", "HTTP/1.1", "200", "Mozilla/5.0", "-", "0.042"};
    long long cap = 64000000 + 256, len = 0;
    unsigned char *text = malloc(cap);
    long long k = 0;
    while (len < 64000000) {
        for (int j = 0; j < 7; j++) {
            const char *w = words[(j + k) % 7];
            size_t n = strlen(w);
            memcpy(text + len, w, n);
            len += (long long)n;
            text[len++] = 32;
        }
        text[len++] = 10;
        k++;
    }
    long long sum = 0;
    for (int r = 0; r < 4; r++) {
        long long lo = 0;
        while (lo < len) {
            long long nl = find_byte(text, lo, len, 10);
            long long i = lo, n = 0;
            while (i < nl) {
                long long sp = find_byte(text, i, nl, 32);
                n++;
                sum += sp - i;
                i = sp + 1;
            }
            sum += n;
            lo = nl + 1;
        }
    }
    printf("%lld %lld\n", len, sum);
    return 0;
}
