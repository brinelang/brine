#!/bin/sh
# Builds the benchmarks with --release and runs each with the GPU off, forced
# on and on auto. Every run must print the same checksum; the times go to
# stderr. "cold" is the first run in the process (for the GPU: getting the
# device ready, the PTX JIT and the first copies), "warm" the best later run.
#   bench/gpu/run.sh [path/to/brine]
set -e
brine=${1:-./brine}
mkdir -p target/bench
for b in vecmap nbody mandelbrot sum; do
    "$brine" build --release "bench/gpu/$b.br" -o "target/bench/$b" -q
done
for b in vecmap nbody mandelbrot sum; do
    for mode in off on auto; do
        out=$(BRINE_GPU=$mode "target/bench/$b" 2>"target/bench/$b.$mode.txt")
        if [ -n "$want" ] && [ "$out" != "$want" ]; then
            echo "$b: BRINE_GPU=$mode printed $out, not $want" >&2
            exit 1
        fi
        want=$out
        sed "s/^/$mode: /" "target/bench/$b.$mode.txt"
    done
    want=
done
