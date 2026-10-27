/* Matrix multiply in C, the same algorithm as matmul.br. */
#include <stdio.h>
#include <stdlib.h>

int main(void) {
    long long n = 800;
    double *a = malloc(n * n * sizeof(double)), *b = malloc(n * n * sizeof(double)),
           *c = calloc(n * n, sizeof(double));
    for (long long i = 0; i < n; i++)
        for (long long j = 0; j < n; j++) {
            a[i * n + j] = (double)((i * 7 + j * 3) % 17) / 17.0;
            b[i * n + j] = (double)((i * 5 + j * 11) % 13) / 13.0;
        }
    for (long long i = 0; i < n; i++)
        for (long long k = 0; k < n; k++) {
            double aik = a[i * n + k];
            for (long long j = 0; j < n; j++) c[i * n + j] += aik * b[k * n + j];
        }
    double trace = 0.0;
    for (long long i = 0; i < n; i++) trace += c[i * n + i];
    printf("%.6f\n", trace);
    return 0;
}
