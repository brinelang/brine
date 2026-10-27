# Benchmarks: par on the CPU and on a GPU

`sh bench/gpu/run.sh [path/to/brine]` builds each program with `--release` and runs
it three times: `BRINE_GPU=off` (the thread pool), `on` (the GPU whenever it
can) and `auto` (the default: the runtime picks). All three must print the same
checksum. The times are those of the construct alone, in milliseconds. *Cold* is
the first run in the process; for the GPU that includes getting the device
ready and compiling the PTX (0.1-0.15 s on Windows, about 0.4 s on Linux).
*Warm* is the best of the later runs.

| Program | What | Bytes copied |
|---|---|---|
| `vecmap.br` | `par [poly(x) for x in xs]`, 16M floats, 16 multiply-adds and divisions each | 268 MB |
| `nbody.br` | accelerations of 16384 bodies, all pairs (268M interactions), f64 and f32 | 0.5-0.9 MB |
| `mandelbrot.br` | 4096 x 4096 pixels, up to 512 iterations, f64 | 67 MB out |
| `sum.br` | `par sum` of 64M floats, of 64M f32s, and of a computed series of 256M terms | 537 / 268 / 0 MB |

## Results

Windows 11, Ryzen 9 9950X3D (16 cores, 32 threads), RTX 4090, brine 0.3.0 plus the GPU
work. The GPU was shared with a running game, so the GPU times varied by up to
1.5x between runs; the table gives typical values.

| Benchmark | CPU warm | GPU cold | GPU warm | auto cold | auto warm | auto picks |
|---|---:|---:|---:|---:|---:|---|
| vecmap f64 | 6.7 | 122-146 | 29-36 | 7.5 | 6.8 | CPU |
| nbody f64 | 41-44 | 139-147 | 37-50 | 43-47 | 42-45 | CPU |
| nbody f32 | 35-40 | 8-10 * | 4.5 | 6-11 * | 4.5-7.5 | GPU |
| mandelbrot f64 | 107-117 | 149-170 | 46-70 | 107-138 | 48-72 | CPU, then GPU |
| sum of [float] | 7.5-8.4 | 154-171 | 43-47 | 9 | 8 | CPU |
| sum of [f32] | 3.9 | 23-28 * | 21-26 | 4.2 | 4.1 | CPU |
| sum of a series | 14-16 | 6.4-12 * | 5.8-10 | 15 | 15 | CPU (see below) |

\* the device was already ready from an earlier construct in the same program.

On Linux (WSL2, same machine, one run), warm CPU / warm GPU: vecmap 7.1 / 47,
nbody f64 44 / 57, nbody f32 31 / 7.8, mandelbrot 164 / 81, sums 8.8 / 47,
4.8 / 23 and 15.7 / 13.4. Cold GPU runs take 0.4 s, and `auto` stays on the CPU
in these short programs, because the device isn't ready before they end.

What the numbers say:
* **Work per byte decides.** The f32 n-body step moves 0.5 MB and does 268M
  interactions: 8x faster on the GPU. A sum or a light map of a big array is
  bound by the copy over PCIe (8-10 GB/s from pageable memory) while the CPU
  reads memory at 60+ GB/s, so it stays on the CPU; `auto` gets that right.
* **f64 is slow on GeForce cards** (1/64 of the f32 rate). Mandelbrot in f64
  still gains 1.6-2.3x; the n-body step in f64, with a division and a square
  root per interaction, runs at about the CPU's speed on the GPU, so `auto` keeps
  it on the CPU.
* **Cold starts.** `auto` never waits for the device unless the gain covers the
  wait. A construct that would gain starts the device in the background and runs
  on the CPU meanwhile; later runs use the GPU (mandelbrot from its third run).
  The series sum would gain 2x, but this program ends before the device is ready.
* **Exact rounding costs a little.** Kernels use IEEE `div.rn` and `sqrt.rn` and
  no fused multiply-adds, so that the results are the CPU's bits.

## Apple silicon (Metal)

Mac mini, M1 (4 performance and 4 efficiency cores, 8-core GPU), macOS 15. Apple
GPUs have no f64, so only the f32 constructs can run there; the others stay on
the CPU whatever the mode.

| Benchmark | CPU warm | GPU cold | GPU warm | auto warm |
|---|---:|---:|---:|---:|
| nbody f32 | 58 | 149 | 63 | 58 (CPU) |
| sum of [f32] | 7.2 | 228 | 31 | 7.4 (CPU) |

The base M1's GPU is small next to its CPU, and exactness costs more there than
on NVIDIA: `sqrt` and `/` use Metal's precise versions, and since Apple GPUs
flush subnormal f32 values to zero, kernels check for a flushed value (once per
loop iteration) and rerun the construct on the CPU if they find one. Without
those, the n-body step takes 24 ms. Memory is shared with the CPU, so copies run
at memcpy speed, but a sum still reads the array faster on the CPU. `auto`
measures both and keeps these on the CPU; GPUs with more cores (M1 Pro and up)
should move the n-body step over.
