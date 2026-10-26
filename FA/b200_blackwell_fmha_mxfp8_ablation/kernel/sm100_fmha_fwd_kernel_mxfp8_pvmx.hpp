/***************************************************************************************************
 * Copyright (c) 2024 - 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
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

// [刀13 E2OFFLOAD_V2] the v2 placement variant reuses the FULL 刀10 E2OFFLOAD
// machinery (pipeline roles, 512-count consumer set, correction wiring) — only
// the slice's position inside the correction loop differs (mainloop hpp).
#if defined(MXFP8_E2OFFLOAD_V2) && !defined(MXFP8_E2OFFLOAD)
#define MXFP8_E2OFFLOAD
#endif

#include "cutlass/cutlass.h"
#include "cute/layout.hpp"
#include "cutlass/arch/arch.h"
#include "cutlass/kernel_hardware_info.h"
#include "cutlass/pipeline/pipeline.hpp"
#include "cute/arch/tmem_allocator_sm100.hpp"

#include "kernel/fmha_options.hpp"
#include "kernel/fmha_tile_scheduler.hpp"
#include "kernel/fmha_causal_tile_scheduler.hpp"
#include "collective/fmha_fusion.hpp"
#include "collective/fmha_common.hpp"

namespace cutlass::fmha::kernel {

using namespace cute;
using namespace cutlass::fmha::collective;

#ifdef MXFP8_EXITTS
// [T2 exit-drain probe] per-warp %globaltimer at the exit cluster barrier of
// cluster 0 (blockIdx.x 0/1): slot = cta_rank*32 + warp_idx. [62]/[63] = post-
// cluster_wait stamp per CTA (warp 0). Driver prints (t - min) deltas to name
// the slowest finisher (the warp every other warp drains on). Zero overhead on
// the hot path (one MOV + one ST per warp, at exit only).
__device__ unsigned long long g_exit_ts[64];
#endif

struct Sm100FmhaCtxKernelWarpspecializedSchedule {

  enum class WarpRole {
    Softmax0,
    Softmax1,
    Correction,
    MMA,
    Load,
    Epilogue,
    Empty
  };

  static constexpr WarpRole warp_idx_to_WarpRole(int warp_idx) {
    int wg_idx = warp_idx / 4;                        // warp_idx
    if (wg_idx == 0) return WarpRole::Softmax0;       //   0 -  3
    if (wg_idx == 1) return WarpRole::Softmax1;       //   4 -  7
    if (wg_idx == 2) return WarpRole::Correction;     //   8 - 11
    if (warp_idx == 12) return WarpRole::MMA;         //       12
    if (warp_idx == 13) return WarpRole::Load;        //       13
    if (warp_idx == 14) return WarpRole::Epilogue;    //       14
    return WarpRole::Empty;                           //       15
  }

  static const int NumWarpsSoftmax = 4;
  static const int NumWarpsCorrection = 4;
  static const int NumWarpsEpilogue = 1;
  static const int NumWarpsLoad = 1;

  static const bool kDebugUsingPrintf = false;
// [刀17 v4] softmax warpgroup reg budget sweep knob (oyhj 甜点 176 vs 我方 192)。
// 默认 192 = px8a 现状 bit-exact。
#ifndef MXFP8_REGS_SOFTMAX
#define MXFP8_REGS_SOFTMAX 192
#endif
  static const int NumRegsSoftmax = MXFP8_REGS_SOFTMAX;
  static const int NumRegsCorrection = 96 - (kDebugUsingPrintf ? 16 : 0);
  static const int NumRegsOther = 32 + (kDebugUsingPrintf ? 16 : 0);
  static const int NumRegsEmpty = 24;
  
  static const int NumWarps = 16;
  
};


struct Sm100MlaFwdCtxKernelWarpspecializedSchedule {

  enum class WarpRole {
    Softmax0,
    Softmax1,
    Correction,
    MMA,
    Load,
    Epilogue,
    Empty
  };

  static constexpr WarpRole warp_idx_to_WarpRole(int warp_idx) {
    int wg_idx = warp_idx / 4;                        // warp_idx
    if (wg_idx == 0) return WarpRole::Softmax0;       //   0 -  3
    if (wg_idx == 1) return WarpRole::Softmax1;       //   4 -  7
    if (wg_idx == 2) return WarpRole::Correction;     //   8 - 11
    if (warp_idx == 12) return WarpRole::MMA;         //       12
    if (warp_idx == 13) return WarpRole::Load;        //       13
    if (warp_idx == 14) return WarpRole::Epilogue;    //       14
    return WarpRole::Empty;                           //       15
  }

  static const int NumWarpsSoftmax = 4;
  static const int NumWarpsCorrection = 4;
  static const int NumWarpsEpilogue = 1;
  static const int NumWarpsLoad = 1;

  static const bool kDebugUsingPrintf = false;
  static const int NumRegsSoftmax = 184;
  static const int NumRegsCorrection = 96 - (kDebugUsingPrintf ? 16 : 0);
  static const int NumRegsOther = 48 + (kDebugUsingPrintf ? 16 : 0);
  static const int NumRegsEmpty = 24;

  static const int NumWarps = 16;

};

template<
  class ProblemShapeIn,
  class CollectiveMainloop,
  class CollectiveEpilogue,
  class TileScheduler,
  class KernelSchedule = Sm100FmhaCtxKernelWarpspecializedSchedule
>
struct Sm100FmhaFwdKernelTmaWarpspecialized {

  using TileShape = typename CollectiveMainloop::TileShape;
  using ProblemShape = ProblemShapeIn;

  using WarpRole = typename KernelSchedule::WarpRole;

  constexpr WarpRole warp_idx_to_WarpRole(int warp_idx) {
    return KernelSchedule::warp_idx_to_WarpRole(warp_idx);
  }

  static const int NumWarpsSoftmax = KernelSchedule::NumWarpsSoftmax;
  static const int NumWarpsCorrection = KernelSchedule::NumWarpsCorrection;
  static const int NumWarpsEpilogue = KernelSchedule::NumWarpsEpilogue;
  static const int NumWarpsLoad = KernelSchedule::NumWarpsLoad;
  
  static_assert(NumWarpsEpilogue == CollectiveEpilogue::NumWarpsEpilogue);
  static_assert(NumWarpsLoad == CollectiveEpilogue::NumWarpsLoad);

  static const int NumRegsSoftmax = KernelSchedule::NumRegsSoftmax;
  static const int NumRegsCorrection = KernelSchedule::NumRegsCorrection;
  static const int NumRegsOther = KernelSchedule::NumRegsOther;
  static const int NumRegsEmpty = 24;

  static const int NumWarps = KernelSchedule::NumWarps;

  static constexpr bool IsMla = std::is_same_v<KernelSchedule, Sm100MlaFwdCtxKernelWarpspecializedSchedule>;

  using ClusterShape = typename CollectiveMainloop::ClusterShape;

  // [2SM 续19t FIX] Allocator2Sm (tcgen05.alloc.cta_group::2) is REQUIRED for the
  // 2-SM cooperative MMA to write the PEER CTA's TMEM. With Allocator1Sm each CTA
  // allocates an INDEPENDENT 1-SM TMEM region; the leader's cta_group::2 MMA cannot
  // route m128-255 into the peer's TMEM -> peer S/O = 0 (the whole peer-row failure).
  // Allocator2Sm requires BOTH CTAs' MMA warps to call allocate() with the SAME
  // dst_ptr (shared_storage.tmem_base_ptr) -> establishes the cooperative TMEM space.
  // 续19k's earlier Allocator2Sm test showed "no effect" ONLY because the peer SFA
  // (Q-scale) was ALSO zero then (续19s fix) -> peer S was zeroed by 2^-127 regardless;
  // the allocator effect was masked. Now both are fixed together. (1-SM cluster: keep
  // Allocator1Sm.)
  using TmemAllocator = cute::conditional_t<(cute::size(ClusterShape{}) > 1),
      cute::TMEM::Allocator2Sm, cute::TMEM::Allocator1Sm>;

  struct SharedStorage {
    using UnionType = union {
      typename CollectiveMainloop::TensorStorage mainloop;
      typename CollectiveEpilogue::TensorStorage epilogue;
    };

    using  StructType = struct {
      typename CollectiveMainloop::TensorStorage mainloop;
      typename CollectiveEpilogue::TensorStorage epilogue;
    };

    static constexpr bool IsPersistent = std::is_same_v<TileScheduler, PersistentTileScheduler> || std::is_same_v<TileScheduler, CausalPersistentTileScheduler>;
    using MainloopEpilogueStorage = std::conditional_t<IsPersistent, 
                                                       std::conditional_t<IsMla, 
                                                                          std::conditional_t<CollectiveMainloop::IsOrderLoadEpilogue, UnionType, StructType>,
                                                                          StructType>,
                                                       UnionType>;

    MainloopEpilogueStorage mainloop_epilogue; 

    struct PipelineStorage {
      alignas(16) typename CollectiveMainloop::PipelineQ::SharedStorage load_q;
      alignas(16) typename CollectiveMainloop::PipelineKV::SharedStorage load_kv;
      alignas(16) typename CollectiveMainloop::PipelineS::SharedStorage mma_s0;
      alignas(16) typename CollectiveMainloop::PipelineS::SharedStorage mma_s1;
      alignas(16) typename CollectiveMainloop::PipelineC::SharedStorage s0_corr;
      alignas(16) typename CollectiveMainloop::PipelineC::SharedStorage s1_corr;
      alignas(16) typename CollectiveMainloop::PipelineO::SharedStorage mma_corr;
      alignas(16) typename CollectiveMainloop::PipelineE::SharedStorage corr_epi;
      alignas(16) typename CollectiveMainloop::OrderBarrierSoftmax::SharedStorage order_s01;
      // [续19x] MMA-warp-only cross-CTA barrier: after both CTAs' MMA warps complete
      // their tcgen05.alloc.cta_group::2, the leader must wait for the PEER's alloc to
      // be cluster-visible BEFORE issuing the cooperative 2-SM MMA (whose write targets
      // the peer's TMEM). Without it the leader can race ahead and the peer-TMEM write
      // is dropped -> peer S=0. (Tutorial does cluster_sync after alloc; we can't use a
      // full cluster barrier since only the MMA warp is here, so use a ClusterBarrier.)
      alignas(16) cutlass::arch::ClusterBarrier tmem_alloc_ready;
#ifdef MXFP8_2SM_EXITDB
      // [刀18 E1 — oyhj 形态] pair-only TMEM-dealloc handshake barrier (replaces the
      // all-warp exit cluster_arrive/wait): only the EPILOGUE warp synchronizes the
      // pair before tcgen05.dealloc; all other warps exit freely (the CTA stays
      // resident until its epilogue warp returns, keeping cluster-scoped smem
      // barriers valid for any in-flight peer ops). Mirrors oyhj kernel two-piece
      // set: pipeline-init cluster handshake + this dealloc pair barrier ONLY.
      alignas(16) cutlass::arch::ClusterBarrier tmem_dealloc;
#endif
#if defined(MXFP8_SM12) && defined(MXFP8_SM12_OWAIT)
      // [SM12 OWAIT] G0 <-> w15 (O-waiter) smem protocol. MUST live here (NOT
      // in the mainloop TensorStorage union — the epilogue smem aliases it and
      // the protocol stays live through the tail).
      //   [0]      full_seq : w15 monotone count of O fulls (osync[0] >= t ⟺ O(t-1) full)
      //   [1]      epi_done : G0 warps atomicAdd after the tail epilogue (w15 waits ==4)
      //   [2..17]  g0_seq[w*4 + (t&3)] = (t<<1)|need_w — per-G0-warp ring-4 flags
      //            (G0 leads w15 by < 4 tiles: PV depth-1 ⟸ w15 release cadence)
      //   [18..21] g0_done[w] = t : warp w's rescale-of-O(t-1) finished (monotone)
      alignas(16) uint32_t sm12_osync[24];
#endif
    } pipelines;

    uint32_t tmem_base_ptr;
  };

  static constexpr int SharedStorageSize = sizeof(SharedStorage);

  struct Arguments {
    ProblemShape problem_shape;
    typename CollectiveMainloop::Arguments mainloop;
    typename CollectiveEpilogue::Arguments epilogue;
    cutlass::KernelHardwareInfo hw_info;
  };

  struct Params {
    ProblemShape problem_shape;
    typename CollectiveMainloop::Params mainloop;
    typename CollectiveEpilogue::Params epilogue;
    typename TileScheduler::Params tile_scheduler;
  };

  static const int MinBlocksPerMultiprocessor = 1;
  static const int MaxThreadsPerBlock = NumWarps * cutlass::NumThreadsPerWarp;
  using ArchTag = cutlass::arch::Sm100;

  static size_t get_workspace_size(Arguments const& args) { return 0; }
  static cutlass::Status initialize_workspace(Arguments const&, void*, cudaStream_t) {
    return cutlass::Status::kSuccess;
  }

  static bool can_implement(Arguments const& args) {
    return CollectiveMainloop::can_implement(args.problem_shape, args.mainloop);
  }

  static dim3 get_grid_shape(Params const& params) {
    return TileScheduler::get_grid_shape(params.tile_scheduler);
  }

  static dim3 get_block_shape() {
    dim3 block(MaxThreadsPerBlock, 1, 1);
    return block;
  }

  static Params to_underlying_arguments(Arguments const& args, void* workspace) {
    return Params{
        args.problem_shape,
        CollectiveMainloop::to_underlying_arguments(args.problem_shape, args.mainloop, workspace),
        CollectiveEpilogue::to_underlying_arguments(args.problem_shape, args.epilogue, workspace),
        TileScheduler::to_underlying_arguments(args.problem_shape, args.hw_info, ClusterShape{}, TileShape{})
    };
  }

  CUTLASS_DEVICE auto apply_batch(const Params &params, ProblemShape const& problem_shape, int batch_idx) {
    return apply_variable_length(params.problem_shape, batch_idx);
  }

  CUTLASS_DEVICE void operator()(const Params &params, char* smem) {
#if (! defined(CUTLASS_ARCH_MMA_SM100A_ENABLED) && ! defined(CUTLASS_ARCH_MMA_SM100F_ENABLED) && \
    ! defined(CUTLASS_ARCH_MMA_SM103A_ENABLED) && ! defined(CUTLASS_ARCH_MMA_SM103F_ENABLED))
    CUTE_INVALID_CONTROL_PATH("ERROR : Arch conditional MMA instruction used without targeting appropriate compute capability. Aborting.\n");
#else

    TileScheduler tile_scheduler{params.tile_scheduler};

    int warp_idx = cutlass::canonical_warp_idx_sync();
    auto role = warp_idx_to_WarpRole(warp_idx);
    uint32_t lane_predicate = cute::elect_one_sync();

    if (role == WarpRole::Load && lane_predicate) {
      CollectiveMainloop::prefetch_tma_descriptors(params.mainloop);
    }

    if (role == WarpRole::Epilogue && lane_predicate) {
      CollectiveEpilogue::prefetch_tma_descriptors(params.epilogue);
    }

    SharedStorage& shared_storage = *reinterpret_cast<SharedStorage*>(smem);

    auto get_epilogue_storage = [&]() {
      if constexpr (IsMla && CollectiveMainloop::IsOrderLoadEpilogue) {
        return reinterpret_cast<typename CollectiveEpilogue::TensorStorage *>(shared_storage.mainloop_epilogue.mainloop.smem_o.data());
      } else {
        return &shared_storage.mainloop_epilogue.epilogue;
      }
    };
    typename CollectiveEpilogue::TensorStorage & epilogue_storage = *get_epilogue_storage();


    typename CollectiveMainloop::PipelineQ::Params pipeline_load_q_params;
    if (role == WarpRole::Load) {
      pipeline_load_q_params.role = CollectiveMainloop::PipelineQ::ThreadCategory::Producer;
    }
    if (role == WarpRole::MMA) {
      pipeline_load_q_params.role = CollectiveMainloop::PipelineQ::ThreadCategory::Consumer;
    }
    // [续19an STOCK-FAITHFUL ARMING — replaces 续15g self-only expect-tx, which was built
    // on a WRONG model. MEASURED truth (isolation test /tmp/tut_tma2cta.cu): ALL our TMA
    // atoms are cta_group::2 (SASS: UTMALDG.2CTA) and a 2CTA TMA's complete-tx ALWAYS
    // lands on the PAIR-CTA0 (leader)'s barrier (peer-bit-masked), regardless of which
    // CTA issues it. So: ONLY the leader CTA's load warp arms (its own barrier) with
    // 2x bytes (both CTAs' deliveries); the peer arms NOTHING (its full barrier is
    // never used — mma() is leader-only). The old self-only arming made every
    // producer_acquire arrive_and_expect_tx(0) -> the full barrier phase completed
    // INSTANTLY -> every consumer_wait was VACUOUS (no data-readiness guarantee at all;
    // small shapes passed on TMA-latency luck, multi-tile shapes raced -> s>=384 bad).
    pipeline_load_q_params.is_leader = lane_predicate && (role == WarpRole::Load)
        && (cute::size(ClusterShape{}) == 1 || cute::block_rank_in_cluster() == 0);
    pipeline_load_q_params.transaction_bytes = CollectiveMainloop::TransactionBytesLoadQ
        * ((cute::size(ClusterShape{}) > 1) ? 2u : 1u);
    typename CollectiveMainloop::PipelineQ pipeline_load_q(
      shared_storage.pipelines.load_q,
      pipeline_load_q_params,
      ClusterShape{},  cute::true_type{}, /*mask calc*/cute::false_type{});
    
    typename CollectiveMainloop::PipelineKV::Params pipeline_load_kv_params;
    if (role == WarpRole::Load) {
      pipeline_load_kv_params.role = CollectiveMainloop::PipelineKV::ThreadCategory::Producer;
    }
    if (role == WarpRole::MMA) {
      pipeline_load_kv_params.role = CollectiveMainloop::PipelineKV::ThreadCategory::Consumer;
    }
    // [续19an] stock-faithful 2SM arming (see pipeline_load_q above): leader-CTA-only,
    // 2x bytes (both CTAs' K/V/SFB/SFV 2CTA-TMA tx land on the leader's barrier).
    pipeline_load_kv_params.is_leader = lane_predicate && (role == WarpRole::Load)
        && (cute::size(ClusterShape{}) == 1 || cute::block_rank_in_cluster() == 0);
    pipeline_load_kv_params.transaction_bytes = CollectiveMainloop::TransactionBytesLoadK
        * ((cute::size(ClusterShape{}) > 1) ? 2u : 1u);
    typename CollectiveMainloop::PipelineKV pipeline_load_kv(
      shared_storage.pipelines.load_kv,
      pipeline_load_kv_params,
      ClusterShape{}, /*barrier init*/ cute::true_type{}, /*mask calc*/cute::false_type{});

    typename CollectiveMainloop::PipelineS::Params pipeline_mma_s0_params;
    if (role == WarpRole::MMA) {
      pipeline_mma_s0_params.role = CollectiveMainloop::PipelineS::ThreadCategory::Producer;
    }
    if (role == WarpRole::Softmax0) {
      pipeline_mma_s0_params.role = CollectiveMainloop::PipelineS::ThreadCategory::Consumer;
    }
#if defined(MXFP8_2SM_N128SINGLE)
    // [刀27 N128SINGLE] BOTH softmax groups consume the single live S pipeline
    // (s0). The MMA issues ONE N128 commit/tile; each group reads its own 64-col
    // half of the 128-col S buffer and releases independently. (oyhj kernel:356.)
    if (role == WarpRole::Softmax1) {
      pipeline_mma_s0_params.role = CollectiveMainloop::PipelineS::ThreadCategory::Consumer;
    }
#endif
#if defined(MXFP8_SM12) && !defined(MXFP8_SM12_SKELETON)
    // [SM12] real three-split: G1 (Softmax1) consumes s0's cols [32:64) too
    // (piece table kSm12Col). Skeleton keeps s0 = G0-only (today's column map).
    if (role == WarpRole::Softmax1) {
      pipeline_mma_s0_params.role = CollectiveMainloop::PipelineS::ThreadCategory::Consumer;
    }
#endif
#ifdef MXFP8_E2OFFLOAD
    // [刀10 E2OFFLOAD] correction joins the S sub-pipeline consumer set: it
    // reads the tail P-slice columns from S TMEM and its release arrival is
    // what gates the PV MMA (producer_acquire) on the slice being written.
    if (role == WarpRole::Correction) {
      pipeline_mma_s0_params.role = CollectiveMainloop::PipelineS::ThreadCategory::Consumer;
    }
#endif
    // [2SM M2 续15] producer (cooperative cta_group::2 MMA) = 1 umma commit (multicast
    // arrive lands on BOTH CTAs' full barriers). consumer (softmax) empty release is
    // CTA-LOCAL: each CTA softmaxes its OWN M=128 half of S in its own TMEM, so the
    // leader's mma_s0.empty barrier is arrived ONLY by the leader's own Softmax0 warps
    // (peer's Softmax0 releases the PEER's barrier, not the leader's). Therefore NO
    // AtomThrShape factor here (unlike stock GEMM where the accumulator is one logical
    // tile consumed cooperatively with multicast release). Bug history: the ×2 factor
    // made the leader's empty barrier wait for 256 arrivals but only 128 ever came
    // (peer completed without arriving on leader's barrier) -> leader MMA producer_acquire
    // deadlock at the tail buf0 re-acquire. See M2_cooperation.md 续14b/续15.
    // [2SM 续15i CORRECTION] consumer_release for a 2-SM PipelineUmmaAsync calls
    // umma_arrive_2x1SM_sm0 -> the empty-barrier arrive ALWAYS targets SM0 (leader),
    // NOT the releasing CTA's own barrier. So BOTH CTAs' softmax releases land on the
    // leader's empty barrier -> it needs size(AtomThr)*NumWarpsSoftmax*32 = 256 (stock).
    // The 续15 value 128 was a band-aid that fixed s=128 ONLY because there the peer is
    // OOB-masked (seqlen_q<256) so only the leader contributes 128; for a TRUE 2-CTA
    // shape (s>=256) both CTAs contribute 256 -> 128 makes the empty barrier flip twice
    // -> producer phase desync -> the cooperative softmax/correction deadlock. Restore
    // stock 256. (Edge case: a fully-OOB peer tile still under-fills this — handled
    // separately by making the peer participate in the cooperative arrives.)
    pipeline_mma_s0_params.producer_arv_count = 1;
#if defined(MXFP8_2SM_N128SINGLE)
    // [刀27 N128SINGLE] s0 is the live S/P pipeline; both softmax groups release
    // it, so each slot needs both groups across the 2-SM atom.
    pipeline_mma_s0_params.consumer_arv_count =
        (int)cute::size(typename CollectiveMainloop::PipelineS::AtomThrShape_MNK{})
        * (2 * NumWarpsSoftmax) * cutlass::NumThreadsPerWarp;
#elif defined(MXFP8_SM12) && !defined(MXFP8_SM12_SKELETON)
    // [SM12 计数] s0 consumers = G0 (4 warps) + G1 (4 warps), both CTAs land
    // on the leader's empty barrier (umma_arrive_2x1SM_sm0):
    //   2 (AtomThr) · 8 warps · 32 = 512   (旧值 256 = 2·4·32, G0-only).
    pipeline_mma_s0_params.consumer_arv_count =
        (int)cute::size(typename CollectiveMainloop::PipelineS::AtomThrShape_MNK{})
        * (2 * NumWarpsSoftmax) * cutlass::NumThreadsPerWarp;
#elif defined(MXFP8_E2OFFLOAD)
    // [刀10] both CTAs' softmax (128 each) + both CTAs' correction (128 each)
    // arrive on the leader's empty barrier (umma_arrive_2x1SM_sm0) -> 512.
    pipeline_mma_s0_params.consumer_arv_count =
        (int)cute::size(typename CollectiveMainloop::PipelineS::AtomThrShape_MNK{})
        * (NumWarpsSoftmax + NumWarpsCorrection) * cutlass::NumThreadsPerWarp;
#else
    pipeline_mma_s0_params.consumer_arv_count =
        (int)cute::size(typename CollectiveMainloop::PipelineS::AtomThrShape_MNK{})
        * NumWarpsSoftmax * cutlass::NumThreadsPerWarp;
#endif
    typename CollectiveMainloop::PipelineS pipeline_mma_s0(
      shared_storage.pipelines.mma_s0,
      pipeline_mma_s0_params,
      ClusterShape{}, /*barrier init*/ cute::true_type{}, /*mask calc*/cute::false_type{});  // [续19x] mask calc tested: s0-only ENABLED -> no hang but peer S still 0 (mask = SIGNAL target, not the TMEM WRITE). all-3 ENABLED -> HANG (arv_count band-aid incompatible w/ stock mask). reverted.

    typename CollectiveMainloop::PipelineS::Params pipeline_mma_s1_params;
    if (role == WarpRole::MMA) {
      pipeline_mma_s1_params.role = CollectiveMainloop::PipelineS::ThreadCategory::Producer;
    }
    if (role == WarpRole::Softmax1) {
      pipeline_mma_s1_params.role = CollectiveMainloop::PipelineS::ThreadCategory::Consumer;
    }
#if defined(MXFP8_SM12) && !defined(MXFP8_SM12_SKELETON)
    // [SM12] real three-split: G2 (Softmax2) consumes s1's cols [80:128).
    if (role == WarpRole::Softmax2) {
      pipeline_mma_s1_params.role = CollectiveMainloop::PipelineS::ThreadCategory::Consumer;
    }
#endif
#ifdef MXFP8_E2OFFLOAD
    if (role == WarpRole::Correction) {
      pipeline_mma_s1_params.role = CollectiveMainloop::PipelineS::ThreadCategory::Consumer;
    }
#endif
    // [2SM 续15i] same SM0-targeted-arrive correction as pipeline_mma_s0 (restore stock 256).
    pipeline_mma_s1_params.producer_arv_count = 1;
#if defined(MXFP8_SM12) && !defined(MXFP8_SM12_SKELETON)
    // [SM12 计数] s1 consumers = G1 + G2: 2·8·32 = 512 (旧值 256, G1-only).
    pipeline_mma_s1_params.consumer_arv_count =
        (int)cute::size(typename CollectiveMainloop::PipelineS::AtomThrShape_MNK{})
        * (2 * NumWarpsSoftmax) * cutlass::NumThreadsPerWarp;
#elif defined(MXFP8_E2OFFLOAD)
    pipeline_mma_s1_params.consumer_arv_count =
        (int)cute::size(typename CollectiveMainloop::PipelineS::AtomThrShape_MNK{})
        * (NumWarpsSoftmax + NumWarpsCorrection) * cutlass::NumThreadsPerWarp;
#else
    pipeline_mma_s1_params.consumer_arv_count =
        (int)cute::size(typename CollectiveMainloop::PipelineS::AtomThrShape_MNK{})
        * NumWarpsSoftmax * cutlass::NumThreadsPerWarp;
#endif
    typename CollectiveMainloop::PipelineS pipeline_mma_s1(
      shared_storage.pipelines.mma_s1,
      pipeline_mma_s1_params,
      ClusterShape{}, /*barrier init*/ cute::true_type{}, /*mask calc*/cute::false_type{});  // [续19x] s1 reverted (isolate s0)

    typename CollectiveMainloop::PipelineC::Params pipeline_s0_corr_params;
    if (role == WarpRole::Softmax0) {
      pipeline_s0_corr_params.role = CollectiveMainloop::PipelineC::ThreadCategory::Producer;
    }
    if (role == WarpRole::Correction) {
      pipeline_s0_corr_params.role = CollectiveMainloop::PipelineC::ThreadCategory::Consumer;
    }
    pipeline_s0_corr_params.producer_arv_count = NumWarpsSoftmax * cutlass::NumThreadsPerWarp;
    pipeline_s0_corr_params.consumer_arv_count = NumWarpsCorrection * cutlass::NumThreadsPerWarp;
    typename CollectiveMainloop::PipelineC pipeline_s0_corr(
      shared_storage.pipelines.s0_corr,
      pipeline_s0_corr_params,
      /*barrier init*/ cute::true_type{});

    typename CollectiveMainloop::PipelineC::Params pipeline_s1_corr_params;
    if (role == WarpRole::Softmax1) {
      pipeline_s1_corr_params.role = CollectiveMainloop::PipelineC::ThreadCategory::Producer;
    }
    if (role == WarpRole::Correction) {
      pipeline_s1_corr_params.role = CollectiveMainloop::PipelineC::ThreadCategory::Consumer;
    }
    pipeline_s1_corr_params.producer_arv_count = NumWarpsSoftmax * cutlass::NumThreadsPerWarp;
    pipeline_s1_corr_params.consumer_arv_count = NumWarpsCorrection * cutlass::NumThreadsPerWarp;
    typename CollectiveMainloop::PipelineC pipeline_s1_corr(
      shared_storage.pipelines.s1_corr,
      pipeline_s1_corr_params,
      /*barrier init*/ cute::true_type{});

    typename CollectiveMainloop::PipelineO::Params pipeline_mma_corr_params;
    if (role == WarpRole::MMA) {
      pipeline_mma_corr_params.role = CollectiveMainloop::PipelineO::ThreadCategory::Producer;
    }
#if defined(MXFP8_SM12) && defined(MXFP8_SM12_OWAIT)
    // [SM12 OWAIT] the O consumer is w15 (ex-Empty): a DEDICATED waiter that
    // absorbs the PV-completion wait in parallel (the role the 4-warp
    // correction group played pre-SM12). G0 only handshakes via smem on
    // rescale tiles. consumer_arv_count: 2 (AtomThr) · 1 warp · 32 = 64.
    if (role == WarpRole::Empty) {
      pipeline_mma_corr_params.role = CollectiveMainloop::PipelineO::ThreadCategory::Consumer;
    }
#elif defined(MXFP8_SM12)
    // [SM12] the O consumer is now Softmax0 (G0 absorbs correction). The
    // consumer_arv_count below is UNCHANGED (2·NumWarpsCorrection·32 = 256):
    // G0 is also exactly 4 warps — only the role owning the count moved.
    if (role == WarpRole::Softmax0) {
      pipeline_mma_corr_params.role = CollectiveMainloop::PipelineO::ThreadCategory::Consumer;
    }
#else
    if (role == WarpRole::Correction) {
      pipeline_mma_corr_params.role = CollectiveMainloop::PipelineO::ThreadCategory::Consumer;
    }
#endif
    // [2SM M2 续15] PipelineO is the O-accumulator pipeline (cooperative cta_group::2
    // PV MMA -> correction). SAME CTA-LOCAL rationale as PipelineS: each CTA's
    // correction consumes its OWN M=128 half of O (in its own TMEM) and calls plain
    // consumer_release (NOT consumer_release_2x1SM — see mainloop L1469/L1515), so the
    // release arrives ONLY on this CTA's own empty barrier. The peer's correction
    // releases the PEER's barrier, never the leader's. Therefore NO AtomThrShape
    // factor (the prior ×2 caused the s>=256 / n>=2 loop hang: the leader's O-empty
    // barrier waited for 256 arrivals but only its own 128 correction threads came,
    // so the loop's 2nd corr producer_acquire deadlocked exactly like PipelineS did
    // at n=1). See M2_cooperation.md 续15. [续15i] correction's consumer_release also
    // targets SM0 (umma_arrive_2x1SM_sm0) so both CTAs' correction land on the leader's
    // O-empty barrier -> restore stock size(AtomThr)*NumWarpsCorrection*32 = 256.
    pipeline_mma_corr_params.producer_arv_count = 1;
#if defined(MXFP8_SM12) && defined(MXFP8_SM12_OWAIT)
    // [SM12 OWAIT 计数] 2 (AtomThr — both CTAs' w15 land on the leader's
    // empty barrier via umma_arrive_2x1SM_sm0) · 1 warp · 32 = 64 (旧 256).
    pipeline_mma_corr_params.consumer_arv_count =
        (int)cute::size(typename CollectiveMainloop::PipelineO::AtomThrShape_MNK{})
        * 1 * cutlass::NumThreadsPerWarp;
#else
    pipeline_mma_corr_params.consumer_arv_count =
        (int)cute::size(typename CollectiveMainloop::PipelineO::AtomThrShape_MNK{})
        * NumWarpsCorrection * cutlass::NumThreadsPerWarp;
#endif
    typename CollectiveMainloop::PipelineO pipeline_mma_corr(
      shared_storage.pipelines.mma_corr,
      pipeline_mma_corr_params,
      ClusterShape{}, /*barrier init*/ cute::true_type{}, /*mask calc*/cute::false_type{});  // [续19x] corr reverted (isolate s0)

    typename CollectiveMainloop::PipelineE::Params pipeline_corr_epi_params;
#ifdef MXFP8_SM12
    // [SM12] the epi producer is now Softmax0 (G0 runs the ex-correction
    // tail). producer_arv_count UNCHANGED: 4 warps · 32 = 128.
    if (role == WarpRole::Softmax0) {
      pipeline_corr_epi_params.role = CollectiveMainloop::PipelineE::ThreadCategory::Producer;
    }
#else
    if (role == WarpRole::Correction) {
      pipeline_corr_epi_params.role = CollectiveMainloop::PipelineE::ThreadCategory::Producer;
    }
#endif
    if (role == WarpRole::Epilogue) {
      pipeline_corr_epi_params.role = CollectiveMainloop::PipelineE::ThreadCategory::Consumer;
    }
    pipeline_corr_epi_params.producer_arv_count = NumWarpsCorrection * cutlass::NumThreadsPerWarp;
    pipeline_corr_epi_params.consumer_arv_count = NumWarpsEpilogue * cutlass::NumThreadsPerWarp;
    typename CollectiveMainloop::PipelineE pipeline_corr_epi(
      shared_storage.pipelines.corr_epi,
      pipeline_corr_epi_params,
      /*barrier init*/ cute::true_type{});

    typename CollectiveMainloop::OrderBarrierSoftmax::Params params_order_s01;
#ifdef MXFP8_SM12
    // [SM12] 3-group ordered chain g0 -> g1 -> g2 (OrderedSequenceBarrier<1,3>).
    params_order_s01.group_id = role == WarpRole::Softmax1 ? 1
                              : (role == WarpRole::Softmax2 ? 2 : 0);
#else
    params_order_s01.group_id = role == WarpRole::Softmax1 ? 1 : 0;
#endif
    params_order_s01.group_size = NumWarpsSoftmax * cutlass::NumThreadsPerWarp;
    typename CollectiveMainloop::OrderBarrierSoftmax order_s01(
      shared_storage.pipelines.order_s01, params_order_s01);

    TmemAllocator tmem_allocator;

    // [续19x] init the MMA-warp cross-CTA TMEM-alloc-ready barrier. One arrival per
    // CTA's MMA warp (the leader waits for the peer's single arrive). Init by one
    // elected thread; the cluster_arrive/wait below publishes it to the peer.
    if constexpr (cute::size(ClusterShape{}) > 1) {
      if (threadIdx.x == 0) {
        shared_storage.pipelines.tmem_alloc_ready.init(1);
#ifdef MXFP8_2SM_EXITDB
        // [刀18 E1] arrive count = the PEER's epilogue warp (32 threads). Published
        // to the peer by the init cluster_arrive/wait below (same as oyhj: dealloc
        // barrier inited before the cluster-wide pipeline-init handshake).
        shared_storage.pipelines.tmem_dealloc.init(NumWarpsEpilogue * cutlass::NumThreadsPerWarp);
#endif
      }
    }

#if defined(MXFP8_SM12) && defined(MXFP8_SM12_OWAIT)
    // [SM12 OWAIT] zero the G0<->w15 protocol counters (published by the
    // __syncthreads below; IndividualTileScheduler = one tile per CTA, so
    // once-per-kernel init suffices).
    if (threadIdx.x == 0) {
      CUTLASS_PRAGMA_UNROLL
      for (int i = 0; i < 24; ++i) { shared_storage.pipelines.sm12_osync[i] = 0u; }
    }
#endif

    __syncthreads();

    pipeline_load_q.init_masks(ClusterShape{});
    pipeline_load_kv.init_masks(ClusterShape{});
    pipeline_mma_s0.init_masks(ClusterShape{});
    pipeline_mma_s1.init_masks(ClusterShape{});
    pipeline_mma_corr.init_masks(ClusterShape{});

    // [2SM M2] cluster-wide barrier: the __syncthreads above only syncs WITHIN
    // one CTA. In a 2-CTA cluster, a consumer must not wait on an mbarrier that
    // the peer CTA hasn't initialized yet. Sync the whole cluster once after all
    // mbarriers are initialized. (1-SM falls through — __syncthreads sufficed.)
    if constexpr (cute::size(ClusterShape{}) > 1) {
      // [2SM FIX] Use the NON-relaxed cluster_arrive (barrier.cluster.arrive,
      // release fence) NOT cluster_arrive_relaxed. The mbarriers were just
      // initialized by the pipeline constructors above; the relaxed arrive does
      // NOT order those init writes before the arrive, so the peer CTA could pass
      // cluster_wait and consumer_wait/producer_acquire on an mbarrier whose init
      // it hasn't observed yet -> intermittent startup deadlock. The fenced
      // arrive gives release/acquire ordering with cluster_wait.
      cute::cluster_arrive();
      cute::cluster_wait();
    }

    typename CollectiveMainloop::PipelineQ::PipelineState pipeline_load_q_consumer_state;
    typename CollectiveMainloop::PipelineQ::PipelineState pipeline_load_q_producer_state = cutlass::make_producer_start_state<typename CollectiveMainloop::PipelineQ>();

    typename CollectiveMainloop::PipelineKV::PipelineState pipeline_load_kv_consumer_state;
    typename CollectiveMainloop::PipelineKV::PipelineState pipeline_load_kv_producer_state = cutlass::make_producer_start_state<typename CollectiveMainloop::PipelineKV>();

    typename CollectiveMainloop::PipelineS::PipelineState pipeline_mma_s0_consumer_state;
    typename CollectiveMainloop::PipelineS::PipelineState pipeline_mma_s0_producer_state = cutlass::make_producer_start_state<typename CollectiveMainloop::PipelineS>();

    typename CollectiveMainloop::PipelineS::PipelineState pipeline_mma_s1_consumer_state;
    typename CollectiveMainloop::PipelineS::PipelineState pipeline_mma_s1_producer_state = cutlass::make_producer_start_state<typename CollectiveMainloop::PipelineS>();

#ifdef MXFP8_E2OFFLOAD
    // [刀10] correction's OWN release-tracking states on the S sub-pipelines
    // (separate from the softmax warps' consumer states).
    typename CollectiveMainloop::PipelineS::PipelineState pipeline_mma_s0_correl_state;
    typename CollectiveMainloop::PipelineS::PipelineState pipeline_mma_s1_correl_state;
#endif
    typename CollectiveMainloop::PipelineC::PipelineState pipeline_s0_corr_consumer_state;
    typename CollectiveMainloop::PipelineC::PipelineState pipeline_s0_corr_producer_state = cutlass::make_producer_start_state<typename CollectiveMainloop::PipelineC>();

    typename CollectiveMainloop::PipelineC::PipelineState pipeline_s1_corr_consumer_state;
    typename CollectiveMainloop::PipelineC::PipelineState pipeline_s1_corr_producer_state = cutlass::make_producer_start_state<typename CollectiveMainloop::PipelineC>();

    typename CollectiveMainloop::PipelineE::PipelineState pipeline_corr_epi_consumer_state;
    typename CollectiveMainloop::PipelineE::PipelineState pipeline_corr_epi_producer_state = cutlass::make_producer_start_state<typename CollectiveMainloop::PipelineE>();

    typename CollectiveMainloop::PipelineO::PipelineState pipeline_mma_corr_consumer_state;
    typename CollectiveMainloop::PipelineO::PipelineState pipeline_mma_corr_producer_state = cutlass::make_producer_start_state<typename CollectiveMainloop::PipelineO>();

    CollectiveMainloop mainloop;
    CollectiveEpilogue epilogue{params.epilogue};

#ifdef MXFP8_SM12
    // [SM12] 12-warp softmax dispatch: all three groups run softmax12 (G0
    // additionally owns the merged correction + tail epilogue). PipelineC
    // (s0_corr/s1_corr) is fully retired — constructed above but never
    // produced/consumed.
    if (role == WarpRole::Softmax0 || role == WarpRole::Softmax1 || role == WarpRole::Softmax2) {
      warpgroup_reg_set<NumRegsSoftmax>();

      CUTLASS_PRAGMA_NO_UNROLL
      for (; tile_scheduler.is_valid(); ++tile_scheduler) {
        auto blk_coord = tile_scheduler.get_block_coord();

        auto logical_problem_shape = apply_batch(params,
            params.problem_shape, get<2,1>(blk_coord));

        // [2SM 续15j] cluster-aware OOB skip (identical rationale to the 8+4 path).
        if ((get<0>(blk_coord) - (int)cute::block_rank_in_cluster()) * get<0>(TileShape{}) >= get<0>(logical_problem_shape)) {
          continue;
        }

        if (get<1>(logical_problem_shape) == 0) {
          // [SM12] G0 inherits correction's empty-tile epilogue signalling.
          if (role == WarpRole::Softmax0) {
            mainloop.correction_empty(
              blk_coord,
              params.mainloop, logical_problem_shape,
              params.problem_shape,
              epilogue_storage,
              pipeline_corr_epi, pipeline_corr_epi_producer_state,
              epilogue
            );
          }
          continue;
        }

        if (role == WarpRole::Softmax0) {
          mainloop.template softmax12<0>(
            blk_coord, params.mainloop, logical_problem_shape, params.problem_shape,
            shared_storage.mainloop_epilogue.mainloop, epilogue_storage,
            pipeline_mma_s0, pipeline_mma_s0_consumer_state,
            pipeline_mma_s1, pipeline_mma_s1_consumer_state,
            pipeline_mma_corr, pipeline_mma_corr_consumer_state,
            pipeline_corr_epi, pipeline_corr_epi_producer_state,
            order_s01, epilogue
#ifdef MXFP8_SM12_OWAIT
            , shared_storage.pipelines.sm12_osync
#endif
            );
        } else if (role == WarpRole::Softmax1) {
          mainloop.template softmax12<1>(
            blk_coord, params.mainloop, logical_problem_shape, params.problem_shape,
            shared_storage.mainloop_epilogue.mainloop, epilogue_storage,
            pipeline_mma_s0, pipeline_mma_s0_consumer_state,
            pipeline_mma_s1, pipeline_mma_s1_consumer_state,
            pipeline_mma_corr, pipeline_mma_corr_consumer_state,
            pipeline_corr_epi, pipeline_corr_epi_producer_state,
            order_s01, epilogue
#ifdef MXFP8_SM12_OWAIT
            , shared_storage.pipelines.sm12_osync
#endif
            );
        } else {
          mainloop.template softmax12<2>(
            blk_coord, params.mainloop, logical_problem_shape, params.problem_shape,
            shared_storage.mainloop_epilogue.mainloop, epilogue_storage,
            pipeline_mma_s0, pipeline_mma_s0_consumer_state,
            pipeline_mma_s1, pipeline_mma_s1_consumer_state,
            pipeline_mma_corr, pipeline_mma_corr_consumer_state,
            pipeline_corr_epi, pipeline_corr_epi_producer_state,
            order_s01, epilogue
#ifdef MXFP8_SM12_OWAIT
            , shared_storage.pipelines.sm12_osync
#endif
            );
        }
      }
    }
#else
    if (role == WarpRole::Softmax0 || role == WarpRole::Softmax1) {
      warpgroup_reg_set<NumRegsSoftmax>();

      CUTLASS_PRAGMA_NO_UNROLL
      for (; tile_scheduler.is_valid(); ++tile_scheduler) {
        auto blk_coord = tile_scheduler.get_block_coord();

        auto logical_problem_shape = apply_batch(params,
            params.problem_shape, get<2,1>(blk_coord));

        // [2SM 续15j] cluster-aware OOB skip: skip only when the COOPERATIVE tile's
        // leader half is OOB. A partial tile (peer half OOB, leader half valid) must
        // run on BOTH CTAs so the peer still issues its umma_arrive_2x1SM_sm0 to the
        // LEADER's S/O empty barriers (which need 256 = both CTAs); else they under-fill
        // -> cooperative softmax/correction deadlock (seen on seqlen_q%256==128: s=128,
        // s=384). Per-row ResidualMask zeroes the peer's OOB rows; epilogue predicates
        // OOB stores. (1-SM: block_rank=0 -> unchanged.)
        if ((get<0>(blk_coord) - (int)cute::block_rank_in_cluster()) * get<0>(TileShape{}) >= get<0>(logical_problem_shape)) {
          continue;
        }

        if (get<1>(logical_problem_shape) == 0) {
          continue;
        }

        bool is_softmax_0 = role == WarpRole::Softmax0;

        mainloop.softmax(
           is_softmax_0 ? 0 : 1, blk_coord,
           params.mainloop, logical_problem_shape,
           shared_storage.mainloop_epilogue.mainloop,   // [PVMX 2a.0] TensorStorage: softmax writes P -> smem_p
#if defined(MXFP8_2SM_N128SINGLE)
           pipeline_mma_s0,
           pipeline_mma_s0_consumer_state,
#else
           is_softmax_0 ? pipeline_mma_s0 : pipeline_mma_s1,
           is_softmax_0 ? pipeline_mma_s0_consumer_state : pipeline_mma_s1_consumer_state,
#endif
           is_softmax_0 ? pipeline_s0_corr : pipeline_s1_corr,
           is_softmax_0 ? pipeline_s0_corr_producer_state : pipeline_s1_corr_producer_state,
           order_s01
         );

       }
    }
#endif
    else if (role == WarpRole::Correction) {
      cutlass::arch::warpgroup_reg_dealloc<NumRegsCorrection>();

      bool has_valid = false;

      CUTLASS_PRAGMA_NO_UNROLL
      for (; tile_scheduler.is_valid(); ++tile_scheduler) {
        auto blk_coord = tile_scheduler.get_block_coord();

        auto logical_problem_shape = apply_batch(params,
            params.problem_shape, get<2,1>(blk_coord));

        // [2SM 续15j] cluster-aware OOB skip: skip only when the COOPERATIVE tile's
        // leader half is OOB. A partial tile (peer half OOB, leader half valid) must
        // run on BOTH CTAs so the peer still issues its umma_arrive_2x1SM_sm0 to the
        // LEADER's S/O empty barriers (which need 256 = both CTAs); else they under-fill
        // -> cooperative softmax/correction deadlock (seen on seqlen_q%256==128: s=128,
        // s=384). Per-row ResidualMask zeroes the peer's OOB rows; epilogue predicates
        // OOB stores. (1-SM: block_rank=0 -> unchanged.)
        if ((get<0>(blk_coord) - (int)cute::block_rank_in_cluster()) * get<0>(TileShape{}) >= get<0>(logical_problem_shape)) {
          continue;
        }

        has_valid = true;

        if (get<1>(logical_problem_shape) == 0) {
          mainloop.correction_empty(
            blk_coord,
            params.mainloop, logical_problem_shape,
            params.problem_shape,
            epilogue_storage,
            pipeline_corr_epi, pipeline_corr_epi_producer_state,
            epilogue
          );
          continue;
        }

        mainloop.correction(
          blk_coord,
          params.mainloop, logical_problem_shape,
          params.problem_shape,
          epilogue_storage,
#ifdef MXFP8_E2OFFLOAD
          shared_storage.mainloop_epilogue.mainloop,
          pipeline_mma_s0, pipeline_mma_s0_correl_state,
          pipeline_mma_s1, pipeline_mma_s1_correl_state,
#endif
          pipeline_s0_corr, pipeline_s0_corr_consumer_state,
          pipeline_s1_corr, pipeline_s1_corr_consumer_state,
          pipeline_mma_corr, pipeline_mma_corr_consumer_state,
          pipeline_corr_epi, pipeline_corr_epi_producer_state,
          epilogue
        );

      }

      if constexpr (NumWarpsEpilogue == 0) {
        static_assert(NumWarpsCorrection == 1);

        if (has_valid) {
          uint32_t free_stage_ptr = shared_storage.tmem_base_ptr;
          tmem_allocator.free(free_stage_ptr, TmemAllocator::Sm100TmemCapacityColumns);
        }
      }

    }
    else if (role == WarpRole::MMA) {
      warpgroup_reg_set<NumRegsOther>();

      bool allocated = false;

      CUTLASS_PRAGMA_NO_UNROLL
      for (; tile_scheduler.is_valid(); ++tile_scheduler) {
        auto blk_coord = tile_scheduler.get_block_coord();

        auto logical_problem_shape = apply_batch(params,
            params.problem_shape, get<2,1>(blk_coord));

        // [2SM 续15j] cluster-aware OOB skip: skip only when the COOPERATIVE tile's
        // leader half is OOB. A partial tile (peer half OOB, leader half valid) must
        // run on BOTH CTAs so the peer still issues its umma_arrive_2x1SM_sm0 to the
        // LEADER's S/O empty barriers (which need 256 = both CTAs); else they under-fill
        // -> cooperative softmax/correction deadlock (seen on seqlen_q%256==128: s=128,
        // s=384). Per-row ResidualMask zeroes the peer's OOB rows; epilogue predicates
        // OOB stores. (1-SM: block_rank=0 -> unchanged.)
        if ((get<0>(blk_coord) - (int)cute::block_rank_in_cluster()) * get<0>(TileShape{}) >= get<0>(logical_problem_shape)) {
          continue;
        }

        if (!allocated) {
          tmem_allocator.allocate(TmemAllocator::Sm100TmemCapacityColumns, &shared_storage.tmem_base_ptr);
#ifdef MXFP8_2SM_RELLOCK
          // [刀18 E3 — oyhj/stock 形态] release the cta_group::2 allocation permit
          // IMMEDIATELY after alloc (oyhj kernel L614 / stock 2-SM GEMM), instead of
          // after the whole MMA loop: unblocks the pair/SM alloc permit for the
          // dealloc path + next resident CTA without holding it across the mainloop.
          tmem_allocator.release_allocation_lock();
#endif
          __syncwarp();
#ifdef MXFP8_DBG
          // [续19t] DECISIVE: is tmem_base_ptr actually 0 for BOTH CTAs? If peer base != 0
          // (or != leader), the leader's cta_group::2 MMA writes peer-TMEM at absolute S0
          // but peer READS peer-TMEM[base+S0] -> mismatch -> peer S=0. (续19n only assumed.)
          if ((blockIdx.x == 0 || blockIdx.x == 1) && blockIdx.y == 0 && blockIdx.z == 0 && cute::elect_one_sync()) {
            g_dbg_tmem_base[blockIdx.x] = shared_storage.tmem_base_ptr;
          }
#endif
          // [2SM] NOTE: tmem_base_ptr==0 (leader works with ABSOLUTE TmemAllocation
          // offsets, which only holds if base==0). So stock's set_tmem_offsets(base)
          // base-relative refactor = identical addrs = NOT the peer-S=0 fix. Peer fix
          // needs cuda-gdb on the UTCQMMA.2CTA peer-TMEM write. See M2 续19m/n.
          allocated = true;

#ifdef MXFP8_2SM_ALLOC_SYNC
          // [续19x] cross-CTA TMEM-alloc-ready sync: each CTA's MMA warp remote-arrives
          // the PEER's barrier (signalling "my tcgen05.alloc.cta_group::2 is done"), then
          // waits its own. The leader's cooperative 2-SM MMA writes the peer's TMEM, so
          // the peer's alloc MUST be cluster-visible first; else the peer-half write is
          // dropped -> peer S=0. (Tutorial does cluster_sync after alloc.)
          if constexpr (cute::size(ClusterShape{}) > 1) {
            uint32_t peer_rank = cute::block_rank_in_cluster() ^ 1u;
            if (cute::elect_one_sync()) {
              shared_storage.pipelines.tmem_alloc_ready.arrive(peer_rank);
            }
            shared_storage.pipelines.tmem_alloc_ready.wait(0);
          }
#endif
        }

        if (get<1>(logical_problem_shape) == 0) {
          continue;
        }

        // [2SM M2] In a 2-SM (cta_group::2) MMA, ONLY the leader CTA issues mma();
        // the single cooperative instruction fills BOTH CTAs' TMEM. The peer CTA
        // must NOT run the mma() consumer loop (it would hang on consumer_wait and
        // race the leader). Matches stock sm100_gemm_tma_warpspecialized.hpp:427/761.
        // 1-SM: block_rank_in_cluster()==0 always => leader always runs (unchanged).
        // [续19v] TEST (reverted): both CTAs issue MMA -> HANG (pipelines are leader-only
        // by design; the peer's mma() consumer_wait deadlocks). Inconclusive for the
        // M-split-vs-N-cooperative question; needs pipeline restructure to test cleanly.
        bool is_mma_leader_cta = (cute::block_rank_in_cluster() == 0);
#ifdef MXFP8_2SM_CLUSTERCHK
        if ((blockIdx.x==0||blockIdx.x==1) && blockIdx.y==0 && blockIdx.z==0 && cute::elect_one_sync()) {
          unsigned nctarank, ctarank, smid;
          asm volatile("mov.u32 %0, %%cluster_nctarank;" : "=r"(nctarank));
          asm volatile("mov.u32 %0, %%cluster_ctarank;" : "=r"(ctarank));
          asm volatile("mov.u32 %0, %%smid;" : "=r"(smid));
          printf("[CLCHK-K] blockIdx=%d MMA-warp cluster_nctarank=%u cluster_ctarank=%u smid=%u rank_in_cluster=%u is_leader=%d base=%u\n",
                 (int)blockIdx.x, nctarank, ctarank, smid, (unsigned)cute::block_rank_in_cluster(),
                 (int)is_mma_leader_cta, shared_storage.tmem_base_ptr);
        }
#endif
        if (is_mma_leader_cta) {
          mainloop.mma(
            blk_coord,
            params.mainloop, logical_problem_shape,
            shared_storage.mainloop_epilogue.mainloop,
            pipeline_load_q, pipeline_load_q_consumer_state,
            pipeline_load_kv, pipeline_load_kv_consumer_state,
            pipeline_mma_s0, pipeline_mma_s0_producer_state,
            pipeline_mma_s1, pipeline_mma_s1_producer_state,
            pipeline_mma_corr, pipeline_mma_corr_producer_state
          );
        }

      }
      // [2SM 续19t] Allocator2Sm: relinquish the alloc permit after the MMA loop so the
      // dealloc (in the correction/epilogue warp) and the next CTA's rasterization can
      // proceed. Mirrors stock sm100_gemm_tma_warpspecialized.hpp:783. Both CTAs' MMA
      // warps issued allocate(); both must relinquish. (1-SM Allocator1Sm relinquish is
      // a cta_group::1 no-op-equivalent; harmless.)
      if (allocated) {
#ifndef MXFP8_2SM_RELLOCK
        tmem_allocator.release_allocation_lock();
#endif
      }
    }
    else if (role == WarpRole::Load) {
      warpgroup_reg_set<NumRegsOther>();

      if constexpr (IsMla && CollectiveMainloop::IsOrderLoadEpilogue) {
        cutlass::arch::NamedBarrier::arrive((NumWarpsLoad + NumWarpsEpilogue) * NumThreadsPerWarp, 
                                      cutlass::arch::ReservedNamedBarriers::EpilogueBarrier);
      }

      CUTLASS_PRAGMA_NO_UNROLL
      for (; tile_scheduler.is_valid(); ++tile_scheduler) {
        auto blk_coord = tile_scheduler.get_block_coord();

        auto logical_problem_shape = apply_batch(params,
            params.problem_shape, get<2,1>(blk_coord));

        // [2SM 续15j] cluster-aware OOB skip: skip only when the COOPERATIVE tile's
        // leader half is OOB. A partial tile (peer half OOB, leader half valid) must
        // run on BOTH CTAs so the peer still issues its umma_arrive_2x1SM_sm0 to the
        // LEADER's S/O empty barriers (which need 256 = both CTAs); else they under-fill
        // -> cooperative softmax/correction deadlock (seen on seqlen_q%256==128: s=128,
        // s=384). Per-row ResidualMask zeroes the peer's OOB rows; epilogue predicates
        // OOB stores. (1-SM: block_rank=0 -> unchanged.)
        if ((get<0>(blk_coord) - (int)cute::block_rank_in_cluster()) * get<0>(TileShape{}) >= get<0>(logical_problem_shape)) {
          continue;
        }

        if (get<1>(logical_problem_shape) == 0) {
          continue;
        }

        mainloop.load(
          blk_coord, logical_problem_shape,
          params.mainloop, params.problem_shape,
          shared_storage.mainloop_epilogue.mainloop,
          pipeline_load_q, pipeline_load_q_producer_state,
          pipeline_load_kv, pipeline_load_kv_producer_state
        );

      }
      // [2SM FIX] producer_tail: in a 2-CTA cluster the Load (producer) warp must
      // NOT exit until every consumer has released all in-flight Q/KV buffers.
      // Without this the Load warp returns early (cuda-gdb confirmed warp 13 exited
      // while all compute warps were still blocked on cluster mbarriers), which in a
      // cluster lets this CTA race toward exit and desync the peer's cluster-scoped
      // pipeline barriers -> deadlock. Stock CUTLASS calls producer_tail for exactly
      // this ("Prevents early exit of producer blocks in Cluster").
      if constexpr (cute::size(ClusterShape{}) > 1) {
        pipeline_load_q.producer_tail(pipeline_load_q_producer_state);
        pipeline_load_kv.producer_tail(pipeline_load_kv_producer_state);
      }
    }
    else if (role == WarpRole::Epilogue) {
      warpgroup_reg_set<NumRegsOther>();

      bool has_valid = false;

      CUTLASS_PRAGMA_NO_UNROLL
      for (; tile_scheduler.is_valid(); ++tile_scheduler) {
        auto blk_coord = tile_scheduler.get_block_coord();

        auto logical_problem_shape = apply_batch(params,
            params.problem_shape, get<2,1>(blk_coord));

        // [2SM 续15j] cluster-aware OOB skip: skip only when the COOPERATIVE tile's
        // leader half is OOB. A partial tile (peer half OOB, leader half valid) must
        // run on BOTH CTAs so the peer still issues its umma_arrive_2x1SM_sm0 to the
        // LEADER's S/O empty barriers (which need 256 = both CTAs); else they under-fill
        // -> cooperative softmax/correction deadlock (seen on seqlen_q%256==128: s=128,
        // s=384). Per-row ResidualMask zeroes the peer's OOB rows; epilogue predicates
        // OOB stores. (1-SM: block_rank=0 -> unchanged.)
        if ((get<0>(blk_coord) - (int)cute::block_rank_in_cluster()) * get<0>(TileShape{}) >= get<0>(logical_problem_shape)) {
          continue;
        }

        has_valid = true;

        epilogue.store(
          blk_coord, logical_problem_shape,
          params.epilogue, params.problem_shape,
          epilogue_storage,
          pipeline_corr_epi, pipeline_corr_epi_consumer_state
        );

      }

      static_assert(NumWarpsEpilogue <= 1);
      if constexpr (NumWarpsEpilogue == 1) {
        if(has_valid) {
#ifdef MXFP8_2SM_EXITDB
          // [刀18 E1 — oyhj 形态] tcgen05.dealloc.cta_group::2 frees the PAIR-wide
          // allocation: neither CTA may free while its peer still issues tcgen05
          // ops. Same handshake as pristine 2-SM GEMM / oyhj: follower arrives
          // first, leader waits, leader arrives back, follower waits — then both
          // free. has_valid is pair-uniform (the 续15j OOB skip subtracts the CTA
          // rank, so both CTAs of a pair evaluate the same leader-half condition).
          if constexpr (cute::size(ClusterShape{}) > 1) {
            auto& dealloc_bar = shared_storage.pipelines.tmem_dealloc;
            uint32_t peer_rank = uint32_t(cute::block_rank_in_cluster()) ^ 1;
            bool is_leader_cta = (cute::block_rank_in_cluster() & 1) == 0;
            dealloc_bar.arrive(peer_rank, !is_leader_cta);
            dealloc_bar.wait(0);
            dealloc_bar.arrive(peer_rank, is_leader_cta);
          }
#endif
          uint32_t free_stage_ptr = shared_storage.tmem_base_ptr;
          tmem_allocator.free(free_stage_ptr, TmemAllocator::Sm100TmemCapacityColumns);
        }
      }

    }
    else if (role == WarpRole::Empty) {
      warpgroup_reg_set<NumRegsEmpty>();

#if defined(MXFP8_SM12) && defined(MXFP8_SM12_OWAIT)
      // [SM12 OWAIT] w15 = dedicated PipelineO waiter: absorbs the PV(t-1)
      // completion wait in PARALLEL with the softmax warps (the role the
      // dedicated correction group played pre-SM12). Releases gate the next
      // PV exactly as before; G0 handshakes via sm12_osync only on rescale
      // tiles (lazy-chain common case: scale==1 -> w15 releases solo).
      CUTLASS_PRAGMA_NO_UNROLL
      for (; tile_scheduler.is_valid(); ++tile_scheduler) {
        auto blk_coord = tile_scheduler.get_block_coord();
        auto logical_problem_shape = apply_batch(params,
            params.problem_shape, get<2,1>(blk_coord));
        if ((get<0>(blk_coord) - (int)cute::block_rank_in_cluster()) * get<0>(TileShape{}) >= get<0>(logical_problem_shape)) {
          continue;
        }
        if (get<1>(logical_problem_shape) == 0) {
          continue;
        }
        mainloop.sm12_owaiter(
          blk_coord, params.mainloop, logical_problem_shape,
          pipeline_mma_corr, pipeline_mma_corr_consumer_state,
          shared_storage.pipelines.sm12_osync);
      }
#else
      /* no-op, donate regs and exit */
#endif
    }

    // [2SM FIX] cluster-wide EXIT barrier. In a 2-CTA cluster the CTAs must not
    // exit at different times: when one CTA finishes and exits, its smem (which
    // hosts cluster-scoped pipeline mbarriers) becomes invalid, and the peer's
    // still-pending cluster ops — umma_arrive_2x1SM_sm0 (consumer_release to sm0)
    // and the multicast producer_commit — then target an absent block, raising
    // CUDA_EXCEPTION_17 "Cluster target block not present" on short shapes (n=1
    // crashes) or deadlocking (even n hangs). cuda-gdb on s=128 caught exactly
    // this. Holding every CTA at a cluster_arrive/wait before any warp exits keeps
    // the early-finishing CTA alive so the peer's cluster ops complete, then both
    // leave together. (1-SM: size(ClusterShape)==1 → skipped, unchanged.)
    if constexpr (cute::size(ClusterShape{}) > 1) {
#ifdef MXFP8_2SM_EXITDB
      // [刀18 E1] all-warp exit cluster barrier REMOVED — pair coherence at exit is
      // carried by the epilogue-warp tmem_dealloc handshake above (oyhj form): the
      // CTA cannot exit while its epilogue warp is in the handshake, so the peer's
      // cluster-scoped ops still find this CTA's smem barriers resident.
#else
#ifdef MXFP8_EXITTS
      // [T2] stamp this warp's arrival at the exit barrier (cluster 0 only)
      if (blockIdx.x < 2 && blockIdx.y == 0 && blockIdx.z == 0 && cute::elect_one_sync()) {
        unsigned long long t; asm volatile("mov.u64 %0, %%globaltimer;" : "=l"(t));
        g_exit_ts[cute::block_rank_in_cluster() * 32 + (threadIdx.x >> 5)] = t;
      }
#endif
      cute::cluster_arrive();
      cute::cluster_wait();
#endif
#ifdef MXFP8_EXITTS
      if (blockIdx.x < 2 && blockIdx.y == 0 && blockIdx.z == 0 &&
          (threadIdx.x >> 5) == 0 && cute::elect_one_sync()) {
        unsigned long long t; asm volatile("mov.u64 %0, %%globaltimer;" : "=l"(t));
        g_exit_ts[62 + cute::block_rank_in_cluster()] = t;
      }
#endif
    }
#endif
  }

};

}  // namespace cutlass::fmha::kernel
