/***************************************************************************************************
 * Copyright (c) 2025  - 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice, this
 * list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 * this list of conditions and the following disclaimer in the documentation
 * and/or other materials provided with the distribution.
 *
 * 3. Neither the name of the copyright holder nor the names of its
 * contributors may be used to endorse or promote products derived from
 * this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 * CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 **************************************************************************************************/


#pragma once

#include "cutlass/cutlass.h"

#include "cute/tensor.hpp"
#include "cute/arch/simd_sm100.hpp"

#include "cutlass/arch/arch.h"
#include "cutlass/arch/memory_sm80.h"
#include "cutlass/gemm/collective/collective_builder.hpp"

#include "collective/fmha_common.hpp"

#include <cmath>

#if defined(FAG_DQ_BF16_STAGING)
#include <cuda_bf16.h>
#endif

// ---------------------------------------------------------------------------
// FAG_DS_RUN_STORES: FA4-style (FP16-equivalent) dS corner turn.  Implies the
// fragment-order-consistent TMEM atom pair: the x32 load atom hands every
// compute thread TWO runs of 32 consecutive Q values at its fixed KV lane
// (fragment element i <-> Q(i) = 32*(wg + 2*(i/32)) + (i%32)), and the 8x
// store atom keeps the P/dS RMEM->TMEM fragments in exactly the same order
// (rest-mode split (2,2) => blocks {wg, wg+2} on both sides).  Each 32-byte
// run is two adjacent 16-byte blocks inside one swizzled row of the SW64
// smem atom, so both STS.128 addresses are loop-invariant bases (precomputed
// once before the mainloop) -- the same ~0-address-ALU store cost model as
// FA4's autovec_copy path.
// ---------------------------------------------------------------------------
#if defined(FAG_DS_RUN_STORES)
#if !defined(FAG_TMEM_LOAD_32X)
#define FAG_TMEM_LOAD_32X
#endif
#if !defined(FAG_TMEM_STORE_8X)
#define FAG_TMEM_STORE_8X
#endif
#endif

namespace cutlass::fmha::kernel {

#if defined(FAG_CTRL_QUANTPIN) || defined(FAG_ABLATE_QUANT_V2)
// Device sink for the rigorous-ablation liveness pin. Never written at runtime
// (the guarding branch is opaque but never taken); its mere presence in the
// CFG forces both nvcc AND ptxas to keep every instruction producing the
// pinned values. Unlike an empty `asm volatile("" : "+f")` pin — which emits
// no PTX instruction and is defeated by ptxas dead-code elimination — this
// cannot be optimized away at any stage. FAG_CTRL_QUANTPIN keeps the pin AND
// the converter, so (ctrl - ablate) isolates the converter cost with the pin
// overhead cancelled out (classic paired-control design).
__device__ unsigned int fag_ablation_sink = 0;
#endif

using namespace cutlass::fmha::collective;

using namespace cute;

template<
    class ProblemShape,
    class Element,
    class ElementAcc,
    class TileShape,
    class Mask
>
struct Sm100FmhaBwdKernelTma2SmWarpSpecialized {

  using TileShapeQ = decltype(get<0>(TileShape{}));
  static_assert(std::is_same_v<TileShapeQ, _128>, "tile shape K must be 128");
  using TileShapeK = decltype(get<1>(TileShape{}));
  static_assert(std::is_same_v<TileShapeK, _128>, "tile shape K must be 128");
  using TileShapeDQK = decltype(get<2>(TileShape{}));
  using TileShapeDVO = decltype(get<2>(TileShape{}));

  using TmemAllocator = cute::TMEM::Allocator2Sm;
  struct TmemAllocation {
    static constexpr uint32_t kDK = 0;                     // TileShapeK x TileShapeDQK x acc
    static constexpr uint32_t kDV = kDK + TileShapeDQK{};  // TileShapeK x TileShapeDVO x acc
    static constexpr uint32_t kDP = kDV + TileShapeDVO{};  // TileShapeK x TileShapeQ   x inp
    static constexpr uint32_t kDS = kDP;                   // quantized dS for TS dK
    static constexpr uint32_t kS = kDP + max(TileShapeQ{}, TileShapeDQK{});
    static constexpr uint32_t kP = kS;
    // FA4: a cooperative dQ accumulator owns 64 rows per CTA, so it fits in
    // the upper half of the S/P region.  dS publication is delayed until the
    // following S tile has been loaded to registers before this region is used.
    static constexpr uint32_t kDQ = kS + TileShapeDQK{} / 2;
    static constexpr uint32_t kTotal = kS + TileShapeQ{};
  };

  static_assert(
      static_cast<int>(TmemAllocation::kTotal) <= TmemAllocator::Sm100TmemCapacityColumns,
      "using too much tmem"
  );
  static_assert(TmemAllocation::kDQ + TileShapeDQK{} / 2 <= TmemAllocation::kTotal,
                "FA4 dQ half-tile must fit in the S/P tail");

  enum class WarpRole {
    Empty = 0x0, Load = 0x1, Mma = 0x2, Compute = 0x3, Reduce = 0x4, Relay = 0x5
  };

  static constexpr unsigned long long kWarpAssignment = 0x0512'3333'3333'4444ull;
  static constexpr int kNumComputeWarps = 8;
  static constexpr int kNumReduceWarps = 4;
  CUTLASS_DEVICE WarpRole warp_idx_to_role(int warp_idx) {
    return static_cast<WarpRole>((kWarpAssignment >> (4 * warp_idx)) & 0xF);
  }

  struct RegisterAllocation {
#if defined(FAG_REGALLOC_P144)
    // P-softmax scheduling probe: keep the 512-register warpgroup pool exact,
    // but move eight registers from the reduction and MMA driver groups to
    // each of the two compute warpgroups.  No arithmetic or data type changes.
    static constexpr int kWarpgroup0 = 128;
    static constexpr int kWarpgroup1 = 144;
    static constexpr int kWarpgroup2 = 96;
#elif defined(FAG_REGALLOC_RED144_COMP136)
    // Keep the polynomial-exp compute allocation while returning the load/MMA
    // group's extra registers to the now-critical dQ writer.
    static constexpr int kWarpgroup0 = 144;
    static constexpr int kWarpgroup1 = 136;
    static constexpr int kWarpgroup2 = 96;
#elif defined(FAG_REGALLOC_RED148_COMP134)
    static constexpr int kWarpgroup0 = 148;
    static constexpr int kWarpgroup1 = 134;
    static constexpr int kWarpgroup2 = 96;
#elif defined(FAG_REGALLOC_RED152_COMP132)
    static constexpr int kWarpgroup0 = 152;
    static constexpr int kWarpgroup1 = 132;
    static constexpr int kWarpgroup2 = 96;
#elif defined(FAG_REGALLOC_COMP140)
    static constexpr int kWarpgroup0 = 136;
    static constexpr int kWarpgroup1 = 140;
    static constexpr int kWarpgroup2 = 96;
#elif defined(FAG_REGALLOC_FA4_CURRENT)
    // Match the current FA4 CuTe-DSL 2-CTA allocation for full attention:
    // move registers from the dQ writer to the two arithmetic warpgroups and
    // give the load/MMA/relay group enough headroom for its longer FP8 path.
    static constexpr int kWarpgroup0 = 136;
    static constexpr int kWarpgroup1 = 136;
    static constexpr int kWarpgroup2 = 104;
#else
    static constexpr int kWarpgroup0 = 160-8;
    static constexpr int kWarpgroup1 = 128;
    static constexpr int kWarpgroup2 = 96;
#endif
    static constexpr int kReduce = kWarpgroup0;
    static constexpr int kCompute = kWarpgroup1;
    static constexpr int kMma = kWarpgroup2;
#if defined(FAG_REGALLOC_P144) || defined(FAG_REGALLOC_RED144_COMP136) || \
    defined(FAG_REGALLOC_RED148_COMP134) || \
    defined(FAG_REGALLOC_RED152_COMP132) || defined(FAG_REGALLOC_COMP140) || \
    defined(FAG_REGALLOC_FA4_CURRENT)
    static constexpr int kEmpty = 24;
#else
    static constexpr int kEmpty = kWarpgroup2;
#endif
    static constexpr int kRelay = kWarpgroup2;
    static constexpr int kLoad = kWarpgroup2;

    static_assert(kWarpgroup0 + 2 * kWarpgroup1 + kWarpgroup2 <= 512);
  };

#ifdef FAG_EX2_EMU_STRIDE
  // FA4's degree-3 minimax 2^x approximation, evaluated for two FP32 values
  // with packed Blackwell instructions.  The coefficients and Cody-Waite
  // range reduction match flash_attn/cute/utils.py::e2e_asm2.  Its maximum
  // relative error is far below one FP8 E4M3 quantization step.
  CUTLASS_DEVICE static float2 ex2_emulation_2(float x, float y) {
    uint32_t out0;
    uint32_t out1;
    asm(
      "{\n\t"
      ".reg .f32 f1, f2, f3, f4, f5, f6, f7;\n\t"
      ".reg .b64 l1, l2, l3, l4, l5, l6, l7, l8, l9, l10;\n\t"
      ".reg .s32 r1, r2, r3, r4, r5, r6, r7, r8;\n\t"
      "max.ftz.f32 f1, %2, 0fC2FE0000;\n\t"
      "max.ftz.f32 f2, %3, 0fC2FE0000;\n\t"
      "mov.b64 l1, {f1, f2};\n\t"
      "mov.f32 f3, 0f4B400000;\n\t"
      "mov.b64 l2, {f3, f3};\n\t"
      "add.rm.ftz.f32x2 l7, l1, l2;\n\t"
      "sub.rn.ftz.f32x2 l8, l7, l2;\n\t"
      "sub.rn.ftz.f32x2 l9, l1, l8;\n\t"
      "mov.f32 f7, 0f3D9DF09D;\n\t"
      "mov.b64 l6, {f7, f7};\n\t"
      "mov.f32 f6, 0f3E6906A4;\n\t"
      "mov.b64 l5, {f6, f6};\n\t"
      "mov.f32 f5, 0f3F31F519;\n\t"
      "mov.b64 l4, {f5, f5};\n\t"
      "mov.f32 f4, 0f3F800000;\n\t"
      "mov.b64 l3, {f4, f4};\n\t"
      "fma.rn.ftz.f32x2 l10, l9, l6, l5;\n\t"
      "fma.rn.ftz.f32x2 l10, l10, l9, l4;\n\t"
      "fma.rn.ftz.f32x2 l10, l10, l9, l3;\n\t"
      "mov.b64 {r1, r2}, l7;\n\t"
      "mov.b64 {r3, r4}, l10;\n\t"
      "shl.b32 r5, r1, 23;\n\t"
      "add.s32 r7, r5, r3;\n\t"
      "shl.b32 r6, r2, 23;\n\t"
      "add.s32 r8, r6, r4;\n\t"
      "mov.b32 %0, r7;\n\t"
      "mov.b32 %1, r8;\n\t"
      "}\n"
      : "=r"(out0), "=r"(out1)
      : "f"(x), "f"(y));
    return make_float2(__uint_as_float(out0), __uint_as_float(out1));
  }
#endif

  using ArchTag = cutlass::arch::Sm100;

  // [2SM] 2-CTA cooperative cluster. The pair {2c,2c+1} cooperates on ONE K-block:
  // cooperative cta_group::2 MMAs (M=256 split in the Q-row output dim), each CTA
  // staging HALF of operand B (K/V) in its own smem (SMEM-traffic halving, FA4
  // §3.2.3). FA4 paper §3.2.
  using ClusterShape = Shape<_2, _1, _1>;
  using Schedule = cutlass::gemm::KernelTmaWarpSpecialized2SmSm100;
  using AtomThrShape_MNK = Shape<_2, _1, _1>;
  static constexpr int kClusterSize = cute::size(ClusterShape{});

  static constexpr int MinBlocksPerMultiprocessor = 1;
  static constexpr int kNumWarps = kNumComputeWarps + kNumReduceWarps + 4;
  static constexpr int MaxThreadsPerBlock = NumThreadsPerWarp * kNumWarps;

  static constexpr int Alignment = 128 / sizeof_bits_v<Element>;
  static constexpr int kStages = 2;

  using TensorStrideContiguousK = Stride<int, _1, Stride<Stride<int,int>, int>>;
  using TensorStrideContiguousMN = Stride<_1, int, Stride<Stride<int,int>, int>>;
  using TensorStrideContiguousK_GQA = Stride<int, _1, Stride<Stride<_0,int>, int>>;
  using TensorStrideContiguousMN_GQA = Stride<_1, int, Stride<Stride<_0,int>, int>>;

  // [2SM] DECOUPLE the cooperative MMA tile (M=256, fed ONLY to the
  // CollectiveBuilder so it builds a cta_group::2 / AtomThrShape=2 atom) from the
  // PER-CTA TileShape (M=128, used for ALL load/smem/index/softmax/store code).
  using MmaTileShapeKQ  = Shape<cute::_256, TileShapeQ,   TileShapeDQK>;
  using MmaTileShapeVDO = Shape<cute::_256, TileShapeQ,   TileShapeDVO>;
  using MmaTileShapePDO = Shape<cute::_256, TileShapeDVO, TileShapeQ>;
  using MmaTileShapeDSQ = Shape<cute::_256, TileShapeDQK, TileShapeQ>;
  // FA4 dQ: the cluster reduces across 256 KV rows while each CTA owns 64 of
  // the cooperative 128 Q output rows.
  using MmaTileShapeDSK = Shape<cute::_128, TileShapeDQK, cute::_256>;

  // compute S
  using CollectiveMmaKQ = typename cutlass::gemm::collective::CollectiveBuilder<
      cutlass::arch::Sm100, cutlass::arch::OpClassTensorOp,
      Element, TensorStrideContiguousK_GQA, Alignment,
      Element, TensorStrideContiguousK, Alignment,
      ElementAcc,
      MmaTileShapeKQ,
      ClusterShape, cutlass::gemm::collective::StageCount<kStages>,
      Schedule>::CollectiveOp;
  using TileShapeKQ = Shape<TileShapeK, TileShapeQ, TileShapeDQK>;
  using TiledMmaKQ = typename CollectiveMmaKQ::TiledMma;

  // compute dP
  using CollectiveMmaVDO = typename cutlass::gemm::collective::CollectiveBuilder<
      cutlass::arch::Sm100, cutlass::arch::OpClassTensorOp,
      Element, TensorStrideContiguousK_GQA, Alignment,
      Element, TensorStrideContiguousK, Alignment,
      ElementAcc,
      MmaTileShapeVDO,
      ClusterShape, cutlass::gemm::collective::StageCount<kStages>,
      Schedule>::CollectiveOp;
  using TileShapeVDO = Shape<TileShapeK, TileShapeQ, TileShapeDVO>;
  using TiledMmaVDO = typename CollectiveMmaVDO::TiledMma;

  // compute dV
  using CollectiveMmaPDO = typename cutlass::gemm::collective::CollectiveBuilder<
      cutlass::arch::Sm100, cutlass::arch::OpClassTensorOp,
      // needs to match ordering of S calculation
      Element, TensorStrideContiguousK, Alignment,
      Element, TensorStrideContiguousMN, Alignment,
      ElementAcc,
      MmaTileShapePDO,
      ClusterShape, cutlass::gemm::collective::StageCount<kStages>,
      Schedule>::CollectiveOp;
  using TileShapePDO = Shape<TileShapeK, TileShapeDVO, TileShapeQ>;
  using TiledMmaPDO = decltype(to_tiled_mma_sm100_ts(typename CollectiveMmaPDO::TiledMma{}));

  // compute dK
  using CollectiveMmaDSQ = typename cutlass::gemm::collective::CollectiveBuilder<
      cutlass::arch::Sm100, cutlass::arch::OpClassTensorOp,
      // somewhat arbitrary since we dump to smem, need to agree with the next one
      Element, TensorStrideContiguousK , Alignment,
      Element, TensorStrideContiguousMN, Alignment,
      ElementAcc,
      MmaTileShapeDSQ,
      ClusterShape, cutlass::gemm::collective::StageCount<kStages>,
      Schedule>::CollectiveOp;
  using TileShapeDSQ = Shape<TileShapeK, TileShapeDQK, TileShapeQ>;
  // FA4: dK consumes quantized dS from TMEM and the physically transposed Qt
  // representation from SMEM (TS MMA).
  using TiledMmaDSQ = decltype(to_tiled_mma_sm100_ts(typename CollectiveMmaDSQ::TiledMma{}));

  // compute dQ
  using CollectiveMmaDSK = typename cutlass::gemm::collective::CollectiveBuilder<
      cutlass::arch::Sm100, cutlass::arch::OpClassTensorOp,
      // somewhat arbitrary since we dump to smem, need to agree with the previous one
      Element, TensorStrideContiguousMN, Alignment,
      Element, TensorStrideContiguousMN_GQA, Alignment,
      ElementAcc,
      MmaTileShapeDSK,
      ClusterShape, cutlass::gemm::collective::StageCount<kStages>,
      Schedule>::CollectiveOp;
  using TileShapeDSK = Shape<cute::_64, TileShapeDQK, cute::_256>;
  using TiledMmaDSK = typename CollectiveMmaDSK::TiledMma;

  // [DBG] confirm each GEMM's atom is 2-SM (AtomThrID size == 2). If any is 1,
  // the CollectiveBuilder fell back to 1-SM (the plain-FP8 2SM constraint was
  // not met for that GEMM's M/N/layout), which would emit cta_group::1 MMA.
#ifdef BWD_2SM_DEBUG
  static_assert(cute::size(typename TiledMmaKQ::AtomThrID{})  == 2, "[DBG] KQ not 2-SM!");
  static_assert(cute::size(typename TiledMmaVDO::AtomThrID{}) == 2, "[DBG] VDO not 2-SM!");
  static_assert(cute::size(typename TiledMmaDSQ::AtomThrID{}) == 2, "[DBG] DSQ not 2-SM!");
  static_assert(cute::size(typename TiledMmaDSK::AtomThrID{}) == 2, "[DBG] DSK not 2-SM!");
#endif

  // pipelines are named Pipeline<Producer><Consumer><Resource>
  // [2SM] HARDWARE RULE: a kernel must use uniformly cta_group::1 OR cta_group::2,
  // never mixed. So ALL UMMA pipelines carry AtomThrShape<_2,_1,_1>.
  static constexpr int kStagesComputeSmem = 1;
  using PipelineLoadMmaQ = PipelineTmaUmmaAsync<2, ClusterShape, AtomThrShape_MNK>;
  using PipelineLoadMmaQT = PipelineTmaUmmaAsync<2, ClusterShape, AtomThrShape_MNK>;
  using PipelineLoadMmaKT = PipelineTmaUmmaAsync<1, ClusterShape, AtomThrShape_MNK>;
  using PipelineLoadMmaDO = PipelineTmaUmmaAsync<1, ClusterShape, AtomThrShape_MNK>;
  using PipelineLoadComputeLSE = PipelineAsync<1>;
  using PipelineLoadComputeSumOdO = PipelineAsync<1>;
  using PipelineMmaComputeS = PipelineUmmaAsync<1, AtomThrShape_MNK>;
  using PipelineMmaComputeDP = PipelineUmmaAsync<1, AtomThrShape_MNK>;
  using PipelineMmaReduceDQ = PipelineUmmaAsync<1, AtomThrShape_MNK>;
  using PipelineComputeMmaP = PipelineUmmaConsumerAsync<1, AtomThrShape_MNK>;
  using PipelineComputeMmaDS = PipelineUmmaConsumerAsync<kStagesComputeSmem, AtomThrShape_MNK>;
  using PipelineMmaComputeDKDV = PipelineUmmaAsync<2, AtomThrShape_MNK>;
#if defined(FAG_DQ_BATCHED_TMA)
  // Keep four independent FP32 dQ publication buffers resident so the reduce
  // warps can acquire and populate the complete 64x128 tile behind one pair of
  // barriers.  The four reduce-add operations and all data types are unchanged.
  static constexpr int kStagesReduceTmaStore = 4;
#elif defined(FAG_DQ_DEEP_SERIAL) || defined(FAG_DQ_HALF_SPLIT)
  // FAG_DQ_DEEP_SERIAL: keep the per-stage serial acquire/STS/TMA issue order
  // (the 2-stage rotation already software-pipelines TMA(i) under
  // staging(i+1)) but deepen the rotation to four buffers so no intra-
  // iteration staging ever waits for an in-flight TMA completion; the waits
  // move to the next iteration's acquires and overlap the whole mainloop.
  static constexpr int kStagesReduceTmaStore = 4;
#else
  static constexpr int kStagesReduceTmaStore = 2;
#endif
  using PipelineReduceTmaStore = PipelineTmaStore<kStagesReduceTmaStore>;

  struct PipelineStorage {
    alignas(16) typename PipelineLoadMmaQ::SharedStorage load_mma_q;
    alignas(16) typename PipelineLoadMmaQT::SharedStorage load_mma_qt;
    alignas(16) typename PipelineLoadMmaKT::SharedStorage load_mma_kt;
    alignas(16) typename PipelineLoadMmaDO::SharedStorage load_mma_do;
    alignas(16) typename PipelineLoadComputeLSE::SharedStorage load_compute_lse;
    alignas(16) typename PipelineLoadComputeSumOdO::SharedStorage load_compute_sum_odo;
    alignas(16) typename PipelineMmaComputeS::SharedStorage mma_compute_s;
    alignas(16) typename PipelineMmaComputeDP::SharedStorage mma_compute_dp;
    alignas(16) typename PipelineMmaReduceDQ::SharedStorage mma_reduce_dq;
    alignas(16) typename PipelineComputeMmaP::SharedStorage compute_mma_p;
    alignas(16) typename PipelineComputeMmaDS::SharedStorage compute_mma_ds;
    alignas(16) typename PipelineMmaComputeDKDV::SharedStorage mma_compute_dkdv;
    // [2SM] cross-CTA handshake so the peer's tcgen05.alloc.cta_group::2 is
    // cluster-visible before the leader issues the cooperative MMA (which writes
    // the peer's TMEM). Leader waits for 1 remote arrive.
    alignas(16) cutlass::arch::ClusterBarrier tmem_alloc_ready;
  };

  template<class Layout, class Stages = _1>
  static CUTE_DEVICE constexpr auto restage(Layout const& layout, Stages stages = {}) {
    return composition(layout, make_tuple(_, _, _, make_layout(stages)));
  }

  using SmemLayoutK = decltype(restage(typename CollectiveMmaKQ::SmemLayoutA{}));
  using SmemLayoutV = decltype(restage(typename CollectiveMmaVDO::SmemLayoutA{}));
  using SmemLayoutQ = decltype(restage(typename CollectiveMmaKQ::SmemLayoutB{}, _2{}));
  using SmemLayoutDO = decltype(restage(typename CollectiveMmaVDO::SmemLayoutB{}, _1{}));
  using SmemLayoutDS = decltype(restage(typename CollectiveMmaDSK::SmemLayoutA{}, Int<kStagesComputeSmem>{}));
  using SmemLayoutLSE = Layout<Shape<TileShapeQ, _1>>;
  using SmemLayoutSumOdO = Layout<Shape<TileShapeQ, _1>>;

  using SmemLayoutQT = decltype(restage(typename CollectiveMmaDSQ::SmemLayoutB{}, _2{}));
  using SmemLayoutKT = decltype(restage(typename CollectiveMmaDSK::SmemLayoutB{}));
  using SmemLayoutDSHalf = decltype(tile_to_shape(
      typename CollectiveMmaDSK::SmemLayoutAtomA{}, Shape<_64, _128>{}, Step<_2, _1>{}));
  using SmemLayoutDST = decltype(restage(typename CollectiveMmaDSQ::SmemLayoutA{}, Int<kStagesComputeSmem>{}));
  using SmemLayoutDOT = decltype(restage(typename CollectiveMmaPDO::SmemLayoutB{}, _1{}));

#if defined(FAG_DQ_STORE_TILE_128)
  // Fixed-shape experiment: stage the complete 64x128 FP32 dQ tile so each
  // iteration needs one TMA reduce-add store instead of four.  This changes
  // only the store staging granularity; the accumulator and output types stay
  // FP32 and FP8 respectively.
  using TileShapeDQ = std::conditional_t<int(TileShapeDQK{}) == 128, _128, _64>;
#elif defined(FAG_DQ_STORE_TILE_64)
  // Two 64-column stages, halving the TMA-store/barrier cadence.
  using TileShapeDQ = _64;
#else
  using TileShapeDQ = _32;
#endif
#if defined(FAG_DQ_BF16_STAGING)
  // FAG_DQ_BF16_STAGING: dQ partial sums are staged to SMEM and reduced into
  // the gmem workspace in BF16 instead of FP32.  This halves the staging STS
  // instruction count, SMEM wavefronts and workspace/TMA-reduce traffic of
  // the #1 exposed bottleneck (the -8.5% STS staging of the dQ drain; see
  // ABLATION_V3_OPT_BASE.md).  Final dQ is FP8 e4m3 regardless; accuracy is
  // empirically gated by --verify / --compare-gradients.
  using ElemDQAcc = cutlass::bfloat16_t;
#else
  using ElemDQAcc = ElementAcc;
#endif
  using SmemAtomDQ = decltype(cutlass::gemm::collective::detail::sm100_smem_selector<
      cute::UMMA::Major::K, ElemDQAcc, TileShapeQ, TileShapeDQ
  >());
  using SmemShapeDQ = Shape<_64, TileShapeDQ, Int<kStagesReduceTmaStore>>;
  using SmemLayoutDQ = decltype(tile_to_shape(SmemAtomDQ{}, SmemShapeDQ{}, Step<_2, _1, _3>{}));

  struct TensorStorage {
    alignas(2048) cute::array<Element, cute::cosize_v<SmemLayoutK>> smem_k;
    alignas(2048) cute::array<Element, cute::cosize_v<SmemLayoutKT>> smem_k_t;
    alignas(2048) cute::array<Element, cute::cosize_v<SmemLayoutV>> smem_v;
    // Q and Qt are independent physical representations. Reinterpreting a KQ-B
    // tile with the DSQ-B layout is not a transpose and corrupts dK.
    alignas(2048) cute::array<Element, cute::cosize_v<SmemLayoutQ>> smem_q;
    alignas(2048) cute::array<Element, cute::cosize_v<SmemLayoutQT>> smem_q_t;
    union {
      alignas(2048) cute::array<Element, cute::cosize_v<SmemLayoutDO>> smem_do;
      alignas(2048) cute::array<Element, cute::cosize_v<SmemLayoutDOT>> smem_do_t;
    };
    union {
      alignas(2048) cute::array<Element, cute::cosize_v<SmemLayoutDS>> smem_ds;
      alignas(2048) cute::array<Element, cute::cosize_v<SmemLayoutDST>> smem_ds_t;
    };
    alignas(2048) cute::array<Element, cute::cosize_v<SmemLayoutDSHalf>> smem_ds_xchg;
    // [dV-axis-fix] Dedicated dO buffer for the dV (PDO) MMA. Lives alongside
    // smem_do (which feeds dP/VDO) so each MMA reads dO laid out for its own N-axis.
    alignas(2048) cute::array<Element, cute::cosize_v<SmemLayoutDOT>> smem_do_dv;
    alignas(1024) cute::array<ElemDQAcc, cute::cosize_v<SmemLayoutDQ>> smem_dq;
    alignas(16) cute::array<ElementAcc, cute::cosize_v<SmemLayoutLSE>> smem_lse;
    alignas(16) cute::array<ElementAcc, cute::cosize_v<SmemLayoutSumOdO>> smem_sum_odo;
    alignas(16) cutlass::arch::ClusterTransactionBarrier ds_full;
    alignas(16) cutlass::arch::ClusterBarrier ds_leader;
#if defined(FAG_OPT_STASYNC_XCHG)
    // Completion tracking for the st.async exchange-buffer stores (see
    // stasync_128): replaces the generic->async proxy fence that the plain
    // STS path needs before the DSMEM bulk copy.
    alignas(16) cutlass::arch::ClusterTransactionBarrier xchg_tx;
#endif
#if defined(FAG_SPLIT_DK_DQ_READY)
    alignas(16) cutlass::arch::ClusterBarrier dk_ready;
#endif
  };

  static constexpr int kTransactionsBytesLoadQ = cutlass::bits_to_bytes(cosize(take<0,3>(SmemLayoutQ{})) * cute::sizeof_bits_v<Element>);
  static constexpr int kTransactionsBytesLoadQT = cutlass::bits_to_bytes(cosize(take<0,3>(SmemLayoutQT{})) * cute::sizeof_bits_v<Element>);
  static constexpr int kTransactionsBytesLoadKT = cutlass::bits_to_bytes(cosize(take<0,3>(SmemLayoutKT{})) * cute::sizeof_bits_v<Element>);
  static constexpr int kTransactionsBytesDSExchange = cutlass::bits_to_bytes(
      cute::cosize_v<SmemLayoutDSHalf> * cute::sizeof_bits_v<Element>);
  static_assert(kTransactionsBytesDSExchange == 8192);
  static constexpr int kTransactionsBytesLoadDO = cutlass::bits_to_bytes(cosize(take<0,3>(SmemLayoutDO{})) * cute::sizeof_bits_v<Element>);

  static constexpr int kTransactionsBytesLoadK = cutlass::bits_to_bytes(cosize(take<0,3>(SmemLayoutK{})) * cute::sizeof_bits_v<Element>);
  static constexpr int kTransactionsBytesLoadV = cutlass::bits_to_bytes(cosize(take<0,3>(SmemLayoutV{})) * cute::sizeof_bits_v<Element>);

  struct SharedStorage {
    TensorStorage tensors;
    PipelineStorage pipelines;
    uint32_t tmem_base_ptr;
  };

  // this is tight enough that it won't work with sizeof due to padding for alignment
  static constexpr int SharedStorageSize = offsetof(SharedStorage, tmem_base_ptr) + sizeof(uint32_t);
  static_assert(SharedStorageSize <= cutlass::arch::sm100_smem_capacity_bytes, "using too much smem");

  using TensorStride = TensorStrideContiguousK;  // S D (H B)
  using TensorStride_GQA = TensorStrideContiguousK_GQA;
  using RowTensorStride = Stride<_1, Stride<Stride<int, int>, int>>;    // S (H B)

  struct MainloopArguments {
    const Element* ptr_q;
    TensorStride stride_q;
    const Element* ptr_k;
    TensorStride_GQA stride_k;
    const Element* ptr_v;
    TensorStride_GQA stride_v;
    const Element* ptr_do;
    TensorStride stride_do;

    const ElementAcc* ptr_lse;
    RowTensorStride stride_lse;

    const ElementAcc* ptr_sum_odo;
    RowTensorStride stride_sum_odo;

    ElemDQAcc* ptr_dq_acc;
    TensorStride stride_dq_acc;

    ElementAcc softmax_scale = 1.0f / sqrtf(TileShapeDQK{});
  };

  using TMA_K = typename CollectiveMmaKQ::Params::TMA_A;
  using TMA_V = typename CollectiveMmaVDO::Params::TMA_A;
  using TMA_Q = typename CollectiveMmaKQ::Params::TMA_B;
  using TMA_QT = typename CollectiveMmaDSQ::Params::TMA_B;
  using TMA_KT = typename CollectiveMmaDSK::Params::TMA_B;
  using TMA_DO = typename CollectiveMmaVDO::Params::TMA_B;
  // [dV-axis-fix] Dedicated dO TMA built from the PDO collective so its box is
  // split along PDO's N-axis (DVO), not VDO's N-axis (Q). See FIXNOTE_dV_axis_mismatch.md.
  using TMA_DO_DV = typename CollectiveMmaPDO::Params::TMA_B;

  using TMA_DQ = decltype(make_tma_copy(SM90_TMA_REDUCE_ADD{},
      make_tensor((const ElemDQAcc*)nullptr, make_shape(1, 1, make_shape(make_shape(1,1), 1)), TensorStride{}),
      SmemLayoutDQ{}(_, _, _0{})
  ));

  struct MainloopParams {
    TMA_K tma_load_k;
    TMA_V tma_load_v;
    TMA_Q tma_load_q;
    TMA_QT tma_load_qt;
    TMA_KT tma_load_kt;
    TMA_DO tma_load_do;
    TMA_DO_DV tma_load_do_dv;   // [dV-axis-fix] dO loaded for the dV (PDO) MMA, split along DVO
    TMA_DQ tma_red_dq;
  };

  struct EpilogueArguments {
    Element* ptr_dk;
    TensorStride_GQA stride_dk;
    Element* ptr_dv;
    TensorStride_GQA stride_dv;
  };

  struct Arguments {
    ProblemShape problem_shape;
    MainloopArguments mainloop;
    EpilogueArguments epilogue;
    KernelHardwareInfo hw_info;
  };

  struct Params {
    ProblemShape problem_shape;
    MainloopArguments mainloop;
    MainloopParams mainloop_params;
    EpilogueArguments epilogue;
    KernelHardwareInfo hw_info;
  };


  static bool can_implement(Arguments const& args) {
#ifdef BWD_2SM_DEBUG
    // [DBG] report per-CTA SMEM layout cosizes (elements) so we can see what the
    // 2-SM atom halved. Host-side print.
    printf("[DBG smem] K=%zu V=%zu Q=%zu DO=%zu | KT=%zu QT=%zu DST=%zu DOT=%zu | (elements; x%d bytes)\n",
           (size_t)cute::cosize_v<SmemLayoutK>, (size_t)cute::cosize_v<SmemLayoutV>,
           (size_t)cute::cosize_v<SmemLayoutQ>, (size_t)cute::cosize_v<SmemLayoutDO>,
           (size_t)cute::cosize_v<SmemLayoutKT>, (size_t)cute::cosize_v<SmemLayoutQT>,
           (size_t)cute::cosize_v<SmemLayoutDST>, (size_t)cute::cosize_v<SmemLayoutDOT>,
           (int)sizeof(Element));
    // Keep the expected-byte accounting visible in the diagnostic target.  Each
    // value is one physical TMA transaction per CTA; a cooperative complete-tx
    // arrives at the leader barrier with kClusterSize times that value.
    printf("[DBG tma-bytes] Q=%d K=%d QT=%d KT=%d DO=%d V=%d DS-peer=%d | cluster=%d | "
           "QK-stage=%d QT-stage=%d KT-stage=%d DO-stage=%d\n",
           kTransactionsBytesLoadQ, kTransactionsBytesLoadK,
           kTransactionsBytesLoadQT, kTransactionsBytesLoadKT,
           kTransactionsBytesLoadDO, kTransactionsBytesLoadV,
           kTransactionsBytesDSExchange, kClusterSize,
           (kTransactionsBytesLoadQ + kTransactionsBytesLoadK) * kClusterSize,
           kTransactionsBytesLoadQT * kClusterSize,
           kTransactionsBytesLoadKT * kClusterSize,
           kTransactionsBytesLoadDO * kClusterSize);
    // [dV-debug] print the exact MMA tile shapes + PDO fragment dims to see why dV
    // only fills DVO 0:64 (N-split boundary). Host-side compile-time print.
    printf("[DBG pdo] MmaTileShapePDO M=%d N=%d K=%d | SmemLayoutDOT cosize=%zu SmemLayoutDO cosize=%zu\n",
           (int)cute::size<0>(MmaTileShapePDO{}), (int)cute::size<1>(MmaTileShapePDO{}),
           (int)cute::size<2>(MmaTileShapePDO{}),
           (size_t)cute::cosize_v<SmemLayoutDOT>, (size_t)cute::cosize_v<SmemLayoutDO>);
    printf("[DBG pdo] AtomThrPDO=%d | TileShapePDO K=%d DVO=%d Q=%d\n",
           (int)cute::size(typename TiledMmaPDO::AtomThrID{}),
           (int)cute::size<0>(TileShapePDO{}), (int)cute::size<1>(TileShapePDO{}),
           (int)cute::size<2>(TileShapePDO{}));
    // [dV-debug] SmemLayoutDOT (PDO B-op) vs SmemLayoutDO (VDO B-op): print the
    // product of mode-0 (M-ish) and mode-1 (N-ish) to see which axis the 2-SM atom
    // halved. cosize=8192 each; need to know if it's 128x64 or 64x128.
    printf("[DBG pdo-layout] DOT rank=%zu | DO rank=%zu\n",
           cute::rank_v<SmemLayoutDOT>, cute::rank_v<SmemLayoutDO>);
    printf("[DBG pdo-layout] PDO::SmemLayoutB rank=%zu cosize=%zu | VDO::SmemLayoutB rank=%zu cosize=%zu\n",
           cute::rank_v<typename CollectiveMmaPDO::SmemLayoutB>,
           (size_t)cute::cosize_v<typename CollectiveMmaPDO::SmemLayoutB>,
           cute::rank_v<typename CollectiveMmaVDO::SmemLayoutB>,
           (size_t)cute::cosize_v<typename CollectiveMmaVDO::SmemLayoutB>);
    // [dV-debug] PDO::SmemLayoutB mode sizes (full per-CTA, before restage) to see
    // which axis the 2-SM atom halves (N=DVO vs K=Q).
    {
      auto layout_pdo_b = typename CollectiveMmaPDO::SmemLayoutB{};
      printf("[DBG pdo-bshape] PDO::SmemLayoutB mode sizes: ");
      cute::for_each(layout_pdo_b.shape(), [](auto s){ printf("%zu ", (size_t)cute::size(s)); });
      printf("\n");
    }
    // [dV-debug] dump the per-mode sizes of SmemLayoutDOT to see N vs K halving.
    // SmemLayoutDOT is rank-4: print flat product of each mode.
    {
      auto layout_dot = SmemLayoutDOT{};
      auto layout_do  = SmemLayoutDO{};
      printf("[DBG pdo-shape] DOT mode sizes: ");
      cute::for_each(layout_dot.shape(), [](auto s){ printf("%zu ", (size_t)cute::size(s)); });
      printf("| DO mode sizes: ");
      cute::for_each(layout_do.shape(), [](auto s){ printf("%zu ", (size_t)cute::size(s)); });
      printf("\n");
    }
    {
      auto layout_ds = SmemLayoutDS{};
      auto layout_dsh = SmemLayoutDSHalf{};
      auto layout_kt = SmemLayoutKT{};
      printf("[DBG dq-shape] DS modes: ");
      cute::for_each(layout_ds.shape(), [](auto s){ printf("%zu ", (size_t)cute::size(s)); });
      printf("| DSHalf modes: ");
      cute::for_each(layout_dsh.shape(), [](auto s){ printf("%zu ", (size_t)cute::size(s)); });
      printf("| KT modes: ");
      cute::for_each(layout_kt.shape(), [](auto s){ printf("%zu ", (size_t)cute::size(s)); });
      printf("| cosize DS=%zu half=%zu KT=%zu\n",
             (size_t)cute::cosize_v<SmemLayoutDS>,
             (size_t)cute::cosize_v<SmemLayoutDSHalf>,
             (size_t)cute::cosize_v<SmemLayoutKT>);
    }
#endif
    auto [Q, K, D, D_VO, HB] = args.problem_shape;
    auto [H, B] = HB;
    auto [H_R, H_K] = H;
    // This cooperative FA4 path is intentionally narrow.  Other variants are
    // handled by the existing 1-SM kernels; silently launching this 2-SM
    // schedule for them can leave an unmatched cluster peer or use layouts
    // whose DSMEM corner turn has not been defined.
    if constexpr (is_variable_length_v<decltype(Q)> ||
                  is_variable_length_v<decltype(K)> ||
                  !std::is_same_v<Mask, NoMask>) {
      return false;
    }
    if (Q <= 0 || K <= 0 || D <= 0 || D_VO <= 0 || H_R <= 0 || H_K <= 0 || B <= 0) {
      return false;
    }
    if (D != 128 || D_VO != 128 || H_R != 1 || Q % 128 != 0 || K % 256 != 0) {
      return false;
    }
    return true;
  }


  static Status initialize_workspace(Arguments const&, void*, cudaStream_t) {
    return Status::kSuccess;
  }


  static Params to_underlying_arguments(Arguments const& args, void*) {
    auto [Q_, K_, D, D_VO, HB] = args.problem_shape;
    int Q = Q_;
    int K = K_;

    if constexpr (is_variable_length_v<decltype(Q_)>) {
      Q = Q_.total_length;
    }
    if constexpr (is_variable_length_v<decltype(K_)>) {
      K = K_.total_length;
    }

    auto params_kq = CollectiveMmaKQ::to_underlying_arguments(
      make_shape(K, Q, D, HB),
      typename CollectiveMmaKQ::Arguments {
        args.mainloop.ptr_k, args.mainloop.stride_k,
        args.mainloop.ptr_q, args.mainloop.stride_q,
      }, /*workspace=*/nullptr);

    auto params_vdo = CollectiveMmaVDO::to_underlying_arguments(
      make_shape(K, Q, D_VO, HB),
      typename CollectiveMmaVDO::Arguments {
        args.mainloop.ptr_v, args.mainloop.stride_v,
        args.mainloop.ptr_do, args.mainloop.stride_do,
      }, /*workspace=*/nullptr);

    // FA4 Qt representation: the same global Q allocation is viewed as
    // (D,Q,HB), with D contiguous, and loaded using DSQ's B descriptor/layout.
    TensorStrideContiguousMN stride_q_mn = make_stride(
        _1{}, get<0>(args.mainloop.stride_q), get<2>(args.mainloop.stride_q));
    auto params_dsq = CollectiveMmaDSQ::to_underlying_arguments(
      make_shape(K, D, Q, HB),
      typename CollectiveMmaDSQ::Arguments {
        args.mainloop.ptr_q, args.mainloop.stride_q, // unused A descriptor
        args.mainloop.ptr_q, stride_q_mn,            // B = physical Qt load
      }, /*workspace=*/nullptr);

    TensorStrideContiguousMN_GQA stride_k_mn = make_stride(
        _1{}, get<0>(args.mainloop.stride_k), get<2>(args.mainloop.stride_k));
    auto params_dsk = CollectiveMmaDSK::to_underlying_arguments(
      make_shape(Q, D, K, HB),
      typename CollectiveMmaDSK::Arguments {
        args.mainloop.ptr_q, stride_q_mn, // unused A descriptor
        args.mainloop.ptr_k, stride_k_mn, // B = physical Kt load
      }, /*workspace=*/nullptr);

    // [dV-axis-fix] Build a dedicated dO TMA from the PDO collective, so its box is
    // split along PDO's N-axis (DVO). PDO's B-op tag expects MN-major stride
    // (TensorStrideContiguousMN = Stride<_1,int,...>), but dO is stored K-major
    // (TensorStrideContiguousK = Stride<int,_1,...>). We reinterpret dO's gmem layout
    // as MN-major here (D_VO contiguous as the N-mode) to match the PDO B-op tag.
    // See FIXNOTE_dV_axis_mismatch.md lines 45-56.
    auto [H_pdo, B_pdo] = HB;
    auto [H_R_pdo, H_K_pdo] = H_pdo;
    TensorStrideContiguousMN stride_do_mn = make_stride(
        _1{}, D_VO,
        make_stride(make_stride(D_VO*Q, D_VO*Q*H_R_pdo), B_pdo == 1 ? 0 : D_VO*Q*H_R_pdo*H_K_pdo));
    auto params_pdo = CollectiveMmaPDO::to_underlying_arguments(
      make_shape(K, D_VO, Q, HB),
      typename CollectiveMmaPDO::Arguments {
        args.mainloop.ptr_do, args.mainloop.stride_do,   // dummy A (K-major, matches PDO A tag)
        args.mainloop.ptr_do, stride_do_mn,              // B = dO reinterpreted MN-major
      }, /*workspace=*/nullptr);

    TMA_DQ tma_red_dq = make_tma_copy(
        SM90_TMA_REDUCE_ADD{},
        make_tensor(args.mainloop.ptr_dq_acc, make_shape(Q_, D, HB), args.mainloop.stride_dq_acc),
        SmemLayoutDQ{}(_, _, _0{})
    );

    return Params{
      args.problem_shape,
      args.mainloop,
      MainloopParams{
        params_kq.tma_load_a,
        params_vdo.tma_load_a,
        params_kq.tma_load_b,
        params_dsq.tma_load_b,
        params_dsk.tma_load_b,
        params_vdo.tma_load_b,
        params_pdo.tma_load_b,
        tma_red_dq
      },
      args.epilogue,
      args.hw_info
    };
  }


  template<class T>
  static CUTLASS_DEVICE auto quantize(T const& input) {
    constexpr int AlignmentS = 4;
    auto output = make_tensor<Element>(shape(input));
    auto input_vec = recast<Array<ElementAcc, AlignmentS>>(input);
    auto output_vec = recast<Array<Element, AlignmentS>>(output);

#if defined(FAG_ABLATE_QUANT_V3)
    // Minimal-instrument quant ablation. Replace the converter with a 3-XOR
    // bit-reduction of its four FP32 inputs and emit the result as the FP8
    // pattern: every producer keeps REAL dataflow (nothing DCE-able at any
    // compiler stage), the output is a RUNTIME value (downstream pack/SMEM/
    // TMEM consumers do not fold), and no branches, constants, or code bloat
    // are introduced. Removes exactly the converter chain (F2FP + its
    // range-scaling FMULs); adds 3 LOP3 per 4-element group.
    CUTLASS_PRAGMA_UNROLL
    for (int i = 0; i < size(input_vec); i++) {
      Array<ElementAcc, AlignmentS> v = input_vec(i);
      union { uint32_t u; Array<Element, AlignmentS> a; } bits;
      bits.u = __float_as_uint(v[0]) ^ __float_as_uint(v[1]) ^
               __float_as_uint(v[2]) ^ __float_as_uint(v[3]);
      output_vec(i) = bits.a;
    }
#elif defined(FAG_ABLATE_QUANT_V2)
    // Rigorous quant ablation (fixes the contaminated FAG_ABLATE_QUANT): the
    // FP32->e4m3 converter is replaced by the same-typed zero pattern, and the
    // FP32 inputs are pinned by an opaque NEVER-TAKEN branch performing a real
    // global read-modify-write. blockIdx.x is always < gridDim.x, but neither
    // is a compile-time constant, so the compiler cannot prove the branch dead
    // and must retain the full producer chain (exp2, scale-FMA, softmax, and
    // the converter's own range-scaling FMUL inputs) all the way into SASS.
    // Hot-path cost: the loop-invariant compare is hoisted; the sink block
    // never executes.
    CUTLASS_PRAGMA_UNROLL
    for (int i = 0; i < size(input_vec); i++) {
      Array<ElementAcc, AlignmentS> v = input_vec(i);
      output_vec(i) = Array<Element, AlignmentS>{};
      if (blockIdx.x == gridDim.x) {
        fag_ablation_sink += __float_as_uint(v[0]) ^ __float_as_uint(v[1]) ^
                             __float_as_uint(v[2]) ^ __float_as_uint(v[3]);
      }
    }
#elif defined(FAG_CTRL_QUANTPIN)
    // Paired control for FAG_ABLATE_QUANT_V2: the converter is KEPT and the
    // identical never-taken pin branch is added. Comparing this against the
    // V2 ablation cancels the pin's own overhead and isolates the converter's
    // true cost. Base path timing impact = pin overhead only (measured).
    cutlass::NumericArrayConverter<Element, ElementAcc, AlignmentS> epilogue_op;
    CUTLASS_PRAGMA_UNROLL
    for (int i = 0; i < size(input_vec); i++) {
      Array<ElementAcc, AlignmentS> v = input_vec(i);
      output_vec(i) = epilogue_op(input_vec(i));
      if (blockIdx.x == gridDim.x) {
        fag_ablation_sink += __float_as_uint(v[0]) ^ __float_as_uint(v[1]) ^
                             __float_as_uint(v[2]) ^ __float_as_uint(v[3]);
      }
    }
#elif !defined(FAG_ABLATE_QUANT)
    cutlass::NumericArrayConverter<Element, ElementAcc, AlignmentS> epilogue_op;

    CUTLASS_PRAGMA_UNROLL
    for (int i = 0; i < size(input_vec); i++) {
      output_vec(i) = epilogue_op(input_vec(i));
    }
#else
    // FAG_ABLATE_QUANT: skip the FP32->e4m3 cvt (FP8-specific tax). Force each
    // input group live (zero-cost register asm) so source loads are not DCE'd;
    // emit a constant FP8 pattern so downstream stores/packs still compile & run.
    // NOTE: contaminated — the empty asm emits no PTX, so ptxas still removes
    // the P-chain exp2/scale-FMA (MUFU 192->4, FMUL.FTZ 196->0). Kept only for
    // comparison; prefer the FAG_ABLATE_QUANT_V2 numbers.
    CUTLASS_PRAGMA_UNROLL
    for (int i = 0; i < size(input_vec); i++) {
      Array<ElementAcc, AlignmentS> v = input_vec(i);
      asm volatile("" : "+f"(v[0]), "+f"(v[1]), "+f"(v[2]), "+f"(v[3]));
      output_vec(i) = Array<Element, AlignmentS>{};
    }
#endif

    return output;
  }

  // Remote shared-memory bulk copy used by the FA4 dS corner turn. Destination
  // and completion barrier are explicitly mapped to the peer CTA; source remains
  // local. PTX requires 16-byte aligned addresses and a byte count divisible by 16.
  static CUTLASS_DEVICE void cpasync_bulk_s2cluster(
      void* dst, void const* src, cutlass::arch::ClusterTransactionBarrier* barrier,
      uint32_t bytes, uint32_t peer_rank) {
    uint32_t dst_addr = cute::set_block_rank(cute::cast_smem_ptr_to_uint(dst), peer_rank);
    uint32_t src_addr = cute::cast_smem_ptr_to_uint(src);
    uint32_t mbar_addr = cute::set_block_rank(
        cute::cast_smem_ptr_to_uint(reinterpret_cast<uint64_t*>(barrier)), peer_rank);
    asm volatile(
        "cp.async.bulk.shared::cluster.shared::cta.mbarrier::complete_tx::bytes "
        "[%0], [%1], %3, [%2];"
        :
        : "r"(dst_addr), "r"(src_addr), "r"(mbar_addr), "r"(bytes)
        : "memory");
  }

#if defined(FAG_DS_DIRECT_DSMEM)
  // Store one already-quantized 16-byte dS vector directly into the peer CTA's
  // distributed shared memory.  The address is the same local shared-memory
  // offset mapped to peer_rank; no numeric conversion or layout change occurs.
  static CUTLASS_DEVICE void store_shared_cluster_128(
      void* dst, uint4 const& value, uint32_t peer_rank) {
    uint32_t dst_addr = cute::set_block_rank(
        cute::cast_smem_ptr_to_uint(dst), peer_rank);
    asm volatile(
        "st.shared::cluster.v4.b32 [%0], {%1, %2, %3, %4};"
        :
        : "r"(dst_addr), "r"(value.x), "r"(value.y), "r"(value.z), "r"(value.w)
        : "memory");
  }
#endif

#if defined(FAG_OPT_STASYNC_XCHG) || defined(FAG_OPT_OWN_LATE) || defined(FAG_DS_RUN_STORES)
  // Pack four adjacent quantized dS bytes (i..i+3) into one 32-bit word.
  // Shared by the reordered optimization loops so their arithmetic is
  // identical to the base path's inline pack4 lambda (base SASS unchanged).
  template<class TensorDST>
  static CUTLASS_DEVICE uint32_t pack_u32(TensorDST const& dst, int i) {
    return uint32_t(dst(i + 0).storage)       |
           (uint32_t(dst(i + 1).storage) << 8)  |
           (uint32_t(dst(i + 2).storage) << 16) |
           (uint32_t(dst(i + 3).storage) << 24);
  }
#endif

#if defined(FAG_OPT_STASYNC_XCHG)
  // Store one already-quantized 16-byte dS vector into the LOCAL exchange
  // buffer with async-proxy semantics, accounting its bytes on an mbarrier.
  // The bulk copy then reads async-proxy data: no generic->async proxy fence
  // is required between these stores and cp.async.bulk (the mbarrier phase
  // completion provides the ordering), which removes the store-pipe drain the
  // fence.proxy.async.shared::cluster forced on every iteration.
  static CUTLASS_DEVICE void stasync_128(
      void* dst, uint4 const& value,
      cutlass::arch::ClusterTransactionBarrier* barrier) {
    uint32_t dst_addr = cute::cast_smem_ptr_to_uint(dst);
    uint32_t bar_addr = cute::cast_smem_ptr_to_uint(
        reinterpret_cast<uint64_t*>(barrier));
    asm volatile(
        "st.async.v4.b32.shared::cta.mbarrier::complete_tx::bytes"
        " [%0], {%1, %2, %3, %4}, [%5];"
        :
        : "r"(dst_addr), "r"(value.x), "r"(value.y), "r"(value.z), "r"(value.w),
          "r"(bar_addr)
        : "memory");
  }
#endif

#if defined(FAG_DS_STASYNC_CLUSTER)
  // Store one already-quantized 16-byte dS vector DIRECTLY into the peer CTA's
  // distributed shared memory through the async proxy, accounting its 16 bytes
  // on the peer's ds_full transaction barrier (st.async.shared::cluster with
  // mbarrier::complete_tx::bytes -- the remote analogue of stasync_128 and the
  // DSMEM equivalent of FA4's copy_utils.store_shared_remote_fp32x4).  512
  // such stores per iteration (256 threads x 2 strips/runs) replace the ENTIRE
  // exchange chain: no exchange-buffer STS, no generic->async proxy fence, and
  // no cp.async.bulk -- the mbarrier tx accounting feeds the unchanged
  // ds_full/relay/ds_leader wait protocol with identical byte counts.
  // Addresses are 32-bit SMEM-window byte addresses (as returned by
  // cast_smem_ptr_to_uint / precomputed swizzled run bases); they are mapped
  // to the peer CTA here via mapa (set_block_rank).
  static CUTLASS_DEVICE void stasync_cluster_128(
      uint32_t dst_smem_addr, uint4 const& value,
      uint32_t barrier_smem_addr, uint32_t peer_rank) {
    uint32_t dst_addr = cute::set_block_rank(dst_smem_addr, peer_rank);
    uint32_t bar_addr = cute::set_block_rank(barrier_smem_addr, peer_rank);
    asm volatile(
        "st.async.shared::cluster.mbarrier::complete_tx::bytes.v4.b32"
        " [%0], {%1, %2, %3, %4}, [%5];"
        :
        : "r"(dst_addr), "r"(value.x), "r"(value.y), "r"(value.z), "r"(value.w),
          "r"(bar_addr)
        : "memory");
  }
#endif



  template<class BlkCoord, class BlkOffset, class ProblemShape_>
  CUTLASS_DEVICE void load(
      BlkCoord const& blk_coord,
      BlkOffset const& blk_offset,
      ProblemShape_ const& problem_shape,
      int iter_start,
      int iter_end,
      int iter_count,
      MainloopArguments const& mainloop_args,
      MainloopParams const& mainloop_params,
      TensorStorage& shared_tensors,
      PipelineLoadMmaQ& pipeline_load_mma_q,
      typename PipelineLoadMmaQ::PipelineState& pipeline_load_mma_q_producer_state,
      PipelineLoadMmaQT& pipeline_load_mma_qt,
      typename PipelineLoadMmaQT::PipelineState& pipeline_load_mma_qt_producer_state,
      PipelineLoadMmaKT& pipeline_load_mma_kt,
      typename PipelineLoadMmaKT::PipelineState& pipeline_load_mma_kt_producer_state,
      PipelineLoadMmaDO& pipeline_load_mma_do,
      typename PipelineLoadMmaDO::PipelineState& pipeline_load_mma_do_producer_state,
      PipelineLoadComputeLSE& pipeline_load_compute_lse,
      typename PipelineLoadComputeLSE::PipelineState& pipeline_load_compute_lse_producer_state,
      PipelineLoadComputeSumOdO& pipeline_load_compute_sum_odo,
      typename PipelineLoadComputeSumOdO::PipelineState& pipeline_load_compute_sum_odo_producer_state) {

    auto [Q, K, D, D_VO, HB] = problem_shape;
    int iter_index = iter_start;

    using X = Underscore;

    // [2SM] multicast masks. CORRECTED: A-op (K,V) self-loads -> self mask;
    // B-op (Q,DO) cooperative box -> cluster mask. For <2,1,1> both end up as
    // (1<<rank) but must be NON-zero (mask 0 = no destination -> TMA hangs).
    uint16_t mcast_mask_a_kq = 0, mcast_mask_b_kq = 0;
    uint16_t mcast_mask_a_vdo = 0, mcast_mask_b_vdo = 0;
    if constexpr (kClusterSize > 1) {
      int rank = (int)cute::block_rank_in_cluster();
      mcast_mask_a_kq = uint16_t(1u << rank);   // K self-load
      mcast_mask_b_kq = uint16_t(1u << rank);   // Q cooperative box (self-half dst)
      mcast_mask_a_vdo = uint16_t(1u << rank);  // V self-load
      mcast_mask_b_vdo = uint16_t(1u << rank);  // DO cooperative box (self-half dst)
    }

    auto mK_in = mainloop_params.tma_load_k.get_tma_tensor(make_shape(K, D, HB));
    auto mV_in = mainloop_params.tma_load_v.get_tma_tensor(make_shape(K, D_VO, HB));
    auto mQ_in = mainloop_params.tma_load_q.get_tma_tensor(make_shape(Q, D, HB));
    auto mQT_in = mainloop_params.tma_load_qt.get_tma_tensor(make_shape(D, Q, HB));
    auto mKT_in = mainloop_params.tma_load_kt.get_tma_tensor(make_shape(D, K, HB));
    auto mDO_in = mainloop_params.tma_load_do.get_tma_tensor(make_shape(Q, D_VO, HB));
    // [dV-axis-fix] gmem dO tensor for the PDO (dV) MMA. stride_do_mn is
    // Stride<_1,int,...> (mode0=DVO contiguous), so the gmem shape is <DVO, Q, HB>.
    auto mDO_dv_in = mainloop_params.tma_load_do_dv.get_tma_tensor(make_shape(D_VO, Q, HB));

    auto mK = domain_offset(select<1,2,4>(blk_offset), mK_in);
    auto mV = domain_offset(select<1,3,4>(blk_offset), mV_in);
    auto mQ = domain_offset(select<0,2,4>(blk_offset), mQ_in);
    auto mQT = domain_offset(select<2,0,4>(blk_offset), mQT_in);
    auto mKT = domain_offset(select<2,1,4>(blk_offset), mKT_in);
    auto mDO = domain_offset(select<0,3,4>(blk_offset), mDO_in);
    // [dV-axis-fix] PDO dO: gmem shape <DVO, Q, HB>. Offset DVO(idx3) and HB(idx4);
    // Q is iterated via local_tile. (DVO is head_dim, offset 0.)
    auto mDO_dv = domain_offset(select<3,0,4>(blk_offset), mDO_dv_in);

    // [dV-large-shape FIX] A-op (K, V) is the LEFT matrix of the cooperative 2SM MMA,
    // split along the M axis: the cluster pair {2c,2c+1} shares ONE 256-row TMA box,
    // and partition_A(get_slice(rank)) + tma_partition select each CTA's own 128-row
    // half. Therefore the gmem source tensor MUST be tiled at the MMA granularity
    // (M=256 = MmaTileShapeKQ), so the copy-site box index k_mma_index = blk_coord_k /
    // kClusterSize (256-row units) addresses the right box. Mirrors stock
    // sm100_blockscaled (gA = local_tile(mA, TileShape{}, ...)) and the mxfp8 fwd
    // "续19an ROOT FIX". Using the per-CTA TileShapeKQ (M=128) here made the box a
    // 128-row unit, so k_mma_index picked the WRONG K rows for every cluster pair
    // past the first (blk>=2 loaded blk1's data -> wrong P -> wrong dV). The B-op
    // (Q, DO) is N-split, iterated by iter_index, so it keeps the per-CTA tile.
    auto gK = local_tile(mK, MmaTileShapeKQ{}, make_coord(_,_,_), Step<_1, X, _1>{});
    auto gQ = local_tile(mQ, TileShapeKQ{}, make_coord(_,_,_), Step<X, _1, _1>{});
    auto gQT = local_tile(mQT, make_shape(TileShapeDQK{}, TileShapeQ{}, _1{}),
                          make_coord(_,_,_), Step<_1, _1, X>{});
    auto gKT = local_tile(mKT, MmaTileShapeDSK{}, make_coord(_,_,_), Step<X, _1, _1>{});
    auto gV = local_tile(mV, MmaTileShapeVDO{}, make_coord(_,_,_), Step<_1, X, _1>{});
    auto gDO = local_tile(mDO, TileShapeVDO{}, make_coord(_,_,_), Step<X, _1, _1>{});
    // [dV-axis-fix] PDO dO tile. mDO_dv gmem shape is <DVO, Q, HB> (matches stride_do_mn
    // Stride<_1,int,...>). PDO B-op: N=DVO (tiled, coord 0), K=Q (iterated via iter_index).
    // Tile shape <TileShapeDVO, TileShapeQ, _1>, Step<_1,_1,X> tiles DVO+Q, leaves HB.
    auto gDO_dv = local_tile(mDO_dv, make_shape(TileShapeDVO{}, TileShapeQ{}, _1{}), make_coord(_,_,_), Step<_1, _1, X>{});

    // [2SM] cluster-aware load partitioning — mirrors mxfp8 fwd EXACTLY, with the
    // bwd operand roles swapped (bwd's A-op = K/V, B-op = Q/dO; mxfp8's A-op = Q,
    // B-op = K). The cluster TMA pipeline is cluster-aware, so EVERY TMA on it is
    // a 2CTA box (leader arms with kClusterSize x bytes). The split axis is set by
    // the tma_partition cluster-mode argument + get_slice:
    //  - A-operand (K, V): LEFT matrix, distinct per CTA (each CTA's own K-block).
    //    Cluster-aware tma_partition projected along the multicast (M) mode, like
    //    mxfp8's Q (its A-op). get_slice(rank).
    //  - B-operand (Q, DO): RIGHT matrix, same tile, each CTA stores its N-half.
    //    Self-only tma_partition (_0,_1), like mxfp8's K (its B-op). get_slice(rank).
    constexpr int kAtomThrKQ = cute::size(typename TiledMmaKQ::AtomThrID{});
    constexpr int kAtomThrVDO = cute::size(typename TiledMmaVDO::AtomThrID{});
    constexpr int kAtomThrPDO = cute::size(typename TiledMmaPDO::AtomThrID{});
    int rank_in_cluster = (kClusterSize == 1) ? 0 : (int)cute::block_rank_in_cluster();

    // A-op cluster layout (M-mode multicast box), used for K and V.
    auto cta_layout_vmnk_a_kq = tiled_divide(make_layout(ClusterShape{}),
        make_tile(typename TiledMmaKQ::AtomThrID{}));
    auto cta_coord_vmnk_a_kq  = cta_layout_vmnk_a_kq.get_flat_coord(rank_in_cluster);
    auto cta_layout_vmnk_a_vdo = tiled_divide(make_layout(ClusterShape{}),
        make_tile(typename TiledMmaVDO::AtomThrID{}));
    auto cta_coord_vmnk_a_vdo  = cta_layout_vmnk_a_vdo.get_flat_coord(rank_in_cluster);

    ThrMMA cta_mma_kq_a = TiledMmaKQ{}.get_slice(rank_in_cluster % kAtomThrKQ);
    ThrMMA cta_mma_kq_b = TiledMmaKQ{}.get_slice(rank_in_cluster % kAtomThrKQ);
    ThrMMA cta_mma_dsq_b = TiledMmaDSQ{}.get_slice(
        rank_in_cluster % cute::size(typename TiledMmaDSQ::AtomThrID{}));
    ThrMMA cta_mma_dsk_b = TiledMmaDSK{}.get_slice(
        rank_in_cluster % cute::size(typename TiledMmaDSK::AtomThrID{}));
    ThrMMA cta_mma_vdo_a = TiledMmaVDO{}.get_slice(rank_in_cluster % kAtomThrVDO);
    ThrMMA cta_mma_vdo_b = TiledMmaVDO{}.get_slice(rank_in_cluster % kAtomThrVDO);

    auto tSTgK = cta_mma_kq_a.partition_A(gK);
    auto tSTgQ = cta_mma_kq_b.partition_B(gQ);
    auto tDKgQT = cta_mma_dsq_b.partition_B(gQT);
    auto tDQgKT = cta_mma_dsk_b.partition_B(gKT);
    auto tDPTgV = cta_mma_vdo_a.partition_A(gV);
    auto tDPTgDO = cta_mma_vdo_b.partition_B(gDO);

    // [dV-axis-fix] PDO (dV) B-op partition of dO. The PDO MMA splits its M-mode
    // (K/cluster-wide); dO is the B-op so each CTA loads its own N-half (DVO).
    ThrMMA cta_mma_pdo_b = TiledMmaPDO{}.get_slice(rank_in_cluster % kAtomThrPDO);
    auto tDVgDO_dv = cta_mma_pdo_b.partition_B(gDO_dv);

    auto sQ = make_tensor(make_smem_ptr(shared_tensors.smem_q.begin()), SmemLayoutQ{});
    auto sQT = make_tensor(make_smem_ptr(shared_tensors.smem_q_t.begin()), SmemLayoutQT{});
    auto sKT = make_tensor(make_smem_ptr(shared_tensors.smem_k_t.begin()), SmemLayoutKT{});
    auto sK = make_tensor(make_smem_ptr(shared_tensors.smem_k.begin()), SmemLayoutK{});
    auto sV = make_tensor(make_smem_ptr(shared_tensors.smem_v.begin()), SmemLayoutV{});
    auto sDO = make_tensor(make_smem_ptr(shared_tensors.smem_do.begin()), SmemLayoutDO{});
    auto sDO_dv = make_tensor(make_smem_ptr(shared_tensors.smem_do_dv.begin()), SmemLayoutDOT{});

    // K (A-op): cluster-aware box (M-split). V (A-op): same.
    auto [tKgK_mkl, tKsK] = tma_partition(
        mainloop_params.tma_load_k, get<2>(cta_coord_vmnk_a_kq), make_layout(cute::size<2>(cta_layout_vmnk_a_kq)),
        group_modes<0,3>(sK), group_modes<0,3>(tSTgK));
    // Q (B-op): self-only partition.
    auto [tQgQ_mkl, tQsQ] = tma_partition(
        mainloop_params.tma_load_q, _0{}, make_layout(_1{}),
        group_modes<0,3>(sQ), group_modes<0,3>(tSTgQ));
    auto [tQTgQT_mkl, tQTsQT] = tma_partition(
        mainloop_params.tma_load_qt, _0{}, make_layout(_1{}),
        group_modes<0,3>(sQT), group_modes<0,3>(tDKgQT));
    auto [tKTgKT_mkl, tKTsKT] = tma_partition(
        mainloop_params.tma_load_kt, _0{}, make_layout(_1{}),
        group_modes<0,3>(sKT), group_modes<0,3>(tDQgKT));
    // V (A-op): cluster-aware box (M-split).
    auto [tVgV_mkl, tVsV] = tma_partition(
        mainloop_params.tma_load_v, get<2>(cta_coord_vmnk_a_vdo), make_layout(cute::size<2>(cta_layout_vmnk_a_vdo)),
        group_modes<0,3>(sV), group_modes<0,3>(tDPTgV));
    // DO (B-op): self-only partition.
    auto [tDOgDO_mkl, tDOsDO] = tma_partition(
        mainloop_params.tma_load_do, _0{}, make_layout(_1{}),
        group_modes<0,3>(sDO), group_modes<0,3>(tDPTgDO));
    // [dV-axis-fix] DO for dV (PDO B-op): self-only partition using the PDO-built TMA
    // so the box splits dO along DVO. NOTE: this is the FIXNOTE atom-mismatch site;
    // if tma_partition asserts here, see FIXNOTE_dV_axis_mismatch.md lines 59-66.
    auto [tDOgDO_dv_mkl, tDOsDO_dv] = tma_partition(
        mainloop_params.tma_load_do_dv, _0{}, make_layout(_1{}),
        group_modes<0,3>(sDO_dv), group_modes<0,3>(tDVgDO_dv));

    // set up lse and sum_odo

    auto [blk_coord_q, blk_coord_k, blk_coord_d, blk_coord_dv, blk_coord_batch] = blk_coord;

    // [2SM A-op M-split FIX — MXFP8 续19an ROOT FIX] The cooperative cta_group::2 TMA
    // box covers a 256-row MMA tile (2 per-CTA K-blocks). The A-op (K, V) gmem m-tile
    // index must be in MMA-tile (256-row) units = blk_coord_k / kClusterSize, so the
    // cluster pair {2c,2c+1} shares the SAME box base; partition_A(get_slice(rank))
    // then selects each CTA's own 128-row M-half. Using the per-CTA blk_coord_k directly
    // makes the peer (rank1) select an OOB box -> TMA zero-fill -> peer S = 0.
    int k_mma_index = (int)(blk_coord_k) / kClusterSize;

#ifdef BWD_2SM_DEBUG
    if (cute::elect_one_sync()) printf("[DBG load-fn] blk=%d rank=%d pre-q-acquire\n", blockIdx.x, (int)cute::block_rank_in_cluster());
#endif
    pipeline_load_mma_q.producer_acquire(pipeline_load_mma_q_producer_state);
#ifdef BWD_2SM_DEBUG
    if (cute::elect_one_sync()) printf("[DBG load-fn] blk=%d rank=%d post-q-acquire\n", blockIdx.x, (int)cute::block_rank_in_cluster());
#endif
    auto tma_barrier = pipeline_load_mma_q.producer_get_barrier(pipeline_load_mma_q_producer_state);

    // [2SM] async-proxy cluster fence: order the producer_acquire's expect-tx
    // arming (generic-proxy smem write to the cluster mbarrier) BEFORE the TMA
    // issue. Without it the TMA byte-count can race the arm on the cluster
    // mbarrier -> full barrier never completes -> MMA consumer_wait hangs.
    if constexpr (kClusterSize > 1) {
      asm volatile("fence.proxy.async.shared::cluster;" ::: "memory");
    }
    // [2SM] K is loaded on this pipeline alongside Q. The 2CTA box's complete-tx
    // lands on the leader's barrier with BOTH CTAs' bytes (K_bytes x cluster for
    // the K load). The pipeline's transaction_bytes already accounts for Q's
    // kClusterSize factor; this expect-tx adds the K factor.
    pipeline_load_mma_q.producer_expect_transaction(
        pipeline_load_mma_q_producer_state, kTransactionsBytesLoadK * kClusterSize);

    // load K (A-op: cluster-aware mask)
    if (cute::elect_one_sync()) {
      cute::copy(
          mainloop_params.tma_load_k.with(*tma_barrier, mcast_mask_a_kq),
          tKgK_mkl(_, k_mma_index, _0{}, blk_coord_batch),
          tKsK(_, _0{})
      );
    }

    // load Q (B-op: self-only mask)
    if (cute::elect_one_sync()) {
      cute::copy(
          mainloop_params.tma_load_q.with(*tma_barrier, mcast_mask_b_kq),
          tQgQ_mkl(_, iter_index, _0{}, blk_coord_batch),
          tQsQ(_, pipeline_load_mma_q_producer_state.index())
      );
    }

    ++pipeline_load_mma_q_producer_state;

    // Load the independent Qt representation on its own two-stage pipeline.
    pipeline_load_mma_qt.producer_acquire(pipeline_load_mma_qt_producer_state);
    auto qt_barrier = pipeline_load_mma_qt.producer_get_barrier(
        pipeline_load_mma_qt_producer_state);
    if constexpr (kClusterSize > 1) {
      asm volatile("fence.proxy.async.shared::cluster;" ::: "memory");
    }
    if (cute::elect_one_sync()) {
      cute::copy(
          mainloop_params.tma_load_qt.with(*qt_barrier, mcast_mask_b_kq),
          tQTgQT_mkl(_, _0{}, iter_index, blk_coord_batch),
          tQTsQT(_, pipeline_load_mma_qt_producer_state.index())
      );
    }
    ++pipeline_load_mma_qt_producer_state;

    // Kt is invariant across the Q loop: load one cooperative 256-row KV tile.
    pipeline_load_mma_kt.producer_acquire(pipeline_load_mma_kt_producer_state);
    auto kt_barrier = pipeline_load_mma_kt.producer_get_barrier(
        pipeline_load_mma_kt_producer_state);
    if constexpr (kClusterSize > 1) {
      asm volatile("fence.proxy.async.shared::cluster;" ::: "memory");
    }
    if (cute::elect_one_sync()) {
      cute::copy(
          mainloop_params.tma_load_kt.with(*kt_barrier, mcast_mask_a_kq),
          tKTgKT_mkl(_, _0{}, k_mma_index, blk_coord_batch),
          tKTsKT(_, pipeline_load_mma_kt_producer_state.index())
      );
    }
    ++pipeline_load_mma_kt_producer_state;

    pipeline_load_compute_lse.producer_acquire(pipeline_load_compute_lse_producer_state);

    // load LSE
    // 32 threads loading 128 values of 32b each
    // so 4*32b=128b

    int thread_idx = threadIdx.x % NumThreadsPerWarp;
    int smem_idx = TileShapeQ{} * pipeline_load_compute_lse_producer_state.index() + thread_idx * 4;
    int gmem_idx = TileShapeQ{} * iter_index + thread_idx * 4;
    auto mLSE = make_tensor(mainloop_args.ptr_lse, make_shape(Q, HB), mainloop_args.stride_lse);
    for (int i = 0; i < 4; i++) {
      cutlass::arch::cp_async_zfill<4>(
          shared_tensors.smem_lse.begin() + smem_idx + i,
          &mLSE(gmem_idx + i, blk_coord_batch),
          gmem_idx + i < Q
      );
    }

    pipeline_load_compute_lse.producer_commit(pipeline_load_compute_lse_producer_state, cutlass::arch::cpasync_barrier_arrive);
    ++pipeline_load_compute_lse_producer_state;


    pipeline_load_mma_do.producer_acquire(pipeline_load_mma_do_producer_state);
    tma_barrier = pipeline_load_mma_do.producer_get_barrier(pipeline_load_mma_do_producer_state);

    // [2SM] V expect-tx with cluster factor (see K above).
    pipeline_load_mma_do.producer_expect_transaction(
        pipeline_load_mma_do_producer_state, kTransactionsBytesLoadV * kClusterSize);
    // [dV-axis-fix] expect-tx for the dV dO copy (self-only, same size as dP dO).
    pipeline_load_mma_do.producer_expect_transaction(
        pipeline_load_mma_do_producer_state, kTransactionsBytesLoadDO * kClusterSize);
    // load V (A-op: cluster-aware mask)

    // load V (A-op: cluster-aware mask)
    if (cute::elect_one_sync()) {
      cute::copy(
          mainloop_params.tma_load_v.with(*tma_barrier, mcast_mask_a_vdo),
          tVgV_mkl(_, k_mma_index, _0{}, blk_coord_batch),
          tVsV(_, _0{})
      );
    }

    // load dO (B-op: self-only mask)
    if (cute::elect_one_sync()) {
      cute::copy(
          mainloop_params.tma_load_do.with(*tma_barrier, mcast_mask_b_vdo),
          tDOgDO_mkl(_, iter_index, _0{}, blk_coord_batch),
          tDOsDO(_, pipeline_load_mma_do_producer_state.index())
      );
      // [dV-axis-fix] load dO for the dV (PDO) MMA into smem_do_dv. Same Q-iter
      // rhythm as the dP dO load; shares the barrier so the dV MMA's consumer_wait
      // on pipeline_load_mma_do also covers this copy.
      cute::copy(
          mainloop_params.tma_load_do_dv.with(*tma_barrier, mcast_mask_b_vdo),
          tDOgDO_dv_mkl(_, _0{}, iter_index, blk_coord_batch),
          tDOsDO_dv(_, pipeline_load_mma_do_producer_state.index())
      );
    }

    ++pipeline_load_mma_do_producer_state;

    pipeline_load_compute_sum_odo.producer_acquire(pipeline_load_compute_sum_odo_producer_state);

    // load sum_OdO
    smem_idx = TileShapeQ{} * pipeline_load_compute_sum_odo_producer_state.index() + thread_idx * 4;
    gmem_idx = TileShapeQ{} * iter_index + thread_idx * 4;
    auto mSumOdO = make_tensor(mainloop_args.ptr_sum_odo, make_shape(Q, HB), mainloop_args.stride_sum_odo);
    for (int i = 0; i < 4; i++) {
      cutlass::arch::cp_async_zfill<4>(
          shared_tensors.smem_sum_odo.begin() + smem_idx + i,
          &mSumOdO(gmem_idx + i, blk_coord_batch),
          gmem_idx + i < Q
      );
    }

    pipeline_load_compute_sum_odo.producer_commit(pipeline_load_compute_sum_odo_producer_state, cutlass::arch::cpasync_barrier_arrive);
    ++pipeline_load_compute_sum_odo_producer_state;

    iter_count -= 1;
    iter_index += 1;

    while (iter_count > 0) {
      if (iter_index == iter_end) {
        iter_index = iter_start;
        get<0,0>(blk_coord_batch) += 1;
      }

      pipeline_load_mma_q.producer_acquire(pipeline_load_mma_q_producer_state);
      tma_barrier = pipeline_load_mma_q.producer_get_barrier(pipeline_load_mma_q_producer_state);

      // load Q (B-op: self-only mask)
      if (cute::elect_one_sync()) {
        cute::copy(
            mainloop_params.tma_load_q.with(*tma_barrier, mcast_mask_b_kq),
            tQgQ_mkl(_, iter_index, _0{}, blk_coord_batch),
            tQsQ(_, pipeline_load_mma_q_producer_state.index())
        );
      }

      ++pipeline_load_mma_q_producer_state;

      pipeline_load_mma_qt.producer_acquire(pipeline_load_mma_qt_producer_state);
      qt_barrier = pipeline_load_mma_qt.producer_get_barrier(
          pipeline_load_mma_qt_producer_state);
      if constexpr (kClusterSize > 1) {
        asm volatile("fence.proxy.async.shared::cluster;" ::: "memory");
      }
      if (cute::elect_one_sync()) {
        cute::copy(
            mainloop_params.tma_load_qt.with(*qt_barrier, mcast_mask_b_kq),
            tQTgQT_mkl(_, _0{}, iter_index, blk_coord_batch),
            tQTsQT(_, pipeline_load_mma_qt_producer_state.index())
        );
      }
      ++pipeline_load_mma_qt_producer_state;

      pipeline_load_compute_lse.producer_acquire(pipeline_load_compute_lse_producer_state);

      // load LSE
      smem_idx = TileShapeQ{} * pipeline_load_compute_lse_producer_state.index() + thread_idx * 4;
      gmem_idx = TileShapeQ{} * iter_index + thread_idx * 4;
      for (int i = 0; i < 4; i++) {
        cutlass::arch::cp_async_zfill<4>(
            shared_tensors.smem_lse.begin() + smem_idx + i,
            &mLSE(gmem_idx + i, blk_coord_batch),
            gmem_idx + i < Q
        );
      }

      pipeline_load_compute_lse.producer_commit(pipeline_load_compute_lse_producer_state, cutlass::arch::cpasync_barrier_arrive);
      ++pipeline_load_compute_lse_producer_state;

      pipeline_load_mma_do.producer_acquire(pipeline_load_mma_do_producer_state);
      tma_barrier = pipeline_load_mma_do.producer_get_barrier(pipeline_load_mma_do_producer_state);

      // [dV-axis-fix] expect-tx for the dV dO copy in the iteration loop (acquire
      // already covers the dP dO copy via transaction_bytes).
      pipeline_load_mma_do.producer_expect_transaction(
          pipeline_load_mma_do_producer_state, kTransactionsBytesLoadDO * kClusterSize);

      // load dO (B-op: self-only mask)
      if (cute::elect_one_sync()) {
        cute::copy(
            mainloop_params.tma_load_do.with(*tma_barrier, mcast_mask_b_vdo),
            tDOgDO_mkl(_, iter_index, _0{}, blk_coord_batch),
            tDOsDO(_, pipeline_load_mma_do_producer_state.index())
        );
        // [dV-axis-fix] load dO for the dV (PDO) MMA (iteration loop).
        cute::copy(
            mainloop_params.tma_load_do_dv.with(*tma_barrier, mcast_mask_b_vdo),
            tDOgDO_dv_mkl(_, _0{}, iter_index, blk_coord_batch),
            tDOsDO_dv(_, pipeline_load_mma_do_producer_state.index())
        );
      }

      ++pipeline_load_mma_do_producer_state;

      pipeline_load_compute_sum_odo.producer_acquire(pipeline_load_compute_sum_odo_producer_state);

      // load sum_OdO
      smem_idx = TileShapeQ{} * pipeline_load_compute_sum_odo_producer_state.index() + thread_idx * 4;
      gmem_idx = TileShapeQ{} * iter_index + thread_idx * 4;
      for (int i = 0; i < 4; i++) {
        cutlass::arch::cp_async_zfill<4>(
            shared_tensors.smem_sum_odo.begin() + smem_idx + i,
            &mSumOdO(gmem_idx + i, blk_coord_batch),
            gmem_idx + i < Q
        );
      }

      pipeline_load_compute_sum_odo.producer_commit(pipeline_load_compute_sum_odo_producer_state, cutlass::arch::cpasync_barrier_arrive);
      ++pipeline_load_compute_sum_odo_producer_state;

      iter_count -= 1;
      iter_index += 1;
    }

    // Drain every producer pipeline before the Load warp reaches the cluster
    // exit barrier. Leaving a full stage outstanding is a synccheck violation
    // and can race reuse of the underlying shared-memory barriers.
    pipeline_load_mma_q.producer_tail(pipeline_load_mma_q_producer_state);
    pipeline_load_mma_qt.producer_tail(pipeline_load_mma_qt_producer_state);
    pipeline_load_mma_kt.producer_tail(pipeline_load_mma_kt_producer_state);
    pipeline_load_mma_do.producer_tail(pipeline_load_mma_do_producer_state);
    pipeline_load_compute_lse.producer_tail(pipeline_load_compute_lse_producer_state);
    pipeline_load_compute_sum_odo.producer_tail(pipeline_load_compute_sum_odo_producer_state);
  }


  template<class BlkCoord, class ProblemShape_>
  CUTLASS_DEVICE void mma(
      BlkCoord const& blk_coord,
      ProblemShape_ const& problem_shape,
      int iter_start,
      int iter_end,
      int iter_count,
      MainloopArguments const& mainloop_args,
      TensorStorage& shared_tensors,
      PipelineLoadMmaQ& pipeline_load_mma_q,
      typename PipelineLoadMmaQ::PipelineState& pipeline_load_mma_q_consumer_state,
      PipelineLoadMmaQT& pipeline_load_mma_qt,
      typename PipelineLoadMmaQT::PipelineState& pipeline_load_mma_qt_consumer_state,
      PipelineLoadMmaKT& pipeline_load_mma_kt,
      typename PipelineLoadMmaKT::PipelineState& pipeline_load_mma_kt_consumer_state,
      PipelineLoadMmaDO& pipeline_load_mma_do,
      typename PipelineLoadMmaDO::PipelineState& pipeline_load_mma_do_consumer_state,
      PipelineMmaComputeS& pipeline_mma_compute_s,
      typename PipelineMmaComputeS::PipelineState& pipeline_mma_compute_s_producer_state,
      PipelineMmaComputeDP& pipeline_mma_compute_dp,
      typename PipelineMmaComputeDP::PipelineState& pipeline_mma_compute_dp_producer_state,
      PipelineMmaReduceDQ& pipeline_mma_reduce_dq,
      typename PipelineMmaReduceDQ::PipelineState& pipeline_mma_reduce_dq_producer_state,
      PipelineComputeMmaP& pipeline_compute_mma_p,
      typename PipelineComputeMmaP::PipelineState& pipeline_compute_mma_p_consumer_state,
      PipelineComputeMmaDS& pipeline_compute_mma_ds,
      typename PipelineComputeMmaDS::PipelineState& pipeline_compute_mma_ds_consumer_state,
      PipelineMmaComputeDKDV& pipeline_mma_compute_dkdv,
      typename PipelineMmaComputeDKDV::PipelineState& pipeline_mma_compute_dkdv_producer_state) {

    auto [Q, K, D, D_VO, HB] = problem_shape;

    auto sQ = make_tensor(make_smem_ptr(shared_tensors.smem_q.begin()), SmemLayoutQ{});
    auto sK = make_tensor(make_smem_ptr(shared_tensors.smem_k.begin()), SmemLayoutK{});
    auto sV = make_tensor(make_smem_ptr(shared_tensors.smem_v.begin()), SmemLayoutV{});
    auto sDO = make_tensor(make_smem_ptr(shared_tensors.smem_do.begin()), SmemLayoutDO{});

    auto sQT = make_tensor(make_smem_ptr(shared_tensors.smem_q_t.begin()), SmemLayoutQT{});
    auto sKT = make_tensor(make_smem_ptr(shared_tensors.smem_k_t.begin()), SmemLayoutKT{});
    auto sDS = make_tensor(make_smem_ptr(shared_tensors.smem_ds.begin()), SmemLayoutDS{});
    auto sDST = make_tensor(make_smem_ptr(shared_tensors.smem_ds_t.begin()), SmemLayoutDST{});
    auto sP = make_tensor(make_smem_ptr((Element*) nullptr), typename CollectiveMmaPDO::SmemLayoutA{});
    // [dV-axis-fix] dV reads dO from the dedicated smem_do_dv (loaded by the PDO TMA,
    // split along DVO), NOT from smem_do_t (which is laid out for VDO's N=Q axis).
    auto sDOT = make_tensor(make_smem_ptr(shared_tensors.smem_do_dv.begin()), SmemLayoutDOT{});

    Tensor tSTrK = TiledMmaKQ::make_fragment_A(sK);
    Tensor tSTrQ = TiledMmaKQ::make_fragment_B(sQ);

    Tensor tDPTrV = TiledMmaVDO::make_fragment_A(sV);
    Tensor tDPTrDO = TiledMmaVDO::make_fragment_B(sDO);

    Tensor tDQrDS = TiledMmaDSK::make_fragment_A(sDS);
    Tensor tDQrKT = TiledMmaDSK::make_fragment_B(sKT);

    Tensor tDKrDST = TiledMmaDSQ::make_fragment_A(sDST)(_, _, _, _0{});
    tDKrDST.data() = TmemAllocation::kDS;
    Tensor tDKrQT = TiledMmaDSQ::make_fragment_B(sQT);

    Tensor tDVrP = TiledMmaPDO::make_fragment_A(sP)(_, _, _, _0{});
    tDVrP.data() = TmemAllocation::kP;
    Tensor tDVrDOT = TiledMmaPDO::make_fragment_B(sDOT);

    TiledMmaKQ tiled_mma_kq;
    TiledMmaVDO tiled_mma_vdo;
    TiledMmaDSK tiled_mma_dsk;
    TiledMmaDSQ tiled_mma_dsq;
    TiledMmaPDO tiled_mma_pdo;

    tiled_mma_dsq.accumulate_ = UMMA::ScaleOut::Zero;
    tiled_mma_pdo.accumulate_ = UMMA::ScaleOut::Zero;
    uint32_t ds_leader_phase = 0;
#if defined(FAG_SPLIT_DK_DQ_READY)
    uint32_t dk_ready_phase = 0;
#endif

    Tensor tSTtST =  partition_fragment_C(tiled_mma_kq, select<0,1>(TileShapeKQ{}));
    tSTtST.data() = TmemAllocation::kS;

    Tensor tDPTtDPT = partition_fragment_C(tiled_mma_vdo, select<0,1>(TileShapeVDO{}));
    tDPTtDPT.data() = TmemAllocation::kDP;

    Tensor tDQtDQ = partition_fragment_C(tiled_mma_dsk, select<0,1>(TileShapeDSK{}));
    tDQtDQ.data() = TmemAllocation::kDQ;

    Tensor tDKtDK = partition_fragment_C(tiled_mma_dsq, select<0,1>(TileShapeDSQ{}));
    tDKtDK.data() = TmemAllocation::kDK;

    Tensor tDVtDV = partition_fragment_C(tiled_mma_pdo, select<0,1>(TileShapePDO{}));
    tDVtDV.data() = TmemAllocation::kDV;

#ifdef BWD_2SM_DEBUG
    if (cute::elect_one_sync()) printf("[DBG mma-fn] blk=%d pre-loadq-wait\n", blockIdx.x);
#endif
    pipeline_load_mma_q.consumer_wait(pipeline_load_mma_q_consumer_state);
#ifdef BWD_2SM_DEBUG
    if (cute::elect_one_sync()) printf("[DBG mma-fn] blk=%d post-loadq-wait\n", blockIdx.x);
#endif
    pipeline_mma_compute_s.producer_acquire(pipeline_mma_compute_s_producer_state);
#ifdef BWD_2SM_DEBUG
    if (cute::elect_one_sync()) printf("[DBG mma-fn] blk=%d post-s-acquire (issuing S gemm)\n", blockIdx.x);
#endif

    // S = Q*K   S^T = Q*K^T 存储的是转置
    tiled_mma_kq.accumulate_ = UMMA::ScaleOut::Zero;
    CUTLASS_PRAGMA_UNROLL
    for (int k_block = 0; k_block < size<2>(tSTrQ); ++k_block) {
#ifndef FAG_ABLATE_S
      cute::gemm(tiled_mma_kq,
                 tSTrK(_,_,k_block,_0{}),
                 tSTrQ(_,_,k_block,pipeline_load_mma_q_consumer_state.index()),
                 tSTtST);
#endif
      tiled_mma_kq.accumulate_ = UMMA::ScaleOut::One;
    }

    pipeline_mma_compute_s.producer_commit(pipeline_mma_compute_s_producer_state);
    ++pipeline_mma_compute_s_producer_state;
    pipeline_load_mma_q.consumer_release(pipeline_load_mma_q_consumer_state);
    ++pipeline_load_mma_q_consumer_state;
#ifdef BWD_2SM_DEBUG
    if (cute::elect_one_sync()) printf("[DBG mma-fn] blk=%d post-S-commit\n", blockIdx.x);
#endif

    pipeline_load_mma_do.consumer_wait(pipeline_load_mma_do_consumer_state);
#ifdef BWD_2SM_DEBUG
    if (cute::elect_one_sync()) printf("[DBG mma-fn] blk=%d post-loaddo-wait\n", blockIdx.x);
#endif

    pipeline_mma_compute_dp.producer_acquire(pipeline_mma_compute_dp_producer_state);

    // dP = dO*V   dP^T = dO*V^T 存储的是转置
    tiled_mma_vdo.accumulate_ = UMMA::ScaleOut::Zero;
    CUTLASS_PRAGMA_UNROLL
    for (int k_block = 0; k_block < size<2>(tDPTrV); ++k_block) {
#ifndef FAG_ABLATE_DP
      cute::gemm(tiled_mma_vdo,
                 tDPTrV(_,_,k_block,_0{}),
                 tDPTrDO(_,_,k_block,pipeline_load_mma_do_consumer_state.index()),
                 tDPTtDPT);
#endif
      tiled_mma_vdo.accumulate_ = UMMA::ScaleOut::One;
    }

    pipeline_mma_compute_dp.producer_commit(pipeline_mma_compute_dp_producer_state);
    ++pipeline_mma_compute_dp_producer_state;
#ifdef BWD_2SM_DEBUG
    if (cute::elect_one_sync()) printf("[DBG mma-fn] blk=%d post-dP-commit\n", blockIdx.x);
#endif

    pipeline_compute_mma_p.consumer_wait(pipeline_compute_mma_p_consumer_state);
#ifdef BWD_2SM_DEBUG
    if (cute::elect_one_sync()) printf("[DBG mma-fn] blk=%d post-P-wait (issuing dV gemm)\n", blockIdx.x);
#endif

    // dV = P*dO   dV = P^T*dO
    CUTLASS_PRAGMA_UNROLL
    for (int k_block = 0; k_block < size<2>(tDVrP); ++k_block) {
#ifndef FAG_ABLATE_DV
      cute::gemm(tiled_mma_pdo,
                 tDVrP(_,_,k_block),
                 tDVrDOT(_,_,k_block,pipeline_load_mma_do_consumer_state.index()),
                 tDVtDV);
#endif
      tiled_mma_pdo.accumulate_ = UMMA::ScaleOut::One;
    }

    pipeline_compute_mma_p.consumer_release(pipeline_compute_mma_p_consumer_state);
    ++pipeline_compute_mma_p_consumer_state;

    pipeline_load_mma_do.consumer_release(pipeline_load_mma_do_consumer_state);
    ++pipeline_load_mma_do_consumer_state;

    // Kt is invariant across the Q loop.  Its TMA runs concurrently with the
    // prologue S/dP/dV work and is only required by the first dQ below.
    pipeline_load_mma_kt.consumer_wait(pipeline_load_mma_kt_consumer_state);
#ifdef BWD_2SM_DEBUG
    if (cute::elect_one_sync()) printf("[DBG mma-fn] blk=%d post-dV-releases iter_count now=%d\n", blockIdx.x, iter_count - 1);
#endif

    iter_count -= 1;

    // S and P overlap. dQ occupies the upper half of the S/P region, and its
    // reuse is protected by the reduce pipeline plus Compute's delayed dS
    // publication.
    while (iter_count > 0) {
      // S_next shares its upper half with the previous dQ accumulator.  Wait
      // until Reduce has consumed that slot before issuing S_next.  On the
      // first main-loop iteration this acquire is immediately ready.
      pipeline_mma_reduce_dq.producer_acquire(pipeline_mma_reduce_dq_producer_state);
#ifdef BWD_2SM_DEBUG
      if (cute::elect_one_sync()) printf("[DBG mma-fn] blk=%d iter-loop pre-loadq-wait2\n", blockIdx.x);
#endif
      pipeline_load_mma_q.consumer_wait(pipeline_load_mma_q_consumer_state);
#ifdef BWD_2SM_DEBUG
      if (cute::elect_one_sync()) printf("[DBG mma-fn] blk=%d iter-loop post-loadq-wait2\n", blockIdx.x);
#endif
      pipeline_mma_compute_s.producer_acquire(pipeline_mma_compute_s_producer_state);

      // S = Q*K
      tiled_mma_kq.accumulate_ = UMMA::ScaleOut::Zero;
      CUTLASS_PRAGMA_UNROLL
      for (int k_block = 0; k_block < size<2>(tSTrQ); ++k_block) {
#ifndef FAG_ABLATE_S
        cute::gemm(tiled_mma_kq,
                   tSTrK(_,_,k_block,_0{}),
                   tSTrQ(_,_,k_block,pipeline_load_mma_q_consumer_state.index()),
                   tSTtST);
#endif
        tiled_mma_kq.accumulate_ = UMMA::ScaleOut::One;
      }

      pipeline_mma_compute_s.producer_commit(pipeline_mma_compute_s_producer_state);
      ++pipeline_mma_compute_s_producer_state;
      pipeline_load_mma_q.consumer_release(pipeline_load_mma_q_consumer_state);
      ++pipeline_load_mma_q_consumer_state;

#ifdef BWD_2SM_DEBUG
    if (cute::elect_one_sync()) printf("[DBG mma-fn] blk=%d iter-loop pre-ds-wait\n", blockIdx.x);
#endif
#if defined(FAG_SPLIT_DK_DQ_READY)
    shared_tensors.dk_ready.wait(dk_ready_phase);
    dk_ready_phase ^= 1;
#else
    pipeline_compute_mma_ds.consumer_wait(pipeline_compute_mma_ds_consumer_state);
#endif
#ifdef BWD_2SM_DEBUG
    if (cute::elect_one_sync()) printf("[DBG mma-fn] blk=%d iter-loop post-ds-wait\n", blockIdx.x);
#endif

#if !defined(BWD_2SM_SERIAL_BASELINE)
      // Acquire the next dP stage after Compute released the previous one.
      pipeline_mma_compute_dp.producer_acquire(pipeline_mma_compute_dp_producer_state);
#endif

      // FA4 dK first: consume dS from TMEM and the independent Qt load stage.
      // Compute's delayed dS commit also guarantees S(i) has already reached
      // registers before the following dQ overwrites S(i)'s upper half.
      pipeline_load_mma_qt.consumer_wait(pipeline_load_mma_qt_consumer_state);
      CUTLASS_PRAGMA_UNROLL
      for (int k_block = 0; k_block < size<2>(tDKrDST); ++k_block) {
#ifndef FAG_ABLATE_DK
        cute::gemm(tiled_mma_dsq,
                   tDKrDST(_,_,k_block),
                   tDKrQT(_,_,k_block,pipeline_load_mma_qt_consumer_state.index()),
                   tDKtDK);
#endif
        tiled_mma_dsq.accumulate_ = UMMA::ScaleOut::One;
      }
      pipeline_load_mma_qt.consumer_release(pipeline_load_mma_qt_consumer_state);
      ++pipeline_load_mma_qt_consumer_state;

#if !defined(BWD_2SM_SERIAL_BASELINE)
      // FA4 overlap: dP(next) may be issued as soon as dK(cur) has consumed
      // the aliased TMEM dS.  It does not overlap the dQ accumulator anymore.
      pipeline_load_mma_do.consumer_wait(pipeline_load_mma_do_consumer_state);
      tiled_mma_vdo.accumulate_ = UMMA::ScaleOut::Zero;
      CUTLASS_PRAGMA_UNROLL
      for (int k_block = 0; k_block < size<2>(tDPTrV); ++k_block) {
#ifndef FAG_ABLATE_DP
        cute::gemm(tiled_mma_vdo,
                   tDPTrV(_,_,k_block,_0{}),
                   tDPTrDO(_,_,k_block,pipeline_load_mma_do_consumer_state.index()),
                   tDPTtDPT);
#endif
        tiled_mma_vdo.accumulate_ = UMMA::ScaleOut::One;
      }
      pipeline_mma_compute_dp.producer_commit(pipeline_mma_compute_dp_producer_state);
      ++pipeline_mma_compute_dp_producer_state;
#endif

      // dQ = dS*K (SMEM dS; writes the TMEM region only after dK completed).   dQ=dS*K^T
#if defined(FAG_SPLIT_DK_DQ_READY)
      // Unlike dK, dQ must wait until Compute has loaded S(next), because its
      // accumulator aliases the upper half of that S/P TMEM allocation.
      pipeline_compute_mma_ds.consumer_wait(pipeline_compute_mma_ds_consumer_state);
#endif
      // Corner-turn sync edge: MMA must not issue dQ before both CTAs' dS
      // exchange completed (relayed here by the Relay warp).
#ifndef FAG_ABLATE_CORNER_WAIT
      shared_tensors.ds_leader.wait(ds_leader_phase);
#endif
      ds_leader_phase ^= 1;
      tiled_mma_dsk.accumulate_ = UMMA::ScaleOut::Zero;
      CUTLASS_PRAGMA_UNROLL
      for (int k_block = 0; k_block < size<2>(tDQrDS); ++k_block) {
#ifndef FAG_ABLATE_DQ
        cute::gemm(tiled_mma_dsk,
                   tDQrDS(_,_,k_block,pipeline_compute_mma_ds_consumer_state.index()),
                   tDQrKT(_,_,k_block,_0{}),
                   tDQtDQ);
#endif
        tiled_mma_dsk.accumulate_ = UMMA::ScaleOut::One;
      }

      pipeline_mma_reduce_dq.producer_commit(pipeline_mma_reduce_dq_producer_state);
      ++pipeline_mma_reduce_dq_producer_state;

      pipeline_compute_mma_ds.consumer_release(pipeline_compute_mma_ds_consumer_state);
      ++pipeline_compute_mma_ds_consumer_state;

#if defined(BWD_2SM_SERIAL_BASELINE)
      // Accuracy-equivalent scheduling baseline used only for the performance
      // gate: wait until dQ(cur) has been issued before starting dP(next).
      pipeline_mma_compute_dp.producer_acquire(pipeline_mma_compute_dp_producer_state);
      pipeline_load_mma_do.consumer_wait(pipeline_load_mma_do_consumer_state);
      tiled_mma_vdo.accumulate_ = UMMA::ScaleOut::Zero;
      CUTLASS_PRAGMA_UNROLL
      for (int k_block = 0; k_block < size<2>(tDPTrV); ++k_block) {
#ifndef FAG_ABLATE_DP
        cute::gemm(tiled_mma_vdo,
                   tDPTrV(_,_,k_block,_0{}),
                   tDPTrDO(_,_,k_block,pipeline_load_mma_do_consumer_state.index()),
                   tDPTtDPT);
#endif
        tiled_mma_vdo.accumulate_ = UMMA::ScaleOut::One;
      }
      pipeline_mma_compute_dp.producer_commit(pipeline_mma_compute_dp_producer_state);
      ++pipeline_mma_compute_dp_producer_state;
#endif

#ifdef BWD_2SM_DEBUG
      if (cute::elect_one_sync()) printf("[DBG mma-fn] blk=%d iter-loop pre-P-wait2\n", blockIdx.x);
#endif
      pipeline_compute_mma_p.consumer_wait(pipeline_compute_mma_p_consumer_state);
#ifdef BWD_2SM_DEBUG
      if (cute::elect_one_sync()) printf("[DBG mma-fn] blk=%d iter-loop post-P-wait2\n", blockIdx.x);
#endif

      // dV = P*dO
      CUTLASS_PRAGMA_UNROLL
      for (int k_block = 0; k_block < size<2>(tDVrP); ++k_block) {
#ifndef FAG_ABLATE_DV
        cute::gemm(tiled_mma_pdo,
                   tDVrP(_,_,k_block),
                   tDVrDOT(_,_,k_block,pipeline_load_mma_do_consumer_state.index()),
                   tDVtDV);
#endif
        tiled_mma_pdo.accumulate_ = UMMA::ScaleOut::One;
      }

      pipeline_compute_mma_p.consumer_release(pipeline_compute_mma_p_consumer_state);
      ++pipeline_compute_mma_p_consumer_state;

      pipeline_load_mma_do.consumer_release(pipeline_load_mma_do_consumer_state);
      ++pipeline_load_mma_do_consumer_state;
#ifdef BWD_2SM_DEBUG
      if (cute::elect_one_sync()) printf("[DBG mma-fn] blk=%d iter-loop end iter_count->%d\n", blockIdx.x, iter_count - 1);
#endif

      iter_count -= 1;
    }
#ifdef BWD_2SM_DEBUG
    if (cute::elect_one_sync()) printf("[DBG mma-fn] blk=%d LOOP-EXIT, tail start\n", blockIdx.x);
#endif

    // signal to the epilogue that dV is ready
    pipeline_mma_compute_dkdv.producer_acquire(pipeline_mma_compute_dkdv_producer_state);
#ifdef BWD_2SM_DEBUG
    if (cute::elect_one_sync()) printf("[DBG mma-fn] blk=%d tail post-dkdv-acq1\n", blockIdx.x);
#endif
    pipeline_mma_compute_dkdv.producer_commit(pipeline_mma_compute_dkdv_producer_state);
    ++pipeline_mma_compute_dkdv_producer_state;

    pipeline_mma_compute_dkdv.producer_acquire(pipeline_mma_compute_dkdv_producer_state);
#ifdef BWD_2SM_DEBUG
    if (cute::elect_one_sync()) printf("[DBG mma-fn] blk=%d tail post-dkdv-acq2\n", blockIdx.x);
#endif

#if defined(FAG_SPLIT_DK_DQ_READY)
    shared_tensors.dk_ready.wait(dk_ready_phase);
    dk_ready_phase ^= 1;
#else
    pipeline_compute_mma_ds.consumer_wait(pipeline_compute_mma_ds_consumer_state);
#endif
#ifdef BWD_2SM_DEBUG
    if (cute::elect_one_sync()) printf("[DBG mma-fn] blk=%d tail post-ds-wait\n", blockIdx.x);
#endif

    // dK = dS*Qt (TMEM dS, independent Qt stage)
    pipeline_load_mma_qt.consumer_wait(pipeline_load_mma_qt_consumer_state);
    CUTLASS_PRAGMA_UNROLL
    for (int k_block = 0; k_block < size<2>(tDKrDST); ++k_block) {
#ifndef FAG_ABLATE_DK
      cute::gemm(tiled_mma_dsq,
                 tDKrDST(_,_,k_block),
                 tDKrQT(_,_,k_block,pipeline_load_mma_qt_consumer_state.index()),
                 tDKtDK);
#endif
      tiled_mma_dsq.accumulate_ = UMMA::ScaleOut::One;
    }
    pipeline_load_mma_qt.consumer_release(pipeline_load_mma_qt_consumer_state);
    ++pipeline_load_mma_qt_consumer_state;

    // signal to epilgue that dK is ready
    pipeline_mma_compute_dkdv.producer_commit(pipeline_mma_compute_dkdv_producer_state);
    ++pipeline_mma_compute_dkdv_producer_state;

    // Acquire the final dQ slot.  For a single-Q-tile problem no main-loop
    // iteration ran, while for longer problems this waits for dQ(N-2) Reduce.
    pipeline_mma_reduce_dq.producer_acquire(pipeline_mma_reduce_dq_producer_state);

    // dQ = dS*K
#if defined(FAG_SPLIT_DK_DQ_READY)
    pipeline_compute_mma_ds.consumer_wait(pipeline_compute_mma_ds_consumer_state);
#endif
    // Corner-turn sync edge (tail dQ): see FAG_ABLATE_CORNER_WAIT note above.
#ifndef FAG_ABLATE_CORNER_WAIT
    shared_tensors.ds_leader.wait(ds_leader_phase);
#endif
    ds_leader_phase ^= 1;
    tiled_mma_dsk.accumulate_ = UMMA::ScaleOut::Zero;
    CUTLASS_PRAGMA_UNROLL
    for (int k_block = 0; k_block < size<2>(tDQrDS); ++k_block) {
#ifndef FAG_ABLATE_DQ
      cute::gemm(tiled_mma_dsk,
                 tDQrDS(_,_,k_block,pipeline_compute_mma_ds_consumer_state.index()),
                 tDQrKT(_,_,k_block,_0{}),
                 tDQtDQ);
#endif
      tiled_mma_dsk.accumulate_ = UMMA::ScaleOut::One;
    }

    pipeline_mma_reduce_dq.producer_commit(pipeline_mma_reduce_dq_producer_state);
    ++pipeline_mma_reduce_dq_producer_state;

    pipeline_compute_mma_ds.consumer_release(pipeline_compute_mma_ds_consumer_state);
    ++pipeline_compute_mma_ds_consumer_state;

    pipeline_load_mma_kt.consumer_release(pipeline_load_mma_kt_consumer_state);
    ++pipeline_load_mma_kt_consumer_state;
#ifdef BWD_2SM_DEBUG
    if (cute::elect_one_sync()) printf("[DBG mma-fn] blk=%d mma() RETURN\n", blockIdx.x);
#endif
  }



  template<class TensorG, class TensorR, class TensorC, class TensorShape>
  CUTLASS_DEVICE void store(
      TensorG gmem,
      TensorR const& regs,
      TensorC const& coord,
      TensorShape const& tensor_shape) {

    Tensor preds = cute::lazy::transform(coord, [&](auto const& c) { return elem_less(c, tensor_shape); });

    auto copy_op = make_cotiled_copy(
        Copy_Atom<UniversalCopy<uint128_t>, Element>{},
        make_layout(make_shape(_1{}, Int<sizeof(uint128_t) / sizeof(Element)>{})),
        regs.layout()
    );
    auto thr_copy = copy_op.get_slice(_0{});

    Tensor quantized_regs = quantize(regs);
    Tensor tCr = thr_copy.partition_S(quantized_regs);
    Tensor tCg = thr_copy.partition_D(gmem);
    Tensor tPc = thr_copy.partition_D(preds);

    copy_if(copy_op, tPc, tCr, tCg);
  }


  template<class BlkCoord, class BlkOffset, class ProblemShape_>
  CUTLASS_DEVICE void epilogue_clear(
      BlkCoord const& blk_coord,
      BlkOffset const& blk_offset,
      ProblemShape_ const& problem_shape,
      MainloopArguments const& mainloop_args,
      EpilogueArguments const& epilogue_args) {

    auto [Q, K, D, D_VO, HB] = problem_shape;
    auto [blk_coord_q, blk_coord_k, blk_coord_d, blk_coord_dv, blk_coord_batch] = blk_coord;

    auto mDK_in = make_tensor(make_gmem_ptr(epilogue_args.ptr_dk), make_shape(K, TileShapeDQK{}, HB), epilogue_args.stride_dk);
    auto mDK = domain_offset(select<1,2,4>(blk_offset), mDK_in);
    auto gDK = local_tile(mDK, TileShapeDSQ{}, make_coord(_,_,_), Step<_1, _1, X>{})
        (_, _, blk_coord_k, _0{}, blk_coord_batch);

    Tensor cDK = domain_offset(
        make_coord(get<1>(blk_coord) * TileShapeK{}, _0{}),
        make_identity_tensor(take<0,2>(TileShapeDSQ{}))
    );

    auto mDV_in = make_tensor(make_gmem_ptr(epilogue_args.ptr_dv), make_shape(K, TileShapeDVO{}, HB), epilogue_args.stride_dv);
    auto mDV = domain_offset(select<1,3,4>(blk_offset), mDV_in);
    auto gDV = local_tile(mDV, TileShapePDO{}, make_coord(_,_,_), Step<_1, _1, X>{})
        (_, _, blk_coord_k, _0{}, blk_coord_batch);

    Tensor cDV = domain_offset(
        make_coord(blk_coord_k * TileShapeK{}, _0{}),
        make_identity_tensor(take<0,2>(TileShapePDO{}))
    );
    
    for (int i = threadIdx.x; i < size(gDK); i += blockDim.x) {
      if (elem_less(cDK(i), select<1,2>(problem_shape))) {
        gDK(i) = Element(0);
      }
    }
    for (int i = threadIdx.x; i < size(gDV); i += blockDim.x) {
      if (elem_less(cDV(i), select<1,3>(problem_shape))) {
        gDV(i) = Element(0);
      }
    }
  }


  template<class BlkCoord, class BlkOffset, class ProblemShape_>
  CUTLASS_DEVICE void epilogue(
      BlkCoord const& blk_coord,
      BlkOffset const& blk_offset,
      ProblemShape_ const& problem_shape,
      MainloopArguments const& mainloop_args,
      EpilogueArguments const& epilogue_args,
      PipelineMmaComputeDKDV& pipeline_mma_compute_dkdv,
      typename PipelineMmaComputeDKDV::PipelineState& pipeline_mma_compute_dkdv_consumer_state) {

    auto [Q, K, D, D_VO, HB] = problem_shape;
    auto [blk_coord_q, blk_coord_k, blk_coord_d, blk_coord_dv, blk_coord_batch] = blk_coord;

    auto load_op = SM100_TMEM_LOAD_32dp32b16x{};

    auto tDKtDK = partition_fragment_C(TiledMmaDSQ{}, select<0,1>(TileShapeDSQ{}))(make_coord(_,_),_0{},_0{});
    tDKtDK.data() = TmemAllocation::kDK;

    auto mDK_in = make_tensor(make_gmem_ptr(epilogue_args.ptr_dk), make_shape(K, TileShapeDQK{}, HB), epilogue_args.stride_dk);
    auto mDK = domain_offset(select<1,2,4>(blk_offset), mDK_in);
    auto gDK = local_tile(mDK, TileShapeDSQ{}, make_coord(_,_,_), Step<_1, _1, X>{})
        (_, _, blk_coord_k, _0{}, blk_coord_batch);

    Tensor cDK = domain_offset(
        make_coord(get<1>(blk_coord) * TileShapeK{}, _0{}),
        make_identity_tensor(take<0,2>(TileShapeDSQ{}))
    );

    constexpr int kNumWarpgroups = kNumComputeWarps / 4;
    int dp_idx = threadIdx.x % 128;
    int wg_idx = (threadIdx.x % (kNumComputeWarps * NumThreadsPerWarp)) / 128;

    auto split_wg = [&](auto const& t) {
      if constexpr (decltype(rank(t))::value == 3) {
        auto p = t.compose(make_layout(make_shape(size<0>(t), size<1>(t), make_shape(Int<kNumWarpgroups>{}, size<2>(t) / Int<kNumWarpgroups>{}))));
        return p(_, _, make_coord(wg_idx, _));
      }
      else {
        auto p = t.compose(make_layout(make_shape(size<0>(t), size<1>(t), size<2>(t), make_shape(Int<kNumWarpgroups>{}, size<3>(t) / Int<kNumWarpgroups>{}))));
        return p(_, _, _, make_coord(wg_idx, _));
      }
    };

    auto tiled_t2r_dk = make_tmem_copy(load_op, tDKtDK);
    auto thread_t2r_dk = tiled_t2r_dk.get_slice(dp_idx);

    Tensor tTR_cDK   = split_wg(thread_t2r_dk.partition_D(cDK));
    Tensor tTR_gDK   = split_wg(thread_t2r_dk.partition_D(gDK));
    Tensor tTR_rDK = make_tensor<ElementAcc>(shape(tTR_cDK));
    Tensor tTR_tDK = split_wg(thread_t2r_dk.partition_S(tDKtDK));

    auto tDVtDV = partition_fragment_C(TiledMmaDSQ{}, select<0,1>(TileShapeDSQ{}))(make_coord(_,_),_0{},_0{});
    tDVtDV.data() = TmemAllocation::kDV;

    auto mDV_in = make_tensor(make_gmem_ptr(epilogue_args.ptr_dv), make_shape(K, TileShapeDVO{}, HB), epilogue_args.stride_dv);
    auto mDV = domain_offset(select<1,3,4>(blk_offset), mDV_in);
    auto gDV = local_tile(mDV, TileShapePDO{}, make_coord(_,_,_), Step<_1, _1, X>{})
        (_, _, blk_coord_k, _0{}, blk_coord_batch);

    Tensor cDV = domain_offset(
        make_coord(blk_coord_k * TileShapeK{}, _0{}),
        make_identity_tensor(take<0,2>(TileShapePDO{}))
    );

    auto tiled_t2r_dv = make_tmem_copy(load_op, tDVtDV);
    auto thread_t2r_dv = tiled_t2r_dv.get_slice(dp_idx);

    Tensor tTR_cDV   = split_wg(thread_t2r_dv.partition_D(cDV));
    Tensor tTR_gDV   = split_wg(thread_t2r_dv.partition_D(gDV));
    Tensor tTR_rDV = make_tensor<ElementAcc>(shape(tTR_cDV));
    Tensor tTR_tDV = split_wg(thread_t2r_dv.partition_S(tDVtDV));

    pipeline_mma_compute_dkdv.consumer_wait(pipeline_mma_compute_dkdv_consumer_state);

    // load tDVtDV
    cute::copy(tiled_t2r_dv, tTR_tDV, tTR_rDV);

    // store tDVgDV
    store(tTR_gDV, tTR_rDV, tTR_cDV, select<1,3>(problem_shape));

    cutlass::arch::fence_view_async_tmem_load();
    pipeline_mma_compute_dkdv.consumer_release(pipeline_mma_compute_dkdv_consumer_state);
    ++pipeline_mma_compute_dkdv_consumer_state;

    pipeline_mma_compute_dkdv.consumer_wait(pipeline_mma_compute_dkdv_consumer_state);

    // load tDKtDK
    cute::copy(tiled_t2r_dk, tTR_tDK, tTR_rDK);

    CUTLASS_PRAGMA_UNROLL
    for (int i = 0; i < size(tTR_rDK); i++) {
      tTR_rDK(i) = mainloop_args.softmax_scale * tTR_rDK(i);
    }

    // store tDKgDK
    store(tTR_gDK, tTR_rDK, tTR_cDK, select<1,2>(problem_shape));

    cutlass::arch::fence_view_async_tmem_load();
    pipeline_mma_compute_dkdv.consumer_release(pipeline_mma_compute_dkdv_consumer_state);
    ++pipeline_mma_compute_dkdv_consumer_state;

  }


  template<class BlkCoord, class BlkOffset, class ProblemShape_>
  CUTLASS_DEVICE void compute(
      BlkCoord const& blk_coord,
      BlkOffset const& blk_offset,
      ProblemShape_ const& problem_shape,
      int iter_start,
      int iter_end,
      int iter_count,
      MainloopArguments const& mainloop_args,
      EpilogueArguments const& epilogue_args,
      TensorStorage& shared_tensors,
      PipelineLoadComputeLSE& pipeline_load_compute_lse,
      typename PipelineLoadComputeLSE::PipelineState& pipeline_load_compute_lse_consumer_state,
      PipelineLoadComputeSumOdO& pipeline_load_compute_sum_odo,
      typename PipelineLoadComputeSumOdO::PipelineState& pipeline_load_compute_sum_odo_consumer_state,
      PipelineMmaComputeS& pipeline_mma_compute_s,
      typename PipelineMmaComputeS::PipelineState& pipeline_mma_compute_s_consumer_state,
      PipelineMmaComputeDP& pipeline_mma_compute_dp,
      typename PipelineMmaComputeDP::PipelineState& pipeline_mma_compute_dp_consumer_state,
      PipelineComputeMmaP& pipeline_compute_mma_p,
      typename PipelineComputeMmaP::PipelineState& pipeline_compute_mma_p_producer_state,
      PipelineComputeMmaDS& pipeline_compute_mma_ds,
      typename PipelineComputeMmaDS::PipelineState& pipeline_compute_mma_ds_producer_state,
      PipelineMmaComputeDKDV& pipeline_mma_compute_dkdv,
      typename PipelineMmaComputeDKDV::PipelineState& pipeline_mma_compute_dkdv_consumer_state) {


    auto [Q, K, D, D_VO, HB] = problem_shape;
    int iter_index = iter_start;

    // S/P overlap in TMEM; dQ uses their upper half under delayed publication.

    // there are two compute wg's that cooperatively compute softmax
    // they are striped by this tmem atom, i.e. wg0 has 16 elems, then wg1 etc

    auto load_op = []() {
#if defined(FAG_TMEM_LOAD_32X)
      return SM100_TMEM_LOAD_32dp32b32x{};
#else
      return SM100_TMEM_LOAD_32dp32b16x{};
#endif
    }();
    auto store_op = []() {
      if constexpr (sizeof(Element) == 1) {
#if defined(FAG_TMEM_STORE_16X)
        return SM100_TMEM_STORE_32dp32b16x{};
#elif defined(FAG_TMEM_STORE_8X)
        return SM100_TMEM_STORE_32dp32b8x{};
#else
        return SM100_TMEM_STORE_32dp32b4x{};
#endif
      }
      else {
        return SM100_TMEM_STORE_32dp32b8x{};
      }
    }();

    Tensor tSTtST =  partition_fragment_C(TiledMmaKQ{}, select<0,1>(TileShapeKQ{}))(make_coord(_,_),_0{},_0{});
    tSTtST.data() = TmemAllocation::kS;

    Tensor tDPTtDPT =  partition_fragment_C(TiledMmaVDO{}, select<0,1>(TileShapeVDO{}))(make_coord(_,_),_0{},_0{});
    tDPTtDPT.data() = TmemAllocation::kDP;

    Tensor cST = make_identity_tensor(take<0,2>(TileShapeKQ{}));
    Tensor cDPT = make_identity_tensor(take<0,2>(TileShapeVDO{}));

    constexpr int kNumWarpgroups = kNumComputeWarps / 4;
    int dp_idx = threadIdx.x % 128;
    int wg_idx = (threadIdx.x % (kNumComputeWarps * NumThreadsPerWarp)) / 128;
    auto tiled_t2r = make_tmem_copy(load_op, tSTtST);
    auto thread_t2r = tiled_t2r.get_slice(dp_idx);

    auto split_wg = [&](auto const& t) {
      if constexpr (decltype(size<1>(t))::value > 1) {
        if constexpr (decltype(rank(t))::value == 3) {
          auto p = t.compose(make_layout(make_shape(size<0>(t), make_shape(Int<kNumWarpgroups>{}, size<1>(t) / Int<kNumWarpgroups>{}), size<2>(t))));
          return p(_, make_coord(wg_idx, _), _);
        }
        else {
          auto p = t.compose(make_layout(make_shape(size<0>(t), make_shape(Int<kNumWarpgroups>{}, size<1>(t) / Int<kNumWarpgroups>{}), size<2>(t), size<3>(t))));
          return p(_, make_coord(wg_idx, _), _, _);
        }
      }
      else {
        if constexpr (decltype(rank(t))::value == 3) {
          auto p = t.compose(make_layout(make_shape(size<0>(t), size<1>(t), make_shape(Int<kNumWarpgroups>{}, size<2>(t) / Int<kNumWarpgroups>{}))));
          return p(_, _, make_coord(wg_idx, _));
        }
        else {
          auto p = t.compose(make_layout(make_shape(size<0>(t), size<1>(t), size<2>(t), make_shape(Int<kNumWarpgroups>{}, size<3>(t) / Int<kNumWarpgroups>{}))));
          return p(_, _, _, make_coord(wg_idx, _));
        }

      }
    };


    Tensor tTR_cST_p = thread_t2r.partition_D(cST);
    Tensor tTR_cST   = split_wg(tTR_cST_p);
    Tensor tTR_rST = make_tensor<ElementAcc>(shape(tTR_cST));
    // Tensor tTR_tST_p = thread_t2r.partition_S(tSTtST);
    Tensor tTR_tST = split_wg(thread_t2r.partition_S(tSTtST));

    Tensor tTR_cDPT_p = thread_t2r.partition_D(cDPT);
    Tensor tTR_cDPT = split_wg(tTR_cDPT_p);
    Tensor tTR_rDPT = make_tensor<ElementAcc>(shape(tTR_cDPT));
    Tensor tTR_tDPT = split_wg(thread_t2r.partition_S(tDPTtDPT));

    Tensor sLSE = make_tensor(make_smem_ptr(shared_tensors.smem_lse.begin()), SmemLayoutLSE{});
    Tensor sSumOdO = make_tensor(make_smem_ptr(shared_tensors.smem_sum_odo.begin()), SmemLayoutSumOdO{});

    auto sP = make_tensor(make_smem_ptr((Element*) nullptr), typename CollectiveMmaPDO::SmemLayoutA{});

    auto tDVrP = TiledMmaPDO::make_fragment_A(sP)(_, _, _, _0{});
    auto tDVcST = TiledMmaPDO{}.get_slice(_0{}).partition_A(cST);
    tDVrP.data() = TmemAllocation::kP;

    auto tiled_r2t = make_tmem_copy(store_op, tDVrP);
    auto thread_r2t = tiled_r2t.get_slice(dp_idx);

    auto tRT_tP = split_wg(thread_r2t.partition_D(tDVrP));
    auto tRT_cST_p = thread_r2t.partition_S(tDVcST);
    auto tRT_cST = split_wg(tRT_cST_p);

#if defined(FAG_DEBUG_TMEM_MAP)
    // Diagnostic only: verify whether the S TMEM-load ownership and P
    // TMEM-store ownership are identical for every element handled by a
    // compute thread.  If they differ, the full compute named barrier between
    // the load and store is required to prevent one thread from overwriting
    // another thread's still-pending S load.
    if (blockIdx.x == 0 && blockIdx.y == 0 && blockIdx.z == 0 &&
        (dp_idx == 0 || dp_idx == 1)) {
      int mismatches = 0;
      CUTLASS_PRAGMA_UNROLL
      for (int i = 0; i < size(tTR_cST); ++i) {
        auto load_coord = tTR_cST(i);
        auto store_coord = tRT_cST(i);
        bool same = int(get<0>(load_coord)) == int(get<0>(store_coord)) &&
                    int(get<1>(load_coord)) == int(get<1>(store_coord));
        mismatches += !same;
        if (!same && mismatches <= 8) {
          printf("[TMEM_MAP] rank=%d wg=%d dp=%d i=%d load=(%d,%d) store=(%d,%d)\n",
                 int(cute::block_rank_in_cluster()), wg_idx, dp_idx, i,
                 int(get<0>(load_coord)), int(get<1>(load_coord)),
                 int(get<0>(store_coord)), int(get<1>(store_coord)));
        }
      }
      printf("[TMEM_MAP_SUMMARY] rank=%d wg=%d dp=%d elems=%d mismatches=%d\n",
             int(cute::block_rank_in_cluster()), wg_idx, dp_idx,
             int(size(tTR_cST)), mismatches);
    }
#endif

    // FA4 Stage A: register-to-TMEM store view for quantized dS. The destination
    // layout is exactly the TS dK MMA's A fragment, so no SMEM reinterpretation
    // or implicit transpose is involved.
    auto sDS_null = make_tensor(
        make_smem_ptr((Element*) nullptr), typename CollectiveMmaDSQ::SmemLayoutA{});
    auto tDKrDS_tmem = TiledMmaDSQ::make_fragment_A(sDS_null)(_, _, _, _0{});
    tDKrDS_tmem.data() = TmemAllocation::kDS;
    auto tiled_r2t_ds = make_tmem_copy(store_op, tDKrDS_tmem);
    auto thread_r2t_ds = tiled_r2t_ds.get_slice(dp_idx);
    auto tRT_tDST = split_wg(thread_r2t_ds.partition_D(tDKrDS_tmem));
    auto tDVcDST = TiledMmaDSQ{}.get_slice(_0{}).partition_A(cDPT);
    auto tRT_cDST = split_wg(thread_r2t_ds.partition_S(tDVcDST));

    int cta_rank = int(cute::block_rank_in_cluster());
    constexpr int kDSHalfElements = cute::cosize_v<SmemLayoutDSHalf>;
    auto sDS_own = make_tensor(
        make_smem_ptr(shared_tensors.smem_ds.begin() + cta_rank * kDSHalfElements),
        SmemLayoutDSHalf{});
    auto sDS_xchg = make_tensor(
        make_smem_ptr(shared_tensors.smem_ds_xchg.begin()), SmemLayoutDSHalf{});

#if defined(FAG_DS_RUN_STORES)
    // Loop-invariant dS corner-turn store addresses (FAG_DS_RUN_STORES).
    // With the x32 TMEM-load atom every thread owns two 32-value runs of
    // consecutive Q at its fixed KV lane dp_idx: fragment element i belongs to
    // Q(i) = 32*(wg + 2*k) + (i % 32), k = i / 32.  Run k carries the Q-half
    // owned by CTA rank k (rows [32*wg, 32*wg+32) of that half after rebasing),
    // so it stores into the own smem_ds half when k == cta_rank and into the
    // exchange staging buffer otherwise -- ownership is decided once per run,
    // never per element or per strip.  In SmemLayoutDSHalf the SW64 atom maps
    // 16-byte row-block b = q>>4 at column kv to physical block b ^ t with
    // t = (kv & 7) >> 1; the two blocks of a run are (2*wg) and (2*wg + 1),
    // whose XOR images differ only in bit 4, so the run's second STS.128 lands
    // at (first address ^ 16).  Both addresses are therefore pure functions of
    // (wg_idx, dp_idx, cta_rank) and are hoisted out of the mainloop entirely.
    uint32_t ds_run_addr[2];
    {
      uint32_t t_swz = uint32_t(dp_idx & 7) >> 1;
      uint32_t blk = (uint32_t(2 * wg_idx) ^ t_swz) * 16;
      CUTLASS_PRAGMA_UNROLL
      for (int k = 0; k < 2; ++k) {
        Element* run_base = (k == cta_rank)
            ? shared_tensors.smem_ds.begin() + cta_rank * kDSHalfElements
            : shared_tensors.smem_ds_xchg.begin();
        ds_run_addr[k] = cute::cast_smem_ptr_to_uint(run_base)
                       + uint32_t(dp_idx) * 64 + blk;
      }
    }
#endif

#if defined(FAG_DS_STASYNC_CLUSTER)
    // Loop-invariant SMEM address of this CTA's ds_full barrier; peer-run
    // st.async stores account their bytes on the PEER's copy (mapped via
    // set_block_rank inside the helper).  Note the peer destination is the
    // own-half formula ds_run_addr[cta_rank] (this CTA's slot inside the
    // peer's smem_ds), not the staging-buffer address.
    uint32_t ds_full_addr = cute::cast_smem_ptr_to_uint(
        reinterpret_cast<uint64_t*>(&shared_tensors.ds_full));
#endif

    bool is_residual_k = get<1>(blk_coord) * TileShapeK{} + TileShapeK{} > get<1>(problem_shape);
    bool have_pending_ds = false;

#if defined(FAG_OPT_STASYNC_XCHG)
    // Arm the first xchg_tx phase before any store can complete. Later phases
    // are armed one iteration ahead (right after this iteration's wait), which
    // is ordered before the next iteration's st.async completions by the dS
    // pipeline round-trip (producer_acquire <- MMA release <- ds_leader <-
    // relay <- ds_full tx = this iteration's bulk copy completion).
    uint32_t xchg_phase = 0;
    if (dp_idx == 0 && wg_idx == 0) {
      shared_tensors.xchg_tx.expect_transaction(kTransactionsBytesDSExchange);
    }
#endif

    CUTLASS_PRAGMA_NO_UNROLL
    while (iter_count > 0) {
      // wait for S and P
#ifdef BWD_2SM_DEBUG
      if (cute::elect_one_sync()) printf("[DBG compute-fn] blk=%d rank=%d pre-S-wait\n", blockIdx.x, (int)cute::block_rank_in_cluster());
#endif
      pipeline_mma_compute_s.consumer_wait(pipeline_mma_compute_s_consumer_state);
#ifdef BWD_2SM_DEBUG
      if (cute::elect_one_sync()) printf("[DBG compute-fn] blk=%d rank=%d post-S-wait\n", blockIdx.x, (int)cute::block_rank_in_cluster());
#endif
      pipeline_compute_mma_p.producer_acquire(pipeline_compute_mma_p_producer_state);
      // wait for LSE
      pipeline_load_compute_lse.consumer_wait(pipeline_load_compute_lse_consumer_state);

#if defined(FAG_PREFETCH_DP)
      // dP for this tile is already produced by the asynchronous MMA path.
      // Issue its TMEM load before the independent softmax arithmetic so the
      // register scoreboard latency can overlap EX2.  The same FP32 fragment is
      // consumed below; only instruction scheduling changes.
      pipeline_mma_compute_dp.consumer_wait(pipeline_mma_compute_dp_consumer_state);
      cute::copy(tiled_t2r, tTR_tDPT, tTR_rDPT);
#endif

      auto dispatch_bool = [](bool b, auto fn) {
        if (b) {
          fn(cute::true_type{});
        }
        else {
          fn(cute::false_type{});
        }
      };

      bool leading_causal_masking = false;
      if constexpr (std::is_base_of_v<cutlass::fmha::collective::CausalMask<true>, Mask>) {
        leading_causal_masking = warp_uniform(iter_index == iter_start);
      } else if constexpr (std::is_base_of_v<cutlass::fmha::collective::CausalMask<false>, Mask>) {
        int offset = get<1>(problem_shape) - get<0>(problem_shape);
        int kv_left = get<1>(blk_coord) * TileShapeK{};
        int kv_right = kv_left + TileShapeK{} - 1;
        int q_left = iter_index * TileShapeQ{} + offset;
        int q_right = q_left + TileShapeQ{} - 1;

        leading_causal_masking = warp_uniform(!((q_left > kv_right) || (q_right < kv_left)));
      }
      bool trailing_residual_masking = false;
      if constexpr (std::is_base_of_v<cutlass::fmha::collective::ResidualMaskForBackward, Mask>) {
        trailing_residual_masking = warp_uniform((iter_index == iter_end - 1) || is_residual_k);
      }

      dispatch_bool(leading_causal_masking || trailing_residual_masking, [&](auto is_masked_tile) {

        // compute P = softmax(S, LSE)
        // FAG_ABLATE_S_LOAD (timing-only): skip the S TMEM->register load
        // (tcgen05.ld). The downstream mask/FMA/exp2/quantize chain still runs
        // on the (garbage) fragment, so nothing else is optimized away.
#ifndef FAG_ABLATE_S_LOAD
        cute::copy(tiled_t2r, tTR_tST, tTR_rST);
#endif

        // FA4 delayed dS publication.  dQ(i-1) aliases the upper half of the
        // S(i) TMEM allocation.  Publish dS(i-1) only after every compute
        // thread has finished loading S(i) into registers; the MMA warp cannot
        // issue the overlapping dQ until this commit becomes visible.
        if (have_pending_ds) {
          cutlass::arch::fence_view_async_tmem_load();
          pipeline_compute_mma_ds.producer_commit(pipeline_compute_mma_ds_producer_state);
          ++pipeline_compute_mma_ds_producer_state;
          have_pending_ds = false;
        }

        if constexpr (decltype(is_masked_tile)::value) {
          Mask{}.apply_mask(tTR_rST, [&](int i) {
            auto c_transpose = tTR_cST(i);
            return make_coord(get<1>(c_transpose) + iter_index * TileShapeQ{}, get<0>(c_transpose) + get<1>(blk_coord) * TileShapeK{});
          }, problem_shape);
        }

        ElementAcc log2_e = static_cast<ElementAcc>(M_LOG2E);
        float2 softmax_scale_log2_e;
        softmax_scale_log2_e.x = mainloop_args.softmax_scale * log2_e;
        softmax_scale_log2_e.y = mainloop_args.softmax_scale * log2_e;

#if defined(FAG_EX2_TWO_PHASE) || defined(FAG_EX2_HW_FIRST)
        // Preserve the exact FP32 FMA and exp2 inputs, but finish all argument
        // formation before issuing the transcendental operations.  This
        // shortens the immediate dependency chain seen by the MUFU scheduler.
        CUTLASS_PRAGMA_UNROLL
        for (int i = 0; i < size(tTR_rST); i += 2) {
          float2 acc;
          float2 lse;
          float2 out;
          acc.x = tTR_rST(i);
          acc.y = tTR_rST(i + 1);
#if defined(FAG_ABLATE_LSE_LOAD)
          // FAG_ABLATE_LSE_LOAD (timing-only): skip the LSE shared-memory
          // reads (feeds garbage to the FMA chain, which still runs).
          lse.x = 0.f;
          lse.y = 0.f;
#else
          lse.x = sLSE(get<1>(tTR_cST(i)), pipeline_load_compute_lse_consumer_state.index());
          lse.y = sLSE(get<1>(tTR_cST(i+1)), pipeline_load_compute_lse_consumer_state.index());
#endif
          cute::fma(out, softmax_scale_log2_e, acc, lse);
          tTR_rST(i) = out.x;
          tTR_rST(i+1) = out.y;
        }

#if defined(FAG_EX2_HW_FIRST)
        // Issue every independent hardware EX2 before the polynomial groups;
        // the packed FP32 FMA work can then cover MUFU result latency.
        CUTLASS_PRAGMA_UNROLL
        for (int i = 0; i < size(tTR_rST); i += 2) {
          if ((i / 2) % FAG_EX2_EMU_STRIDE != FAG_EX2_EMU_STRIDE - 1) {
            tTR_rST(i) = ::exp2f(tTR_rST(i));
            tTR_rST(i+1) = ::exp2f(tTR_rST(i+1));
          }
        }
        CUTLASS_PRAGMA_UNROLL
        for (int i = 0; i < size(tTR_rST); i += 2) {
          if ((i / 2) % FAG_EX2_EMU_STRIDE == FAG_EX2_EMU_STRIDE - 1) {
            float2 emu = ex2_emulation_2(tTR_rST(i), tTR_rST(i+1));
            tTR_rST(i) = emu.x;
            tTR_rST(i+1) = emu.y;
          }
        }
#else
        CUTLASS_PRAGMA_UNROLL
        for (int i = 0; i < size(tTR_rST); i += 2) {
          float2 out = make_float2(tTR_rST(i), tTR_rST(i+1));
#ifdef FAG_EX2_EMU_STRIDE
          static_assert(FAG_EX2_EMU_STRIDE == 1 || FAG_EX2_EMU_STRIDE == 2 ||
                        FAG_EX2_EMU_STRIDE == 4 || FAG_EX2_EMU_STRIDE == 8 ||
                        FAG_EX2_EMU_STRIDE == 16,
                        "supported software-ex2 fractions are 100%, 50%, 25%, 12.5%, and 6.25%");
          // Launch hardware EX2 first, then place an emulated pair at the end
          // of each group.  The fully-unrolled loop resolves this branch at
          // compile time and interleaves MUFU and packed-FMA pipelines.
          if ((i / 2) % FAG_EX2_EMU_STRIDE == FAG_EX2_EMU_STRIDE - 1) {
            float2 emu = ex2_emulation_2(out.x, out.y);
            tTR_rST(i) = emu.x;
            tTR_rST(i+1) = emu.y;
          } else {
            tTR_rST(i) = ::exp2f(out.x);
            tTR_rST(i+1) = ::exp2f(out.y);
          }
#else
          tTR_rST(i) = ::exp2f(out.x);
          tTR_rST(i+1) = ::exp2f(out.y);
#endif
        }
#endif
#else
        CUTLASS_PRAGMA_UNROLL
        for (int i = 0; i < size(tTR_rST); i += 2) {
          float2 acc;
          float2 lse;
          float2 out;
          acc.x = tTR_rST(i);
          acc.y = tTR_rST(i + 1);
#if defined(FAG_ABLATE_LSE_LOAD)
          // FAG_ABLATE_LSE_LOAD (timing-only): skip the LSE shared-memory
          // reads (feeds garbage to the FMA chain, which still runs).
          lse.x = 0.f;
          lse.y = 0.f;
#else
          lse.x = sLSE(get<1>(tTR_cST(i)), pipeline_load_compute_lse_consumer_state.index());
          lse.y = sLSE(get<1>(tTR_cST(i+1)), pipeline_load_compute_lse_consumer_state.index());
#endif
#if defined(FAG_ABLATE_SCALE_LSE)
          // FAG_ABLATE_SCALE_LSE (timing-only): remove BOTH the scale*S+LSE FMA
          // and every exp2 (hardware MUFU and software emulation), keeping the
          // S fragment and the LSE shared-memory reads live via zero-cost
          // register asm. P degenerates to raw S.
          asm volatile("" : "+f"(acc.x), "+f"(acc.y), "+f"(lse.x), "+f"(lse.y));
          tTR_rST(i)   = acc.x;
          tTR_rST(i+1) = acc.y;
#else
          cute::fma(out, softmax_scale_log2_e, acc, lse);
#ifndef FAG_ABLATE_EXP2
#ifdef FAG_EX2_EMU_STRIDE
          static_assert(FAG_EX2_EMU_STRIDE == 1 || FAG_EX2_EMU_STRIDE == 2 ||
                        FAG_EX2_EMU_STRIDE == 4 || FAG_EX2_EMU_STRIDE == 8 ||
                        FAG_EX2_EMU_STRIDE == 16,
                        "supported software-ex2 fractions are 100%, 50%, 25%, 12.5%, and 6.25%");
          if ((i / 2) % FAG_EX2_EMU_STRIDE == FAG_EX2_EMU_STRIDE - 1) {
            float2 emu = ex2_emulation_2(out.x, out.y);
            tTR_rST(i) = emu.x;
            tTR_rST(i+1) = emu.y;
          } else {
            tTR_rST(i) = ::exp2f(out.x);
            tTR_rST(i+1) = ::exp2f(out.y);
          }
#else
          tTR_rST(i) = ::exp2f(out.x);
          tTR_rST(i+1) = ::exp2f(out.y);
#endif
#else
          // FAG_ABLATE_EXP2: skip exp2 (identity). out = scale*S - LSE is still
          // produced by the FMA above and consumed here, so the S/LSE input
          // loads and the FMA stay live; only the exp2 instruction is removed.
          tTR_rST(i)   = out.x;
          tTR_rST(i+1) = out.y;
#endif
#endif  // FAG_ABLATE_SCALE_LSE
        }
#endif

        auto tRT_rST = quantize(tTR_rST);
        auto tRT_rST_reshaped = make_tensor(tRT_rST.data(), shape(tRT_cST));

        cutlass::arch::fence_view_async_tmem_load();
#if defined(FAG_P_WG_BARRIERS)
        // Test whether the two compute warpgroups can independently complete
        // their disjoint P fragments.  Each group still synchronizes all 128
        // of its threads before any of them overwrites S in the aliased TMEM
        // region; only cross-warpgroup waiting is removed.
        if (wg_idx == 0) {
          cutlass::arch::NamedBarrier(
            4 * NumThreadsPerWarp,
            cutlass::arch::ReservedNamedBarriers::TransformBarrier
          ).arrive_and_wait();
        } else {
          cutlass::arch::NamedBarrier(
            4 * NumThreadsPerWarp,
            cutlass::arch::ReservedNamedBarriers::StreamkBarrier0
          ).arrive_and_wait();
        }
#elif !defined(FAG_P_SELF_OWNED_TMEM)
#if !defined(FAG_ABLATE_COMPUTE_BAR)
        cutlass::arch::NamedBarrier(
          kNumComputeWarps * NumThreadsPerWarp,
          cutlass::arch::ReservedNamedBarriers::TransformBarrier
        ).arrive_and_wait();
#endif
#endif

#if defined(FAG_ABLATE_P_STORE)
        // FAG_ABLATE_P_STORE (timing-only): skip the P register->TMEM store
        // (tcgen05.st). The quantize feeding it stays live by consuming its
        // packed u32 output with zero-cost register asm. dV reads stale TMEM.
        {
          auto live_u32 = recast<uint32_t>(tRT_rST_reshaped);
          CUTLASS_PRAGMA_UNROLL
          for (int i = 0; i < size(live_u32); ++i) {
            uint32_t live = live_u32(i);
            asm volatile("" : "+r"(live));
          }
        }
#else
        cute::copy(tiled_r2t, tRT_rST_reshaped, tRT_tP);
#endif
      });

      // notify for P
      cutlass::arch::fence_view_async_tmem_store();
#ifdef BWD_2SM_DEBUG
      if (cute::elect_one_sync()) printf("[DBG compute-fn] blk=%d rank=%d pre-P-commit\n", blockIdx.x, (int)cute::block_rank_in_cluster());
#endif
      pipeline_compute_mma_p.producer_commit(pipeline_compute_mma_p_producer_state);
#ifdef BWD_2SM_DEBUG
      if (cute::elect_one_sync()) printf("[DBG compute-fn] blk=%d rank=%d post-P-commit\n", blockIdx.x, (int)cute::block_rank_in_cluster());
#endif
      ++pipeline_compute_mma_p_producer_state;
      // release S
      pipeline_mma_compute_s.consumer_release(pipeline_mma_compute_s_consumer_state);
      ++pipeline_mma_compute_s_consumer_state;
      // release LSE
      pipeline_load_compute_lse.consumer_release(pipeline_load_compute_lse_consumer_state);
      ++pipeline_load_compute_lse_consumer_state;

#if defined(FAG_PREFETCH_DP_AFTER_P)
      // P has now been published, so the MMA producer can make forward
      // progress without forming the dependency cycle of FAG_PREFETCH_DP.
      // Load the already-FP32 dP fragment before waiting on OdO so TMEM load
      // completion can overlap that independent producer wait.  Arithmetic,
      // storage types, and pipeline release order are unchanged.
      pipeline_mma_compute_dp.consumer_wait(pipeline_mma_compute_dp_consumer_state);
      cute::copy(tiled_t2r, tTR_tDPT, tTR_rDPT);
#endif

      // wait for OdO
      pipeline_load_compute_sum_odo.consumer_wait(pipeline_load_compute_sum_odo_consumer_state);
#if !defined(FAG_PREFETCH_DP) && !defined(FAG_PREFETCH_DP_AFTER_P)
      // wait for dP
      pipeline_mma_compute_dp.consumer_wait(pipeline_mma_compute_dp_consumer_state);

#if defined(FAG_DS_COLUMN_SPLIT)
      // FAG_DS_COLUMN_SPLIT: FA4-style two-chunk dS phase (requires
      // FAG_DS_RUN_STORES' x32 fragment order; the two flags share the same
      // run decomposition i <-> Q(i) = 32*(wg + 2*(i/32)) + (i%32)).  Each
      // chunk consumes exactly the fp32 P values of one 32-value run -- they
      // die with the chunk, cutting the transient P-live range in half --
      // and quantizes its 32 bytes into the same full-order fragment the
      // downstream stores consume.  The TMEM load stays whole-fragment (the
      // two warpgroups own different TMEM column blocks, so a sliced tiled
      // copy cannot express both); chunked math+quantize alone still shortens
      // the P/dS overlap and spreads F2FP converter groups across the store
      // chain.  Bytes, fragment order and handshakes unchanged.
      Tensor tTR_rDST = make_tensor<Element>(shape(tTR_cDPT));
#ifndef FAG_ABLATE_DP_LOAD
      cute::copy(tiled_t2r, tTR_tDPT, tTR_rDPT);
#endif
      CUTLASS_PRAGMA_UNROLL
      for (int k = 0; k < 2; ++k) {
        CUTLASS_PRAGMA_UNROLL
        for (int j = 0; j < 32; j += 2) {
          int i = 32 * k + j;
          float2 st;
          st.x = tTR_rST(i);
          st.y = tTR_rST(i+1);
          float2 dpt;
          dpt.x = tTR_rDPT(i);
          dpt.y = tTR_rDPT(i+1);
          float2 odo;
#if defined(FAG_ABLATE_LSE_LOAD)
          odo.x = 0.f;
          odo.y = 0.f;
#else
          odo.x = sSumOdO(get<1>(tTR_cDPT(i)), pipeline_load_compute_sum_odo_consumer_state.index());
          odo.y = sSumOdO(get<1>(tTR_cDPT(i+1)), pipeline_load_compute_sum_odo_consumer_state.index());
#endif
#ifndef FAG_ABLATE_SOFTMAX
          float2 dif;
          // sum odo is negated during preprocess
          cute::add(dif, dpt, odo);
          float2 out;
          cute::mul(out, dif, st);
          tTR_rDPT(i) = out.x;
          tTR_rDPT(i+1) = out.y;
#else
          asm volatile("" : "+f"(st.x), "+f"(st.y), "+f"(odo.x), "+f"(odo.y));
          tTR_rDPT(i)   = dpt.x;
          tTR_rDPT(i+1) = dpt.y;
#endif
        }
        Tensor run_view = make_tensor(tTR_rDPT.data() + 32 * k, Shape<_32>{});
        auto run_dst = quantize(run_view);
        CUTLASS_PRAGMA_UNROLL
        for (int j = 0; j < 32; ++j) {
          tTR_rDST(32 * k + j) = run_dst(j);
        }
      }
#else  // !FAG_DS_COLUMN_SPLIT
      // compute dS = dsoftmax(P, dP, sum_OdO)
      // FAG_ABLATE_DP_LOAD (timing-only): skip the dP TMEM->register load.
      // The softmax add+mul, quantize and pack below still run on the garbage
      // fragment, so nothing downstream is optimized away.
#ifndef FAG_ABLATE_DP_LOAD
      cute::copy(tiled_t2r, tTR_tDPT, tTR_rDPT);
#endif
#endif  // FAG_DS_COLUMN_SPLIT
#endif

#if !defined(FAG_DS_COLUMN_SPLIT)
      CUTLASS_PRAGMA_UNROLL
      for (int i = 0; i < size(tTR_rDPT); i += 2) {
        float2 st;
        st.x = tTR_rST(i);
        st.y = tTR_rST(i+1);
        float2 dpt;
        dpt.x = tTR_rDPT(i);
        dpt.y = tTR_rDPT(i+1);
        float2 odo;
        odo.x = sSumOdO(get<1>(tTR_cDPT(i)), pipeline_load_compute_sum_odo_consumer_state.index());
        odo.y = sSumOdO(get<1>(tTR_cDPT(i+1)), pipeline_load_compute_sum_odo_consumer_state.index());
#ifndef FAG_ABLATE_SOFTMAX
        float2 dif;
        // sum odo is negated during preprocess
        cute::add(dif, dpt, odo);
        float2 out;
        cute::mul(out, dif, st);
        tTR_rDPT(i) = out.x;
        tTR_rDPT(i+1) = out.y;
#else
        // FAG_ABLATE_SOFTMAX: skip the add+mul (the dS derivative). Mark st/odo
        // live with zero-cost register asm so the P/sumOdO loads are NOT DCE'd;
        // leave dP in place so the downstream quantize still compiles & runs.
        asm volatile("" : "+f"(st.x), "+f"(st.y), "+f"(odo.x), "+f"(odo.y));
        tTR_rDPT(i)   = dpt.x;
        tTR_rDPT(i+1) = dpt.y;
#endif
      }

      auto tTR_rDST = quantize(tTR_rDPT);
#endif  // !FAG_DS_COLUMN_SPLIT
#if defined(FAG_DS_RECAST_PACK)
      // NumericArrayConverter already materializes each four adjacent FP8
      // values as one 32-bit register group.  Recast that exact bit pattern
      // instead of rebuilding it with twelve scalar shift/or instructions.
      // This is bitwise-only: no arithmetic or precision is changed.
      auto tTR_rDST_u32 = recast<uint32_t>(tTR_rDST);
#endif

      // release dP
      cutlass::arch::fence_view_async_tmem_load();
      pipeline_mma_compute_dp.consumer_release(pipeline_mma_compute_dp_consumer_state);
      ++pipeline_mma_compute_dp_consumer_state;

      // Keep dP(next)->dS(next) arithmetic overlapped with dQ(cur).  Only wait
      // for the single reusable dS stage after the result is quantized and the
      // aliased dP TMEM input has been released.
      pipeline_compute_mma_ds.producer_acquire(pipeline_compute_mma_ds_producer_state);

#if defined(FAG_DS_STASYNC_CLUSTER)
      // Arm the PEER's ds_full for this iteration BEFORE any st.async can
      // complete against it.  producer_acquire above is ordered after the
      // previous iteration's ds_full phase completed (kStagesComputeSmem == 1:
      // acquire <- MMA dQ release <- ds_leader <- relay <- ds_full tx), so
      // this arrive+expect_tx lands in the new phase and provides the single
      // arrival; the 512 x 16-byte st.async stores below provide exactly
      // kTransactionsBytesDSExchange of complete_tx.  The named barrier then
      // orders the arm before every compute thread's st.async (user barrier id
      // 0 => hardware id 8, unused by this kernel).
      if (dp_idx == 0 && wg_idx == 0) {
        shared_tensors.ds_full.arrive_and_expect_tx(
            kTransactionsBytesDSExchange, uint32_t(cta_rank ^ 1));
      }
      cutlass::arch::NamedBarrier(
        kNumComputeWarps * NumThreadsPerWarp, /*user id*/ 0
      ).arrive_and_wait();
#endif

      // FP8-specific vector R2S corner turn.  The TMEM load gives each lane one
      // fixed KV column and four 16-row Q strips.  In SmemLayoutDSHalf, groups
      // of four adjacent Q values at a fixed KV coordinate are physically
      // contiguous and 32-bit aligned.  Explicitly pack them so ptxas cannot
      // fall back to four bank-conflicting byte stores.  Ownership is decided
      // once per 16-row strip instead of once per scalar element.
      static_assert(sizeof(Element) == 1, "2-SM FP8 corner turn expects byte elements");
#if defined(FAG_ABLATE_PACK)
      // FAG_ABLATE_PACK (timing-only, correctness intentionally broken): drop
      // the ENTIRE SMEM corner turn — every pack4 chain plus both the own-half
      // and exchange-half stores. The dQ MMA therefore reads stale SMEM. The
      // FP32->e4m3 quantize above is kept live by consuming its packed u32
      // output with zero-cost register asm, so the converter and its dP inputs
      // are NOT optimized away. The TMEM dS store feeding dK below and every
      // pipeline/mbarrier handshake are preserved. Zero effect on the base path.
      {
        auto live_u32 = recast<uint32_t>(tTR_rDST);
        CUTLASS_PRAGMA_UNROLL
        for (int i = 0; i < size(live_u32); ++i) {
          uint32_t live = live_u32(i);
          asm volatile("" : "+r"(live));
        }
      }
#elif defined(FAG_OPT_OWN_LATE)
      // FAG_OPT_OWN_LATE: issue ONLY the exchange-half stores here so the
      // generic->async proxy fence below has half as many in-flight stores to
      // drain. The own-half stores are deferred to after the exchange launch;
      // the dQ MMA reads them much later, ordered by the iteration-end
      // fence_view_async_shared (unchanged). Identical bytes and handshakes.
      CUTLASS_PRAGMA_UNROLL
      for (int strip = 0; strip < 4; ++strip) {
        if (strip / 2 == cta_rank) {
          continue;
        }
        int q_strip_base = (strip % 2) * 32 + wg_idx * 16;
        int src = strip * 16;
        uint4 lane_q16 = {
            pack_u32(tTR_rDST, src + 0), pack_u32(tTR_rDST, src + 4),
            pack_u32(tTR_rDST, src + 8), pack_u32(tTR_rDST, src + 12)};
        *reinterpret_cast<uint4*>(&sDS_xchg(q_strip_base, dp_idx)) = lane_q16;
      }
#elif defined(FAG_DS_RUN_STORES)
      // FAG_DS_RUN_STORES: FA4-style corner turn.  Two 32-value runs per
      // thread; each run is two adjacent 16-byte blocks of one swizzled row,
      // so the stores are STS.128 at a precomputed loop-invariant base and
      // base^16 -- zero address ALU and zero per-strip ownership branching
      // inside the mainloop (see ds_run_addr above).  Bytes, destinations and
      // the exchange handshake are identical to the base path; only the
      // register->SMEM mapping and its addressing change.
      CUTLASS_PRAGMA_UNROLL
      for (int k = 0; k < 2; ++k) {
        int src = 32 * k;
        uint4 lane_q32a = {
            pack_u32(tTR_rDST, src +  0), pack_u32(tTR_rDST, src +  4),
            pack_u32(tTR_rDST, src +  8), pack_u32(tTR_rDST, src + 12)};
        uint4 lane_q32b = {
            pack_u32(tTR_rDST, src + 16), pack_u32(tTR_rDST, src + 20),
            pack_u32(tTR_rDST, src + 24), pack_u32(tTR_rDST, src + 28)};
#if defined(FAG_DS_STASYNC_CLUSTER)
        // Own run -> plain local STS (visible to the cta_group::2 UMMA via the
        // iteration-end fence_view_async_shared, as in the base path).  Peer
        // run -> direct async-proxy store into the peer CTA's smem_ds at THIS
        // CTA's half offset (= ds_run_addr[cta_rank]'s own-half formula),
        // accounting 2x16 bytes on the peer's ds_full.  No staging buffer, no
        // proxy fence, no bulk copy.
        uint32_t addr = (k == cta_rank) ? ds_run_addr[k] : ds_run_addr[cta_rank];
        if (k == cta_rank) {
          asm volatile("st.shared.v4.b32 [%0], {%1, %2, %3, %4};"
                       :: "r"(addr),
                          "r"(lane_q32a.x), "r"(lane_q32a.y),
                          "r"(lane_q32a.z), "r"(lane_q32a.w)
                       : "memory");
          asm volatile("st.shared.v4.b32 [%0], {%1, %2, %3, %4};"
                       :: "r"(addr ^ 16),
                          "r"(lane_q32b.x), "r"(lane_q32b.y),
                          "r"(lane_q32b.z), "r"(lane_q32b.w)
                       : "memory");
        }
        else {
          stasync_cluster_128(
              addr, lane_q32a, ds_full_addr, uint32_t(cta_rank ^ 1));
          stasync_cluster_128(
              addr ^ 16, lane_q32b, ds_full_addr, uint32_t(cta_rank ^ 1));
        }
#else
        uint32_t addr = ds_run_addr[k];
        bool store_run = true;
#if defined(FAG_ABLATE_CORNER) || defined(FAG_ABLATE_XCHG_STORE)
        // Timing-only: skip the peer-bound run stores.  ABLATE_XCHG_STORE
        // keeps the bulk copy below moving the same byte count (isolates the
        // peer-run STS cost); ABLATE_CORNER additionally replaces the copy
        // with a no-tx arrive (drops the whole exchange).  The own-run stores
        // keep the pack chain live in both cases.
        if (k != cta_rank) {
          store_run = false;
        }
#endif
#if defined(FAG_ABLATE_OWN_STORE)
        // Timing-only: skip the locally-owned run stores (the peer-run stores
        // and every handshake are preserved).
        if (k == cta_rank) {
          store_run = false;
        }
#endif
        if (store_run) {
          asm volatile("st.shared.v4.b32 [%0], {%1, %2, %3, %4};"
                       :: "r"(addr),
                          "r"(lane_q32a.x), "r"(lane_q32a.y),
                          "r"(lane_q32a.z), "r"(lane_q32a.w)
                       : "memory");
          asm volatile("st.shared.v4.b32 [%0], {%1, %2, %3, %4};"
                       :: "r"(addr ^ 16),
                          "r"(lane_q32b.x), "r"(lane_q32b.y),
                          "r"(lane_q32b.z), "r"(lane_q32b.w)
                       : "memory");
        }
#endif
      }
#else
      CUTLASS_PRAGMA_UNROLL
      for (int strip = 0; strip < 4; ++strip) {
        int q_strip_base = (strip % 2) * 32 + wg_idx * 16;
        bool store_own = strip / 2 == cta_rank;
#if defined(FAG_ABLATE_CORNER)
        // Ablation (timing-only, correctness intentionally broken): drop the
        // peer-half dS exchange entirely. Only the locally-owned half is packed
        // and written to SMEM; the cross-CTA DSMEM bulk copy below is replaced
        // by a no-tx arrive. The dQ MMA therefore reads garbage for the peer
        // half, which is expected. Own-half SMEM store + the TMEM dS store that
        // feeds dK are preserved. Zero effect on the base path.
        if (!store_own) {
          continue;
        }
#endif
        int src = strip * 16;
#if defined(FAG_DS_RECAST_PACK)
        uint4 lane_q16 = {
            tTR_rDST_u32(src / 4 + 0), tTR_rDST_u32(src / 4 + 1),
            tTR_rDST_u32(src / 4 + 2), tTR_rDST_u32(src / 4 + 3)};
#else
        auto pack4 = [&](int i) {
          return uint32_t(tTR_rDST(i + 0).storage)       |
                 (uint32_t(tTR_rDST(i + 1).storage) << 8)  |
                 (uint32_t(tTR_rDST(i + 2).storage) << 16) |
                 (uint32_t(tTR_rDST(i + 3).storage) << 24);
        };
        uint4 lane_q16 = {
            pack4(src + 0), pack4(src + 4), pack4(src + 8), pack4(src + 12)};
#endif

        if (store_own) {
#if !defined(FAG_ABLATE_OWN_STORE)
          *reinterpret_cast<uint4*>(&sDS_own(q_strip_base, dp_idx)) = lane_q16;
#endif
#if defined(FAG_PROBE_DBL_STS) && !defined(FAG_ABLATE_OWN_STORE)
          // LSU competition probe: DUPLICATE the own-half store to the SAME
          // address with the SAME data. Idempotent (no correctness change).
          // Occupies one extra LSU slot per execution. If the LSU/TMA
          // competition hypothesis is correct, this should cost the same as
          // the ownstore ablation saves (~6%), proving the cost is from the
          // LSU slot itself, not from the data or address.
          {
            uint32_t dup_addr = cute::cast_smem_ptr_to_uint(
                &sDS_own(q_strip_base, dp_idx));
            asm volatile("st.shared.v4.b32 [%0], {%1,%2,%3,%4};"
                         :: "r"(dup_addr), "r"(lane_q16.x), "r"(lane_q16.y),
                            "r"(lane_q16.z), "r"(lane_q16.w) : "memory");
          }
#endif
        }
        else {
#if defined(FAG_DS_STASYNC_CLUSTER)
          // FAG_DS_STASYNC_CLUSTER (strip form): the peer half goes directly
          // to the peer CTA's smem_ds slot through the async proxy with tx
          // accounting on the peer's ds_full (armed just after
          // producer_acquire).  No staging STS, no proxy fence, no bulk copy.
          stasync_cluster_128(
              cute::cast_smem_ptr_to_uint(&sDS_own(q_strip_base, dp_idx)),
              lane_q16, ds_full_addr, uint32_t(cta_rank ^ 1));
#elif defined(FAG_DS_DIRECT_DSMEM)
          store_shared_cluster_128(
              reinterpret_cast<void*>(&sDS_own(q_strip_base, dp_idx)),
              lane_q16, uint32_t(cta_rank ^ 1));
#elif defined(FAG_OPT_STASYNC_XCHG)
          // FAG_OPT_STASYNC_XCHG: the exchange half goes through the async
          // proxy (st.async + mbarrier tx) so the DSMEM bulk copy below needs
          // no fence.proxy.async.shared::cluster. Own-half stores stay plain
          // STS and keep their existing fence_view_async_shared ordering.
          stasync_128(
              reinterpret_cast<void*>(&sDS_xchg(q_strip_base, dp_idx)),
              lane_q16, &shared_tensors.xchg_tx);
#elif !defined(FAG_ABLATE_XCHG_STORE)
          // FAG_ABLATE_XCHG_STORE (timing-only): keep the pack arithmetic but
          // skip the exchange-half SMEM store. The bulk copy below still moves
          // the same byte count (stale data), isolating the STS cost.
          *reinterpret_cast<uint4*>(&sDS_xchg(q_strip_base, dp_idx)) = lane_q16;
#endif
        }
      }
#endif  // FAG_ABLATE_PACK

#if defined(FAG_PROBE_DBL_LDS)
      // LSU competition probe: 2 extra LDS.128 reads from the dS SMEM buffer.
      // Same LSU pipe as STS but READS don't compete for SMEM WRITE ports
      // (TMA loads write; our STS writes; LDS only reads). If the cost is
      // specifically about WRITE port competition with TMA, LDS should be
      // cheaper than STS per slot.
      {
        uint32_t addr = cute::cast_smem_ptr_to_uint(&sDS_own(0, 0));
        uint32_t d0, d1, d2, d3;
        asm volatile("ld.shared.v4.b32 {%0,%1,%2,%3}, [%4];"
                     : "=r"(d0), "=r"(d1), "=r"(d2), "=r"(d3)
                     : "r"(addr) : "memory");
        asm volatile("ld.shared.v4.b32 {%0,%1,%2,%3}, [%4];"
                     : "=r"(d0), "=r"(d1), "=r"(d2), "=r"(d3)
                     : "r"(addr) : "memory");
        asm volatile("" :: "r"(d0), "r"(d1), "r"(d2), "r"(d3));
      }
#endif

#if defined(FAG_PROBE_EXTRA_ALU_24)
      // Control probe: 24 integer ALU ops (matching the ~24 address
      // instructions that accompany the 2 own STS.128). These use the ALU
      // pipe, NOT the LSU pipe. If the LSU hypothesis is correct, these
      // should cost far less than 2 extra STS (~6%) because ALU slots
      // don't compete with TMA for SMEM ports.
      {
        uint32_t a = uint32_t(dp_idx), b = uint32_t(wg_idx);
        CUTLASS_PRAGMA_UNROLL
        for (int j = 0; j < 24; ++j) {
          asm volatile("lop3.b32 %0, %0, %1, %2, 0x78;"
                       : "+r"(a) : "r"(b), "r"(j));
        }
        asm volatile("" :: "r"(a));
      }
#endif

      // The same quantized dS is also written to TMEM for dK. All compute warps
      // must finish reading the aliased dP region before any warp overwrites it.
      cutlass::arch::fence_view_async_tmem_load();
      cutlass::arch::fence_view_async_shared();
#if !defined(FAG_ABLATE_COMPUTE_BAR)
      // FAG_ABLATE_COMPUTE_BAR (timing-only, ordering broken): isolates the
      // 256-thread join that orders dP TMEM reads before the dS store.
      cutlass::arch::NamedBarrier(
        kNumComputeWarps * NumThreadsPerWarp,
        cutlass::arch::ReservedNamedBarriers::TransformBarrier
      ).arrive_and_wait();
#endif
#if defined(FAG_OPT_EARLY_COPY)
      // FAG_OPT_EARLY_COPY: the barrier above just published every xchg store
      // of this CTA, which is the only precondition of the DSMEM exchange.
      // Issuing it here lets the copy engine run concurrently with the TMEM
      // dS store below (independent sinks) instead of behind its issue chain.
      // Bit-identical handshake and byte count; base path unchanged.
      if (dp_idx == 0 && wg_idx == 0) {
        uint32_t peer_rank = uint32_t(cta_rank ^ 1);
        shared_tensors.ds_full.arrive_and_expect_tx(
            kTransactionsBytesDSExchange, peer_rank);
        asm volatile("fence.proxy.async.shared::cluster;" ::: "memory");
        cpasync_bulk_s2cluster(
            shared_tensors.smem_ds.begin() + cta_rank * kDSHalfElements,
            shared_tensors.smem_ds_xchg.begin(),
            &shared_tensors.ds_full,
            kTransactionsBytesDSExchange,
            peer_rank);
      }
#elif defined(FAG_OPT_OWN_LATE)
      // FAG_OPT_OWN_LATE: only the exchange-half stores were issued above, so
      // the generic->async proxy fence below drains half as many in-flight
      // stores as the base ordering. The deferred own-half stores then overlap
      // the bulk copy engine; the dQ MMA reads them much later, ordered by the
      // iteration-end fence_view_async_shared (unchanged). Identical bytes,
      // addresses and handshakes; base path unchanged.
      if (dp_idx == 0 && wg_idx == 0) {
        uint32_t peer_rank = uint32_t(cta_rank ^ 1);
        shared_tensors.ds_full.arrive_and_expect_tx(
            kTransactionsBytesDSExchange, peer_rank);
#if defined(FAG_OPT_FENCE_CTA)
        asm volatile("fence.proxy.async.shared::cta;" ::: "memory");
#else
        asm volatile("fence.proxy.async.shared::cluster;" ::: "memory");
#endif
        cpasync_bulk_s2cluster(
            shared_tensors.smem_ds.begin() + cta_rank * kDSHalfElements,
            shared_tensors.smem_ds_xchg.begin(),
            &shared_tensors.ds_full,
            kTransactionsBytesDSExchange,
            peer_rank);
      }
      CUTLASS_PRAGMA_UNROLL
      for (int strip = 0; strip < 4; ++strip) {
        if (strip / 2 != cta_rank) {
          continue;
        }
        int q_strip_base = (strip % 2) * 32 + wg_idx * 16;
        int src = strip * 16;
        uint4 lane_q16 = {
            pack_u32(tTR_rDST, src + 0), pack_u32(tTR_rDST, src + 4),
            pack_u32(tTR_rDST, src + 8), pack_u32(tTR_rDST, src + 12)};
        *reinterpret_cast<uint4*>(&sDS_own(q_strip_base, dp_idx)) = lane_q16;
      }
#endif
      {
        auto tRT_rDST = make_tensor(tTR_rDST.data(), shape(tRT_cDST));
        // FAG_ABLATE_DS_TMEM_STORE (timing-only): skip the dS register->TMEM
        // store feeding dK. The quantize output stays live through the SMEM
        // corner-turn pack above, so the converter is not optimized away.
#ifndef FAG_ABLATE_DS_TMEM_STORE
        cute::copy(tiled_r2t_ds, tRT_rDST, tRT_tDST);
#endif
        cutlass::arch::fence_view_async_tmem_store();
      }

#if defined(FAG_SPLIT_DK_DQ_READY)
      // Each warp executes one uniform TMEM-store sequence.  Publish one
      // arrival per compute warp after its async-view fence, instead of adding
      // a second 512-thread UMMA pipeline barrier.  dQ lifetime is still owned
      // by the original delayed dS pipeline.
      __syncwarp();
      if ((threadIdx.x & (NumThreadsPerWarp - 1)) == 0) {
        shared_tensors.dk_ready.arrive(uint32_t(0), uint32_t(1));
      }
#endif

      // Send the Q half owned by the peer. The destination K-half is selected by
      // the sender rank, yielding peer-local [K0|K1] reduction order.
      // (FAG_OPT_EARLY_COPY / FAG_OPT_OWN_LATE moved this exchange block to
      //  just after the compute named barrier above; nothing remains here in
      //  those configurations.  FAG_DS_STASYNC_CLUSTER replaces the exchange
      //  with per-store remote st.async, so the arm/copy block is gone too.)
#if !defined(FAG_OPT_EARLY_COPY) && !defined(FAG_OPT_OWN_LATE) && !defined(FAG_DS_STASYNC_CLUSTER)
      if (dp_idx == 0 && wg_idx == 0) {
        uint32_t peer_rank = uint32_t(cta_rank ^ 1);
#if defined(FAG_ABLATE_CORNER) || defined(FAG_ABLATE_PACK)
        // Ablation: peer dS bytes are never exchanged, so do NOT arm a
        // transaction-byte expectation (that would deadlock the relay wait).
        // A plain arrive on the peer's ds_full lets its wait complete. No fence
        // is needed here: there are no peer stores left to order against.
        shared_tensors.ds_full.arrive(peer_rank);
#elif defined(FAG_ABLATE_BULK)
        // Ablation (timing-only): both SMEM stores above still run; only the
        // DSMEM bulk copy and its transaction-byte handshake are dropped
        // (plain arrive so the peer's relay wait completes). Isolates the
        // copy+tx cost from the STS cost. dQ reads a stale peer half.
        shared_tensors.ds_full.arrive(peer_rank);
#elif defined(FAG_DS_DIRECT_DSMEM)
        // The compute named barrier above has joined all dS stores in this CTA.
        // Publish them at cluster scope before signaling the peer's completion
        // barrier.  The relay/MMA wait chain remains unchanged.
        asm volatile("fence.acq.rel.cluster;" ::: "memory");
        shared_tensors.ds_full.arrive(peer_rank);
#elif defined(FAG_OPT_STASYNC_XCHG)
        // The exchange half was written by st.async (async proxy) with its
        // bytes accounted on xchg_tx. Wait for that phase completion instead
        // of fence.proxy.async.shared::cluster: the bulk copy then reads
        // async-proxy data with no generic->async proxy transition and no
        // forced drain of the plain own-half STS still in flight. Re-arm the
        // next phase BEFORE issuing the copy so the expectation always leads
        // the next iteration's st.async completions (see the comment at the
        // xchg_phase declaration for the ordering proof).
        shared_tensors.xchg_tx.arrive();
        shared_tensors.xchg_tx.wait(xchg_phase);
        xchg_phase ^= 1;
        shared_tensors.xchg_tx.expect_transaction(kTransactionsBytesDSExchange);
        shared_tensors.ds_full.arrive_and_expect_tx(
            kTransactionsBytesDSExchange, peer_rank);
        cpasync_bulk_s2cluster(
            shared_tensors.smem_ds.begin() + cta_rank * kDSHalfElements,
            shared_tensors.smem_ds_xchg.begin(),
            &shared_tensors.ds_full,
            kTransactionsBytesDSExchange,
            peer_rank);
#else
        shared_tensors.ds_full.arrive_and_expect_tx(
            kTransactionsBytesDSExchange, peer_rank);
#ifndef FAG_ABLATE_FENCE
        // FAG_ABLATE_FENCE (timing-only, ordering intentionally broken):
        // isolates the generic->async proxy fence cost from the copy bytes.
        // FAG_OPT_FENCE_CTA: downgrade the scope from .shared::cluster to
        // .shared::cta. The accesses being ordered (generic STS and the bulk
        // engine's SOURCE read) are both in THIS CTA's shared memory; the
        // peer-side consumption is separately ordered by ds_full. FA4's own
        // 2-CTA exchange uses exactly the cta-scope fence before the same
        // cp.async.bulk pattern. If the cluster-scope visibility flush is the
        // measured -11.7%, this recovers it; if the cost is scope-independent
        // proxy quiesce, it is neutral (adversarial test of the wall).
#if defined(FAG_OPT_FENCE_CTA)
        asm volatile("fence.proxy.async.shared::cta;" ::: "memory");
#else
        asm volatile("fence.proxy.async.shared::cluster;" ::: "memory");
#endif
#endif
#if defined(FAG_ADVERSARIAL_DOUBLE_FENCE)
        // Adversarial control for the fence attribution: a SECOND identical
        // fence. If the measured -11.7% is the fence's real pipeline-quiesce
        // cost, adding one more must ADD a comparable amount; if it was
        // scheduling perturbation from removing code, doubling is neutral.
        asm volatile("fence.proxy.async.shared::cluster;" ::: "memory");
#endif
        cpasync_bulk_s2cluster(
            shared_tensors.smem_ds.begin() + cta_rank * kDSHalfElements,
            shared_tensors.smem_ds_xchg.begin(),
            &shared_tensors.ds_full,
            kTransactionsBytesDSExchange,
            peer_rank);
#endif
      }
#endif  // !FAG_OPT_EARLY_COPY && !FAG_OPT_OWN_LATE

      // Do not notify MMA yet: dQ aliases S/P at kS + D/2.  The next loop
      // iteration publishes this dS after loading the following S tile.  The
      // final iteration is committed explicitly after the loop.
      cutlass::arch::fence_view_async_shared();
      have_pending_ds = true;
      // release OdO
      pipeline_load_compute_sum_odo.consumer_release(pipeline_load_compute_sum_odo_consumer_state);
      ++pipeline_load_compute_sum_odo_consumer_state;

      iter_count -= 1;
      iter_index += 1;
      if (iter_index == iter_end) {
        iter_index = iter_start;
      }
    }

    // Tail: there is no following S tile.  The last S was loaded at the start
    // of the final iteration, so the overlapping dQ region is now safe.
    if (have_pending_ds) {
      pipeline_compute_mma_ds.producer_commit(pipeline_compute_mma_ds_producer_state);
      ++pipeline_compute_mma_ds_producer_state;
    }

#ifdef BWD_2SM_DEBUG
    if (cute::elect_one_sync()) printf("[DBG compute-fn] blk=%d rank=%d LOOP-EXIT, pre-epilogue\n", blockIdx.x, (int)cute::block_rank_in_cluster());
#endif
    epilogue(
        blk_coord, blk_offset, problem_shape, mainloop_args, epilogue_args,
        pipeline_mma_compute_dkdv, pipeline_mma_compute_dkdv_consumer_state
    );
#ifdef BWD_2SM_DEBUG
    if (cute::elect_one_sync()) printf("[DBG compute-fn] blk=%d rank=%d post-epilogue\n", blockIdx.x, (int)cute::block_rank_in_cluster());
#endif
  }

  template<class BlkCoord, class ProblemShape_>
  CUTLASS_DEVICE void reduce(
      BlkCoord const& blk_coord,
      ProblemShape_ const& problem_shape,
      int iter_start,
      int iter_end,
      int iter_count,
      MainloopArguments const& mainloop_args,
      MainloopParams const& mainloop_params,
      TensorStorage& shared_tensors,
      PipelineMmaReduceDQ& pipeline_mma_reduce_dq,
      typename PipelineMmaReduceDQ::PipelineState& pipeline_mma_reduce_dq_consumer_state,
      PipelineReduceTmaStore& pipeline_reduce_tma_store,
      typename PipelineReduceTmaStore::PipelineState& pipeline_reduce_tma_store_producer_state) {

    using X = Underscore;

    auto [Q, K, D, D_VO, HB] = problem_shape;
    int iter_index = iter_start;

    auto [blk_coord_q, blk_coord_k, blk_coord_d, blk_coord_dv, blk_coord_batch] = blk_coord;

    // must match TileShapeDQ
    auto load_op = SM100_TMEM_LOAD_32dp32b32x{};

    auto tDQtDQ = partition_fragment_C(TiledMmaDSK{}, select<0,1>(TileShapeDSK{}))(make_coord(_,_),_0{},_0{});
    tDQtDQ.data() = TmemAllocation::kDQ;

    Tensor mDQ = mainloop_params.tma_red_dq.get_tma_tensor(make_shape(Q, D, HB));
    using TileShapeDQStore = Shape<_1, _64, TileShapeDQK>;
    auto gDQ = local_tile(mDQ, TileShapeDQStore{}, make_coord(_,_,_), Step<X, _1, _1>{})
        (_, _, _, _0{}, _);

    Tensor cDQ = make_identity_tensor(take<0,2>(TileShapeDSK{}));

    Tensor sDQ = make_tensor(make_smem_ptr(shared_tensors.smem_dq.begin()), SmemLayoutDQ{});

    // Keep all 128 reduce threads for the TMEM load: slices 64..127 carry the
    // upper 32 rows of this CTA's 64-row cooperative result.  The old generic
    // partition_D(sDQ) mapping aliases those slices onto the lower 32 rows, so
    // below we scatter through the identity coordinates instead.
    int reduce_thread_idx = threadIdx.x % (kNumReduceWarps * NumThreadsPerWarp);
    int thread_idx = reduce_thread_idx;
    auto tiled_t2r = make_tmem_copy(load_op, tDQtDQ);
    auto thread_t2r = tiled_t2r.get_slice(thread_idx);

    Tensor tTR_cDQ   = thread_t2r.partition_D(cDQ);
    Tensor tTR_tDQ = thread_t2r.partition_S(tDQtDQ);

    auto block_tma = mainloop_params.tma_red_dq.get_slice(_0{});

    Tensor tDQsDQ = block_tma.partition_S(sDQ);
    Tensor tDQcDQ = block_tma.partition_S(cDQ);
    Tensor tDQgDQ = block_tma.partition_D(gDQ);

    int lane_predicate = (threadIdx.x % (kNumReduceWarps * NumThreadsPerWarp)) == 0;
    int cta_rank = int(cute::block_rank_in_cluster());

    while (iter_count > 0) {
#ifdef BWD_2SM_DEBUG
      if (cute::elect_one_sync()) printf("[DBG reduce-fn] blk=%d rank=%d pre-dq-wait\n", blockIdx.x, (int)cute::block_rank_in_cluster());
#endif
      pipeline_mma_reduce_dq.consumer_wait(pipeline_mma_reduce_dq_consumer_state);
#ifdef BWD_2SM_DEBUG
      if (cute::elect_one_sync()) printf("[DBG reduce-fn] blk=%d rank=%d post-dq-wait\n", blockIdx.x, (int)cute::block_rank_in_cluster());
#endif

      Tensor tTR_rDQ = make_tensor<ElementAcc>(shape(tTR_cDQ));

      // load dQ from tmem to rmem
      // FAG_ABLATE_REDUCE (timing-only): remove the reduce warpgroup's data
      // path (TMEM load + SMEM staging + TMA reduce-add store) while keeping
      // every pipeline handshake below, so the MMA producer side is unchanged.
#ifndef FAG_ABLATE_REDUCE
      cute::copy(tiled_t2r, tTR_tDQ, tTR_rDQ);
#endif

#ifdef BWD_2SM_DEBUG
      if (blockIdx.x == 0 &&
          (reduce_thread_idx == 0 || reduce_thread_idx == 32 ||
           reduce_thread_idx == 64 || reduce_thread_idx == 96)) {
        CUTLASS_PRAGMA_UNROLL
        for (int mi = 0; mi < size<2>(tTR_cDQ); ++mi) {
          auto cmi = tTR_cDQ(_, _, mi);
          auto rmi = tTR_rDQ(_, _, mi);
          auto c0 = cmi(0);
          auto cN = cmi(size(cmi) - 1);
          printf("[DBG DQMAP] tid=%d mode=%d elems=%d first=(%d,%d):%.7g last=(%d,%d):%.7g\n",
                 reduce_thread_idx, mi, int(size(cmi)),
                 int(get<0>(c0)), int(get<1>(c0)), double(rmi(0)),
                 int(get<0>(cN)), int(get<1>(cN)), double(rmi(size(rmi) - 1)));
        }
      }
#endif

      cutlass::arch::fence_view_async_tmem_load();
      pipeline_mma_reduce_dq.consumer_release(pipeline_mma_reduce_dq_consumer_state);
      ++pipeline_mma_reduce_dq_consumer_state;

      // we don't have enough smem to dump it all to smem, so we do it in stages
      // Fragment mode 2 has two tcgen05 repetitions, not the number of
      // 32-column global-store tiles.  D=128 requires four distinct TMA
      // stages (0:32, 32:64, 64:96, 96:128).
      static_assert(int(TileShapeDQK{}) % int(TileShapeDQ{}) == 0);
      constexpr int kNumDQStoreTiles = int(TileShapeDQK{}) / int(TileShapeDQ{});
#if defined(FAG_DQ_BATCHED_TMA)
      static_assert(int(TileShapeDQK{}) == 128 && int(TileShapeDQ{}) == 32);
      static_assert(kNumDQStoreTiles == 4);
      static_assert(kStagesReduceTmaStore == kNumDQStoreTiles);

      // Preserve both index and phase for all four outstanding buffers.  Four
      // advances return the index to zero and toggle the phase for the next
      // dQ tile exactly as the serial path does.
      auto store_state_0 = pipeline_reduce_tma_store_producer_state;
      ++pipeline_reduce_tma_store_producer_state;
      auto store_state_1 = pipeline_reduce_tma_store_producer_state;
      ++pipeline_reduce_tma_store_producer_state;
      auto store_state_2 = pipeline_reduce_tma_store_producer_state;
      ++pipeline_reduce_tma_store_producer_state;
      auto store_state_3 = pipeline_reduce_tma_store_producer_state;
      ++pipeline_reduce_tma_store_producer_state;

      if (lane_predicate) {
        pipeline_reduce_tma_store.producer_acquire(store_state_0);
        pipeline_reduce_tma_store.producer_acquire(store_state_1);
        pipeline_reduce_tma_store.producer_acquire(store_state_2);
        pipeline_reduce_tma_store.producer_acquire(store_state_3);
      }
      cutlass::arch::NamedBarrier(
          kNumReduceWarps * NumThreadsPerWarp,
          cutlass::arch::ReservedNamedBarriers::TransposeBarrier
      ).arrive_and_wait();

      int dq_row = reduce_thread_idx % 64;
      int dq_half = reduce_thread_idx / 64;
      auto stage_to_smem = [&](auto const& store_state, auto stage_c) {
        constexpr int i = decltype(stage_c)::value;
        if (dq_half == i / 2) {
          copy_aligned(
              tTR_rDQ(_, _, i % 2),
              sDQ(dq_row, _, store_state.index()));
        }
      };
      stage_to_smem(store_state_0, Int<0>{});
      stage_to_smem(store_state_1, Int<1>{});
      stage_to_smem(store_state_2, Int<2>{});
      stage_to_smem(store_state_3, Int<3>{});

      cutlass::arch::fence_view_async_shared();
      cutlass::arch::NamedBarrier(
          kNumReduceWarps * NumThreadsPerWarp,
          cutlass::arch::ReservedNamedBarriers::TransposeBarrier
      ).arrive_and_wait();

      if (lane_predicate) {
        auto launch_store = [&](auto& store_state, auto stage_c) {
          constexpr int i = decltype(stage_c)::value;
          copy(mainloop_params.tma_red_dq,
               tDQsDQ(_, _, _0{}, store_state.index()),
               tDQgDQ(_, _, i, iter_index * kClusterSize + cta_rank,
                       blk_coord_batch));
          pipeline_reduce_tma_store.producer_commit(store_state);
        };
        launch_store(store_state_0, Int<0>{});
        launch_store(store_state_1, Int<1>{});
        launch_store(store_state_2, Int<2>{});
        launch_store(store_state_3, Int<3>{});
      }
#elif defined(FAG_DQ_HALF_SPLIT)
      // FAG_DQ_HALF_SPLIT: the four 32-column store tiles are data-DISJOINT
      // between thread halves (tid/64 == tile/2), yet the serial loop joins
      // all 128 reduce threads around every round's barriers and re-diverges
      // on the dq_half store guard -- NCU shows 22.4% of all stall samples
      // parked by the reduce warps, dominated by barrier waits + BSSY
      // reconvergence (10983 + 5773 samples) rather than the STS themselves.
      // Here each half runs its two rounds as an independent 64-thread
      // chain: per-half named barriers (hardware ids 5/6, unused), its own
      // lane-0 TMA issuer, and NO intra-round divergence (the store guard is
      // uniform within a half).  The two halves' staging chains execute in
      // parallel, halving the drain's barrier critical path.  Requires the
      // four-stage rotation so the halves' buffers never collide.
      static_assert(kStagesReduceTmaStore == 4, "FAG_DQ_HALF_SPLIT needs 4 stages");
      {
        int dq_row = reduce_thread_idx % 64;
        int dq_half = reduce_thread_idx / 64;
        bool half_leader = (reduce_thread_idx % 64) == 0;
        cutlass::arch::ReservedNamedBarriers half_bar_id =
            (dq_half == 0) ? cutlass::arch::ReservedNamedBarriers::StreamkBarrier1
                           : cutlass::arch::ReservedNamedBarriers::TmemAllocBarrier;
        cutlass::arch::NamedBarrier half_barrier(
            2 * 32 /* two warps per half */, half_bar_id);
        // PipelineTmaStore is a per-LEADER TMA bulk-group counter (acquire ==
        // cp.async.bulk.wait_group, commit == commit_group), NOT a per-stage
        // mbarrier pipeline -- two issuing leaders must therefore track their
        // OWN group counts.  With two SMEM buffers per half, the safety rule
        // is: before restaging a buffer, wait until at most one of this
        // leader's own TMA groups is still in flight (classic 2-deep store
        // rotation), which restores the serial path's double-buffering.
        int stage_idx = (int(pipeline_reduce_tma_store_producer_state.index())
                         + 2 * dq_half) % kStagesReduceTmaStore;
        uint32_t half_group_count = uint32_t(iter_index) * 2;  // this half's committed groups so far
        CUTLASS_PRAGMA_UNROLL
        for (int j = 0; j < 2; ++j) {
          int tile = 2 * dq_half + j;
#if !defined(FAG_ABLATE_REDUCE_TMA)
          if (half_leader && half_group_count + uint32_t(j) > 1) {
            cute::tma_store_wait<1>();
          }
#endif
          half_barrier.arrive_and_wait();
#ifndef FAG_ABLATE_REDUCE
          copy_aligned(
              tTR_rDQ(_, _, tile % 2),
              sDQ(dq_row, _, stage_idx));
#endif
          cutlass::arch::fence_view_async_shared();
          half_barrier.arrive_and_wait();
          if (half_leader) {
#if !defined(FAG_ABLATE_REDUCE) && !defined(FAG_ABLATE_REDUCE_TMA)
            copy(mainloop_params.tma_red_dq,
                 tDQsDQ(_,_,_0{}, stage_idx),
                 tDQgDQ(_,_,tile,iter_index * kClusterSize + cta_rank,blk_coord_batch));
#endif
            cute::tma_store_arrive();
          }
          stage_idx = (stage_idx + 1) % kStagesReduceTmaStore;
        }
        // Both halves consumed two stages each; advance the shared rotation
        // by the same four steps in every thread to stay in lockstep.
        ++pipeline_reduce_tma_store_producer_state;
        ++pipeline_reduce_tma_store_producer_state;
        ++pipeline_reduce_tma_store_producer_state;
        ++pipeline_reduce_tma_store_producer_state;
      }
#else
      CUTLASS_PRAGMA_UNROLL
      for (int i = 0; i < kNumDQStoreTiles; i++) {
        if (lane_predicate) {
          pipeline_reduce_tma_store.producer_acquire(pipeline_reduce_tma_store_producer_state);
        }
        // wait in all threads for the acquire to complete
        cutlass::arch::NamedBarrier(
            kNumReduceWarps * NumThreadsPerWarp,
            cutlass::arch::ReservedNamedBarriers::TransposeBarrier
        ).arrive_and_wait();

        // The 2-SM TMEM load maps the 128 reduce threads as follows:
        //   tid % 64 -> output row
        //   tid / 64 -> low/high 64-column half
        // and its two fragment modes are the two 32-column quarters inside
        // that half.  Select the one matching this TMA stage and copy the full
        // 32-float row at once.  Besides avoiding a 256-element coordinate scan
        // per stage, copy_aligned lets CUTE emit wide shared-memory stores.
        int dq_row = reduce_thread_idx % 64;
        int dq_half = reduce_thread_idx / 64;
        if (dq_half == i / 2) {
#if !defined(FAG_ABLATE_REDUCE) && !defined(FAG_ABLATE_REDUCE_STS)
          // FAG_ABLATE_REDUCE_STS (timing-only): keep the TMEM load, the
          // tmem-load fence and the consumer_release (the release chain that
          // serializes consecutive dQ MMAs), skip only the SMEM staging and
          // the TMA store.  (REDUCE - REDUCE_STS) isolates the staging cost;
          // REDUCE_STS alone isolates the release-chain / LDTM-latency cost.
#if defined(FAG_DQ_BF16_STAGING)
          // FAG_DQ_BF16_STAGING: convert the 32-f32 run to packed BF16 pairs
          // (F2FP.BF16.PACK) and stage 64B via 2xSTS.128 instead of 128B via
          // 4xSTS.128 -- half the LSU wavefronts of the #1 bottleneck.
          Tensor run_f32 = tTR_rDQ(_, _, i % 2);
          Tensor pack16 = make_tensor<uint32_t>(Shape<Int<size(run_f32) / 2>>{});
          CUTLASS_PRAGMA_UNROLL
          for (int e = 0; e < size(run_f32); e += 2) {
            __nv_bfloat162 pair = __floats2bfloat162_rn(run_f32(e), run_f32(e + 1));
            pack16(e / 2) = *reinterpret_cast<uint32_t*>(&pair);
          }
          copy_aligned(
              pack16,
              recast<uint32_t>(sDQ(dq_row, _, pipeline_reduce_tma_store_producer_state.index())));
#else
          copy_aligned(
              tTR_rDQ(_, _, i % 2),
              sDQ(dq_row, _, pipeline_reduce_tma_store_producer_state.index()));
#endif
#endif
        }

        // wait for the stores to all be visible to the TMA
        // FAG_ABLATE_REDUCE_FENCE (timing-only, ordering broken): isolates
        // the per-round generic->async proxy fence of the drain staging.
#ifndef FAG_ABLATE_REDUCE_FENCE
        cutlass::arch::fence_view_async_shared();
#endif
        cutlass::arch::NamedBarrier(
            kNumReduceWarps * NumThreadsPerWarp,
            cutlass::arch::ReservedNamedBarriers::TransposeBarrier
        ).arrive_and_wait();
        if (lane_predicate) {
          // launch tma store
          // FAG_ABLATE_REDUCE_TMA (timing-only): keep the TMEM->RMEM load and
          // the SMEM staging, but skip the TMA reduce-add issue.  (REDUCE -
          // REDUCE_TMA) then isolates the atomic reduce-add traffic from the
          // drain (TMEM load + SMEM staging) cost.
#if !defined(FAG_ABLATE_REDUCE) && !defined(FAG_ABLATE_REDUCE_TMA) && !defined(FAG_ABLATE_REDUCE_STS)
          copy(mainloop_params.tma_red_dq,
               tDQsDQ(_,_,_0{}, pipeline_reduce_tma_store_producer_state.index()),
               tDQgDQ(_,_,i,iter_index * kClusterSize + cta_rank,blk_coord_batch));
#endif
          pipeline_reduce_tma_store.producer_commit(pipeline_reduce_tma_store_producer_state);
        }

        ++pipeline_reduce_tma_store_producer_state;
      }
#endif

      iter_count -= 1;
      iter_index += 1;
      if (iter_index == iter_end) {
        iter_index = iter_start;
        get<0,0>(blk_coord_batch) += 1;
      }
    }
  }


  CUTLASS_DEVICE void operator()(Params const& params, char* smem) {
#if (! defined(CUTLASS_ARCH_MMA_SM100A_ENABLED) && ! defined(CUTLASS_ARCH_MMA_SM100F_ENABLED) && \
    ! defined(CUTLASS_ARCH_MMA_SM103A_ENABLED) && ! defined(CUTLASS_ARCH_MMA_SM103F_ENABLED))
    CUTE_INVALID_CONTROL_PATH("ERROR : Arch conditional MMA instruction used without targeting appropriate compute capability. Aborting.\n");
#else
    int warp_idx = cutlass::canonical_warp_idx_sync();
    auto role = warp_idx_to_role(warp_idx);
    uint32_t lane_predicate = cute::elect_one_sync();

    if (role == WarpRole::Load && lane_predicate) {
      prefetch_tma_descriptor(params.mainloop_params.tma_load_q.get_tma_descriptor());
      prefetch_tma_descriptor(params.mainloop_params.tma_load_qt.get_tma_descriptor());
      prefetch_tma_descriptor(params.mainloop_params.tma_load_kt.get_tma_descriptor());
      prefetch_tma_descriptor(params.mainloop_params.tma_load_k.get_tma_descriptor());
      prefetch_tma_descriptor(params.mainloop_params.tma_load_v.get_tma_descriptor());
      prefetch_tma_descriptor(params.mainloop_params.tma_load_do.get_tma_descriptor());
      prefetch_tma_descriptor(params.mainloop_params.tma_load_do_dv.get_tma_descriptor());
    }

    SharedStorage& shared_storage = *reinterpret_cast<SharedStorage*>(smem);

    int initializing_warp = 0;
    typename PipelineLoadMmaQ::Params pipeline_load_mma_q_params;
    if (role == WarpRole::Load) {
      pipeline_load_mma_q_params.role = PipelineLoadMmaQ::ThreadCategory::Producer;
    }
    if (role == WarpRole::Mma) {
      pipeline_load_mma_q_params.role = PipelineLoadMmaQ::ThreadCategory::Consumer;
    }
    pipeline_load_mma_q_params.is_leader = lane_predicate && (role == WarpRole::Load)
        && (kClusterSize == 1 || cute::block_rank_in_cluster() == 0);
    // [2SM] The TMA pipeline is cluster-aware (built with ClusterShape<2,1,1> +
    // AtomThrShape). A 2CTA TMA's complete-tx ALWAYS lands on the leader's barrier
    // with BOTH CTAs' bytes, so ONLY the leader arms (is_leader) with kClusterSize x
    // bytes. (Synccheck: arming both CTAs with 1x -> "Barrier missing wait".) K is
    // also loaded on this pipeline in the first iteration.
    pipeline_load_mma_q_params.transaction_bytes = kTransactionsBytesLoadQ * kClusterSize;
    pipeline_load_mma_q_params.initializing_warp = initializing_warp++;
    PipelineLoadMmaQ pipeline_load_mma_q(shared_storage.pipelines.load_mma_q, pipeline_load_mma_q_params,
      ClusterShape{}, /*barrier init*/ cute::true_type{}, /*mask calc*/cute::false_type{});

    typename PipelineLoadMmaQT::Params pipeline_load_mma_qt_params;
    if (role == WarpRole::Load) {
      pipeline_load_mma_qt_params.role = PipelineLoadMmaQT::ThreadCategory::Producer;
    }
    if (role == WarpRole::Mma) {
      pipeline_load_mma_qt_params.role = PipelineLoadMmaQT::ThreadCategory::Consumer;
    }
    pipeline_load_mma_qt_params.is_leader = lane_predicate && (role == WarpRole::Load)
        && (kClusterSize == 1 || cute::block_rank_in_cluster() == 0);
    pipeline_load_mma_qt_params.transaction_bytes = kTransactionsBytesLoadQT * kClusterSize;
    pipeline_load_mma_qt_params.initializing_warp = initializing_warp++;
    PipelineLoadMmaQT pipeline_load_mma_qt(
      shared_storage.pipelines.load_mma_qt, pipeline_load_mma_qt_params,
      ClusterShape{}, /*barrier init*/ cute::true_type{}, /*mask calc*/cute::false_type{});

    typename PipelineLoadMmaKT::Params pipeline_load_mma_kt_params;
    if (role == WarpRole::Load) {
      pipeline_load_mma_kt_params.role = PipelineLoadMmaKT::ThreadCategory::Producer;
    }
    if (role == WarpRole::Mma) {
      pipeline_load_mma_kt_params.role = PipelineLoadMmaKT::ThreadCategory::Consumer;
    }
    pipeline_load_mma_kt_params.is_leader = lane_predicate && (role == WarpRole::Load)
        && (kClusterSize == 1 || cute::block_rank_in_cluster() == 0);
    pipeline_load_mma_kt_params.transaction_bytes = kTransactionsBytesLoadKT * kClusterSize;
    pipeline_load_mma_kt_params.initializing_warp = initializing_warp++;
    PipelineLoadMmaKT pipeline_load_mma_kt(
      shared_storage.pipelines.load_mma_kt, pipeline_load_mma_kt_params,
      ClusterShape{}, /*barrier init*/ cute::true_type{}, /*mask calc*/cute::false_type{});

    typename PipelineLoadMmaDO::Params pipeline_load_mma_do_params;
    if (role == WarpRole::Load) {
      pipeline_load_mma_do_params.role = PipelineLoadMmaDO::ThreadCategory::Producer;
    }
    if (role == WarpRole::Mma) {
      pipeline_load_mma_do_params.role = PipelineLoadMmaDO::ThreadCategory::Consumer;
    }
    pipeline_load_mma_do_params.is_leader = lane_predicate && (role == WarpRole::Load)
        && (kClusterSize == 1 || cute::block_rank_in_cluster() == 0);
    // [2SM] leader-only arming with kClusterSize x bytes (see load_mma_q). V is
    // also loaded on this pipeline in the first iteration.
    pipeline_load_mma_do_params.transaction_bytes = kTransactionsBytesLoadDO * kClusterSize;
    pipeline_load_mma_do_params.initializing_warp = initializing_warp++;
    PipelineLoadMmaDO pipeline_load_mma_do(shared_storage.pipelines.load_mma_do, pipeline_load_mma_do_params,
      ClusterShape{}, /*barrier init*/ cute::true_type{}, /*mask calc*/cute::false_type{});

    typename PipelineLoadComputeLSE::Params pipeline_load_compute_lse_params;
    if (role == WarpRole::Load) {
      pipeline_load_compute_lse_params.role = PipelineLoadComputeLSE::ThreadCategory::Producer;
    }
    if (role == WarpRole::Compute) {
      pipeline_load_compute_lse_params.role = PipelineLoadComputeLSE::ThreadCategory::Consumer;
    }
    pipeline_load_compute_lse_params.producer_arv_count = NumThreadsPerWarp;
    pipeline_load_compute_lse_params.consumer_arv_count = kNumComputeWarps * NumThreadsPerWarp;
    pipeline_load_compute_lse_params.initializing_warp = initializing_warp++;
    PipelineLoadComputeLSE pipeline_load_compute_lse(
      shared_storage.pipelines.load_compute_lse,
      pipeline_load_compute_lse_params,
      /*barrier init*/ cute::true_type{});

    typename PipelineLoadComputeSumOdO::Params pipeline_load_compute_sum_odo_params;
    if (role == WarpRole::Load) {
      pipeline_load_compute_sum_odo_params.role = PipelineLoadComputeSumOdO::ThreadCategory::Producer;
    }
    if (role == WarpRole::Compute) {
      pipeline_load_compute_sum_odo_params.role = PipelineLoadComputeSumOdO::ThreadCategory::Consumer;
    }
    pipeline_load_compute_sum_odo_params.producer_arv_count = NumThreadsPerWarp;
    pipeline_load_compute_sum_odo_params.consumer_arv_count = kNumComputeWarps * NumThreadsPerWarp;
    pipeline_load_compute_sum_odo_params.initializing_warp = initializing_warp++;
    PipelineLoadComputeSumOdO pipeline_load_compute_sum_odo(
      shared_storage.pipelines.load_compute_sum_odo,
      pipeline_load_compute_sum_odo_params,
      /*barrier init*/ cute::true_type{});

    typename PipelineMmaComputeS::Params pipeline_mma_compute_s_params;
    if (role == WarpRole::Mma) {
      pipeline_mma_compute_s_params.role = PipelineMmaComputeS::ThreadCategory::Producer;
    }
    if (role == WarpRole::Compute) {
      pipeline_mma_compute_s_params.role = PipelineMmaComputeS::ThreadCategory::Consumer;
    }
    pipeline_mma_compute_s_params.consumer_arv_count = cute::size(AtomThrShape_MNK{}) * kNumComputeWarps * cutlass::NumThreadsPerWarp;
    pipeline_mma_compute_s_params.initializing_warp = initializing_warp++;
    PipelineMmaComputeS pipeline_mma_compute_s(
      shared_storage.pipelines.mma_compute_s,
      pipeline_mma_compute_s_params,
      ClusterShape{}, /*barrier init*/ cute::true_type{}, /*mask calc*/cute::false_type{});

    typename PipelineMmaComputeDP::Params pipeline_mma_compute_dp_params;
    if (role == WarpRole::Mma) {
      pipeline_mma_compute_dp_params.role = PipelineMmaComputeDP::ThreadCategory::Producer;
    }
    if (role == WarpRole::Compute) {
      pipeline_mma_compute_dp_params.role = PipelineMmaComputeDP::ThreadCategory::Consumer;
    }
    pipeline_mma_compute_dp_params.consumer_arv_count = cute::size(AtomThrShape_MNK{}) * kNumComputeWarps * cutlass::NumThreadsPerWarp;
    pipeline_mma_compute_dp_params.initializing_warp = initializing_warp++;
    PipelineMmaComputeDP pipeline_mma_compute_dp(
      shared_storage.pipelines.mma_compute_dp,
      pipeline_mma_compute_dp_params,
      ClusterShape{}, /*barrier init*/ cute::true_type{}, /*mask calc*/cute::false_type{});

    typename PipelineMmaReduceDQ::Params pipeline_mma_reduce_dq_params;
    if (role == WarpRole::Mma) {
      pipeline_mma_reduce_dq_params.role = PipelineMmaReduceDQ::ThreadCategory::Producer;
    }
    if (role == WarpRole::Reduce) {
      pipeline_mma_reduce_dq_params.role = PipelineMmaReduceDQ::ThreadCategory::Consumer;
    }
    pipeline_mma_reduce_dq_params.consumer_arv_count = cute::size(AtomThrShape_MNK{}) * kNumReduceWarps * cutlass::NumThreadsPerWarp;
    pipeline_mma_reduce_dq_params.initializing_warp = initializing_warp++;
    PipelineMmaReduceDQ pipeline_mma_reduce_dq(
      shared_storage.pipelines.mma_reduce_dq,
      pipeline_mma_reduce_dq_params,
      ClusterShape{}, /*barrier init*/ cute::true_type{}, /*mask calc*/cute::false_type{});

    typename PipelineComputeMmaP::Params pipeline_compute_mma_p_params;
    if (role == WarpRole::Mma) {
      pipeline_compute_mma_p_params.role = PipelineComputeMmaP::ThreadCategory::Consumer;
    }
    if (role == WarpRole::Compute) {
      pipeline_compute_mma_p_params.role = PipelineComputeMmaP::ThreadCategory::Producer;
    }
    // [2SM] compute_mma_p producer = Compute. With AtomThr, producer_commit does
    // umma_arrive_2x1SM_sm0 -> BOTH CTAs' Compute commits land on the leader's
    // full barrier. So producer_arv_count needs the AtomThr factor.
    pipeline_compute_mma_p_params.producer_arv_count = cute::size(AtomThrShape_MNK{}) * kNumComputeWarps * cutlass::NumThreadsPerWarp;
    pipeline_compute_mma_p_params.consumer_arv_count = 1;
    pipeline_compute_mma_p_params.initializing_warp = initializing_warp++;
    PipelineComputeMmaP pipeline_compute_mma_p(
      shared_storage.pipelines.compute_mma_p,
      pipeline_compute_mma_p_params,
      ClusterShape{}, /*barrier init*/ cute::true_type{}, /*mask calc*/cute::false_type{});

    typename PipelineComputeMmaDS::Params pipeline_compute_mma_ds_params;
    if (role == WarpRole::Mma) {
      pipeline_compute_mma_ds_params.role = PipelineComputeMmaDS::ThreadCategory::Consumer;
    }
    if (role == WarpRole::Compute) {
      pipeline_compute_mma_ds_params.role = PipelineComputeMmaDS::ThreadCategory::Producer;
    }
    pipeline_compute_mma_ds_params.producer_arv_count = cute::size(AtomThrShape_MNK{}) * kNumComputeWarps * cutlass::NumThreadsPerWarp;
    pipeline_compute_mma_ds_params.consumer_arv_count = 1;
    pipeline_compute_mma_ds_params.initializing_warp = initializing_warp++;
    PipelineComputeMmaDS pipeline_compute_mma_ds(
      shared_storage.pipelines.compute_mma_ds,
      pipeline_compute_mma_ds_params,
      ClusterShape{}, /*barrier init*/ cute::true_type{}, /*mask calc*/cute::false_type{});

    typename PipelineMmaComputeDKDV::Params pipeline_mma_compute_dkdv_params;
    if (role == WarpRole::Mma) {
      pipeline_mma_compute_dkdv_params.role = PipelineMmaComputeDKDV::ThreadCategory::Producer;
    }
    if (role == WarpRole::Compute) {
      pipeline_mma_compute_dkdv_params.role = PipelineMmaComputeDKDV::ThreadCategory::Consumer;
    }
    pipeline_mma_compute_dkdv_params.consumer_arv_count = cute::size(AtomThrShape_MNK{}) * kNumComputeWarps * cutlass::NumThreadsPerWarp;
    pipeline_mma_compute_dkdv_params.initializing_warp = initializing_warp++;
    PipelineMmaComputeDKDV pipeline_mma_compute_dkdv(
      shared_storage.pipelines.mma_compute_dkdv,
      pipeline_mma_compute_dkdv_params,
      ClusterShape{}, /*barrier init*/ cute::true_type{}, /*mask calc*/cute::false_type{});
    PipelineReduceTmaStore pipeline_reduce_tma_store;

    TmemAllocator tmem_allocator;

    // [2SM] init the cross-CTA TMEM-alloc handshake barrier (leader waits for 1).
      if (threadIdx.x == 0) {
      shared_storage.pipelines.tmem_alloc_ready.init(1);
      shared_storage.tensors.ds_full.init(1);
      shared_storage.tensors.ds_leader.init(2);
#if defined(FAG_OPT_STASYNC_XCHG)
      shared_storage.tensors.xchg_tx.init(1);
#endif
#if defined(FAG_SPLIT_DK_DQ_READY)
      shared_storage.tensors.dk_ready.init(kClusterSize * kNumComputeWarps);
#endif
      cutlass::arch::fence_barrier_init();
    }

    pipeline_load_mma_q.init_masks(ClusterShape{});
    pipeline_load_mma_qt.init_masks(ClusterShape{});
    pipeline_load_mma_kt.init_masks(ClusterShape{});
    pipeline_load_mma_do.init_masks(ClusterShape{});
    pipeline_mma_compute_s.init_masks(ClusterShape{});
    pipeline_mma_compute_dp.init_masks(ClusterShape{});
    pipeline_mma_reduce_dq.init_masks(ClusterShape{});
    pipeline_compute_mma_p.init_masks(ClusterShape{});
    pipeline_compute_mma_ds.init_masks(ClusterShape{});
    pipeline_mma_compute_dkdv.init_masks(ClusterShape{});

    typename decltype(pipeline_load_mma_q)::PipelineState pipeline_load_mma_q_consumer_state;
    typename decltype(pipeline_load_mma_qt)::PipelineState pipeline_load_mma_qt_consumer_state;
    typename decltype(pipeline_load_mma_kt)::PipelineState pipeline_load_mma_kt_consumer_state;
    typename decltype(pipeline_load_mma_do)::PipelineState pipeline_load_mma_do_consumer_state;
    typename decltype(pipeline_load_compute_lse)::PipelineState pipeline_load_compute_lse_consumer_state;
    typename decltype(pipeline_load_compute_sum_odo)::PipelineState pipeline_load_compute_sum_odo_consumer_state;
    typename decltype(pipeline_mma_compute_s)::PipelineState pipeline_mma_compute_s_consumer_state;
    typename decltype(pipeline_mma_compute_dp)::PipelineState pipeline_mma_compute_dp_consumer_state;
    typename decltype(pipeline_mma_reduce_dq)::PipelineState pipeline_mma_reduce_dq_consumer_state;
    typename decltype(pipeline_compute_mma_p)::PipelineState pipeline_compute_mma_p_consumer_state;
    typename decltype(pipeline_compute_mma_ds)::PipelineState pipeline_compute_mma_ds_consumer_state;
    typename decltype(pipeline_mma_compute_dkdv)::PipelineState pipeline_mma_compute_dkdv_consumer_state;

    auto pipeline_load_mma_q_producer_state = make_producer_start_state<decltype(pipeline_load_mma_q)>();
    auto pipeline_load_mma_qt_producer_state = make_producer_start_state<decltype(pipeline_load_mma_qt)>();
    auto pipeline_load_mma_kt_producer_state = make_producer_start_state<decltype(pipeline_load_mma_kt)>();
    auto pipeline_load_mma_do_producer_state = make_producer_start_state<decltype(pipeline_load_mma_do)>();
    auto pipeline_load_compute_lse_producer_state = make_producer_start_state<decltype(pipeline_load_compute_lse)>();
    auto pipeline_load_compute_sum_odo_producer_state = make_producer_start_state<decltype(pipeline_load_compute_sum_odo)>();
    auto pipeline_mma_compute_s_producer_state = make_producer_start_state<decltype(pipeline_mma_compute_s)>();
    auto pipeline_mma_compute_dp_producer_state = make_producer_start_state<decltype(pipeline_mma_compute_dp)>();
    auto pipeline_mma_reduce_dq_producer_state = make_producer_start_state<decltype(pipeline_mma_reduce_dq)>();
    auto pipeline_compute_mma_p_producer_state = make_producer_start_state<decltype(pipeline_compute_mma_p)>();
    auto pipeline_compute_mma_ds_producer_state = make_producer_start_state<decltype(pipeline_compute_mma_ds)>();
    auto pipeline_mma_compute_dkdv_producer_state = make_producer_start_state<decltype(pipeline_mma_compute_dkdv)>();
    auto pipeline_reduce_tma_store_producer_state = make_producer_start_state<decltype(pipeline_reduce_tma_store)>();

    // [2SM] intra-CTA sync of barrier-init writes, then a FENCED cluster
    // arrive/wait so both CTAs observe each other's mbarrier init before any
    // consumer_wait. (Relaxed arrive does NOT order init writes -> deadlock.)
    __syncthreads();
    if constexpr (kClusterSize > 1) {
      cute::cluster_arrive();
      cute::cluster_wait();
    }

    // [2SM] CORRECTED: each CTA owns its OWN K-block (blockIdx.x). Adjacent CTAs
    // {2p, 2p+1} form a cluster pair covering TWO adjacent K-blocks, which the
    // cooperative MMA splices into the M=256 axis (leader K-block -> M[0:128),
    // peer K-block -> M[128:256)). Both CTAs loop over the SAME Q-tile range.
    int k_block_idx = blockIdx.x;
    auto blk_coord = make_coord(_0{}, k_block_idx, _0{}, _0{}, make_coord(make_coord(0, blockIdx.y), blockIdx.z));
    auto [problem_shape, blk_offset] = apply_variable_length_offset(
        params.problem_shape,
        blk_coord
    );
    int iter_end = ceil_div(get<0>(problem_shape), TileShapeQ{});
    int iter_start = 0;
    if constexpr (std::is_base_of_v<cutlass::fmha::collective::CausalMask<true>, Mask>) {
      iter_start = (get<1>(blk_coord) * TileShapeK{}) / TileShapeQ{};
    } else if constexpr (std::is_base_of_v<cutlass::fmha::collective::CausalMask<false>, Mask>) {
      int offset = get<1>(problem_shape) - get<0>(problem_shape);
      iter_start = max(0, (int(get<1>(blk_coord) * TileShapeK{}) - offset) / (int)TileShapeQ{});
    }
    if (get<1>(blk_coord) * TileShapeK{} >= get<1>(problem_shape)) {
      return;
    }
    int iter_count = (iter_end - iter_start) * get<4,0,0>(problem_shape);

#ifdef BWD_2SM_DEBUG
    if (threadIdx.x == 0) {
      printf("[DBG start] blk=(%d,%d,%d) rank=%d role=%d k_block=%d iter=[%d,%d) cnt=%d\n",
             blockIdx.x, blockIdx.y, blockIdx.z, (int)cute::block_rank_in_cluster(),
             (int)role, k_block_idx, iter_start, iter_end, iter_count);
    }
#endif

    if (iter_count <= 0) {
      epilogue_clear(
          blk_coord,
          blk_offset,
          problem_shape,
          params.mainloop,
          params.epilogue
      );
      return;
    }

    if (role == WarpRole::Load) {
      warpgroup_reg_set<RegisterAllocation::kLoad>();
#ifdef BWD_2SM_DEBUG
      if (cute::elect_one_sync()) printf("[DBG load] blk=%d rank=%d enter\n", blockIdx.x, (int)cute::block_rank_in_cluster());
#endif

      load(
          blk_coord,
          blk_offset,
          problem_shape,
          iter_start,
          iter_end,
          iter_count,
          params.mainloop,
          params.mainloop_params,
          shared_storage.tensors,
          pipeline_load_mma_q, pipeline_load_mma_q_producer_state,
          pipeline_load_mma_qt, pipeline_load_mma_qt_producer_state,
          pipeline_load_mma_kt, pipeline_load_mma_kt_producer_state,
          pipeline_load_mma_do, pipeline_load_mma_do_producer_state,
          pipeline_load_compute_lse, pipeline_load_compute_lse_producer_state,
          pipeline_load_compute_sum_odo, pipeline_load_compute_sum_odo_producer_state
      );

    }
    else if (role == WarpRole::Mma) {
      warpgroup_reg_set<RegisterAllocation::kMma>();

      // [2SM] BOTH CTAs allocate the cooperative TMEM space (the leader's
      // cta_group::2 MMA writes BOTH CTAs' TMEM), then handshake so the peer's
      // alloc is cluster-visible before the leader issues the cooperative MMA.
      tmem_allocator.allocate(TmemAllocator::Sm100TmemCapacityColumns, &shared_storage.tmem_base_ptr);
      __syncwarp();
#ifdef BWD_2SM_DEBUG
      if (cute::elect_one_sync()) printf("[DBG mma] blk=%d rank=%d post-allocate\n", blockIdx.x, (int)cute::block_rank_in_cluster());
#endif
      if constexpr (kClusterSize > 1) {
        uint32_t peer_rank = cute::block_rank_in_cluster() ^ 1u;
        if (cute::elect_one_sync()) {
          shared_storage.pipelines.tmem_alloc_ready.arrive(peer_rank);
        }
        shared_storage.pipelines.tmem_alloc_ready.wait(0);
      }
#ifdef BWD_2SM_DEBUG
      if (cute::elect_one_sync()) printf("[DBG mma] blk=%d rank=%d post-handshake\n", blockIdx.x, (int)cute::block_rank_in_cluster());
#endif

      bool is_mma_leader_cta = (kClusterSize == 1) || (cute::block_rank_in_cluster() == 0);
      if (is_mma_leader_cta) {
        // [2SM] Only the leader CTA issues the cooperative cta_group::2 MMAs;
        // a single instruction fills BOTH CTAs' TMEM (peer participates via the
        // cluster implicitly). The peer's MMA warp must NOT run the consumer
        // loop (it would race / double-issue).
#ifdef BWD_2SM_NOMMA
        // DIAG: skip the cooperative MMA to isolate whether the crash is in mma().
        if (cute::elect_one_sync()) printf("[DBG mma] blk=%d leader SKIPPING mma() (BWD_2SM_NOMMA)\n", blockIdx.x);
#else
        mma(
            blk_coord,
            problem_shape,
            iter_start,
            iter_end,
            iter_count,
            params.mainloop,
            shared_storage.tensors,
            pipeline_load_mma_q, pipeline_load_mma_q_consumer_state,
            pipeline_load_mma_qt, pipeline_load_mma_qt_consumer_state,
            pipeline_load_mma_kt, pipeline_load_mma_kt_consumer_state,
            pipeline_load_mma_do, pipeline_load_mma_do_consumer_state,
            pipeline_mma_compute_s, pipeline_mma_compute_s_producer_state,
            pipeline_mma_compute_dp, pipeline_mma_compute_dp_producer_state,
            pipeline_mma_reduce_dq, pipeline_mma_reduce_dq_producer_state,
            pipeline_compute_mma_p, pipeline_compute_mma_p_consumer_state,
            pipeline_compute_mma_ds, pipeline_compute_mma_ds_consumer_state,
            pipeline_mma_compute_dkdv, pipeline_mma_compute_dkdv_producer_state
        );
#endif
      }
      else {
        // [2SM] Peer MMA warp: issue NO cooperative MMA (the leader's single
        // cta_group::2 instruction fills the peer's TMEM). Under is_leader arming
        // the TMA pipelines are leader-only (complete-tx lands on the leader's
        // barrier; the peer's Load issues no TMA, the peer's barriers are never
        // armed). So the peer MMA must NOT consumer_wait them (it would hang on
        // an un-armed barrier). The peer MMA just stays alive (its TMEM/SMEM stay
        // resident for the leader's cooperative writes) until the kernel-exit
        // cluster barrier. Mirrors mxfp8 fwd where the peer MMA warp does nothing.
      }

    }
    else if (role == WarpRole::Compute) {
      warpgroup_reg_set<RegisterAllocation::kCompute>();

      compute(
          blk_coord,
          blk_offset,
          problem_shape,
          iter_start,
          iter_end,
          iter_count,
          params.mainloop,
          params.epilogue,
          shared_storage.tensors,
          pipeline_load_compute_lse, pipeline_load_compute_lse_consumer_state,
          pipeline_load_compute_sum_odo, pipeline_load_compute_sum_odo_consumer_state,
          pipeline_mma_compute_s, pipeline_mma_compute_s_consumer_state,
          pipeline_mma_compute_dp, pipeline_mma_compute_dp_consumer_state,
          pipeline_compute_mma_p, pipeline_compute_mma_p_producer_state,
          pipeline_compute_mma_ds, pipeline_compute_mma_ds_producer_state,
          pipeline_mma_compute_dkdv, pipeline_mma_compute_dkdv_consumer_state
      );

    }
    else if (role == WarpRole::Reduce) {
      warpgroup_reg_set<RegisterAllocation::kReduce>();

      reduce(
          blk_coord,
          problem_shape,
          iter_start,
          iter_end,
          iter_count,
          params.mainloop,
          params.mainloop_params,
          shared_storage.tensors,
          pipeline_mma_reduce_dq, pipeline_mma_reduce_dq_consumer_state,
          pipeline_reduce_tma_store, pipeline_reduce_tma_store_producer_state
      );

      pipeline_reduce_tma_store.producer_tail(pipeline_reduce_tma_store_producer_state);
    }
    else if (role == WarpRole::Relay) {
      warpgroup_reg_set<RegisterAllocation::kRelay>();

      // One relay warp per CTA turns remote-copy completion into a two-CTA
      // arrival on rank 0. The MMA leader waits this barrier before every dQ.
      uint32_t phase = 0;
      for (int i = 0; i < iter_count; ++i) {
        shared_storage.tensors.ds_full.wait(phase);
        if (cute::elect_one_sync()) {
          shared_storage.tensors.ds_leader.arrive(uint32_t(0), uint32_t(1));
        }
        phase ^= 1;
      }
    }
    else {
      warpgroup_reg_set<RegisterAllocation::kEmpty>();

      /* no-op */

    }

    // Compute and Reduce occupy warps 0..11 (threads 0..383).  Keep this as a
    // single static named-barrier instruction: issuing the same barrier ID from
    // two role-specific PCs is legal in hardware but is diagnosed as divergent
    // by synccheck.  Reduce reaches here only after its TMA-store tail, so TMEM
    // cannot be freed while either dQ loads or epilogue stores are still live.
    if (role == WarpRole::Compute || role == WarpRole::Reduce) {
      cutlass::arch::NamedBarrier(
          (kNumComputeWarps + kNumReduceWarps) * NumThreadsPerWarp,
          cutlass::arch::ReservedNamedBarriers::EpilogueBarrier
      ).arrive_and_wait();
    }

    if (role == WarpRole::Compute && warp_idx % kNumComputeWarps == 0) {
      uint32_t free_stage_ptr = shared_storage.tmem_base_ptr;
      tmem_allocator.free(free_stage_ptr, TmemAllocator::Sm100TmemCapacityColumns);
    }
#endif

#ifdef BWD_2SM_DEBUG
    if (cute::elect_one_sync()) printf("[DBG exit] blk=%d rank=%d pre-exit-barrier\n", blockIdx.x, (int)cute::block_rank_in_cluster());
#endif
    // [2SM] Cluster exit barrier: when one CTA finishes its smem (which hosts
    // cluster-scoped pipeline mbarriers) becomes invalid; the peer's still-
    // pending cluster ops must complete first. All warps participate.
    if constexpr (kClusterSize > 1) {
      cute::cluster_arrive();
      cute::cluster_wait();
    }
#ifdef BWD_2SM_DEBUG
    if (cute::elect_one_sync()) printf("[DBG exit] blk=%d rank=%d post-exit-barrier (kernel done)\n", blockIdx.x, (int)cute::block_rank_in_cluster());
#endif
  }

  static dim3 get_block_shape() {
    dim3 block(MaxThreadsPerBlock, 1, 1);
    return block;
  }

  static dim3 get_grid_shape(Params const& params) {
    auto [Q, K, D, D_VO, HB] = params.problem_shape;
    auto [H, B] = HB;
    auto [H_R, H_K] = H;
    // [2SM] grid.x is the number of CTAs and must be a multiple of the cluster
    // size. Two consecutive CTAs form a cluster pair that cooperates on one
    // K-block (blockIdx.x / kClusterSize); each CTA owns an M=128 half.
    dim3 grid(cutlass::round_up(ceil_div(K, TileShapeK{}), int(kClusterSize)), H_K, B);
    return grid;
  }
};

}  // namespace cutlass::fmha::kernel
