#!/usr/bin/env bash
set -euo pipefail

ROOT=/home/ubuntu/workspace/oyhj/temp/test/test_merge/Benchmark/Hadamard
TEMPLATE="$ROOT/src/doublewarp_fusion_ablation.cu"
RESULTS="$ROOT/ablation_results_20260829_r2"
OBJECT=/workspace/hadamard_base/TransformerEngine/build/cmake/CMakeFiles/transformer_engine.dir/hadamard_transform/row_cast_col_hadamard_transform_cast_fusion.cu.o
PYBIN=/home/ubuntu/workspace/wzg/gjl/env/bin/python
export PYTHONPATH=/workspace/hadamard_base/TransformerEngine
export LD_LIBRARY_PATH=/home/ubuntu/workspace/wzg/gjl/env/lib/python3.12/site-packages/torch/lib
export NVTE_USE_FAST_MATH=1

mkdir -p "$RESULTS/sass" "$RESULTS/build_logs" "$ROOT/src/generated_ablation_r2"

wait_for_idle_gpu() {
  local stable=0
  local attempt
  for attempt in $(seq 1 300); do
    local apps
    local util
    apps=$(nvidia-smi --query-compute-apps=pid --format=csv,noheader 2>/dev/null | sed '/^[[:space:]]*$/d' | wc -l)
    util=$(nvidia-smi --query-gpu=utilization.gpu --format=csv,noheader,nounits | head -n 1 | tr -d ' ')
    if [[ "$apps" -eq 0 && "$util" -le 5 ]]; then
      stable=$((stable + 1))
      if [[ "$stable" -ge 3 ]]; then
        return 0
      fi
    else
      stable=0
    fi
    sleep 2
  done
  echo "GPU did not remain idle within the guard window" >&2
  return 1
}

build_and_measure() {
  local label="$1"
  shift
  local generated="$ROOT/src/generated_ablation_r2/${label}.cu"
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

  echo "===== WAIT IDLE ${label} ====="
  wait_for_idle_gpu
  nvidia-smi --query-gpu=timestamp,pstate,clocks.sm,clocks.mem,power.draw \
    --format=csv,noheader >"$RESULTS/${label}.gpu_before.csv"
  echo "===== BENCH ${label} ====="
  "$PYBIN" "$ROOT/scripts/ablation_benchmark.py" \
    --label "$label" --output "$RESULTS/${label}.json" \
    --rounds 17 --iters 80 --warmup 80
  nvidia-smi --query-compute-apps=pid,process_name,used_gpu_memory \
    --format=csv,noheader >"$RESULTS/${label}.apps_after.csv" || true
  nvidia-smi --query-gpu=timestamp,pstate,clocks.sm,clocks.mem,power.draw \
    --format=csv,noheader >"$RESULTS/${label}.gpu_after.csv"
}

build_and_measure baseline_r2
build_and_measure no_had_r2 HAD_ABLATE_TC_MMA
build_and_measure no_dq_r2 HAD_ABLATE_DQ_TOTAL
build_and_measure no_had_no_dq_r2 HAD_ABLATE_TC_MMA HAD_ABLATE_DQ_TOTAL
build_and_measure no_amax_reduction_r2 HAD_ABLATE_DQ_AMAX_REDUCTION
build_and_measure no_had_no_amax_reduction_r2 HAD_ABLATE_TC_MMA HAD_ABLATE_DQ_AMAX_REDUCTION
build_and_measure no_scale_chain_r2 HAD_ABLATE_DQ_SCALE_CHAIN
build_and_measure no_had_no_scale_chain_r2 HAD_ABLATE_TC_MMA HAD_ABLATE_DQ_SCALE_CHAIN

echo "===== R2 COMPLETE ====="
