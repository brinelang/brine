/* Mandelbrot in C, the same algorithm as mandelbrot.br. */
#include <stdio.h>

int main(void) {
    int n = 4000;
    long long count = 0;
    for (int py = 0; py < n; py++) {
        double ci = 2.0 * (double)py / (double)n - 1.0;
        for (int px = 0; px < n; px++) {
            double cr = 2.0 * (double)px / (double)n - 1.5;
            double zr = 0.0, zi = 0.0;
            int inside = 1;
            for (int i = 0; i < 50; i++) {
                double tr = zr * zr - zi * zi + cr;
                zi = 2.0 * zr * zi + ci;
                zr = tr;
                if (zr * zr + zi * zi > 4.0) {
                    inside = 0;
                    break;
                }
            }
            if (inside) count++;
        }
    }
    printf("%lld\n", count);
    return 0;
}
