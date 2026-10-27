/* The n-body benchmark in C, the same algorithm as nbody.br. */
#include <math.h>
#include <stdio.h>

typedef struct {
    double x, y, z, vx, vy, vz, mass;
} Body;

#define N 5

static double energy(const Body *bs) {
    double e = 0.0;
    for (int i = 0; i < N; i++) {
        const Body *b = &bs[i];
        e += 0.5 * b->mass * (b->vx * b->vx + b->vy * b->vy + b->vz * b->vz);
        for (int j = i + 1; j < N; j++) {
            double dx = b->x - bs[j].x, dy = b->y - bs[j].y, dz = b->z - bs[j].z;
            e -= b->mass * bs[j].mass / sqrt(dx * dx + dy * dy + dz * dz);
        }
    }
    return e;
}

static void advance(Body *bs, double dt) {
    for (int i = 0; i < N; i++) {
        for (int j = i + 1; j < N; j++) {
            double dx = bs[i].x - bs[j].x, dy = bs[i].y - bs[j].y, dz = bs[i].z - bs[j].z;
            double d2 = dx * dx + dy * dy + dz * dz;
            double mag = dt / (d2 * sqrt(d2));
            double mi = bs[i].mass * mag, mj = bs[j].mass * mag;
            bs[i].vx -= dx * mj;
            bs[i].vy -= dy * mj;
            bs[i].vz -= dz * mj;
            bs[j].vx += dx * mi;
            bs[j].vy += dy * mi;
            bs[j].vz += dz * mi;
        }
    }
    for (int i = 0; i < N; i++) {
        bs[i].x += dt * bs[i].vx;
        bs[i].y += dt * bs[i].vy;
        bs[i].z += dt * bs[i].vz;
    }
}

int main(void) {
    const double pi = 3.141592653589793, sm = 4.0 * pi * pi, days = 365.24;
    Body bs[N] = {
        {0, 0, 0, 0, 0, 0, sm},
        {4.84143144246472090e+00, -1.16032004402742839e+00, -1.03622044471123109e-01,
         1.66007664274403694e-03 * days, 7.69901118419740425e-03 * days,
         -6.90460016972063023e-05 * days, 9.54791938424326609e-04 * sm},
        {8.34336671824457987e+00, 4.12479856412430479e+00, -4.03523417114321381e-01,
         -2.76742510726862411e-03 * days, 4.99852801234917238e-03 * days,
         2.30417297573763929e-05 * days, 2.85885980666130812e-04 * sm},
        {1.28943695621391310e+01, -1.51111514016986312e+01, -2.23307578892655734e-01,
         2.96460137564761618e-03 * days, 2.37847173959480950e-03 * days,
         -2.96589568540237556e-05 * days, 4.36624404335156298e-05 * sm},
        {1.53796971148509165e+01, -2.59193146099879641e+01, 1.79258772950371181e-01,
         2.68067772490389322e-03 * days, 1.62824170038242295e-03 * days,
         -9.51592254519715870e-05 * days, 5.15138902046611451e-05 * sm},
    };
    double px = 0, py = 0, pz = 0;
    for (int i = 0; i < N; i++) {
        px += bs[i].vx * bs[i].mass;
        py += bs[i].vy * bs[i].mass;
        pz += bs[i].vz * bs[i].mass;
    }
    bs[0].vx = -px / sm;
    bs[0].vy = -py / sm;
    bs[0].vz = -pz / sm;
    printf("%.9f\n", energy(bs));
    for (int k = 0; k < 20000000; k++) advance(bs, 0.01);
    printf("%.9f\n", energy(bs));
    return 0;
}
