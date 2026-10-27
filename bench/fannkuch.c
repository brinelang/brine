/* The fannkuch-redux benchmark in C, the same algorithm as fannkuch.br. */
#include <stdio.h>

int main(void) {
    enum { n = 11 };
    long long perm1[n], perm[n], count[n];
    for (int i = 0; i < n; i++) perm1[i] = i;
    long long max_flips = 0, checksum = 0, perm_count = 0;
    int r = n, done = 0;
    while (!done) {
        while (r != 1) {
            count[r - 1] = r;
            r--;
        }
        for (int i = 0; i < n; i++) perm[i] = perm1[i];
        long long flips = 0;
        long long k = perm[0];
        while (k != 0) {
            long long i = 0, j = k;
            while (i < j) {
                long long t = perm[i];
                perm[i] = perm[j];
                perm[j] = t;
                i++;
                j--;
            }
            flips++;
            k = perm[0];
        }
        if (flips > max_flips) max_flips = flips;
        checksum += perm_count % 2 == 0 ? flips : -flips;
        int more = 1;
        while (more) {
            if (r == n) {
                done = 1;
                more = 0;
            } else {
                long long perm0 = perm1[0];
                for (int i = 0; i < r; i++) perm1[i] = perm1[i + 1];
                perm1[r] = perm0;
                count[r]--;
                if (count[r] > 0) more = 0;
                else r++;
            }
        }
        perm_count++;
    }
    printf("%lld\nPfannkuchen(%d) = %lld\n", checksum, n, max_flips);
    return 0;
}
