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
  static const int NumRegsSoftmax = 176;
  static const int NumRegsCorrection = 96 - (kDebugUsingPrintf ? 16 : 0);
  static const int NumRegsOther = 32 + (kDebugUsingPrintf ? 16 : 0);
  static const int NumRegsEmpty = 24;

  static const int NumWarps = 16;
};

}  // namespace cutlass::fmha::kernel
