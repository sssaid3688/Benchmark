#!/usr/bin/env bash
set -u

ROOT=/home/ubuntu/workspace/oyhj/staticQuant_mxfp8/Benchmark/FA
BUILD_DIR="$ROOT/build_ablation/b200_blackwell_fmha_mxfp8"
OUT_DIR="$ROOT/b200_blackwell_fmha_mxfp8/ablation_results_$(date +%Y%m%d_%H%M%S)"
ARGS=(--b=1 --h=40 --q=170100 --k=170100 --d=128 --mask=no --warmup_iterations=2 --iterations=5)
RUNS=3
TIMEOUT_SECONDS=240

mkdir -p "$OUT_DIR/logs"

cat > "$OUT_DIR/results.csv" <<'CSV'
variant,target,category,removed_defs,run,status,perf_tflops,log
CSV

variants=(
  "baseline|op6static2sm_b200_blackwell_fmha_mxfp8|main|none"
  "no_crel|op6static2sm_no_crel_b200_blackwell_fmha_mxfp8|main|MXFP8_2SM_CREL"
  "no_e2rsf|op6static2sm_no_e2rsf_b200_blackwell_fmha_mxfp8|main|MXFP8_E2RSF"
  "no_exitdb|op6static2sm_no_exitdb_b200_blackwell_fmha_mxfp8|main|MXFP8_2SM_EXITDB"
  "no_n128single|op6static2sm_no_n128single_b200_blackwell_fmha_mxfp8|main|MXFP8_2SM_N128SINGLE"
  "no_vprefetch|op6static2sm_no_vprefetch_b200_blackwell_fmha_mxfp8|main|MXFP8_2SM_VPREFETCH"
  "no_m2_combo|op6static2sm_no_m2_combo_b200_blackwell_fmha_mxfp8|main|MXFP8_M2_COMBO"
  "no_g_combo|op6static2sm_no_g_combo_b200_blackwell_fmha_mxfp8|main|MXFP8_G_COMBO"
  "no_r15_orvlog|op6static2sm_no_r15_orvlog_b200_blackwell_fmha_mxfp8|main|MXFP8_R15_ORVLOG"
  "kv_default|op6static2sm_kv_default_b200_blackwell_fmha_mxfp8|main|MXFP8_KV_STAGES=12"
  "regsm_default|op6static2sm_regsm_default_b200_blackwell_fmha_mxfp8|main|MXFP8_WS_REGSM;MXFP8_REGSM_SOFTMAX=184"
  "no_2cta|op6static2sm_no_2cta_b200_blackwell_fmha_mxfp8|main|FMHA_2CTA"
  "no_amaxfuse|op6static2sm_no_amaxfuse_b200_blackwell_fmha_mxfp8|sanity|MXFP8_AMAXFUSE"
  "no_psf_vec16|op6static2sm_no_psf_vec16_b200_blackwell_fmha_mxfp8|sanity|MXFP8_PSF_VEC16"
)

for entry in "${variants[@]}"; do
  IFS='|' read -r variant target category removed_defs <<<"$entry"
  bin="$BUILD_DIR/$target"
  for run in $(seq 1 "$RUNS"); do
    log="$OUT_DIR/logs/${variant}_run${run}.log"
    status=missing_binary
    perf=""
    if [[ -x "$bin" ]]; then
      echo "RUN $variant $run/$RUNS: $bin ${ARGS[*]}"
      timeout "$TIMEOUT_SECONDS" "$bin" "${ARGS[@]}" >"$log" 2>&1
      rc=$?
      if [[ $rc -eq 0 ]]; then
        status=ok
      elif [[ $rc -eq 124 ]]; then
        status=timeout
      else
        status="exit_$rc"
      fi
      perf=$(grep -oE '[0-9]+([.][0-9]+)?[[:space:]]+TFLOPS/s' "$log" | tail -1 | awk '{print $1}')
    else
      echo "Missing binary: $bin" >"$log"
    fi
    printf '%s,%s,%s,%s,%s,%s,%s,%s\n' \
      "$variant" "$target" "$category" "$removed_defs" "$run" "$status" "$perf" "$log" \
      >> "$OUT_DIR/results.csv"
  done
done

echo "$OUT_DIR"
