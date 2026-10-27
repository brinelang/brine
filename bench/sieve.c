/* The sieve of Eratosthenes in C, the same algorithm as sieve.br. */
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

int main(void) {
    long long n = 100000000;
    bool *is_prime = malloc(n + 1);
    for (long long k = 0; k <= n; k++) is_prime[k] = true;
    is_prime[0] = is_prime[1] = false;
    for (long long i = 2; i * i <= n; i++)
        if (is_prime[i])
            for (long long j = i * i; j <= n; j += i) is_prime[j] = false;
    long long count = 0;
    for (long long k = 0; k <= n; k++) count += is_prime[k];
    printf("%lld\n", count);
    return 0;
}
