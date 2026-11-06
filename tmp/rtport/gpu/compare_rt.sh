#!/bin/bash
# compare_rt.sh <old compiler> <new compiler> <program> [args...]: the same
# native code with the C runtime's GPU code (old) and rt.br's (new), under
# BRINE_GPU=on, off and auto with BRINE_GPU_LOG=1; stdout, stderr (times
# masked) and the exit code must match.
A=$1; B=$2; P=$3; shift 3
out=${OUT:-tmp/rtport/gpu/out2}
mkdir -p $out
norm() { sed -E 's/[0-9]+\.[0-9]+ (ms|MB)/X \1/g; s/ready in [0-9]+ ms/ready in X ms/'; }
fail=0
for mode in on off auto; do
  for prof in "" "--release"; do
    i=0
    for c in $A $B; do
      BRINE_BACKEND=native BRINE_GPU=$mode BRINE_GPU_LOG=1 $c run $prof -q $P -- "$@" >$out/o.$i 2>$out/e.$i
      echo "exit $?" >>$out/o.$i
      norm <$out/e.$i >$out/n.$i
      i=$((i+1))
    done
    if ! cmp -s $out/o.0 $out/o.1 || ! cmp -s $out/n.0 $out/n.1; then
      echo "DIFF $P $* mode=$mode $prof"; diff $out/o.0 $out/o.1 | head -5; diff $out/n.0 $out/n.1 | head -10; fail=1
    fi
    echo "$P $* mode=$mode $prof: $(tail -1 $out/o.1), $(wc -l <$out/e.1) log lines, $(grep -c ' -> gpu ' $out/e.1) on gpu"
  done
done
exit $fail
