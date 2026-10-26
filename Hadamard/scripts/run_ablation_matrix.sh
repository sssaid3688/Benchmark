#!/usr/bin/env bash
set -euo pipefail

ROOT=/home/ubuntu/workspace/oyhj/temp/test/test_merge/Benchmark/Hadamard
TEMPLATE="$ROOT/src/doublewarp_fusion_ablation.cu"
RESULTS="$ROOT/ablation_results_20260829"
OBJECT=/workspace/hadamard_base/TransformerEngine/build/cmake/CMakeFiles/transformer_engine.dir/hadamard_transform/row_cast_col_hadamard_transform_cast_fusion.cu.o
PYBIN=/home/ubuntu/workspace/wzg/gjl/env/bin/python
export PYTHONPATH=/workspace/hadamard_base/TransformerEngine
export LD_LIBRARY_PATH=/home/ubuntu/workspace/wzg/gjl/env/lib/python3.12/site-packages/torch/lib
export NVTE_USE_FAST_MATH=1

mkdir -p "$RESULTS/sass" "$RESULTS/build_logs" "$ROOT/src/generated_ablation"

build_and_measure() {
  local label="$1"
  shift
  local generated="$ROOT/src/generated_ablation/${label}.cu"
  cp "$TEMPLATE" "$generated"
  local define
  for define in "$@"; do
    sed -i "1i#define ${define} 1" "$generated"
  done

  echo "===== BUILD ${label}: $* ====="
  bash "$ROOT/scripts/swap_and_build.sh" "$generated" \
    >"$RESULTS/build_logs/${label}.log" 2>&1
  md5sum "$generated" >"$RESULTS/build_logs/${label}.md5"
  /usr/local/cuda-13.3/bin/cuobjdump -sass "$OBJECT" >"$RESULTS/sass/${label}.sass"
  /usr/local/cuda-13.3/bin/cuobjdump -res-usage "$OBJECT" \
    >"$RESULTS/sass/${label}.resources.txt" 2>&1

  echo "===== BENCH ${label} ====="
  "$PYBIN" "$ROOT/scripts/ablation_benchmark.py" \
    --label "$label" --output "$RESULTS/${label}.json"
}

build_and_measure no_had HAD_ABLATE_TC_MMA
build_and_measure no_dq HAD_ABLATE_DQ_TOTAL
build_and_measure no_had_no_dq HAD_ABLATE_TC_MMA HAD_ABLATE_DQ_TOTAL
build_and_measure no_pts HAD_ABLATE_PER_TENSOR_SCALE
build_and_measure no_had_no_pts HAD_ABLATE_TC_MMA HAD_ABLATE_PER_TENSOR_SCALE
build_and_measure no_amax_reduction HAD_ABLATE_DQ_AMAX_REDUCTION
build_and_measure no_had_no_amax_reduction HAD_ABLATE_TC_MMA HAD_ABLATE_DQ_AMAX_REDUCTION
build_and_measure no_scale_chain HAD_ABLATE_DQ_SCALE_CHAIN
build_and_measure no_had_no_scale_chain HAD_ABLATE_TC_MMA HAD_ABLATE_DQ_SCALE_CHAIN
build_and_measure no_fp4_convert HAD_ABLATE_DQ_FP4_CONVERT
build_and_measure no_had_no_fp4_convert HAD_ABLATE_TC_MMA HAD_ABLATE_DQ_FP4_CONVERT

echo "===== MATRIX COMPLETE ====="
