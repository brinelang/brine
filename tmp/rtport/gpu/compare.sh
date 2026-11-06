#!/bin/bash
# compare.sh <compiler> <program> [args...]: runs the program with the C
# backend (the C runtime) and natively (rt.br), under BRINE_GPU=on, off and
# auto with BRINE_GPU_LOG=1, and compares stdout, stderr (times masked) and
# the exit code.
B=$1; shift
P=$1; shift
out=${OUT:-tmp/rtport/gpu/out}
mkdir -p $out
norm() { sed -E 's/[0-9]+\.[0-9]+ (ms|MB)/X \1/g; s/ready in [0-9]+ ms/ready in X ms/'; }
fail=0
for mode in on off auto; do
  i=0
  for how in "run --release" "run" "native"; do
    if [ "$how" = native ]; then
      BRINE_BACKEND=native BRINE_GPU=$mode BRINE_GPU_LOG=1 $B run --release -q $P -- "$@" >$out/o.$i 2>$out/e.$i
    else
      BRINE_GPU=$mode BRINE_GPU_LOG=1 $B $how -q $P -- "$@" >$out/o.$i 2>$out/e.$i
    fi
    echo "exit $?" >>$out/o.$i
    norm <$out/e.$i >$out/n.$i
    i=$((i+1))
  done
  for j in 1 2; do
    if ! cmp -s $out/o.0 $out/o.$j || ! cmp -s $out/n.0 $out/n.$j; then
      echo "DIFF $P $* mode=$mode run$j"; diff $out/o.0 $out/o.$j | head -5; diff $out/n.0 $out/n.$j | head -10; fail=1
    fi
  done
  echo "$P $* mode=$mode: $(tail -1 $out/o.0), $(wc -l <$out/e.0) log lines, $(grep -c ' -> gpu ' $out/e.0) on gpu"
done
exit $fail
