#!/bin/bash
# Run component-ablation timing for baseline + C2..C8 on a given shape.
# (C1/TMA-load cannot be stub-removed — pipeline deadlocks; measured via NCU SOL.)
set -u
BINDIR=/home/ubuntu/workspace/oyhj/temp/test/test_merge/Benchmark/FA/b200_blackwell_fmha_mxfp8_ablation/build_abl/b200_blackwell_fmha_mxfp8_ablation
OUTDIR=/home/ubuntu/workspace/oyhj/temp/test/test_merge/Benchmark/FA/b200_blackwell_fmha_mxfp8_ablation/timings
mkdir -p $OUTDIR

TAG=${1:-S1}
SEQ=${2:-170100}
REPS=3

run_one() {
  local bin=$1
  local name=$(basename $bin)
  local csv=$OUTDIR/${name}_${TAG}.csv
  echo "variant,run,tflops" > $csv
  for r in $(seq 1 $REPS); do
    tf=$($bin --b=1 --h=40 --q=$SEQ --k=$SEQ --d=128 --mask=no --warmup_iterations=2 --iterations=5 2>/dev/null | grep -o '[0-9.]* TFLOPS' | grep -o '[0-9.]*' | head -1)
    echo "$name,$r,$tf" >> $csv
    printf "  %-52s run%d: %s TFLOPS\n" "$name" "$r" "$tf"
  done
}

echo "=== TAG=$TAG SEQ=$SEQ ==="
for bin in op6static2sm_b200_blackwell_fmha_mxfp8 \
           abl_c2_no_qk_mma_b200_blackwell_fmha_mxfp8 \
           abl_c3_no_tmem_load_s_b200_blackwell_fmha_mxfp8 \
           abl_c4_no_softmax_b200_blackwell_fmha_mxfp8 \
           abl_c5_no_tmem_store_p_b200_blackwell_fmha_mxfp8 \
           abl_c6_no_pv_mma_b200_blackwell_fmha_mxfp8 \
           abl_c7_no_correction_b200_blackwell_fmha_mxfp8 \
           abl_c8_no_epi_store_b200_blackwell_fmha_mxfp8 ; do
  echo "--- $bin ---"
  run_one $BINDIR/$bin
done
echo "=== DONE $TAG ==="
