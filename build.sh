#!/usr/bin/env bash
# ============================================================================
# op6 MXFP8 FlashAttention forward (B200 / sm_100a)
# Variant: 2-SM cooperative, STATIC P-quant (SF=1.0), fp32 softmax.  <-- client spec
#   SOW1 (B=1 H=40 S=170100 D=128, NoMask) = ~1424 TFLOPS  (dedicated B200)
#   SOW2 (B=1 H=40 S=510300 D=128, NoMask) = ~1429 TFLOPS
#
# Prereqs:  CUDA 13.0+ (nvcc on PATH, or set NVCC=...), a B200 (sm_100a),
#           thirdparty/cutlass cloned (see README "thirdparty").
# Output:   build/op6/fmha_mxfp8_pvmx   (name expected by the golden verifier)
# ============================================================================
set -euo pipefail
ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
cd "$ROOT"
NVCC=${NVCC:-nvcc}
mkdir -p build/op6

COMMON="-DCUTLASS_VERSIONS_GENERATED -O3 -std=c++17 \
  --generate-code=arch=compute_100a,code=[sm_100a] -lineinfo \
  -DCUTLASS_ENABLE_TENSOR_CORE_MMA=1 -DCUTLASS_ENABLE_GDC_FOR_SM100=1 \
  --expt-relaxed-constexpr --use_fast_math \
  -I $ROOT/src \
  -I $ROOT/thirdparty/cutlass/include \
  -I $ROOT/thirdparty/cutlass/examples/common \
  -I $ROOT/thirdparty/cutlass/examples/77_blackwell_fmha \
  -I $ROOT/thirdparty/cutlass/tools/util/include"

SRC=src/operators/op6_mxfp8_fmha/custom_2sm/fmha_mxfp8_pvmx.cu

# ---- static-fp32 king flags (PSTATIC = static P-quant; fp32 softmax) ---------
# + warp-spec reg-down (softmax warpgroup setmaxnreg 192->184): removes an 8-byte
#   critical-path register spill -> +12 TFLOPS SOW1, bit-exact numerics.
KING="-DMXFP8_FULL -DMXFP8_N128 -DMXFP8_KV_STAGES=12 -DMXFP8_AMAXFUSE -DMXFP8_PSF_VEC16 \
  -DMXFP8_PSTATIC -DMXFP8_2SM_CREL -DMXFP8_E2RSF -DMXFP8_2SM_EXITDB -DMXFP8_2SM_N128SINGLE \
  -DMXFP8_2SM_VPREFETCH -DMXFP8_M2_COMBO -DMXFP8_G_COMBO -DMXFP8_R15_ORVLOG \
  -DMXFP8_WS_REGSM -DMXFP8_REGSM_SOFTMAX=184"

echo "[build] op6 static-fp32 (best) -> build/op6/fmha_mxfp8_pvmx"
$NVCC $KING $COMMON "$SRC" -o build/op6/fmha_mxfp8_pvmx
echo "DONE -> build/op6/fmha_mxfp8_pvmx"
echo
echo "Benchmark:"
echo "  ./build/op6/fmha_mxfp8_pvmx --b=1 --h=40 --s=170100 --d=128 --verify=0   # SOW1 ~1424 TFLOPS"
echo "  ./build/op6/fmha_mxfp8_pvmx --b=1 --h=40 --s=510300 --d=128 --verify=0   # SOW2 ~1429 TFLOPS"
echo "Correctness (built-in fp32 SDPA check):"
echo "  ./build/op6/fmha_mxfp8_pvmx --b=1 --h=8 --s=4096 --d=128"
echo "Tight golden (per-32 MXFP8 P-quant CPU ref, needs python3+numpy+torch):"
echo "  bash src/operators/op6_mxfp8_fmha/verify/run_op6_precision.sh"
