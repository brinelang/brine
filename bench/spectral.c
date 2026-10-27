/* The spectral-norm benchmark in C, the same algorithm as spectral.br. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

static double a(long long i, long long j) {
    long long s = i + j;
    return 1.0 / (double)(s * (s + 1) / 2 + i + 1);
}

static void mul_av(const double *v, double *out, int n) {
    for (int i = 0; i < n; i++) {
        double s = 0.0;
        for (int j = 0; j < n; j++) s += a(i, j) * v[j];
        out[i] = s;
    }
}

static void mul_atv(const double *v, double *out, int n) {
    for (int i = 0; i < n; i++) {
        double s = 0.0;
        for (int j = 0; j < n; j++) s += a(j, i) * v[j];
        out[i] = s;
    }
}

static void mul_atav(const double *v, double *out, double *tmp, int n) {
    mul_av(v, tmp, n);
    mul_atv(tmp, out, n);
}

int main(void) {
    int n = 3000;
    double *u = malloc(n * sizeof(double)), *v = malloc(n * sizeof(double)),
           *tmp = malloc(n * sizeof(double));
    for (int i = 0; i < n; i++) u[i] = 1.0, v[i] = 0.0;
    for (int k = 0; k < 10; k++) {
        mul_atav(u, v, tmp, n);
        mul_atav(v, u, tmp, n);
    }
    double vbv = 0, vv = 0;
    for (int i = 0; i < n; i++) {
        vbv += u[i] * v[i];
        vv += v[i] * v[i];
    }
    printf("%.9f\n", sqrt(vbv / vv));
    return 0;
}
