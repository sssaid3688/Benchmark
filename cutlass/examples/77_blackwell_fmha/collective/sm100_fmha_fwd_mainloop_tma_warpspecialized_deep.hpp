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

#include "cutlass/cutlass.h"
#include "cutlass/arch/memory_sm80.h"
#include "cutlass/gemm/collective/collective_builder.hpp"
#include "cutlass/detail/sm100_tmem_helper.hpp"
#include "cute/arch/simd_sm100.hpp"
#include "cute/tensor.hpp"
#include "cute/layout.hpp"

#include "collective/fmha_common.hpp"
#include "collective/fmha_fusion.hpp"
#include "collective/sm100_fmha_load_tma_warpspecialized.hpp"

#include "cute/util/print.hpp"
// #include "cute/util/print_layout.hpp"
#include "cute/util/print_tensor.hpp"
namespace cutlass::fmha::collective {

using namespace cute;

template<
  class Element_,
  class ElementQK_,
  class ElementPV_,
  class TileShape_,
  class StrideQ_,
  class StrideK_,
  class StrideV_,
  class Mask_,
  class ThreadShape = Shape<_2, _1, _1>,
  class OrderLoadEpilogue = cute::false_type
>
struct Sm100FmhaFwdMainloopTmaWarpspecialized {

  using Element = Element_;
  using ElementQK = ElementQK_;
  using ElementPV = ElementPV_;
  using TileShape = TileShape_;
  using StrideQ = StrideQ_;
  using StrideK = StrideK_;
  using StrideV = StrideV_;
  using Mask = Mask_;

  static constexpr int StageCountQ = 2;
  static constexpr int StageCountKV = sizeof(Element_) == 1 ? 4 : 3;

  using StagesQ = cutlass::gemm::collective::StageCount<StageCountQ>;
  using StagesKV = cutlass::gemm::collective::StageCount<StageCountKV>;
  using ElementData = typename Element::DataType;
  using ElementScale = typename Element::ScaleFactorType;
  using ClusterShape = Shape<_1, _1, _1>;

  static const int Alignment = 128 / sizeof_bits_v<Element>;

  using TileShapeQK = decltype(shape_div(TileShape{}, ThreadShape{}));

  using TileShapePV = decltype(select<0,2,1>(TileShapeQK{}));
  // using MmaTileShape        = Shape<_256,_256,_256>;

  using CollectiveMmaQK = typename cutlass::gemm::collective::CollectiveBuilder<
      cutlass::arch::Sm100, cutlass::arch::OpClassBlockScaledTensorOp,
      Element, StrideQ, Alignment,
      Element, StrideK, Alignment,
      ElementQK,
      TileShapeQK, ClusterShape, cutlass::gemm::collective::StageCount<3> /* we change it later anyways*/,
      cutlass::gemm::collective::KernelScheduleAuto>::CollectiveOp;

  using CollectiveMmaPV = typename cutlass::gemm::collective::CollectiveBuilder<
      cutlass::arch::Sm100, cutlass::arch::OpClassBlockScaledTensorOp,
      Element, StrideK, Alignment,
      Element, decltype(select<1,0,2>(StrideV{})), Alignment,
      ElementPV,
      TileShapePV, ClusterShape, cutlass::gemm::collective::StageCount<3> /* we change it later anyways*/,
      cutlass::gemm::collective::KernelScheduleAuto>::CollectiveOp;

  using SmemLayoutQ = decltype(unstageSmemLayout(typename CollectiveMmaQK::SmemLayoutA{}, Int<StageCountQ>{}));
  using SmemLayoutK = decltype(unstageSmemLayout(typename CollectiveMmaQK::SmemLayoutB{}, Int<StageCountKV>{}));
  using SmemLayoutP = decltype(unstageSmemLayout(typename CollectiveMmaPV::SmemLayoutA{}, Int<StageCountQ>{}));
  using SmemLayoutV = decltype(unstageSmemLayout(typename CollectiveMmaPV::SmemLayoutB{}, Int<StageCountKV>{}));

  using SmemLayoutSFQ = decltype(unstageSmemLayout(typename CollectiveMmaQK::SmemLayoutSFA{}, Int<StageCountQ>{}));
  using SmemLayoutSFK = decltype(unstageSmemLayout(typename CollectiveMmaQK::SmemLayoutSFB{}, Int<StageCountKV>{}));
  using SmemLayoutSFP = decltype(unstageSmemLayout(typename CollectiveMmaPV::SmemLayoutSFA{}, Int<StageCountQ>{}));
  using SmemLayoutSFV = decltype(unstageSmemLayout(typename CollectiveMmaPV::SmemLayoutSFB{}, Int<StageCountKV>{}));
  
  // SF TMEM fragment types
  using FrgTypeSFA_QK = typename CollectiveMmaQK::TiledMma::FrgTypeSFA;
  using FrgTypeSFB_QK = typename CollectiveMmaQK::TiledMma::FrgTypeSFB;
  using FrgTypeSFA_PV = typename CollectiveMmaPV::TiledMma::FrgTypeSFA;
  using FrgTypeSFB_PV = typename CollectiveMmaPV::TiledMma::FrgTypeSFB;
// layout T:tmem_[8b](0x0000.0180) o (((_32,_4,(_4,_4)),_1),_1,_1,_2):(((_262144,_1,(_4,_8388608)),_0),_0,_0,_16) BLOCK_SCALE_ORI
// layout S:UMMA::DescriptorIterator o (((_32,_1,_1,_4),_1),_1,_1,_2,_1):(((_1,_1,_1,_0),_0),_0,_0,_32,_64)

// layout 1T:tmem_[8b](0x0000.0100) o (((_32,_4,(_4,_4)),_1),_1,_1,_1):(((_262144,_1,(_4,_8388608)),_0),_0,_0,_0)
// layout 2S:UMMA::DescriptorIterator o (((_32,_1,_1,_4),_1),_1,_1,_1,_2):(((_1,_1,_1,_0),_0),_0,_0,_0,_32)

// layout T:tmem_[8b](0x0000.0002) o (((_32,_4,(_4,_4)),_1),_1,_1,_1):(((_262144,_1,(_4,_8388608)),_0),_0,_0,_0)
// layout S:UMMA::DescriptorIterator o (((_32,_1,_1,_4),_1),_1,_1,_1,_2):(((_1,_1,_1,_0),_0),_0,_0,_0,_32)
  // SF SMEM atom layouts (without staging)
  using SmemLayoutAtomSFA_QK = typename CollectiveMmaQK::SmemLayoutAtomSFA;
  using SmemLayoutAtomSFB_QK = typename CollectiveMmaQK::SmemLayoutAtomSFB;
  using SmemLayoutAtomSFA_PV = typename CollectiveMmaPV::SmemLayoutAtomSFA;
  using SmemLayoutAtomSFB_PV = typename CollectiveMmaPV::SmemLayoutAtomSFB;


  using SmemLayoutSFQ_Staged = decltype(make_layout(    //这个用来在Load处初始化SFQ的layout
      append(shape(SmemLayoutAtomSFA_QK{}), Int<StageCountQ>{}),
      append(stride(SmemLayoutAtomSFA_QK{}), size(filter_zeros(SmemLayoutAtomSFA_QK{})))
  ));
  using SmemLayoutSFK_Staged = decltype(make_layout(
      append(shape(SmemLayoutAtomSFB_QK{}), Int<StageCountKV>{}),
      append(stride(SmemLayoutAtomSFB_QK{}), size(filter_zeros(SmemLayoutAtomSFB_QK{})))
  ));
  using SmemLayoutSFP_Staged = decltype(make_layout(
      append(shape(SmemLayoutAtomSFA_PV{}), Int<StageCountQ>{}),
      append(stride(SmemLayoutAtomSFA_PV{}), size(filter_zeros(SmemLayoutAtomSFA_PV{})))
  ));
  using SmemLayoutSFV_Staged = decltype(make_layout(
      append(shape(SmemLayoutAtomSFB_PV{}), Int<StageCountKV>{}),
      append(stride(SmemLayoutAtomSFB_PV{}), size(filter_zeros(SmemLayoutAtomSFB_PV{})))
  ));
  
  // Reuse shared memory for V and O.
  static constexpr bool IsOrderLoadEpilogue = std::is_same_v<OrderLoadEpilogue, cute::true_type>;
  struct TensorStorage {
    cute::array_aligned<ElementData, cute::cosize_v<SmemLayoutQ>> smem_q;
    union {
      cute::array_aligned<ElementData, cute::cosize_v<SmemLayoutK>> smem_k;
      cute::array_aligned<ElementData, cute::cosize_v<SmemLayoutV>> smem_v;
    };
    cute::array_aligned<ElementData, cute::cosize_v<SmemLayoutP>> smem_p;

    cute::array_aligned<ElementScale, cute::cosize_v<SmemLayoutSFQ_Staged>> smem_sfq;
    union {
      cute::array_aligned<ElementScale, cute::cosize_v<SmemLayoutSFK_Staged>> smem_sfk;
      cute::array_aligned<ElementScale, cute::cosize_v<SmemLayoutSFV_Staged>> smem_sfv;
    };
    cute::array_aligned<ElementScale, cute::cosize_v<SmemLayoutSFP_Staged>> smem_sfp;
    // P data SMEM (MXFP8 E4M3, written by softmax, consumed by PV MMA)
  };

  enum class TmemAllocation : uint32_t {
    kSizeS = 128,
    kSizeO = 128,
    kSizeP = 32,
    kSizeP1 = 16,
    kSizeP2 = 64,
    S0 = 0,
    S1 = S0 + kSizeS,
    V0 = S0,
    V1 = S1,
    // P0 = S0 + kSizeP,
    // P1 = S1 + kSizeP,
    O0 = S1 + kSizeS,
    O1 = O0 + kSizeO,
    // SFQK ping-pong on S0/S1: when QK MMA writes to S0, SFQK must be on S1 (vice versa)
    // to avoid hardware conflict between accumulator writes and SF reads in same S half.
    // SFPV shares the same S0/S1 offsets (QK and PV phases are non-overlapping in time).
    SFQK0 = S1 + kSizeP,   // S1+32=160, used with Q1*K -> S0
    SFQK1 = S0 + kSizeP,   // S0+32=32,  used with Q2*K -> S1
    SFPV0 = S0 + kSizeP,   // S0+32=32,  same as SFQK1 (PV phase)
    SFPV1 = S1 + kSizeP,   // S1+32=160, same as SFQK0 (PV phase)


    kSizeSF = 8,  // 8 columns for (SFQ + SFK) or (SFP + SFV) per half
    kEnd = O1 + kSizeO
  };

  // indices for V0 / V1
  enum : int {
    kIdxOldRowMax = 0,
    kIdxNewRowMax = 1,
    kIdxFinalRowSum = 0,
    kIdxFinalRowMax = 1
  };

  // from load to mma warp, protects q in smem
  using PipelineQ = cutlass::PipelineTmaUmmaAsync<
    StageCountQ,
    typename CollectiveMmaQK::AtomThrShapeMNK
  >;

  // from load to mma warp, protects k/v in smem
  using PipelineKV = cutlass::PipelineTmaUmmaAsync<
    StageCountKV,
    typename CollectiveMmaQK::AtomThrShapeMNK
  >;

  using PipelineSFP = cutlass::PipelineTmaUmmaAsync<
    StageCountQ,  // /2
    typename CollectiveMmaQK::AtomThrShapeMNK
  >;


  // from mma to softmax0/1 warp, protects S in tmem
  using PipelineS = cutlass::PipelineUmmaAsync<1>;

  // from softmax0/1/ to correction wg
  using PipelineC = cutlass::PipelineAsync<1>;

  // from mma to correction
  using PipelineO = cutlass::PipelineUmmaAsync<2>;

  // from corr to epilogue
  using PipelineE = cutlass::PipelineAsync<2>;

  // from correction to mma: signals that O0/O1 buffer is free for QK SF reuse
  using PipelineO_SF = cutlass::PipelineAsync<2>;

  using OrderBarrierSoftmax = cutlass::OrderedSequenceBarrier<
    /*stages*/ 1, /*groups*/ 2>;

  static const int TransactionBytesLoadQ = cutlass::bits_to_bytes(
    cosize(take<0,3>(SmemLayoutQ{})) * cute::sizeof_bits_v<ElementData>+
      cosize(take<0,3>(SmemLayoutSFQ_Staged{})) * cute::sizeof_bits_v<ElementScale>);

  static const int TransactionBytesLoadK = cutlass::bits_to_bytes(
    cosize(take<0,3>(SmemLayoutK{})) * cute::sizeof_bits_v<ElementData>+
      cosize(take<0,3>(SmemLayoutSFK_Staged{})) * cute::sizeof_bits_v<ElementScale>);
      
  static const int TransactionBytesLoadV = cutlass::bits_to_bytes(
    cosize(take<0,3>(SmemLayoutV{})) * cute::sizeof_bits_v<ElementData>+
      cosize(take<0,3>(SmemLayoutSFV_Staged{})) * cute::sizeof_bits_v<ElementScale>);

  static const int TransactionBytesLoadSFP = cutlass::bits_to_bytes(
      cosize(take<0,3>(SmemLayoutSFP_Staged{})) * cute::sizeof_bits_v<ElementScale>);

  // static_assert((TransactionBytesLoadK + cosize(take<0,3>(SmemLayoutSFP{})) * cute::sizeof_bits_v<ElementScale>)
  // == TransactionBytesLoadV, "K and V smem layouts must be of equal size");

  using Load = Sm100FmhaLoadTmaWarpspecialized<
    Element, StrideQ, StrideK, StrideV,
    CollectiveMmaQK, CollectiveMmaPV,
    SmemLayoutQ, SmemLayoutK, SmemLayoutV,
    TensorStorage, PipelineQ, PipelineKV, PipelineSFP, Mask, TileShape,
    SmemLayoutSFQ_Staged, SmemLayoutSFK_Staged, SmemLayoutSFP_Staged, SmemLayoutSFV_Staged
  >;

  struct Arguments {
    typename Load::Arguments load;

    float scale_softmax = 0.0f;

    float scale_q = 1.0f;
    float scale_k = 1.0f;
    float scale_v = 1.0f;

    float inv_scale_o = 1.0f;
  };

  struct Params {
    typename Load::Params load;

    float scale_softmax;
    float scale_softmax_log2;

    float scale_output;
  };

  template<class ProblemShape>
  static bool can_implement(ProblemShape const& problem_shape, Arguments const& args) {
    return true;
  }

  template<class ProblemShape>
  static Params to_underlying_arguments(
      ProblemShape const& problem_shape,
      Arguments const& args,
      void* workspace) {

    float scale_softmax = args.scale_softmax;
    if (scale_softmax == 0.0f) {
      scale_softmax = 1.0f / (float) std::sqrt(get<2>(problem_shape));
    }
    float log2_e = static_cast<float>(std::log2(std::exp(1.0)));

    return Params{
        Load::to_underlying_arguments(problem_shape, args.load, workspace),
        args.scale_q * args.scale_k * scale_softmax,
        args.scale_q * args.scale_k * log2_e * scale_softmax,
        args.scale_v * args.inv_scale_o
    };
  }

  CUTLASS_DEVICE
  static void prefetch_tma_descriptors(Params const& params) {
      Load::prefetch_tma_descriptors(params.load);
  }

  template<class BlkCoord, class ProblemShape, class ParamsProblemShape>
  CUTLASS_DEVICE void
  load(
      BlkCoord const& blk_coord, ProblemShape const& problem_shape,
      Params const& params, ParamsProblemShape const& params_problem_shape,
      TensorStorage& storage,
      PipelineQ& pipeline_q, typename PipelineQ::PipelineState& pipeline_q_producer_state,
      PipelineKV& pipeline_kv, typename PipelineKV::PipelineState& pipeline_kv_producer_state,
      PipelineSFP& pipeline_sfp, typename PipelineSFP::PipelineState& pipeline_sfp_producer_state) {
      uint32_t cta_rank_in_cluster = cute::block_rank_in_cluster();

    Load load;
    load.load(blk_coord, problem_shape, params.load, params_problem_shape,
        storage,
        pipeline_q, pipeline_q_producer_state,
        pipeline_kv, pipeline_kv_producer_state,
        pipeline_sfp, pipeline_sfp_producer_state);
  }

  // Helper: set up SMEM→TMEM UTCCP copy infrastructure for a single SF tensor
  // FrgType: TMEM fragment type (e.g., FrgTypeSFA_QK)
  // SmemAtomLayout: SMEM atom layout without staging (e.g., SmemLayoutAtomSFA_QK)
  // SmemLayoutFull: SMEM layout with staging (e.g., SmemLayoutSFQ)
  template<class FrgType, class SmemAtomLayout, class SmemLayoutFull, class TiledMma>
  CUTLASS_DEVICE auto
  setup_sf_utccp_copy(
      ElementScale* smem_ptr,
      uint32_t tmem_offset) {
    // Create SMEM SF tensor with full staging layout
    Tensor sSF = make_tensor(make_smem_ptr(smem_ptr), SmemLayoutFull{});
    auto sSF_compact = make_tensor(sSF.data(), filter_zeros(sSF.layout()));

    // Create TMEM SF tensor using the atom layout shape (no staging)
    Tensor tSF = make_tensor<FrgType>(shape(SmemAtomLayout{}));
    tSF.data() = tSF.data().get() + tmem_offset;
    // Compact both tensors (remove zero strides, preserves staging in SMEM)
    auto tSF_compact = make_tensor(tSF.data(), filter_zeros(tSF.layout()));

    // Determine UTCCP op based on CTA count (1CTA vs 2CTA)
    using AtomThrID = typename TiledMma::AtomThrID;
    using UtccpOp = cute::conditional_t<(decltype(cute::size(AtomThrID{}) == Int<2>{})::value),
        SM100_UTCCP_4x32dp128bit_2cta, SM100_UTCCP_4x32dp128bit_1cta>;
// int thread_idx =  % (4 * cutlass::NumThreadsPerWarp);
    auto tiled_copy_s2t = make_utccp_copy(UtccpOp{}, tSF_compact);
    auto thr_copy_s2t = tiled_copy_s2t.get_slice(0);
    auto thr_sSF_s2t_ = thr_copy_s2t.partition_S(sSF_compact);
    auto thr_sSF_s2t = get_utccp_smem_desc_tensor<UtccpOp>(thr_sSF_s2t_);
    auto thr_tSF_s2t = thr_copy_s2t.partition_D(tSF_compact);

    return cute::make_tuple(tiled_copy_s2t, thr_sSF_s2t, thr_tSF_s2t, tSF);
  }


  template<class BlkCoord, class ProblemShape>
  CUTLASS_DEVICE auto
  mma(
      BlkCoord const& blk_coord,
      Params const& params, ProblemShape const& problem_shape,
      TensorStorage& storage,
      PipelineQ& pipeline_q, typename PipelineQ::PipelineState& pipeline_q_consumer_state,
      PipelineKV& pipeline_kv, typename PipelineKV::PipelineState& pipeline_kv_consumer_state,
      PipelineSFP& pipeline_sfp, typename PipelineSFP::PipelineState& pipeline_sfp_consumer_state,
      PipelineS& pipeline_s0, typename PipelineS::PipelineState& pipeline_s0_producer_state,  //对应softmax中的pipeline_s_consumer_state
      PipelineS& pipeline_s1, typename PipelineS::PipelineState& pipeline_s1_producer_state,
      PipelineO& pipeline_corr, typename PipelineO::PipelineState& pipeline_corr_producer_state,
      PipelineO_SF& pipeline_o_sf, typename PipelineO_SF::PipelineState& pipeline_o_sf_consumer_state) {//对应softmax中的pipeline_c_producer_state

    auto pipeline_q_release_state = pipeline_q_consumer_state;
    auto pipeline_kv_release_state = pipeline_kv_consumer_state;
    auto pipeline_sfp_release_state = pipeline_sfp_consumer_state;
    // auto pipeline_o_sf_release_state = pipeline_o_sf_consumer_state;
    // ===================================================================
    // DEBUG: Comprehensive SFQ layout comparison
    // ===================================================================
    if(blockIdx.x==0&&blockIdx.y==0&&threadIdx.x==384){
        printf("\n========== SFQ Layout Deep Comparison ==========\n");

        // --- Atom layout (raw, no staging) ---
        printf("--- SmemLayoutAtomSFA_QK (your FMHA) ---\n");
        printf("  shape: "); print(shape(SmemLayoutAtomSFA_QK{})); printf("\n");
        printf("  cosize=%d  size=%d\n",
            int(cute::cosize_v<SmemLayoutAtomSFA_QK>),
            int(cute::size(filter_zeros(SmemLayoutAtomSFA_QK{}))));
        print(SmemLayoutAtomSFA_QK{});

        // --- Staged layout ---
        printf("\n--- SmemLayoutSFQ_Staged (stageCount=%d) ---\n", StageCountQ);
        printf("  cosize=%d  per_stage_stride=%d\n",
            int(cute::cosize_v<SmemLayoutSFQ_Staged>),
            int(cute::size(filter_zeros(SmemLayoutAtomSFA_QK{}))));
        print(SmemLayoutSFQ_Staged{});

        // --- Compacted staged (what UTCCP actually sees) ---
        Tensor sSF_debug = make_tensor(make_smem_ptr(storage.smem_sfq.data()), SmemLayoutSFQ_Staged{});
        auto sSF_compact = make_tensor(sSF_debug.data(), filter_zeros(sSF_debug.layout()));
        printf("\n--- sSF_compact (filter_zeros of staged, UTCCP source) ---\n");
        printf("  cosize=%d\n", int(cute::cosize(sSF_compact.layout())));
        print(sSF_compact);

        // --- TMEM FrgType ---
        printf("\n--- FrgTypeSFA_QK (TMEM, 1CTA) ---\n");
        Tensor tSF_debug = make_tensor<FrgTypeSFA_QK>(shape(SmemLayoutAtomSFA_QK{}));
        printf("  shape: "); print(shape(tSF_debug)); printf("\n");
        printf("  cosize(uint32): %d cols\n",
            int(cute::cosize(recast<uint32_t>(tSF_debug).layout()) & 0xFFFF));
        print(tSF_debug);

        // --- UTCCP copy traits ---
        using AtomThrID_QK = typename CollectiveMmaQK::TiledMma::AtomThrID;
        using UtccpOp_QK = cute::conditional_t<(decltype(cute::size(AtomThrID_QK{}) == Int<2>{})::value),
            SM100_UTCCP_4x32dp128bit_2cta, SM100_UTCCP_4x32dp128bit_1cta>;
        printf("\n--- UTCCP Op (CTA_count=%d) SrcLayout ---\n",
            int(cute::size(AtomThrID_QK{})));
        print(typename Copy_Traits<UtccpOp_QK>::SrcLayout{});

        // --- SMEM byte offsets for key rows (stage 0, rough estimate) ---
        printf("\n--- Per-row SMEM SFQ byte offset (stage 0, row in [0,31]) ---\n");
        auto* sfq_raw_debug = reinterpret_cast<uint8_t const*>(storage.smem_sfq.begin());
        // Print raw dump: first 16 rows, 4 bytes each, to see the actual byte layout
        printf("  Raw bytes (row*4 SFQ):\n");
        for (int r = 0; r < 16; r++) {
            // Approximate: the compact layout has rows separated by stride pattern
            int offset = r * 16;  // row stride _16 from sSF_compact mode 0_0_0
            printf("  row%2d @%3d: ", r, offset);
            for (int b = 0; b < 4; b++) {
                printf("%02x ", sfq_raw_debug[offset + b * 4]);  // K-group stride _4
            }
            printf("| ");
            for (int b = 0; b < 4; b++) {
                printf("%.2f ", (float)cutlass::float_ue8m0_t(sfq_raw_debug[offset + b * 4]));
            }
            printf("\n");
        }

        printf("==========================================================\n\n");
    }
    int mask_tile_count = Mask{}.get_trip_count(blk_coord, TileShape{}, problem_shape);  //problemshape: (2048,2048,128), tileshape: (256,128,128)
    // if (blockIdx.x == 1 && blockIdx.y == 7 && threadIdx.x==384) {
    //   printf("\ntileshape: ");
    //   print(TileShape{});
    //   printf("problem_shape: ");
    //   print(problem_shape);
    //   printf("\n");
    // }

    typename CollectiveMmaQK::TiledMma mma_qk;
    ThrMMA thr_mma_qk = mma_qk.get_slice(0);

    typename CollectiveMmaPV::TiledMma mma_pv;
    ThrMMA thr_mma_pv = mma_pv.get_slice(0);

    Tensor sQ = make_tensor(make_smem_ptr(storage.smem_q.data()), SmemLayoutQ{});
    Tensor sK = make_tensor(make_smem_ptr(storage.smem_k.data()), SmemLayoutK{});
    Tensor sV = make_tensor(make_smem_ptr(storage.smem_v.data()), SmemLayoutV{});


    Tensor tSrQ = thr_mma_qk.make_fragment_A(sQ);
    Tensor tSrK = thr_mma_qk.make_fragment_B(sK);
    Tensor tOrV = thr_mma_pv.make_fragment_B(sV);

    // ============================================================
    // -TCCP copies
    // ============================================================
    // TMEM layout:
    //   S0: cols   0-127  (QK MMA accumulator, S tile 0; V stats 0; SFPV0/SFQK1 SF)
    //   S1: cols 128-255  (QK MMA accumulator, S tile 1; V stats 1; SFPV1/SFQK0 SF)
    //   O0: cols 256-383  (PV MMA accumulator, O tile 0 — preserved for correction)
    //   O1: cols 384-511  (PV MMA accumulator, O tile 1 — preserved for correction)
    //
    // SFQK ping-pong: when QK writes S0, SFQK placed on S1 (vice versa) to avoid conflict.
    // SFPV and SFQK share the same S region offsets (QK/PV phases are time-disjoint).
    uint32_t sf_tmem_base_qk0 = uint32_t(TmemAllocation::SFQK0);   // S1+32=160, used with Q1*K -> S0
    uint32_t sf_tmem_base_qk1 = uint32_t(TmemAllocation::SFQK1);   // S0+32=32,  used with Q2*K -> S1
    uint32_t sf_tmem_base_pv0 = uint32_t(TmemAllocation::SFPV0);   // S0+32=32,  same as SFQK1 (PV phase)
    uint32_t sf_tmem_base_pv1 = uint32_t(TmemAllocation::SFPV1);   // S1+32=160, same as SFQK0 (PV phase)


    // Set up UTCCP copies and TMEM SF tensors for QK MMA (SF placed in S region, ping-pong on S0/S1)
    uint32_t sfq0_tmem_offset = sf_tmem_base_qk0;
    uint32_t sfk0_tmem_offset = sf_tmem_base_qk0 + 16;  // 8 columns gap between SFQ and SFK

    uint32_t sfq1_tmem_offset = sf_tmem_base_qk1;
    uint32_t sfk1_tmem_offset = sf_tmem_base_qk1 + 16;  // 8 columns gap between SFQ and SFK

    auto [tiled_copy_s2t_SFQ0, thr_sSFQ0_s2t, thr_tSFQ0_s2t, tCtSFQ0] =
        setup_sf_utccp_copy<FrgTypeSFA_QK, SmemLayoutAtomSFA_QK, SmemLayoutSFQ_Staged, typename CollectiveMmaQK::TiledMma>(
            storage.smem_sfq.begin(), sfq0_tmem_offset);
    // if(blockIdx.x == 0 && blockIdx.y == 0 && threadIdx.x == 384) {
    //     printf("layout thr_sSFQ_s2t:\n");
    //     print(thr_sSFQ0_s2t);
    //     printf("layout thr_tSFQ0_s2t:\n");
    //     print(thr_tSFQ0_s2t);
    //     printf("\n");
    // }
    auto [tiled_copy_s2t_SFQ1, thr_sSFQ1_s2t, thr_tSFQ1_s2t, tCtSFQ1] =
        setup_sf_utccp_copy<FrgTypeSFA_QK, SmemLayoutAtomSFA_QK, SmemLayoutSFQ_Staged, typename CollectiveMmaQK::TiledMma>(
            storage.smem_sfq.begin(), sfq1_tmem_offset);
    // if(blockIdx.x == 0 && blockIdx.y == 0 && threadIdx.x==384){
    //   printf("layout thr_sSFQ_s2t:\n");
    //   print(thr_sSFQ0_s2t);
    //   printf("\nlayout thr_tSFQ_s2t");
    //   print(thr_tSFQ0_s2t);
    //   print("\n");
    // }
    auto [tiled_copy_s2t_SFK0, thr_sSFK0_s2t, thr_tSFK0_s2t, tCtSFK0] =
        setup_sf_utccp_copy<FrgTypeSFB_QK, SmemLayoutAtomSFB_QK, SmemLayoutSFK_Staged, typename CollectiveMmaQK::TiledMma>(
            storage.smem_sfk.begin(), sfk0_tmem_offset);

    auto [tiled_copy_s2t_SFK1, thr_sSFK1_s2t, thr_tSFK1_s2t, tCtSFK1] =
        setup_sf_utccp_copy<FrgTypeSFB_QK, SmemLayoutAtomSFB_QK, SmemLayoutSFK_Staged, typename CollectiveMmaQK::TiledMma>(
            storage.smem_sfk.begin(), sfk1_tmem_offset);
    // Set up UTCCP copies and TMEM SF tensors for PV MMA (SF placed in S region gap)
    uint32_t sfp0_tmem_offset = sf_tmem_base_pv0;
    uint32_t sfv0_tmem_offset = sf_tmem_base_pv0 + 16;

    uint32_t sfp1_tmem_offset = sf_tmem_base_pv1;
    uint32_t sfv1_tmem_offset = sf_tmem_base_pv1 + 16;

    auto [tiled_copy_s2t_SFP0, thr_sSFP0_s2t, thr_tSFP0_s2t, tCtSFP0] =
        setup_sf_utccp_copy<FrgTypeSFA_PV, SmemLayoutAtomSFA_PV, SmemLayoutSFP_Staged, typename CollectiveMmaPV::TiledMma>(
            storage.smem_sfp.begin(), sfp0_tmem_offset);

    auto [tiled_copy_s2t_SFP1, thr_sSFP1_s2t, thr_tSFP1_s2t, tCtSFP1] =
        setup_sf_utccp_copy<FrgTypeSFA_PV, SmemLayoutAtomSFA_PV, SmemLayoutSFP_Staged, typename CollectiveMmaPV::TiledMma>(
            storage.smem_sfp.begin(), sfp1_tmem_offset);

    auto [tiled_copy_s2t_SFV0, thr_sSFV0_s2t, thr_tSFV0_s2t, tCtSFV0] =
        setup_sf_utccp_copy<FrgTypeSFB_PV, SmemLayoutAtomSFB_PV, SmemLayoutSFV_Staged, typename CollectiveMmaPV::TiledMma>(
            storage.smem_sfv.begin(), sfv0_tmem_offset);

      auto [tiled_copy_s2t_SFV1, thr_sSFV1_s2t, thr_tSFV1_s2t, tCtSFV1] =
        setup_sf_utccp_copy<FrgTypeSFB_PV, SmemLayoutAtomSFB_PV, SmemLayoutSFV_Staged, typename CollectiveMmaPV::TiledMma>(
            storage.smem_sfv.begin(), sfv1_tmem_offset);
            
    // ===================================================================
    // DEBUG: Print actual TMEM addresses of all SF tensors
    // ===================================================================
    if(blockIdx.x == 0 && blockIdx.y == 0 && threadIdx.x == 384) {
        printf("\n========== SF TMEM Address Dump ==========\n");
        printf("FrgType default = 0x%x (should be 0x180 for 2CTA, 0x100 for 1CTA)\n",
               (unsigned)tCtSFQ0.data().get() - sfq0_tmem_offset);
        printf("sfq0_tmem_offset=%u → SFQ0 addr=0x%x\n", sfq0_tmem_offset, (unsigned)tCtSFQ0.data().get());
        printf("sfk0_tmem_offset=%u → SFK0 addr=0x%x\n", sfk0_tmem_offset, (unsigned)tCtSFK0.data().get());
        printf("sfq1_tmem_offset=%u → SFQ1 addr=0x%x\n", sfq1_tmem_offset, (unsigned)tCtSFQ1.data().get());
        printf("sfk1_tmem_offset=%u → SFK1 addr=0x%x\n", sfk1_tmem_offset, (unsigned)tCtSFK1.data().get());
        printf("sfp0_tmem_offset=%u → SFP0 addr=0x%x\n", sfp0_tmem_offset, (unsigned)tCtSFP0.data().get());
        printf("sfv0_tmem_offset=%u → SFV0 addr=0x%x\n", sfv0_tmem_offset, (unsigned)tCtSFV0.data().get());
        printf("sfp1_tmem_offset=%u → SFP1 addr=0x%x\n", sfp1_tmem_offset, (unsigned)tCtSFP1.data().get());
        printf("sfv1_tmem_offset=%u → SFV1 addr=0x%x\n", sfv1_tmem_offset, (unsigned)tCtSFV1.data().get());
        printf("S0=0x%x S1=0x%x O0=0x%x O1=0x%x\n",
               (unsigned)uint32_t(TmemAllocation::S0),
               (unsigned)uint32_t(TmemAllocation::S1),
               (unsigned)uint32_t(TmemAllocation::O0),
               (unsigned)uint32_t(TmemAllocation::O1));
        printf("==========================================\n\n");
    }

    // ============================================================

    Tensor tStS = partition_fragment_C(mma_qk, select<0,1>(TileShapeQK{}));
    Tensor tOtO = partition_fragment_C(mma_pv, select<0,1>(TileShapePV{}));

    Tensor tStS0 = tStS;
    tStS0.data() = tStS.data().get() + uint32_t(TmemAllocation::S0);
    Tensor tStS1 = tStS;
    tStS1.data() = tStS.data().get() + uint32_t(TmemAllocation::S1);

    Tensor tOtO0 = tOtO;
    tOtO0.data() = tOtO.data().get() + uint32_t(TmemAllocation::O0);
    Tensor tOtO1 = tOtO;
    tOtO1.data() = tOtO.data().get() + uint32_t(TmemAllocation::O1);

    // Tensor sP = make_tensor(make_smem_ptr((Element*)nullptr), typename CollectiveMmaPV::SmemLayoutA{});
    // Tensor tOrP = thr_mma_pv.make_fragment_A(sP)(_, _, _, _0{});

    // Tensor tOrP0 = tOrP;
    // tOrP0.data() = tOrP0.data() + uint32_t(TmemAllocation::P0);
    // Tensor tOrP1 = tOrP;
    // tOrP1.data() = tOrP1.data() + uint32_t(TmemAllocation::P1);
    Tensor sP = make_tensor(make_smem_ptr(storage.smem_p.data()), SmemLayoutP{});
    Tensor tOrP = thr_mma_pv.make_fragment_A(sP);

    // Select stage 0 (P from softmax 0) and stage 1 (P from softmax 1)
    Tensor tOrP0 = tOrP(_, _, _, _0{});
    Tensor tOrP1 = tOrP(_, _, _, _1{});

    int k_index = 0;
    int v_index = 0;
    int q_index = 0;
    int sp_index = 0;

    // ============================================================
    // Initialization: wait for Q1, K1
    // ============================================================

    // wait for Q1
    q_index = pipeline_q_consumer_state.index();
    pipeline_q.consumer_wait(pipeline_q_consumer_state);
    ++pipeline_q_consumer_state;
    
      // if (blockIdx.x == 0 && blockIdx.y == 0 && threadIdx.x == 384 || threadIdx.x == 385) {
      //   constexpr int SFQ_size = cute::cosize_v<SmemLayoutSFQ>;
      //   printf("=== smem_sfq, total elements=%d ===\n", SFQ_size);
      //   for (int i = 0; i < SFQ_size; ++i) {
      //     // if ((float)storage.smem_sfv[i] == 0 || (float)storage.smem_sfv[i] >=11111111) {
      //     // if ((float)storage.smem_sfq[i] == 0) {
      //       printf("| sfq[%d] = %f |", i, (float)storage.smem_sfq[i]);
      //     // }
      //   }
      // }
      // if (blockIdx.x == 0 && blockIdx.y == 0) {
      //   constexpr int SFK_size = cute::cosize_v<SmemLayoutSFK>;
      //   printf("=== smem_sfk, total elements=%d ===\n", SFK_size);
      //   for (int i = 0; i < SFK_size; ++i) {
      //     // if ((float)storage.smem_sfv[i] == 0 || (float)storage.smem_sfv[i] >=11111111) {
      //     if ((float)storage.smem_sfk[i] == 0) {
      //       printf("  sfk[%d] = %f  |", i, (float)storage.smem_sfk[i]);
      //     }
      //   }
      // }
    Tensor tSrQ0 = tSrQ(_,_,_,q_index);
    // if(blockIdx.x==0 && blockIdx.y==0 && threadIdx.x==384){

    // wait for K1
    k_index = pipeline_kv_consumer_state.index();
    pipeline_kv.consumer_wait(pipeline_kv_consumer_state);
    ++pipeline_kv_consumer_state;
    //  if (blockIdx.x == 0 && blockIdx.y == 0 ) {
    //     constexpr int K_size = cute::cosize_v<SmemLayoutK>;
    //     printf("=== smem_K, total elements=%d ===\n", K_size);
    //     for (int i = 0; i < K_size; ++i) {
    //       // if ((float)storage.smem_sfv[i] == 0 || (float)storage.smem_sfv[i] >=11111111) {
    //       if ((float)storage.smem_k[i] == 0) {
    //         printf("  K[%d] = %f  |", i, (float)storage.smem_k[i]);
    //       }
    //     }
    //   }
    
    // ============================================================
    // FIX: SFQ SMEM layout has 4 UTCCP lanes (128-row capacity) but
    // TMA only fills 2 lanes (64 rows per thread-tile). Replicate
    // lanes 0,1 → lanes 2,3 so UTCCP reads valid data at all positions.
    // Byte layout: [L0,L1,L2,L3] interleaved at stride 1 byte.
    // ============================================================
    {
      constexpr int stage_bytes = cute::cosize_v<SmemLayoutAtomSFA_QK>; // 512
      uint8_t* sfq = reinterpret_cast<uint8_t*>(storage.smem_sfq.begin());
      uint8_t* sfk = reinterpret_cast<uint8_t*>(storage.smem_sfk.begin());
      for (int stage = 0; stage < StageCountQ; stage++) {
        uint8_t* sq = sfq + stage * stage_bytes;
        uint8_t* sk = sfk + stage * stage_bytes;
        for (int i = 0; i < stage_bytes; i += 4) {
          sq[i + 2] = sq[i + 0];  // lane 0 → lane 2
          sq[i + 3] = sq[i + 1];  // lane 1 → lane 3
          sk[i + 2] = sk[i + 0];  // same for SFK
          sk[i + 3] = sk[i + 1];
        }
      }
    }
    __syncwarp();  // ensure all lanes see the replicated data

    // ============================================================
    // Copy Q SF and K SF from SMEM to TMEM (UTCCP) for first QK MMA
    // ============================================================
    // printf("opy(tiled_copy_s2t_SFQ ");
    // copy sfqk0 to tmem:O
    // Acquire both S0 (Q1*K output) and S1 (SFQK0 SF data goes to S1+32)
    pipeline_s0.producer_acquire(pipeline_s0_producer_state);
    pipeline_s1.producer_acquire(pipeline_s1_producer_state);

    // ===================================================================
    // DEBUG: Dump SMEM SFQ/SFK raw data to verify TMA loaded correctly.
    // Each stage's start address in SMEM:
    //   stage 0: storage.smem_sfq.begin()
    //   stage 1: storage.smem_sfq.begin() + cosize(SmemLayoutAtomSFA_QK)/2 bytes
    // ===================================================================
    if (blockIdx.x == 0 && blockIdx.y == 0 && threadIdx.x == 384) {
        int sfq_stage_stride = int(cute::cosize_v<SmemLayoutAtomSFA_QK>) / 2;  // uint16_t = 2 bytes
        int sfk_stage_stride = int(cute::cosize_v<SmemLayoutAtomSFB_QK>) / 2;
        auto* sfq_smem = reinterpret_cast<uint16_t const*>(storage.smem_sfq.begin());
        auto* sfk_smem = reinterpret_cast<uint16_t const*>(storage.smem_sfk.begin());

        printf("\n========== SMEM SFQ/SFK Raw Data (q_index=%d, k_index=%d) ==========\n",
               int(q_index), int(k_index));

        printf("--- SFQ stage 0 (first 16 uint16) ---\n  ");
        for (int i = 0; i < 16; i++) printf("%04x ", sfq_smem[i]);
        printf("\n--- SFQ stage 1 (first 16 uint16) ---\n  ");
        for (int i = 0; i < 16; i++) printf("%04x ", sfq_smem[sfq_stage_stride + i]);

        printf("\n--- SFK stage 0 (first 16 uint16) ---\n  ");
        for (int i = 0; i < 16; i++) printf("%04x ", sfk_smem[i]);
        printf("\n--- SFK stage 1 (first 16 uint16) ---\n  ");
        for (int i = 0; i < 16; i++) printf("%04x ", sfk_smem[sfk_stage_stride + i]);

        printf("\n  SFQ stage_stride=%d uint16  SFK stage_stride=%d uint16\n",
               sfq_stage_stride, sfk_stage_stride);
        // Check: data should not be all-zeros (would indicate TMA didn't load)
        bool sfq0_zero = true, sfq1_zero = true, sfk0_zero = true, sfk1_zero = true;
        for (int i = 0; i < 16; i++) {
            if (sfq_smem[i] != 0) sfq0_zero = false;
            if (sfq_smem[sfq_stage_stride + i] != 0) sfq1_zero = false;
            if (sfk_smem[i] != 0) sfk0_zero = false;
            if (sfk_smem[sfk_stage_stride + i] != 0) sfk1_zero = false;
        }
        printf("  SFQ stage0 %s, SFQ stage1 %s\n",
               sfq0_zero ? "ALL ZERO!!" : "has data",
               sfq1_zero ? "ALL ZERO!!" : "has data");
        printf("  SFK stage0 %s, SFK stage1 %s\n",
               sfk0_zero ? "ALL ZERO!!" : "has data",
               sfk1_zero ? "ALL ZERO!!" : "has data");
        printf("============================================================\n\n");
    }

    if (cute::elect_one_sync()) {
      copy(tiled_copy_s2t_SFQ0, thr_sSFQ0_s2t(_,_,_,_,q_index), thr_tSFQ0_s2t);  //Q1 * K1 (SFQ0→S1+32, output→S0)
      copy(tiled_copy_s2t_SFK0, thr_sSFK0_s2t(_,_,_,_,k_index), thr_tSFK0_s2t);
    }
    cutlass::arch::fence_view_async_tmem_store();
    //     // ===== DEBUG: read SFQ back from TMEM (same pattern as softmax) =====
    // {
    //   using TMEM_LOAD_SF = SM100_TMEM_LOAD_32dp32b16x;
    //   Tensor tSFQ_r = recast<uint16_t>(tCtSFQ0);
    //   auto tiled_sf = make_tmem_copy(TMEM_LOAD_SF{}, tSFQ_r);
    //   int sm_idx = threadIdx.x % (4 * NumThreadsPerWarp);
    //   auto thr_sf = tiled_sf.get_slice(sm_idx);
    //   Tensor tSFQ_t = thr_sf.partition_S(tSFQ_r);
    //   Tensor tSFQ_reg = make_tensor<uint16_t>(shape(thr_sf.partition_D(make_identity_tensor(shape(tSFQ_r)))));
    //   copy(tiled_sf, tSFQ_t, tSFQ_reg);
    //   if (blockIdx.x == 0 && blockIdx.y == 0 && sm_idx < 4) {
    //     printf("[TMEM_SFQ] thr=%d first 8: ", sm_idx);
    //     for (int i=0; i<8&&i<size(tSFQ_reg); i++) {
    //       uint8_t lo = tSFQ_reg(i)&0xFF, hi = tSFQ_reg(i)>>8;
    //       printf("%.3f %.3f|", (float)(cutlass::float_ue8m0_t(lo)), (float)(cutlass::float_ue8m0_t(hi)));
    //     }
    //     printf("\n");
    //   }
    // }
    // // =========================================================

    gemm_zero_acc(mma_qk, tSrQ0, tSrK(_,_,_,k_index), tStS0, tCtSFQ0, tCtSFK0);  // Q1*K1→S0, reads SF from S1
    // gemm_zero_acc(mma_qk, tSrQ0, tSrK(_,_,_,k_index), tStS0);  //

    pipeline_s0.producer_commit(pipeline_s0_producer_state);    //commit之后softmax中计算pipeline_s.consumer_wait(pipeline_s_consumer_state);
    ++pipeline_s0_producer_state;

    // release K1
    if constexpr (get<1>(ThreadShape{}) > 1) {
      pipeline_kv.consumer_release(pipeline_kv_release_state);
      ++pipeline_kv_release_state;
    }

    // wait for Q2
    if constexpr (get<0>(ThreadShape{}) > 1 || get<2>(ThreadShape{}) > 1) {
      q_index = pipeline_q_consumer_state.index();
      pipeline_q.consumer_wait(pipeline_q_consumer_state);
      ++pipeline_q_consumer_state;
    }

    Tensor tSrQ1 = tSrQ(_,_,_,q_index);

    if constexpr (get<1>(ThreadShape{}) > 1) {
      k_index = pipeline_kv_consumer_state.index();
      pipeline_kv.consumer_wait(pipeline_kv_consumer_state);
      ++pipeline_kv_consumer_state;
    }

    // ============================================================
    // Copy Q SF and K SF for second QK MMA
    // Q2*K1: SFQK1 goes to S0+32, output goes to S1
    // Need to re-acquire S0 (was committed above, now owned by softmax)
    // S1 is still held from the acquire above (never committed yet)
    // ============================================================
    pipeline_s0.producer_acquire(pipeline_s0_producer_state);  // wait for softmax to release S0

    if (cute::elect_one_sync()) {
      copy(tiled_copy_s2t_SFQ1, thr_sSFQ1_s2t(_,_,_,_,q_index), thr_tSFQ1_s2t);  //Q2 * K1 (SFQ1→S0+32, output→S1)
      copy(tiled_copy_s2t_SFK1, thr_sSFK1_s2t(_,_,_,_,k_index), thr_tSFK1_s2t);
    }
    cutlass::arch::fence_view_async_tmem_store();

    // gemm Q2 * K1 -> S1  (WITH SCALE FACTORS)
    gemm_zero_acc(mma_qk, tSrQ1, tSrK(_,_,_,k_index), tStS1, tCtSFQ1, tCtSFK1);
    // gemm_zero_acc(mma_qk, tSrQ1, tSrK(_,_,_,k_index), tStS1);


    pipeline_s1.producer_commit(pipeline_s1_producer_state);
    ++pipeline_s1_producer_state;

    // release K1
    pipeline_kv.consumer_release(pipeline_kv_release_state);
    ++pipeline_kv_release_state;

    // wait for V1   V，SFV和SFP一起搬
    v_index = pipeline_kv_consumer_state.index();
    pipeline_kv.consumer_wait(pipeline_kv_consumer_state);
    ++pipeline_kv_consumer_state;
    
    // if (blockIdx.x == 0 && blockIdx.y == 0 ) {
    //     constexpr int SFV_size = cute::cosize_v<SmemLayoutSFV>;
    //     printf("=== smem_sfv, total elements=%d ===\n", SFV_size);
    //     for (int i = 0; i < SFV_size; ++i) {
    //       // if ((float)storage.smem_sfv[i] == 0 || (float)storage.smem_sfv[i] >=11111111) {
    //       if ((float)storage.smem_sfv[i] == 0) {
    //         printf("  sfv[%d] = %f  |", i, (float)storage.smem_sfv[i]);
    //       }
    //     }
    //   }

    // if (blockIdx.x == 0 && blockIdx.y == 0 && threadIdx.x==384) {
    //     constexpr int V_size = cute::cosize_v<SmemLayoutV>;
    //     printf("=== smem_sfv, total elements=%d ===\n", V_size);
    //     for (int i = 0; i < V_size; ++i) {
    //       // if ((float)storage.smem_sfv[i] == 0 || (float)storage.smem_sfv[i] >=11111111) {
    //       if ((float)storage.smem_v[i] == 0) {
    //         printf("  v[%d] = %f  |", i, (float)storage.smem_v[i]);
    //       }
    //     }
    //   }

    // wait for SFP1
    sp_index = pipeline_sfp_consumer_state.index();
    pipeline_sfp.consumer_wait(pipeline_sfp_consumer_state);
    ++pipeline_sfp_consumer_state;
    // gemm P1 * V1 -> O1  (WITH SCALE FACTORS)
    // if (blockIdx.x == 0 && blockIdx.y == 0 && threadIdx.x==384) {
    //   printf("=== smem_sfp (sp_index=%d, block=%d,%d) ===\n",
    //          sp_index, blockIdx.x, blockIdx.y);
    //   constexpr int sfp_size = cute::cosize_v<SmemLayoutSFP>;
    //   for (int i = 0; i < sfp_size; ++i) {
    //     printf("  sfp[%d] = %a (%f)\n", i,
    //            (float)storage.smem_sfp[i], (float)storage.smem_sfp[i]);
    //   }
    // }
    //打印sfp看看


    pipeline_corr.producer_acquire(pipeline_corr_producer_state);
    pipeline_s0.producer_acquire(pipeline_s0_producer_state); //等 Softmax0 消费完上一轮 S0 并释放。

    // ===================================================================
    // DEBUG: Dump SMEM SFP/SFV raw data to verify TMA loaded correctly
    // ===================================================================
    if (blockIdx.x == 0 && blockIdx.y == 0 && threadIdx.x == 384) {
        int sfp_stage_stride = int(cute::cosize_v<SmemLayoutAtomSFA_PV>) / 2;
        int sfv_stage_stride = int(cute::cosize_v<SmemLayoutAtomSFB_PV>) / 2;
        auto* sfp_smem = reinterpret_cast<uint16_t const*>(storage.smem_sfp.begin());
        auto* sfv_smem = reinterpret_cast<uint16_t const*>(storage.smem_sfv.begin());

        printf("\n========== SMEM SFP/SFV Raw Data (sp_index=%d, v_index=%d) ==========\n",
               int(sp_index), int(v_index));

        printf("--- SFP stage 0 (first 16 uint16) ---\n  ");
        for (int i = 0; i < 16; i++) printf("%04x ", sfp_smem[i]);
        printf("\n--- SFP stage 1 (first 16 uint16) ---\n  ");
        for (int i = 0; i < 16; i++) printf("%04x ", sfp_smem[sfp_stage_stride + i]);

        printf("\n--- SFV stage 0 (first 16 uint16) ---\n  ");
        for (int i = 0; i < 16; i++) printf("%04x ", sfv_smem[i]);
        printf("\n--- SFV stage 1 (first 16 uint16) ---\n  ");
        for (int i = 0; i < 16; i++) printf("%04x ", sfv_smem[sfv_stage_stride + i]);

        printf("\n  SFP stage_stride=%d uint16  SFV stage_stride=%d uint16\n",
               sfp_stage_stride, sfv_stage_stride);
        bool sfp0_zero = true, sfp1_zero = true, sfv0_zero = true, sfv1_zero = true;
        for (int i = 0; i < 16; i++) {
            if (sfp_smem[i] != 0) sfp0_zero = false;
            if (sfp_smem[sfp_stage_stride + i] != 0) sfp1_zero = false;
            if (sfv_smem[i] != 0) sfv0_zero = false;
            if (sfv_smem[sfv_stage_stride + i] != 0) sfv1_zero = false;
        }
        printf("  SFP stage0 %s, SFP stage1 %s\n",
               sfp0_zero ? "ALL ZERO!!" : "has data",
               sfp1_zero ? "ALL ZERO!!" : "has data");
        printf("  SFV stage0 %s, SFV stage1 %s\n",
               sfv0_zero ? "ALL ZERO!!" : "has data",
               sfv1_zero ? "ALL ZERO!!" : "has data");
        printf("===============================================================\n\n");
    }

    if (cute::elect_one_sync()) {
      copy(tiled_copy_s2t_SFP0, thr_sSFP0_s2t(_,_,_,_,sp_index), thr_tSFP0_s2t);    //释放了S0，SFPV使用S0
      copy(tiled_copy_s2t_SFV0, thr_sSFV0_s2t(_,_,_,_,v_index), thr_tSFV0_s2t);
    }
    cutlass::arch::fence_view_async_tmem_store();

    //P1 * V1 -> O1
    gemm_zero_acc(mma_pv, tOrP0, tOrV(_,_,_,v_index), tOtO0, tCtSFP0, tCtSFV0);  //写O0，此时SFQK的O0已经释放
    // gemm_zero_acc(mma_pv, tOrP0, tOrV(_,_,_,v_index), tOtO0);


    pipeline_corr.producer_commit(pipeline_corr_producer_state);    //计算完O后去矫正
    ++pipeline_corr_producer_state;

    pipeline_sfp.consumer_release(pipeline_sfp_release_state);
    ++pipeline_sfp_release_state;

    if constexpr (get<1>(ThreadShape{}) > 1) {
      pipeline_kv.consumer_release(pipeline_kv_release_state);
      ++pipeline_kv_release_state;
    }

    mma_pv.accumulate_ = UMMA::ScaleOut::Zero;


    // pipeline_o_sf.consumer_release(pipeline_o_sf_release_state);
    mask_tile_count -= 1;
    for (; mask_tile_count > 0; mask_tile_count -= 1) {
         // P2计算之前，SFP2要搬运进SMEM
      // gemm P2 * V(i-1) -> O2    P2*V1
      if constexpr (get<1>(ThreadShape{}) > 1) {
        v_index = pipeline_kv_consumer_state.index();
        pipeline_kv.consumer_wait(pipeline_kv_consumer_state);
        ++pipeline_kv_consumer_state;
      }

      pipeline_corr.producer_acquire(pipeline_corr_producer_state);
      pipeline_s1.producer_acquire(pipeline_s1_producer_state);
      // printf("im waite\n");

      sp_index = pipeline_sfp_consumer_state.index();       //PV，V用上次的，P需要搬运
      pipeline_sfp.consumer_wait(pipeline_sfp_consumer_state);
      ++pipeline_sfp_consumer_state;

      if (cute::elect_one_sync()) {  //wait SFP2的搬运
          copy(tiled_copy_s2t_SFP1, thr_sSFP1_s2t(_,_,_,_,sp_index), thr_tSFP1_s2t);    //增加一个sfp2的搬运
          copy(tiled_copy_s2t_SFV1, thr_sSFV1_s2t(_,_,_,_,v_index), thr_tSFV1_s2t);
        // }
      }

      cutlass::arch::fence_view_async_tmem_store();

      // gemm P2 * V(i-1)(hc_index=1时v1, =2时v2) -> O2  (WITH SCALE FACTORS)
      // gemm P2 * V(i-1) -> O2
      gemm_reset_zero_acc(mma_pv, tOrP1, tOrV(_,_,_,v_index), tOtO1, tCtSFP1, tCtSFV1);


      pipeline_corr.producer_commit(pipeline_corr_producer_state);
      ++pipeline_corr_producer_state;

      // release sfp
      pipeline_sfp.consumer_release(pipeline_sfp_release_state);
      ++pipeline_sfp_release_state;

      // release V(i-1)
      pipeline_kv.consumer_release(pipeline_kv_release_state);
      ++pipeline_kv_release_state;

      // pipeline_sfp.consumer_release(pipeline_sfp_release_state);
      // ++pipeline_sfp_release_state;

      // wait for Ki
      k_index = (pipeline_kv_consumer_state.index());
      pipeline_kv.consumer_wait(pipeline_kv_consumer_state);
      ++pipeline_kv_consumer_state;


      // //pipeline_corr_acquire()计算完correction，获取O的区域    这里要等O0correction 消费完，需要acquire?  跟correction中的同步
      // pipeline_o_sf.consumer_wait(pipeline_o_sf_consumer_state);
      // ++pipeline_o_sf_consumer_state;
      
      // pipeline_o_sf.consumer_wait(pipeline_o_sf_consumer_state);
      // ++pipeline_o_sf_consumer_state;
      // Wait for O0 buffer to be free for QK SF reuse  等待上一次correction O1做完，因为QK需要用o1的空间，for i 为1时可以使用O1，因为在算O0
      // pipeline_o_sf.producer_acquire(pipeline_o_sf_producer_state);
      if (cute::elect_one_sync()) {
          copy(tiled_copy_s2t_SFQ0, thr_sSFQ0_s2t(_,_,_,_,0), thr_tSFQ0_s2t);
          copy(tiled_copy_s2t_SFK0, thr_sSFK0_s2t(_,_,_,_,k_index), thr_tSFK0_s2t);  //都是使用O0
      }
      cutlass::arch::fence_view_async_tmem_store();

      // gemm Q1 * Ki -> S1  (WITH SCALE FACTORS)   这一SFQK会用到O0的位置，所以等待O0 correction做完
      gemm_zero_acc(mma_qk, tSrQ0, tSrK(_,_,_,k_index), tStS0, tCtSFQ0, tCtSFK0);  //K的取数有问题，Q的不用管×
      
      pipeline_s0.producer_commit(pipeline_s0_producer_state);  //s0计算完通知后面做softmax
      ++pipeline_s0_producer_state;

      // pipeline_o_sf.consumer_release(pipeline_o_sf_release_state);
      // ++pipeline_o_sf_release_state;
      // //pipeline_corr_commit QK计算完，释放O的区域



      if constexpr (get<1>(ThreadShape{}) > 1) {
        pipeline_kv.consumer_release(pipeline_kv_release_state);
        ++pipeline_kv_release_state;
      }
       //************************************************ */

      if constexpr (get<1>(ThreadShape{}) > 1) {
        k_index = (pipeline_kv_consumer_state.index());
        pipeline_kv.consumer_wait(pipeline_kv_consumer_state);
        ++pipeline_kv_consumer_state;
      }

      // if (cute::elect_one_sync()) {
      //   copy(tiled_copy_s2t_SFQ, thr_sSFQ_s2t(_,_,_,_,Int<1>{}), thr_tSFQ_s2t);
      //   copy(tiled_copy_s2t_SFK, thr_sSFK_s2t(_,_,_,_,k_index), thr_tSFK_s2t);
      // }


      // SFQK1 goes to S0+32 → must re-acquire S0 (was committed in Q1*K→S0 above)
      pipeline_s0.producer_acquire(pipeline_s0_producer_state);
      if (cute::elect_one_sync()) {
          copy(tiled_copy_s2t_SFQ1, thr_sSFQ1_s2t(_,_,_,_,1), thr_tSFQ1_s2t);  // SFQ1→S0+32
          copy(tiled_copy_s2t_SFK1, thr_sSFK1_s2t(_,_,_,_,k_index), thr_tSFK1_s2t);
      }


      cutlass::arch::fence_view_async_tmem_store();
      // gemm Q2 * Ki -> S1  (WITH SCALE FACTORS, SFQK1 from S0+32)
      gemm_zero_acc(mma_qk, tSrQ1, tSrK(_,_,_,k_index), tStS1, tCtSFQ1, tCtSFK1);

      pipeline_s1.producer_commit(pipeline_s1_producer_state);
      ++pipeline_s1_producer_state;
      
      // pipeline_o_sf.consumer_release(pipeline_o_sf_release_state);
      // ++pipeline_o_sf_release_state;
      // pipeline_o_sf.consumer_release(pipeline_o_sf_release_state);
      // ++pipeline_o_sf_release_state;
      // Signal O1 SF consumed
      // pipeline_o_sf.producer_commit(pipeline_o_sf_producer_state);
      // ++pipeline_o_sf_producer_state;

      // release Ki
      pipeline_kv.consumer_release(pipeline_kv_release_state);
      ++pipeline_kv_release_state;

      // wait for Vi   //第二个V wait
      v_index = (pipeline_kv_consumer_state.index());
      pipeline_kv.consumer_wait(pipeline_kv_consumer_state);
      ++pipeline_kv_consumer_state;

      // // v_index = (pipeline_kv_consumer_state.index());
      // pipeline_sfp.consumer_wait(pipeline_sfp_consumer_state);
      // ++pipeline_sfp_consumer_state;
      
      // next P了， wait SFP0
      sp_index = pipeline_sfp_consumer_state.index();
      pipeline_sfp.consumer_wait(pipeline_sfp_consumer_state);
      ++pipeline_sfp_consumer_state;

      // if (blockIdx.x == 0 && blockIdx.y == 0) {
      //   constexpr int SFP_size = cute::cosize_v<SmemLayoutSFP>;
      //   printf("=== smem_sfp, total elements=%d ===\n", SFP_size);
      //   for (int i = 0; i < SFP_size; ++i) {
      //     if ((float)storage.smem_sfp[i] == 0) {
      //       printf("threadIdx.x=%d  sfp[%d] = %f  |  ",threadIdx.x, i, (float)storage.smem_sfp[i]);
      //     }
      //   }
      // }
      // if (blockIdx.x == 0 && blockIdx.y == 0) {
      //   constexpr int P_size = cute::cosize_v<SmemLayoutP>;
      //   printf("=== smem_p, total elements=%d ===\n", P_size);
      //   for (int i = 0; i < P_size; ++i) {
      //     if ((float)storage.smem_p[i] == 0) {
      //       printf("p[%d] = %f  |",i, (float)storage.smem_p[i]);
      //     }
      //   }
      // }

      // gemm P1 * Vi -> O1  (WITH SCALE FACTORS)
      pipeline_corr.producer_acquire(pipeline_corr_producer_state);
      pipeline_s0.producer_acquire(pipeline_s0_producer_state);  //P1 * Vi -> O1


      // // Copy P SF and V SF for this V tile
      // if (cute::elect_one_sync()) {
      //   copy(tiled_copy_s2t_SFP, thr_sSFP_s2t(_,_,_,_,v_index), thr_tSFP_s2t);
      //   copy(tiled_copy_s2t_SFV, thr_sSFV_s2t(_,_,_,_,v_index), thr_tSFV_s2t);
      // }
      if (cute::elect_one_sync()) {  
        copy(tiled_copy_s2t_SFP0, thr_sSFP0_s2t(_,_,_,_,sp_index), thr_tSFP0_s2t);  
        copy(tiled_copy_s2t_SFV0, thr_sSFV0_s2t(_,_,_,_,v_index), thr_tSFV0_s2t);
      }
      cutlass::arch::fence_view_async_tmem_store();
      // gemm P1 * Vi -> O1  (WITH SCALE FACTORS)
      gemm_reset_zero_acc(mma_pv, tOrP0, tOrV(_,_,_,v_index), tOtO0, tCtSFP0, tCtSFV0);
      // gemm_reset_zero_acc(mma_pv, tOrP0, tOrV(_,_,_,v_index), tOtO0);


      pipeline_corr.producer_commit(pipeline_corr_producer_state);
      ++pipeline_corr_producer_state;

      pipeline_sfp.consumer_release(pipeline_sfp_release_state);
      ++pipeline_sfp_release_state;

      if constexpr (get<1>(ThreadShape{}) > 1) {
        pipeline_kv.consumer_release(pipeline_kv_release_state);
        ++pipeline_kv_release_state;
      }  
    }

    // ============================================================
    // Loop epilogue
    // ============================================================

    // release Q1
    pipeline_q.consumer_release(pipeline_q_release_state);
    ++pipeline_q_release_state;

    // release Q2
    if constexpr (get<0>(ThreadShape{}) > 1) {
      pipeline_q.consumer_release(pipeline_q_release_state);
      ++pipeline_q_release_state;
    }
    

    // wait for Vi
    if constexpr (get<1>(ThreadShape{}) > 1) {
      v_index = pipeline_kv_consumer_state.index();
      pipeline_kv.consumer_wait(pipeline_kv_consumer_state);
      ++pipeline_kv_consumer_state;
    }
    //wait for sfp
    sp_index = pipeline_sfp_consumer_state.index();
    pipeline_sfp.consumer_wait(pipeline_sfp_consumer_state);
    ++pipeline_sfp_consumer_state;

    // gemm P2 * Vi -> O2  (WITH SCALE FACTORS)
    pipeline_corr.producer_acquire(pipeline_corr_producer_state);
    pipeline_s1.producer_acquire(pipeline_s1_producer_state);
    
    // Copy P SF and V SF for final PV MMA
    // if (cute::elect_one_sync()) {
    //   copy(tiled_copy_s2t_SFP, thr_sSFP_s2t(_,_,_,_,v_index), thr_tSFP_s2t);
    //   copy(tiled_copy_s2t_SFV, thr_sSFV_s2t(_,_,_,_,v_index), thr_tSFV_s2t);
    // }
    if (cute::elect_one_sync()) {
        copy(tiled_copy_s2t_SFP1, thr_sSFP1_s2t(_,_,_,_,sp_index), thr_tSFP1_s2t);    //问题在这
        copy(tiled_copy_s2t_SFV1, thr_sSFV1_s2t(_,_,_,_,v_index), thr_tSFV1_s2t);
    }
    cutlass::arch::fence_view_async_tmem_store();
    gemm_reset_zero_acc(mma_pv, tOrP1, tOrV(_,_,_,v_index), tOtO1, tCtSFP1, tCtSFV1);
    // gemm_reset_zero_acc(mma_pv, tOrP1, tOrV(_,_,_,v_index), tOtO1);
    

    pipeline_corr.producer_commit(pipeline_corr_producer_state);
    ++pipeline_corr_producer_state;

    pipeline_sfp.consumer_release(pipeline_sfp_release_state);
    ++pipeline_sfp_release_state;
    // release Vi
    pipeline_kv.consumer_release(pipeline_kv_release_state);
    ++pipeline_kv_release_state;

    // pipeline_sfp.consumer_release(pipeline_sfp_release_state);
    // ++pipeline_sfp_release_state;

    pipeline_s0.producer_commit(pipeline_s0_producer_state);
    ++pipeline_s0_producer_state;

    pipeline_s1.producer_commit(pipeline_s1_producer_state);
    ++pipeline_s1_producer_state;
  }

 
  template<bool need_apply_mask, class Stage, class BlkCoord, class CoordTensor, class ProblemShape>
  CUTLASS_DEVICE auto
  softmax_step(
      float& row_max, float& row_sum,
      Stage stage, bool final_call,
      BlkCoord const& blk_coord, CoordTensor const& cS,
      Params const& params, ProblemShape const& problem_shape,
      PipelineS& pipeline_s, typename PipelineS::PipelineState& pipeline_s_consumer_state,
      PipelineC& pipeline_c, typename PipelineC::PipelineState& pipeline_c_producer_state,
      OrderBarrierSoftmax& order_s,
      TensorStorage& storage) {

    Tensor tScS = typename CollectiveMmaQK::TiledMma{}.get_slice(0).partition_C(cS);

    Tensor tStS = partition_fragment_C(typename CollectiveMmaQK::TiledMma{}, select<0,1>(TileShapeQK{}));
    tStS.data() = uint32_t(stage == _0{} ? TmemAllocation::S0 : TmemAllocation::S1);

    Tensor tStS_v = tStS.compose(make_layout(make_shape(_128{}, _2{})));
    tStS_v.data() = uint32_t(stage == _0{} ? TmemAllocation::V0 : TmemAllocation::V1);
    Tensor tScS_v = tScS.compose(make_layout(make_shape(_128{}, _2{})));

    // Each thread owns a single row
    #if defined CUTE_ARCH_TCGEN05_TMEM_STAT_ENABLED
      using TMEM_LOAD = SM100_TMEM_LOAD_STAT_32dp32b32x;
    #else
      using TMEM_LOAD = SM100_TMEM_LOAD_32dp32b32x; // 4x32 threads with 128 cols of 32b elem
    #endif
    using TMEM_STORE_V = SM100_TMEM_STORE_32dp32b2x;   // 4x32 threads with 2 cols of 32b elem

    int thread_idx = threadIdx.x % (4 * cutlass::NumThreadsPerWarp);

    auto tiled_tmem_load = make_tmem_copy(TMEM_LOAD{}, tStS);
    auto thr_tmem_load   = tiled_tmem_load.get_slice(thread_idx);

    Tensor tTMEM_LOADtS = thr_tmem_load.partition_S(tStS);
    Tensor tTMEM_LOADcS = thr_tmem_load.partition_D(tScS);

    auto tiled_tmem_storev = make_tmem_copy(TMEM_STORE_V{}, tStS_v);
    auto thr_tmem_storev  = tiled_tmem_storev.get_slice(thread_idx);

    Tensor tTMEM_STOREVtS = thr_tmem_storev.partition_D(tStS_v);
    Tensor tTMEM_STOREVcS = thr_tmem_storev.partition_S(tScS_v);

    // wait on tensor core pipe   等待tensorCore中QK->S计算完
    pipeline_s.consumer_wait(pipeline_s_consumer_state);

    // read all of S from tmem into reg mem
    Tensor tTMEM_LOADrS = make_tensor<ElementQK>(shape(tTMEM_LOADcS));  //S计算完再使用S数据
    copy(tiled_tmem_load, tTMEM_LOADtS, tTMEM_LOADrS);  //从TMEM中搬运到寄存器中，tTMEM_LOADtS是从S0而来

    {
      // DEBUG: print S[0] single-shot to avoid CUDA printf interleaving
      // Softmax0: threads   0..127,  Softmax1: threads 128..255
      int sm_wg = threadIdx.x / 128;   // 0=Softmax0, 1=Softmax1
      int sm_tid = threadIdx.x % 128;  // 0..127 within softmax group
      int sm_warp = sm_tid / 32;       // 0,1,2,3
      int sm_lane = sm_tid % 32;       // 0..31

      if (blockIdx.x == 0 && blockIdx.y == 0 && sm_lane < 2) {
        auto first_coord = tTMEM_LOADcS(_0{});
        int my_row = get<0>(first_coord);
        // Single printf: row + S[0] (all S[k] should be equal if Q=1,K=1)
        printf("[SM%d_W%d_L%d] row=%-4d S0=%.4f S32=%.4f S64=%.4f S96=%.4f\n",
            sm_wg, sm_warp, sm_lane, my_row,
            (float)tTMEM_LOADrS(0), (float)tTMEM_LOADrS(32),
            (float)tTMEM_LOADrS(64), (float)tTMEM_LOADrS(96));
      }
    }

    if constexpr (need_apply_mask) {
      Mask{}.apply_mask(tTMEM_LOADrS, tTMEM_LOADcS, problem_shape);
    }
    ElementQK old_row_max = row_max;
    #if defined CUTE_ARCH_TCGEN05_TMEM_STAT_ENABLED
      auto pos = tTMEM_LOADcS(0);
      if (!need_apply_mask || (need_apply_mask && (get<0>(pos) >= get<1>(pos) + 12) && (get<1>(pos) < get<1>(problem_shape)))) {
        float curr_max = tiled_tmem_load.get_max();
        row_max = ::fmax(row_max, curr_max);
      }
      else
    #endif
    {
      float row_max_0 = row_max;
      float row_max_1 = row_max;
      float row_max_2 = row_max;
      float row_max_3 = row_max;
      CUTLASS_PRAGMA_UNROLL
      for (int i = 0; i < size(tTMEM_LOADrS); i += 4) {
        row_max_0  = ::fmax(row_max_0, tTMEM_LOADrS(i));  //从寄存器中获取值
        row_max_1 = ::fmax(row_max_1, tTMEM_LOADrS(i+1));
        row_max_2 = ::fmax(row_max_2, tTMEM_LOADrS(i+2));
        row_max_3 = ::fmax(row_max_3, tTMEM_LOADrS(i+3));
        // if(blockIdx.x==0 && blockIdx.y==0 && threadIdx.x==0){
        //   printf("tTMEM_LOADrS[%d]: %f\n",i,tTMEM_LOADrS(i));
        //   printf("tTMEM_LOADrS[%d]: %f\n",i+1,tTMEM_LOADrS(i+1));
        //   printf("tTMEM_LOADrS[%d]: %f\n",i+2,tTMEM_LOADrS(i+2));
        // }
      }
      row_max = ::fmax(row_max_0, row_max_1);
      row_max = ::fmax(row_max, row_max_2);
      row_max = ::fmax(row_max, row_max_3);
    }

    ElementQK row_max_safe = row_max == -INFINITY ? 0 : row_max;

    Tensor tTMEM_STOREVrS = make_tensor<ElementQK>(shape(tTMEM_STOREVcS));  //定义寄存器
    tTMEM_STOREVrS(kIdxOldRowMax) = old_row_max;
    tTMEM_STOREVrS(kIdxNewRowMax) = row_max_safe;
    copy(tiled_tmem_storev, tTMEM_STOREVrS, tTMEM_STOREVtS);   //寄存器->TMEM V，搬运目标：TMEM 的 V0 或 V1 区域
                                                        // - 用途：Correction warp 读这些 stats 来判断是否需要 rescale O
    // cutlass::arch::fence_view_async_tmem_store();
    //   __syncwarp();
    pipeline_c.producer_commit(pipeline_c_producer_state);  //等待
    ++pipeline_c_producer_state;

    ElementQK scale = params.scale_softmax_log2;   //计算 exp2(scale * (S - row_max)), ElementQK:float
    // scale_softmax_log2 = scale_q * scale_k * log2(e) / sqrt(d)
    ElementQK row_max_scale = row_max_safe * scale;

    float2 scale_fp32x2 = make_float2(scale, scale);
    float2 minus_row_max_scale_fp32x2 = make_float2(-row_max_scale, -row_max_scale);

    // ================================================================
    // P → SMEM_P via direct row-wise write
    //
    // Flow: S from TMEM → reg → softmax → quantize → pack into
    //       tTMEM_STORErS_x4 (reg buffer, 32×uint32_t per thread)
    //       → write 32 uint32_t to SMEM row = this thread's row in tile
    //
    // We reuse the TMEM_STORE coordinate system (tTMEM_STOREcS) to define
    // the register packing buffer shape, but skip TMEM store and
    // write to SMEM directly instead.
    // ================================================================

    // --- SMEM P tile (swizzled layout, for PV MMA) ---
    Tensor sP_full = make_tensor(make_smem_ptr(storage.smem_p.data()), SmemLayoutP{});
    Tensor sP_tile = (stage == _0{}) ? sP_full(_, _, _, _0{}) : sP_full(_, _, _, _1{});

    // --- Register buffer coordinate system (from TMEM_STORE layout) ---
    using TMEM_STORE_P = SM100_TMEM_STORE_32dp32b32x;

    auto tilePlikeFP32 = size<1>(TileShapeQK{}) / Int<sizeof(float)>{} * Int<sizeof(Element)>{};
    Tensor tStS_P = tStS.compose(make_layout(make_shape(_128{}, tilePlikeFP32)));
    // TMEM base unused, only need the coordinate shape
    tStS_P.data() = warp_uniform(0);
    Tensor tScS_P = tScS.compose(make_layout(make_shape(_128{}, tilePlikeFP32)));

    auto tiled_tmem_store = make_tmem_copy(TMEM_STORE_P{}, tStS_P);
    auto thr_tmem_store = tiled_tmem_store.get_slice(thread_idx);
    Tensor tTMEM_STOREcS = thr_tmem_store.partition_S(tScS_P);

    constexpr int kConversionsPerStep = 2;
    Tensor tTMEM_STORErS_x4 = make_tensor<uint32_t>(shape(tTMEM_STOREcS));
    Tensor tTMEM_STORErS_x4_e = recast<Array<ElementData, kConversionsPerStep>>(tTMEM_STORErS_x4);

    NumericArrayConverter<ElementData, ElementQK, kConversionsPerStep> convert;

    const int kReleasePipeCount = 10;  // must be multiple of 2

    order_s.wait();

    // Phase 1: Compute softmax P = exp2(scale * (S - row_max)),
    //          quantize FP32 → E4M3, pack into register buffer
    CUTLASS_PRAGMA_UNROLL
    for (int i = 0; i < size(tTMEM_LOADrS); i += 2) {
      float2 in = make_float2(
        tTMEM_LOADrS(i + 0),
        tTMEM_LOADrS(i + 1)
      );
      float2 out;
      cute::fma(out, scale_fp32x2, in, minus_row_max_scale_fp32x2);
      tTMEM_LOADrS(i + 0) = out.x;
      tTMEM_LOADrS(i + 1) = out.y;

      tTMEM_LOADrS(i+0) = ::exp2f(tTMEM_LOADrS(i+0));
      tTMEM_LOADrS(i+1) = ::exp2f(tTMEM_LOADrS(i+1));

      // Quantize FP32 → E4M3 and pack into register buffer (uint32_t per 4×FP8)
      Array<ElementQK, kConversionsPerStep> in_conv;
      CUTLASS_PRAGMA_UNROLL
      for (int j = 0; j < kConversionsPerStep; j++) {
        in_conv[j] = tTMEM_LOADrS(i + j);
      }
      tTMEM_STORErS_x4_e[i / kConversionsPerStep] = convert(in_conv);

      if (i == size(tTMEM_LOADrS) - kReleasePipeCount) {
        order_s.arrive();
      }
    }

    // ================================================================
    // Phase 2: Write P from registers → SMEM (row-wise, per thread)
    // ================================================================
    // Each thread owns exactly one row (128 fp8 = 32 uint32_t) of the
    // (128, 128) P tile.  Recast the swizzled SMEM tile to uint32_t view
    // and write the thread's row.
    //
    //   sP_tile (8b):  (128, 128)  →  16384 fp8
    //   sP_u32 (32b):  (128,  32)  →   4096 uint32_t
    //   thread row:    sP_u32(thread_idx, _)  →  32 uint32_t
    //
    // Swizzle (Sw<3,4,3>) is part of the smem_ptr type, so recast
    // preserves the correct bank-conflict-avoiding addresses.
    // ================================================================
    // Tensor sP_u32 = recast<uint32_t>(sP_tile);
    // auto my_coord = tTMEM_LOADcS(_0{});                      // first coord element
    // int my_row = get<0>(my_coord);                            // row in [0, 128)
    // Tensor sP_my_row = sP_u32(my_row, _, _);                     // (32,) uint32_t


    int my_row = get<0>(tTMEM_LOADcS(_0{})) % 128;

    // coalesce: flatten swizzled 3-mode layout -> true 2D (128, 128) FP8
    // then recast to uint32_t: inner 4 FP8 -> 1 uint32_t -> (128, 32)
    auto sP_tile_flat = coalesce(sP_tile);
    Tensor sP_u32 = recast<uint32_t>(sP_tile_flat);
    CUTLASS_PRAGMA_UNROLL
    for (int j = 0; j < size(tTMEM_STORErS_x4); ++j) {
      sP_u32(my_row, j) = tTMEM_STORErS_x4(j);
    }
    // if (threadIdx.x == 0) {
    //   printf("sP_u32 layout: "); print(sP_u32.layout()); printf("\n");
    // }
    
    cutlass::arch::fence_view_async_shared();

    
    // notify tensor core warp that P is ready
    pipeline_s.consumer_release(pipeline_s_consumer_state);  //执行到这，在等待C释放
    ++pipeline_s_consumer_state;



    pipeline_c.producer_acquire(pipeline_c_producer_state);  //等待O计算完

    ElementQK acc_scale = (old_row_max == row_max_safe) ? 0.5f : 0.5f * ::exp2f(scale * (old_row_max - row_max_safe));
    row_sum *= acc_scale;
    float2 local_row_sum_f32x2 = make_float2(row_sum, row_sum);
    float2 local_row_sum_1 = make_float2(0, 0);
    float2 local_row_sum_2 = make_float2(0, 0);
    float2 local_row_sum_3 = make_float2(0, 0);

    CUTLASS_PRAGMA_UNROLL
    for (int i = 0; i < size(tTMEM_LOADrS); i += 8) {
      float2 in = make_float2(tTMEM_LOADrS(i), tTMEM_LOADrS(i+1));
      cute::add(local_row_sum_f32x2, local_row_sum_f32x2, in);

      in = make_float2(tTMEM_LOADrS(i+2), tTMEM_LOADrS(i+2+1));
      cute::add(local_row_sum_1, local_row_sum_1, in);

      in = make_float2(tTMEM_LOADrS(i+4), tTMEM_LOADrS(i+4+1));
      cute::add(local_row_sum_2, local_row_sum_2, in);

      in = make_float2(tTMEM_LOADrS(i+6), tTMEM_LOADrS(i+6+1));
      cute::add(local_row_sum_3, local_row_sum_3, in);
    }

    cute::add(local_row_sum_f32x2, local_row_sum_f32x2, local_row_sum_1);
    cute::add(local_row_sum_2, local_row_sum_2, local_row_sum_3);
    cute::add(local_row_sum_f32x2, local_row_sum_f32x2, local_row_sum_2);
    float local_row_sum = local_row_sum_f32x2.x + local_row_sum_f32x2.y;

    row_sum = local_row_sum;

    if (final_call) {
      pipeline_s.consumer_wait(pipeline_s_consumer_state);

      Tensor tTMEM_STOREVrS = make_tensor<ElementQK>(shape(tTMEM_STOREVcS));
      tTMEM_STOREVrS(kIdxFinalRowMax) = row_max;
      tTMEM_STOREVrS(kIdxFinalRowSum) = row_sum;
      copy(tiled_tmem_storev, tTMEM_STOREVrS, tTMEM_STOREVtS);
    //       cutlass::arch::fence_view_async_tmem_store();
    // __syncwarp();
    }
  }




  template<class Stage, class BlkCoord, class ProblemShape>
  CUTLASS_DEVICE auto
  softmax(
      Stage stage,
      BlkCoord const& blk_coord,
      Params const& params, ProblemShape const& problem_shape,
      PipelineS& pipeline_s, typename PipelineS::PipelineState& pipeline_s_consumer_state,  
      PipelineC& pipeline_c, typename PipelineC::PipelineState& pipeline_c_producer_state,
      OrderBarrierSoftmax& order_s,
      TensorStorage& storage) {

    int mask_tile_count = Mask{}.get_unmasked_trip_count(blk_coord, TileShape{}, problem_shape);

    ElementQK row_max = -INFINITY;
    ElementQK row_sum = 0;

    Tensor cS_base = make_identity_tensor(select<0,1>(TileShapeQK{}));
    auto logical_offset = make_coord(
        get<0>(blk_coord) * get<0>(TileShape{}) + (stage % get<0>(ThreadShape{})) * get<0>(TileShapeQK{}),
        0 + (stage % get<1>(ThreadShape{})) * get<1>(TileShapeQK{})
    );
    Tensor cS = domain_offset(logical_offset, cS_base);

    pipeline_c.producer_acquire(pipeline_c_producer_state);

    // mask_tile_count = 1;
    CUTLASS_PRAGMA_NO_UNROLL
    for (; mask_tile_count > 0; mask_tile_count -= 1) {
      softmax_step<false /* need_apply_mask */>(
          row_max, row_sum, stage,
          (mask_tile_count == 1) &&
              (Mask{}.get_masked_trip_count(blk_coord, TileShape{}, problem_shape) == 0),
          blk_coord, cS, params, problem_shape,
          pipeline_s, pipeline_s_consumer_state,
          pipeline_c, pipeline_c_producer_state,
          order_s,
          storage
      );

      cS.data() = cS.data() + E<1>{} * get<1>(ThreadShape{}) * get<1>(TileShapeQK{});
    }

    // Masked iterations
    mask_tile_count = Mask{}.get_masked_trip_count(blk_coord, TileShape{}, problem_shape);

    // mask_tile_count = 1;
    CUTLASS_PRAGMA_NO_UNROLL
    for (; mask_tile_count > 0; mask_tile_count -= 1) {
      softmax_step<true /* need_apply_mask */>(
          row_max, row_sum, stage, mask_tile_count == 1,
          blk_coord, cS, params, problem_shape,
          pipeline_s, pipeline_s_consumer_state,
          pipeline_c, pipeline_c_producer_state,
          order_s,
          storage
      );

      cS.data() = cS.data() + E<1>{} * get<1>(ThreadShape{}) * get<1>(TileShapeQK{});
    }

    pipeline_c.producer_commit(pipeline_c_producer_state);
    ++pipeline_c_producer_state;

    pipeline_c.producer_acquire(pipeline_c_producer_state);
    // empty step to sync against pipe s
    pipeline_s.consumer_release(pipeline_s_consumer_state);
    ++pipeline_s_consumer_state;
  }


  template<class Stage, class TensorO>
  CUTLASS_DEVICE auto
  correction_epilogue(
      float scale,
      Stage stage,
      TensorO const& sO_01) {

    using ElementOut = typename TensorO::value_type;

    int thread_idx = threadIdx.x % (4 * cutlass::NumThreadsPerWarp);

    Tensor sO = sO_01(_,_,stage);

    const int kCorrectionTileSize = 32 / sizeof(ElementOut);

    using TMEM_LOAD = std::conditional_t<kCorrectionTileSize == 32, SM100_TMEM_LOAD_32dp32b32x, SM100_TMEM_LOAD_32dp32b16x>;

    typename CollectiveMmaPV::TiledMma mma;
    Tensor cO = make_identity_tensor(select<0,1>(TileShapePV{}));
    Tensor tOtO = partition_fragment_C(mma, select<0,1>(TileShapePV{}));
    Tensor tOcO = mma.get_slice(0).partition_C(cO);
    Tensor tOsO = mma.get_slice(0).partition_C(sO);

    Tensor tOtO_i = logical_divide(tOtO, make_layout(make_shape(_128{}, Int<kCorrectionTileSize>{})));
    Tensor tOcO_i = logical_divide(tOcO, make_layout(make_shape(_128{}, Int<kCorrectionTileSize>{})));
    Tensor tOsO_i = logical_divide(tOsO, make_layout(make_shape(_128{}, Int<kCorrectionTileSize>{})));

    if constexpr (decltype(stage == _0{})::value) {
      tOtO_i.data() = tOtO_i.data().get() + uint32_t(TmemAllocation::O0);
    }
    else {
      static_assert(decltype(stage == _1{})::value, "stage is either 0 or 1");
      tOtO_i.data() = tOtO_i.data().get() + uint32_t(TmemAllocation::O1);
    }

    auto tiled_tmem_load = make_tmem_copy(TMEM_LOAD{}, tOtO_i(make_coord(_, _), _0{}));
    auto thr_tmem_load   = tiled_tmem_load.get_slice(thread_idx);

    Tensor tTMEM_LOADtO = thr_tmem_load.partition_S(tOtO_i(make_coord(_, _), _));
    Tensor tTMEM_LOADcO = thr_tmem_load.partition_D(tOcO_i(make_coord(_, _), _));
    Tensor tTMEM_LOADsO = thr_tmem_load.partition_D(tOsO_i(make_coord(_, _), _));

    float2 scale_f32x2 = make_float2(scale, scale);

    CUTLASS_PRAGMA_UNROLL
    for (int i = 0; i < get<2>(TileShape{}) / kCorrectionTileSize; i++) {
      Tensor tTMEM_LOADtO_i = tTMEM_LOADtO(_, _0{}, _0{}, i);
      Tensor tTMEM_LOADsO_i = tTMEM_LOADsO(_, _0{}, _0{}, i);

      Tensor tTMrO = make_tensor<ElementPV>(shape(tTMEM_LOADcO(_, _0{}, _0{}, i)));

      copy(tiled_tmem_load, tTMEM_LOADtO_i, tTMrO);
      // printf("threadIdx.x:%d \n", threadIdx.x);
      // if (blockIdx.x == 0 && blockIdx.y == 0 && threadIdx.x == 256) {
      //   printf("=== O TMEM stage=%d ===\n", int(stage));
      //   for (int j = 0; j < (int)size(tTMrO) && j < 16; ++j) {
      //     printf("  O[%d] = %f\n", j, (float)tTMrO(j));
      //   }
      // }
#ifndef ONLY_SOFTMAX
      CUTLASS_PRAGMA_UNROLL
      for (int j = 0; j < size(tTMrO); j += 2) {
        float2 in = make_float2(tTMrO(j), tTMrO(j+1));
        float2 out;
        cute::mul(out, scale_f32x2, in);
        tTMrO(j) = out.x;
        tTMrO(j+1) = out.y;
      }
#endif

      constexpr int N = 4 / sizeof(ElementOut);
      NumericArrayConverter<ElementOut, ElementPV, N> convert;

      Tensor tSMrO = make_tensor_like<ElementOut>(tTMrO);

      Tensor tCs = recast<decltype(convert)::source_type>(tTMrO);
      Tensor tCd = recast<decltype(convert)::result_type>(tSMrO);

      CUTLASS_PRAGMA_UNROLL
      for (int j = 0; j < size(tCs); j++) {
        tCd(j) = convert.convert(tCs(j));
      }

      Tensor tSMsO_i = recast<uint32_t>(tTMEM_LOADsO_i);
      Tensor tSMrO_i = recast<uint32_t>(tSMrO);

      copy(AutoVectorizingCopyWithAssumedAlignment<128>{}, tSMrO_i, tSMsO_i);
    }

    cutlass::arch::fence_view_async_shared();
  }

  CUTLASS_DEVICE auto
  correction_rescale(
      float scale,
      uint32_t tmem_O) {

    int thread_idx = threadIdx.x % (4 * cutlass::NumThreadsPerWarp);

    const int kCorrectionTileSize = 16;

    using TMEM_LOAD = SM100_TMEM_LOAD_32dp32b16x;
    using TMEM_STORE = SM100_TMEM_STORE_32dp32b16x;

    typename CollectiveMmaPV::TiledMma mma;
    Tensor cO = make_identity_tensor(select<0,1>(TileShapePV{}));
    Tensor tOtO = partition_fragment_C(mma, select<0,1>(TileShapePV{}));
    Tensor tOcO = mma.get_slice(0).partition_C(cO);

    Tensor tOtO_i = tOtO.compose(make_layout(make_shape(_128{}, Int<kCorrectionTileSize>{})));
    Tensor tOcO_i = tOcO.compose(make_layout(make_shape(_128{}, Int<kCorrectionTileSize>{})));

    tOtO_i.data() = tOtO_i.data().get() + tmem_O;

    auto tiled_tmem_load = make_tmem_copy(TMEM_LOAD{}, tOtO_i);
    auto thr_tmem_load   = tiled_tmem_load.get_slice(thread_idx);
    auto tiled_tmem_store = make_tmem_copy(TMEM_STORE{}, tOtO_i);
    auto thr_tmem_store   = tiled_tmem_store.get_slice(thread_idx);

    Tensor tTMEM_LOADtO = thr_tmem_load.partition_S(tOtO_i);
    Tensor tTMEM_LOADcO = thr_tmem_load.partition_D(tOcO_i);
    Tensor tTMEM_STOREtO = thr_tmem_store.partition_D(tOtO_i);
    Tensor tTMEM_STOREcO = thr_tmem_store.partition_S(tOcO_i);
    static_assert(shape(tTMEM_STOREcO) == shape(tTMEM_LOADcO));

    float2 scale_f32x2 = make_float2(scale, scale);

    Tensor tTMrO = make_tensor<ElementPV>(make_shape(shape(tTMEM_LOADcO), Int<128 / kCorrectionTileSize>{}));

    auto copy_in = [&](int i) {
      Tensor tTMEM_LOADtO_i = tTMEM_LOADtO;
      tTMEM_LOADtO_i.data() = tTMEM_LOADtO_i.data().get() + uint32_t(i * kCorrectionTileSize);
      Tensor tTMrO_i = tTMrO(_, i).compose(make_layout(shape<0>(tTMrO)));
      copy(tiled_tmem_load, tTMEM_LOADtO_i, tTMrO_i);
    };

    auto copy_out = [&](int i) {
      Tensor tTMEM_STOREtO_i = tTMEM_STOREtO;
      tTMEM_STOREtO_i.data() = tTMEM_STOREtO_i.data().get() + uint32_t(i * kCorrectionTileSize);
      Tensor tTMrO_i = tTMrO(_, i).compose(make_layout(shape<0>(tTMrO)));
      copy(tiled_tmem_store, tTMrO_i, tTMEM_STOREtO_i);
    };

    copy_in(0);

    int count = get<2>(TileShape{}) / kCorrectionTileSize;

    CUTLASS_PRAGMA_UNROLL
    for (int i = 0; i < count; i++) {
      if (i != count - 1) {
        copy_in(i+1);
      }

      Tensor tTMrO_i = tTMrO(_, i).compose(make_layout(shape<0>(tTMrO)));

      if (scale != 1.0f) {
        CUTLASS_PRAGMA_UNROLL
        for (int j = 0; j < size(tTMrO_i); j += 2) {
          float2 in = make_float2(tTMrO_i(j), tTMrO_i(j+1));
          float2 out;
          cute::mul(out, scale_f32x2, in);
          tTMrO_i(j) = out.x;
          tTMrO_i(j+1) = out.y;
        }
      }

      copy_out(i);
    }
  }

  template<
    class BlkCoord, class ProblemShape, class ParamsProblemShape,
    class TensorStorageEpi, class CollectiveEpilogue
  >
  CUTLASS_DEVICE auto
  correction(
      BlkCoord const& blk_coord,
      Params const& params, ProblemShape const& problem_shape,
      ParamsProblemShape const& params_problem_shape,
      TensorStorageEpi& shared_storage_epi,
      PipelineC& pipeline_s0_c, typename PipelineC::PipelineState& pipeline_s0_c_consumer_state,
      PipelineC& pipeline_s1_c, typename PipelineC::PipelineState& pipeline_s1_c_consumer_state,
      PipelineO& pipeline_o, typename PipelineO::PipelineState& pipeline_o_consumer_state,
      PipelineO_SF& pipeline_o_sf, typename PipelineO_SF::PipelineState& pipeline_o_sf_producer_state,
      PipelineE& pipeline_epi, typename PipelineE::PipelineState& pipeline_epi_producer_state,
      CollectiveEpilogue& epilogue) {

    int mask_tile_count = Mask{}.get_trip_count(blk_coord, TileShape{}, problem_shape);

    int thread_idx = threadIdx.x % (4 * cutlass::NumThreadsPerWarp);

    Tensor tStS = partition_fragment_C(typename CollectiveMmaQK::TiledMma{}, select<0,1>(TileShapeQK{}));

    Tensor cS = make_identity_tensor(select<0,1>(TileShapeQK{}));
    Tensor tScS = typename CollectiveMmaQK::TiledMma{}.get_slice(0).partition_C(cS);

    Tensor tStS_v = tStS.compose(make_layout(make_shape(_128{}, _2{})));
    Tensor tScS_v = tScS.compose(make_layout(make_shape(_128{}, _2{})));

    using TMEM_LOAD_V = SM100_TMEM_LOAD_32dp32b2x;

    auto tiled_tmem_loadv = make_tmem_copy(TMEM_LOAD_V{}, tStS_v);
    auto thr_tmem_loadv  = tiled_tmem_loadv.get_slice(thread_idx);

    Tensor tTMEM_LOADVtS = thr_tmem_loadv.partition_S(tStS_v);
    Tensor tTMEM_LOADVcS = thr_tmem_loadv.partition_D(tScS_v);

    Tensor tTMEM_LOADVtS0 = tTMEM_LOADVtS;
    tTMEM_LOADVtS0.data() = tTMEM_LOADVtS0.data().get() + uint32_t(TmemAllocation::V0);
    Tensor tTMEM_LOADVtS1 = tTMEM_LOADVtS;
    tTMEM_LOADVtS1.data() = tTMEM_LOADVtS1.data().get() + uint32_t(TmemAllocation::V1);

    // ignore first signal from softmax as no correction is required
    pipeline_s0_c.consumer_wait(pipeline_s0_c_consumer_state);
    pipeline_s0_c.consumer_release(pipeline_s0_c_consumer_state);
    ++pipeline_s0_c_consumer_state;

    pipeline_s1_c.consumer_wait(pipeline_s1_c_consumer_state);

    // handle the last iteration differently (i.e. tmem_load/stsm for epi)
    mask_tile_count -= 1;
    // mask_tile_count = 1;

    CUTLASS_PRAGMA_NO_UNROLL
    for (; mask_tile_count > 0; mask_tile_count -= 1) {
      // printf("123\n");
      pipeline_s0_c.consumer_wait(pipeline_s0_c_consumer_state);  //做完softmax再来这里
      // printf("456\n");
      Tensor tTMEM_LOADVrS = make_tensor<ElementQK>(shape(tTMEM_LOADVcS));

      // read row_wise new global max
      copy(tiled_tmem_loadv, tTMEM_LOADVtS0, tTMEM_LOADVrS);
      // cutlass::arch::fence_view_async_tmem_load();
      // __syncwarp();
      // e^(scale * (old_max - new_max)
      float scale = (tTMEM_LOADVrS(kIdxOldRowMax) == tTMEM_LOADVrS(kIdxNewRowMax)) ? 1.0f : ::exp2f(params.scale_softmax_log2 * (tTMEM_LOADVrS(kIdxOldRowMax) - tTMEM_LOADVrS(kIdxNewRowMax)));

      pipeline_o.consumer_wait(pipeline_o_consumer_state);

      // pipeline_o_sf.producer_acquire(pipeline_o_sf_producer_state);

      bool warp_do_correction = __any_sync(0xFFFFFFFF, scale != 1.0f);
      if (warp_do_correction) {
        correction_rescale(scale, uint32_t(TmemAllocation::O0));
      }

      pipeline_s1_c.consumer_release(pipeline_s1_c_consumer_state);
      ++pipeline_s1_c_consumer_state;

      cutlass::arch::fence_view_async_tmem_store();

      
      // pipeline_o_sf.producer_commit(pipeline_o_sf_producer_state);  
      // ++pipeline_o_sf_producer_state;  

      pipeline_o.consumer_release(pipeline_o_consumer_state);
      ++pipeline_o_consumer_state;

      
      
      // pipeline_s0_c.consumer_wait(pipeline_s0_c_consumer_state);  //在做QK->S0的计算，因为QK要用到O0

      // pipeline_o.consumer_wait(pipeline_o_consumer_state);   //等待另一边MMA的O0用完FOR循环
      // pipeline_o.consumer_release(pipeline_o_consumer_state);
      // ++pipeline_o_consumer_state;


      pipeline_s1_c.consumer_wait(pipeline_s1_c_consumer_state);

      copy(tiled_tmem_loadv, tTMEM_LOADVtS1, tTMEM_LOADVrS);
      // cutlass::arch::fence_view_async_tmem_load();
      // __syncwarp();
      scale = (tTMEM_LOADVrS(kIdxOldRowMax) == tTMEM_LOADVrS(kIdxNewRowMax)) ? 1.0f : ::exp2f(params.scale_softmax_log2 * (tTMEM_LOADVrS(kIdxOldRowMax) - tTMEM_LOADVrS(kIdxNewRowMax)));
      //pipeline_o.wait(pipeline_o_consumer_state); //等待QK 的 O01计算完
      //pipeline_o.consumer_release(pipeline_o_consumer_state);
      //++pipeline_o_consumer_state;    

      pipeline_o.consumer_wait(pipeline_o_consumer_state);   //等待另一边MMA的O1

      // pipeline_o_sf.producer_acquire(pipeline_o_sf_producer_state);

      warp_do_correction = __any_sync(0xFFFFFFFF, scale != 1.0f);
      if (warp_do_correction) {
        correction_rescale(scale, uint32_t(TmemAllocation::O1));
      }

      pipeline_s0_c.consumer_release(pipeline_s0_c_consumer_state);
      ++pipeline_s0_c_consumer_state;

      cutlass::arch::fence_view_async_tmem_store();

      

      pipeline_o.consumer_release(pipeline_o_consumer_state);
      ++pipeline_o_consumer_state;

      // pipeline_o_sf.producer_commit(pipeline_o_sf_producer_state);  //给另一边QK计算
      // ++pipeline_o_sf_producer_state;  

      // pipeline_s1_c.consumer_wait(pipeline_s1_c_consumer_state);   //再做QK到O1的搬运

      // pipeline_o.consumer_wait(pipeline_o_consumer_state);   //等待另一边MMA的O1用完FOR循环
      // pipeline_o.consumer_release(pipeline_o_consumer_state);
      // ++pipeline_o_consumer_state;

      // pipeline_o_sf.consumer_wait(pipeline_o_sf_consumer_state);
      // pipeline_o_sf.consumer_release(pipeline_o_sf_consumer_state);
      // ++pipeline_o_sf_consumer_state;
    }

    pipeline_s1_c.consumer_release(pipeline_s1_c_consumer_state);
    ++pipeline_s1_c_consumer_state;

    pipeline_s0_c.consumer_wait(pipeline_s0_c_consumer_state);

    // read from V0
    Tensor tTMEM_LOADVrS = make_tensor<ElementQK>(shape(tTMEM_LOADVcS));
    copy(tiled_tmem_loadv, tTMEM_LOADVtS0, tTMEM_LOADVrS);
      // cutlass::arch::fence_view_async_tmem_load();
      // __syncwarp();
    pipeline_s0_c.consumer_release(pipeline_s0_c_consumer_state);
    ++pipeline_s0_c_consumer_state;

    pipeline_o.consumer_wait(pipeline_o_consumer_state);
    pipeline_epi.producer_acquire(pipeline_epi_producer_state);

    Tensor sO = make_tensor(make_smem_ptr(shared_storage_epi.smem_o.data()), typename TensorStorageEpi::SmemLayoutO{});
    Tensor gLSE = make_tensor(make_gmem_ptr(epilogue.params.ptr_LSE), select<0,3>(problem_shape), epilogue.params.dLSE);

    correction_epilogue(params.scale_output / tTMEM_LOADVrS(kIdxFinalRowSum), _0{}, sO);

    if (epilogue.params.ptr_LSE != nullptr) {
      int row_idx = get<0>(tTMEM_LOADVcS(_0{})) + get<0>(TileShape{}) * get<0>(blk_coord);

      int row_offset = 0;
      if constexpr (is_variable_length_v<tuple_element_t<0, ParamsProblemShape>>) {
        row_offset = get<0>(params_problem_shape).cumulative_length[get<2,1>(blk_coord)];
      }

      ElementPV lse = cutlass::fast_log(tTMEM_LOADVrS(kIdxFinalRowSum)) + params.scale_softmax * tTMEM_LOADVrS(kIdxFinalRowMax);

      if (row_idx < get<0>(problem_shape)) {
        gLSE(row_idx + row_offset, get<2>(blk_coord)) = lse;
      }
    }

    cutlass::arch::fence_view_async_tmem_load();

    pipeline_o.consumer_release(pipeline_o_consumer_state);
    ++pipeline_o_consumer_state;

    pipeline_epi.producer_commit(pipeline_epi_producer_state);
    ++pipeline_epi_producer_state;

    pipeline_s1_c.consumer_wait(pipeline_s1_c_consumer_state);

    // load from V1
    copy(tiled_tmem_loadv, tTMEM_LOADVtS1, tTMEM_LOADVrS);

    // cutlass::arch::fence_view_async_tmem_load();
    // __syncwarp();
    pipeline_s1_c.consumer_release(pipeline_s1_c_consumer_state);
    ++pipeline_s1_c_consumer_state;

    pipeline_o.consumer_wait(pipeline_o_consumer_state);
    pipeline_epi.producer_acquire(pipeline_epi_producer_state);

    correction_epilogue(params.scale_output / tTMEM_LOADVrS(kIdxFinalRowSum), _1{}, sO);

    if (epilogue.params.ptr_LSE != nullptr) {
      int row_idx = get<0>(tTMEM_LOADVcS(_0{})) + get<0>(TileShape{}) * get<0>(blk_coord) + get<0>(TileShapeQK{});

      ElementPV lse = cutlass::fast_log(tTMEM_LOADVrS(kIdxFinalRowSum)) + params.scale_softmax * tTMEM_LOADVrS(kIdxFinalRowMax);

      int row_offset = 0;
      if constexpr (is_variable_length_v<tuple_element_t<0, ParamsProblemShape>>) {
        row_offset = get<0>(params_problem_shape).cumulative_length[get<2,1>(blk_coord)];
      }

      if (row_idx < get<0>(problem_shape)) {
        gLSE(row_idx + row_offset, get<2>(blk_coord)) = lse;
      }
    }

    cutlass::arch::fence_view_async_tmem_load();

    pipeline_o.consumer_release(pipeline_o_consumer_state);
    ++pipeline_o_consumer_state;

    pipeline_epi.producer_commit(pipeline_epi_producer_state);
    ++pipeline_epi_producer_state;
  }


  template<
    class BlkCoord, class ProblemShape, class ParamsProblemShape,
    class TensorStorageEpi, class CollectiveEpilogue
  >
  CUTLASS_DEVICE auto
  correction_empty(
      BlkCoord const& blk_coord,
      Params const& params, ProblemShape const& problem_shape,
      ParamsProblemShape const& params_problem_shape,
      TensorStorageEpi& shared_storage_epi,
      PipelineE& pipeline_epi, typename PipelineE::PipelineState& pipeline_epi_producer_state,
      CollectiveEpilogue& epilogue) {

    pipeline_epi.producer_acquire(pipeline_epi_producer_state);

    Tensor sO = make_tensor(make_smem_ptr(shared_storage_epi.smem_o.data()), typename TensorStorageEpi::SmemLayoutO{});
    Tensor gLSE = make_tensor(make_gmem_ptr(epilogue.params.ptr_LSE), select<0,3>(problem_shape), epilogue.params.dLSE);
    float lse = -INFINITY;
    int thread_idx = threadIdx.x % (4 * NumThreadsPerWarp);

#define DSHOW(x) print(#x ": "); print(x); print("\n")
    if (threadIdx.x % 128 == 0 && block0()) {
      DSHOW(sO);
    }
#if 1

    using ElementOut = typename CollectiveEpilogue::ElementOut;
    auto tiled_copy = make_cotiled_copy(
        Copy_Atom<UniversalCopy<uint32_t>, ElementOut>{},
        make_ordered_layout(make_shape(_128{}, Int<sizeof(uint32_t) / sizeof(ElementOut)>{}), Step<_1, _0>{}),
        sO.layout());

    auto thr_copy = tiled_copy.get_slice(thread_idx);
    auto tOgO = thr_copy.partition_D(sO);
    auto tOrO = make_tensor<ElementOut>(shape(tOgO(_,_,_,_0{})));
    clear(tOrO);

    copy(tiled_copy, tOrO, tOgO(_,_,_,_0{}));
#endif

    if (epilogue.params.ptr_LSE != nullptr) {
      int row_idx = thread_idx + get<0>(TileShape{}) * get<0>(blk_coord);

      int row_offset = 0;
      if constexpr (is_variable_length_v<tuple_element_t<0, ParamsProblemShape>>) {
        row_offset = get<0>(params_problem_shape).cumulative_length[get<2,1>(blk_coord)];
      }

      if (row_idx < get<0>(problem_shape)) {
        gLSE(row_idx + row_offset, get<2>(blk_coord)) = lse;
      }
    }

    pipeline_epi.producer_commit(pipeline_epi_producer_state);
    ++pipeline_epi_producer_state;

    copy(tiled_copy, tOrO, tOgO(_,_,_,_1{}));
    cutlass::arch::fence_view_async_shared();
    pipeline_epi.producer_acquire(pipeline_epi_producer_state);

    if (epilogue.params.ptr_LSE != nullptr) {
      int row_idx = thread_idx + get<0>(TileShape{}) * get<0>(blk_coord) + get<0>(TileShapeQK{});

      int row_offset = 0;
      if constexpr (is_variable_length_v<tuple_element_t<0, ParamsProblemShape>>) {
        row_offset = get<0>(params_problem_shape).cumulative_length[get<2,1>(blk_coord)];
      }

      if (row_idx < get<0>(problem_shape)) {
        gLSE(row_idx + row_offset, get<2>(blk_coord)) = lse;
      }
    }

    cutlass::arch::fence_view_async_shared();
    pipeline_epi.producer_commit(pipeline_epi_producer_state);
    ++pipeline_epi_producer_state;
  }

};

}  // namespace cutlass::fmha::collective
