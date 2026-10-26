#!/usr/bin/env bash
set -euo pipefail

ROOT=/home/ubuntu/workspace/oyhj/temp/test/test_merge/Benchmark/Hadamard
TEMPLATE="$ROOT/src/doublewarp_fusion_ablation.cu"
RESULTS="$ROOT/ablation_results_20260829_final_pairs"
OBJECT=/workspace/hadamard_base/TransformerEngine/build/cmake/CMakeFiles/transformer_engine.dir/hadamard_transform/row_cast_col_hadamard_transform_cast_fusion.cu.o
DEPLOYED=/workspace/hadamard_base/TransformerEngine/transformer_engine/common/hadamard_transform/row_cast_col_hadamard_transform_cast_fusion.cu
ORIGINAL="$ROOT/src/doublewarp_fusion.cu"
ORIGINAL_MD5=e6ff5d9c1896c34dbbbafbe02bdea0ca
PYBIN=/home/ubuntu/workspace/wzg/gjl/env/bin/python
export PYTHONPATH=/workspace/hadamard_base/TransformerEngine
export LD_LIBRARY_PATH=/home/ubuntu/workspace/wzg/gjl/env/lib/python3.12/site-packages/torch/lib
export NVTE_USE_FAST_MATH=1

mkdir -p "$RESULTS/sass" "$RESULTS/build_logs" "$ROOT/src/generated_ablation_final_pairs"

restore_original() {
  local status=$?
  set +e
  bash "$ROOT/scripts/swap_and_build.sh" "$ORIGINAL" \
    >"$RESULTS/build_logs/restore_original.log" 2>&1
  local restore_status=$?
  local deployed_md5
  deployed_md5=$(md5sum "$DEPLOYED" 2>/dev/null | cut -d' ' -f1)
  nvidia-smi -rgc >"$RESULTS/restore_clocks.log" 2>&1
  {
    echo "experiment_status=$status"
    echo "restore_build_status=$restore_status"
    echo "expected_original_md5=$ORIGINAL_MD5"
    echo "deployed_md5=$deployed_md5"
    date -Is
  } >"$RESULTS/RESTORE_STATUS.txt"
  if [[ "$restore_status" -ne 0 || "$deployed_md5" != "$ORIGINAL_MD5" ]]; then
    echo "ERROR: original source restoration failed" >&2
  fi
}
trap restore_original EXIT

wait_for_idle_gpu() {
  local stable=0
  local attempt
  # Up to six hours: unrelated long-running jobs may retain the B200.
  for attempt in $(seq 1 10800); do
    local apps util
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

build_and_measure_no_fp4() {
  local label=no_fp4_convert_r3
  local generated="$ROOT/src/generated_ablation_final_pairs/${label}.cu"
  wait_for_idle_gpu
  cp "$TEMPLATE" "$generated"
  sed -i '1i#define HAD_ABLATE_DQ_FP4_CONVERT 1' "$generated"
  bash "$ROOT/scripts/swap_and_build.sh" "$generated" \
    >"$RESULTS/build_logs/${label}.log" 2>&1
  md5sum "$generated" >"$RESULTS/build_logs/${label}.md5"
  /usr/local/cuda-13.3/bin/cuobjdump -sass "$OBJECT" >"$RESULTS/sass/${label}.sass"
  /usr/local/cuda-13.3/bin/cuobjdump -res-usage "$OBJECT" \
    >"$RESULTS/sass/${label}.resources.txt" 2>&1
  nvidia-smi -lgc 1965,1965 >"$RESULTS/lock_clocks.log" 2>&1
  wait_for_idle_gpu
  nvidia-smi --query-gpu=timestamp,pstate,clocks.sm,clocks.mem,power.draw \
    --format=csv,noheader >"$RESULTS/${label}.gpu_before.csv"
  "$PYBIN" "$ROOT/scripts/ablation_benchmark.py" \
    --label "$label" --output "$RESULTS/${label}.json" \
    --rounds 17 --iters 80 --warmup 80
  nvidia-smi --query-compute-apps=pid,process_name,used_gpu_memory \
    --format=csv,noheader >"$RESULTS/${label}.apps_after.csv" || true
  nvidia-smi --query-gpu=timestamp,pstate,clocks.sm,clocks.mem,power.draw \
    --format=csv,noheader >"$RESULTS/${label}.gpu_after.csv"
}

build_and_measure_no_had_no_fp4() {
  local label=no_had_no_fp4_convert_r3
  local generated="$ROOT/src/generated_ablation_final_pairs/${label}.cu"
  cp "$TEMPLATE" "$generated"
  sed -i '1i#define HAD_ABLATE_DQ_FP4_CONVERT 1' "$generated"
  sed -i '1i#define HAD_ABLATE_TC_MMA 1' "$generated"
  bash "$ROOT/scripts/swap_and_build.sh" "$generated" \
    >"$RESULTS/build_logs/${label}.log" 2>&1
  md5sum "$generated" >"$RESULTS/build_logs/${label}.md5"
  /usr/local/cuda-13.3/bin/cuobjdump -sass "$OBJECT" >"$RESULTS/sass/${label}.sass"
  /usr/local/cuda-13.3/bin/cuobjdump -res-usage "$OBJECT" \
    >"$RESULTS/sass/${label}.resources.txt" 2>&1
  wait_for_idle_gpu
  nvidia-smi --query-gpu=timestamp,pstate,clocks.sm,clocks.mem,power.draw \
    --format=csv,noheader >"$RESULTS/${label}.gpu_before.csv"
  "$PYBIN" "$ROOT/scripts/ablation_benchmark.py" \
    --label "$label" --output "$RESULTS/${label}.json" \
    --rounds 17 --iters 80 --warmup 80
  nvidia-smi --query-compute-apps=pid,process_name,used_gpu_memory \
    --format=csv,noheader >"$RESULTS/${label}.apps_after.csv" || true
  nvidia-smi --query-gpu=timestamp,pstate,clocks.sm,clocks.mem,power.draw \
    --format=csv,noheader >"$RESULTS/${label}.gpu_after.csv"
}

build_and_measure_no_fp4
build_and_measure_no_had_no_fp4
"$PYBIN" /tmp/hadamard_audit_sass.py "$RESULTS/sass" "$RESULTS/sass_audit.csv"
date -Is >"$RESULTS/FP4_PAIR_DONE.txt"
echo "===== FP4 PAIR COMPLETE ====="
