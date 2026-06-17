/***************************************************************************************************
 * Operator 6 — M3 / CtaN=128 rewrite : split-N cooperative-softmax kernel schedule.
 *
 * The pristine ex77 forward kernel (kernel/sm100_fmha_fwd_kernel_tma_warpspecialized.hpp)
 * is templated on a KernelSchedule that maps warp index -> role. The default
 * schedule runs TWO softmax warp groups (Softmax0 = warps 0-3, Softmax1 = 4-7)
 * for the dual-stage M=256 design.
 *
 * Route C at CtaN=128 cannot afford dual-stage: S0+S1+O0+O1 = 4x128 = 512 TMEM
 * columns, leaving zero room for the block-scaled SF operands. The CtaN=128
 * variant runs a SINGLE M=128 tile per CTA — but uses BOTH warp groups
 * cooperatively on it (M3b "split-N"): Softmax0 (warps 0-3) reduces KV columns
 * [0:64], Softmax1 (warps 4-7) reduces [64:128]. Two softmax warp groups per
 * scheduler hide the softmax TMEM-load latency that single-group M3a exposed.
 *
 * This schedule reuses the pristine kernel header unchanged — warps 4-7 are
 * mapped back to Softmax1. The mainloop's softmax()/softmax_step() is the only
 * other change: group 0 owns the mma->softmax / softmax->correction pipelines
 * (pipeline_mma_s0 / s0_corr) exactly as the single-group variant did; group 1
 * holds NO pipeline (pipeline_mma_s1 / s1_corr stay inert — never produced or
 * consumed) and stays lockstepped with group 0 purely via NamedBarriers, which
 * also carry the cross-group row_max / row_sum reduction. Consequently mma()
 * and correction() are UNCHANGED from the verified single-stage variant.
 ***************************************************************************************************/
#pragma once

#include "cutlass/cutlass.h"

namespace cutlass::fmha::kernel {

#ifdef MXFP8_SM12
// ── [刀15 SM12] 12-warp softmax schedule: correction merged into softmax ──
// The TMEM quadrant wall (tcgen05.ld row = (warp%4)*32 + lane) means the only
// way to raise per-quadrant exp2 capacity is MORE softmax warps per quadrant:
// 12 warps = 3 per SMSP/quadrant. The 4 ex-Correction warps become Softmax2;
// the per-tile O rescale + tail epilogue migrate into Softmax0 (G0), whose
// per-row chain registers already hold the old/new row-max stats (PipelineC
// and the V-stats TMEM round-trip retire entirely). Column split (16-col
// STS.128 granules, see mainloop kSm12Col): G0=32 cols (+correction duty),
// G1=48, G2=48 — critical P-done exp2 chain 64 -> 48 per warp.
// Register budget: 12·32·160 + 3·32·32 + 1·32·24 = 65280 <= 65536 (px14
// probe: compiles at 160, SASS TRY_ALLOC 0xa0, LOCAL:0).
struct Sm100FmhaCtxKernelWarpspecializedScheduleSingleStage {

  enum class WarpRole {
    Softmax0,
    Softmax1,
    Softmax2,      // [SM12] third softmax group (ex-Correction warps 8-11)
    Correction,    // kept in the enum for kernel-code compatibility; never mapped
    MMA,
    Load,
    Epilogue,
    Empty
  };

  // warps 0-3 = Softmax0, 4-7 = Softmax1, 8-11 = Softmax2, 12 = MMA,
  // 13 = Load, 14 = Epilogue, 15 = Empty. (No Correction warps.)
  static constexpr WarpRole warp_idx_to_WarpRole(int warp_idx) {
    int wg_idx = warp_idx / 4;
    if (wg_idx == 0) return WarpRole::Softmax0;       //   0 -  3
    if (wg_idx == 1) return WarpRole::Softmax1;       //   4 -  7
    if (wg_idx == 2) return WarpRole::Softmax2;       //   8 - 11  [SM12]
    if (warp_idx == 12) return WarpRole::MMA;         //       12
    if (warp_idx == 13) return WarpRole::Load;        //       13
    if (warp_idx == 14) return WarpRole::Epilogue;    //       14
    return WarpRole::Empty;                           //       15
  }

  // NumWarpsSoftmax = warps per GROUP (3 groups). NumWarpsCorrection kept at 4:
  // it still sizes PipelineO's consumer_arv_count (2·4·32 = 256) and
  // PipelineE's producer_arv_count (128) — G0 (4 warps) now plays that part,
  // so the VALUES are unchanged, only the role owning them moved.
  static const int NumWarpsSoftmax = 4;
  static const int NumWarpsCorrection = 4;
  static const int NumWarpsEpilogue = 1;
  static const int NumWarpsLoad = 1;

  static const bool kDebugUsingPrintf = false;
  // [SM12] 12·32·R + 3·32·32 + 1·32·24 <= 65536 ⟹ R <= 160.
  static const int NumRegsSoftmax = 160;
  static const int NumRegsCorrection = 96;   // unused (no Correction warps)
  static const int NumRegsOther = 32;
  static const int NumRegsEmpty = 24;

  static const int NumWarps = 16;
};

#else  // !MXFP8_SM12 — pristine 8+4 schedule below, byte-identical

struct Sm100FmhaCtxKernelWarpspecializedScheduleSingleStage {

  enum class WarpRole {
    Softmax0,
    Softmax1,
    Correction,
    MMA,
    Load,
    Epilogue,
    Empty
  };

  // warps 0-3 = Softmax0, 4-7 = Softmax1, 8-11 = Correction, 12 = MMA,
  // 13 = Load, 14 = Epilogue, 15 = Empty.
  static constexpr WarpRole warp_idx_to_WarpRole(int warp_idx) {
    int wg_idx = warp_idx / 4;
    if (wg_idx == 0) return WarpRole::Softmax0;       //   0 -  3
    if (wg_idx == 1) return WarpRole::Softmax1;       //   4 -  7  (M3b split-N)
    if (wg_idx == 2) return WarpRole::Correction;     //   8 - 11
    if (warp_idx == 12) return WarpRole::MMA;         //       12
    if (warp_idx == 13) return WarpRole::Load;        //       13
    if (warp_idx == 14) return WarpRole::Epilogue;    //       14
    return WarpRole::Empty;                           //       15
  }

  // NumWarpsSoftmax stays 4: it sizes pipeline_mma_s0's consumer_arv_count and
  // pipeline_s0_corr's producer_arv_count, both of which the 4 Softmax0 warps
  // (the only softmax warps) still satisfy.
  static const int NumWarpsSoftmax = 4;
  static const int NumWarpsCorrection = 4;
  static const int NumWarpsEpilogue = 1;
  static const int NumWarpsLoad = 1;

  static const bool kDebugUsingPrintf = false;
  // [MXFP8 N128 M3b] split-N halves each softmax thread's S register array back
  // to 64 fp32 (each group owns 64 KV cols), so the M3a spill is gone and the
  // quota can drop from 240. But there are now EIGHT softmax warps, so the
  // 16-warp register budget (65536) is the binding constraint:
  //   8·32·NumRegsSoftmax + 4·32·96 + 3·32·32 + 1·32·24 <= 65536
  //   ⟹ NumRegsSoftmax <= 193.  192 ⟹ total 65280 < 65536, no spill at a
  // 64-element S array (the verified CtaN=64 path runs spill-free at 192).
  // [wsA reg-rebalance] default-OFF gated override of the per-warpgroup register
  // budget. Gate OFF (no -DMXFP8_WS_REGSM) ⟹ king bit-exact (192/96/32/24).
  // Budget (16-warp SM, setmaxnreg granularity 8, total <= 65536):
  //   8·32·SM + 4·32·CORR + 3·32·OTH + 1·32·EMP <= 65536
  //   non-softmax fixed @96/32/24 = 12288+3072+768 = 16128 ⟹ 256·SM + extra <= 65536.
#ifdef MXFP8_WS_REGSM
#ifndef MXFP8_REGSM_SOFTMAX
#define MXFP8_REGSM_SOFTMAX 192
#endif
#ifndef MXFP8_REGSM_CORR
#define MXFP8_REGSM_CORR 96
#endif
#ifndef MXFP8_REGSM_OTHER
#define MXFP8_REGSM_OTHER 32
#endif
  static const int NumRegsSoftmax = MXFP8_REGSM_SOFTMAX;
  static const int NumRegsCorrection = MXFP8_REGSM_CORR - (kDebugUsingPrintf ? 16 : 0);
  static const int NumRegsOther = MXFP8_REGSM_OTHER + (kDebugUsingPrintf ? 16 : 0);
#else
  static const int NumRegsSoftmax = 192;
  static const int NumRegsCorrection = 96 - (kDebugUsingPrintf ? 16 : 0);
  static const int NumRegsOther = 32 + (kDebugUsingPrintf ? 16 : 0);
#endif
  static const int NumRegsEmpty = 24;

  static const int NumWarps = 16;
};

#endif  // MXFP8_SM12

}  // namespace cutlass::fmha::kernel
