#!/usr/bin/env bash
set -euo pipefail

ROOT=/home/ubuntu/workspace/oyhj/temp/test/test_merge/Benchmark/Hadamard
TEMPLATE="$ROOT/src/doublewarp_fusion_ablation.cu"
ORIGINAL="$ROOT/src/doublewarp_fusion.cu"
RESULTS="$ROOT/ablation_results_20260830_dq_strict_anchor"
OBJECT=/workspace/hadamard_base/TransformerEngine/build/cmake/CMakeFiles/transformer_engine.dir/hadamard_transform/row_cast_col_hadamard_transform_cast_fusion.cu.o
DEPLOYED=/workspace/hadamard_base/TransformerEngine/transformer_engine/common/hadamard_transform/row_cast_col_hadamard_transform_cast_fusion.cu
ORIGINAL_MD5=e6ff5d9c1896c34dbbbafbe02bdea0ca
PYBIN=/home/ubuntu/workspace/wzg/gjl/env/bin/python
export PYTHONPATH=/workspace/hadamard_base/TransformerEngine
export LD_LIBRARY_PATH=/home/ubuntu/workspace/wzg/gjl/env/lib/python3.12/site-packages/torch/lib
export NVTE_USE_FAST_MATH=1

mkdir -p "$RESULTS/sass" "$RESULTS/build_logs" "$ROOT/src/generated_ablation_dq_strict_anchor"

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
}
trap restore_original EXIT

wait_for_idle_gpu() {
  local stable=0
  local attempt
  for attempt in $(seq 1 1800); do
    local apps util
    apps=$(nvidia-smi --query-compute-apps=pid --format=csv,noheader 2>/dev/null | sed '/^[[:space:]]*$/d' | wc -l)
    util=$(nvidia-smi --query-gpu=utilization.gpu --format=csv,noheader,nounits | head -n 1 | tr -d ' ')
    if [[ "$apps" -eq 0 && "$util" -le 5 ]]; then
      stable=$((stable + 1))
      [[ "$stable" -ge 3 ]] && return 0
    else
      stable=0
    fi
    sleep 2
  done
  return 1
}

build_and_measure() {
  local label="$1"
  shift
  local generated="$ROOT/src/generated_ablation_dq_strict_anchor/${label}.cu"
  wait_for_idle_gpu
  cp "$TEMPLATE" "$generated"
  local define
  for define in "$@"; do
    sed -i "1i#define ${define} 1" "$generated"
  done
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

same_count() {
  local pattern="$1"
  local lhs rhs
  lhs=$(grep -cE "$pattern" "$RESULTS/sass/baseline_dq_strict_anchor.sass" || true)
  rhs=$(grep -cE "$pattern" "$RESULTS/sass/no_dq_strict_anchor.sass" || true)
  if [[ "$lhs" != "$rhs" ]]; then
    echo "SASS preservation failed for $pattern: $lhs != $rhs" >&2
    return 1
  fi
}

nvidia-smi -lgc 1965,1965 >"$RESULTS/lock_clocks.log" 2>&1

if [[ ! -s "$RESULTS/baseline_dq_strict_anchor.json" || ! -s "$RESULTS/sass/baseline_dq_strict_anchor.sass" ]]; then
  build_and_measure baseline_dq_strict_anchor HAD_DQ_TMEM_ANCHOR HAD_DQ_PTS_ANCHOR
fi
if [[ ! -s "$RESULTS/no_dq_strict_anchor.json" || ! -s "$RESULTS/sass/no_dq_strict_anchor.sass" ]]; then
  build_and_measure no_dq_strict_anchor HAD_DQ_TMEM_ANCHOR HAD_DQ_PTS_ANCHOR HAD_ABLATE_DQ_TOTAL
fi

same_count 'UTCHMMA'
same_count 'UTMALDG\.2D'
same_count 'LDTM\.x64'
same_count '\bSTG(\.|\b)'
# Preserve the 32-bit global-amax load.  LDG.E.64.CONSTANT is deliberately
# allowed to drop because it loads RNG seed/offset owned by the DQ path.
same_count 'LDG\.E[[:space:]]'

build_and_measure no_had_dq_strict_anchor HAD_DQ_TMEM_ANCHOR HAD_DQ_PTS_ANCHOR HAD_ABLATE_TC_MMA
build_and_measure no_had_no_dq_strict_anchor HAD_DQ_TMEM_ANCHOR HAD_DQ_PTS_ANCHOR HAD_ABLATE_TC_MMA HAD_ABLATE_DQ_TOTAL

"$PYBIN" /tmp/hadamard_audit_sass.py "$RESULTS/sass" "$RESULTS/sass_audit.csv"
date -Is >"$RESULTS/DQ_STRICT_ANCHOR_DONE.txt"
echo "===== STRICT TOTAL-DQ ANCHOR MATRIX COMPLETE ====="
