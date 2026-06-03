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
#include "cute/tensor.hpp"
#include "cute/layout.hpp"

#include "collective/fmha_common.hpp"
#include "collective/fmha_fusion.hpp"
#include "cute/util/print_tensor.hpp"
namespace cutlass::fmha::collective {

using namespace cute;

template<
  class Element,
  class StrideQ,
  class StrideK,
  class StrideV,
  class CollectiveMmaQK,
  class CollectiveMmaPV,
  class SmemLayoutQ,
  class SmemLayoutK,
  class SmemLayoutV,
  class TensorStorage,
  class PipelineQ,
  class PipelineKV,
  class PipelineSFP,
  class Mask,
  class TileShape,
  class SmemLayoutSFQ,
  class SmemLayoutSFK,
  class SmemLayoutSFP,
  class SmemLayoutSFV
>
struct Sm100FmhaLoadTmaWarpspecialized {

  using TileShapeQK = typename CollectiveMmaQK::TileShape;
  using TileShapePV = typename CollectiveMmaPV::TileShape;
  using layout_SF = typename CollectiveMmaQK::Sm1xxBlkScaledConfig::LayoutSF;

  using ElementData = typename Element::DataType;
  using ElementScale = typename Element::ScaleFactorType;
  struct Arguments {
    const ElementData* ptr_Q; StrideQ dQ;
    const ElementScale* ptr_SFQ; layout_SF lQ;
    const ElementData* ptr_K; StrideK dK;
    const ElementScale* ptr_SFK; layout_SF lK;
    const ElementScale* ptr_SFP; layout_SF lP;
    const ElementData* ptr_V; StrideV dV;
    const ElementScale* ptr_SFV; layout_SF lV;
  };

  using TMA_Q = typename CollectiveMmaQK::Params::TMA_A;
  using TMA_K = typename CollectiveMmaQK::Params::TMA_B;
  using TMA_V = typename CollectiveMmaPV::Params::TMA_B;
  using TMA_SFQ = typename CollectiveMmaQK::Params::TMA_SFA;
  using TMA_SFK = typename CollectiveMmaQK::Params::TMA_SFB;
  using TMA_SFP = typename CollectiveMmaPV::Params::TMA_SFA;
  using TMA_SFV = typename CollectiveMmaPV::Params::TMA_SFB;

  struct Params {
    TMA_Q tma_load_q;
    TMA_K tma_load_k;
    TMA_V tma_load_v;
    TMA_SFQ tma_load_sfq;
    TMA_SFK tma_load_sfk;
    TMA_SFP tma_load_sfp;
    TMA_SFV tma_load_sfv;
    layout_SF lQ;
    layout_SF lK;
    layout_SF lP;
    layout_SF lV;

  };

  template<class ProblemShape>
  static Params to_underlying_arguments(
      ProblemShape const& problem_shape,
      Arguments const& args,
      void* workspace) {

    auto ptr_Q = args.ptr_Q;
    auto ptr_K = args.ptr_K;
    auto ptr_V = args.ptr_V;
    auto ptr_SFQ = args.ptr_SFQ;
    auto ptr_SFK = args.ptr_SFK;
    auto ptr_SFP = args.ptr_SFP;
    auto ptr_SFV = args.ptr_SFV;
    auto dQ = args.dQ;
    auto dK = args.dK;
    auto dV = args.dV;
    auto lQ = args.lQ;
    auto lK = args.lK;
    auto lP = args.lP;
    auto lV = args.lV;

    using IntProblemShape = cute::tuple<int, int, int, cute::tuple<cute::tuple<int, int>, int>>;

    IntProblemShape problem_shape_qk;
    if constexpr (is_variable_length_v<tuple_element_t<0, ProblemShape>>) {
      auto cumulative_length_q = get<0>(problem_shape).cumulative_length;
      auto cumulative_length_k = get<1>(problem_shape).cumulative_length;
      if (cumulative_length_q != nullptr && cumulative_length_k != nullptr ) {
          get<0>(problem_shape_qk) = get<0>(problem_shape).total_length;
          get<1>(problem_shape_qk) = get<1>(problem_shape).total_length;
          get<2>(problem_shape_qk) = get<2>(problem_shape);
          get<3>(problem_shape_qk) = get<3>(problem_shape);
      }
    } else {
      problem_shape_qk = problem_shape;
    }

    auto params_qk = CollectiveMmaQK::to_underlying_arguments(
        problem_shape_qk,
        typename CollectiveMmaQK::Arguments {
            ptr_Q, dQ,
            ptr_K, dK,
            ptr_SFQ, lQ,
            ptr_SFK, lK,
        }, /*workspace=*/ nullptr);

    auto problem_shape_pv = select<0,2,1,3>(problem_shape_qk);
    auto params_pv = CollectiveMmaPV::to_underlying_arguments(
        problem_shape_pv,
        typename CollectiveMmaPV::Arguments {
            ptr_K, dK,  // never used, dummy (P is in TMEM)
            ptr_V, select<1,0,2>(dV),
            ptr_SFP, lP,
            ptr_SFV, lV,
        }, /*workspace=*/ nullptr);

    return Params{
        params_qk.tma_load_a,
        params_qk.tma_load_b,
        params_pv.tma_load_b,
        params_qk.tma_load_sfa,
        params_qk.tma_load_sfb,
        params_pv.tma_load_sfa,
        params_pv.tma_load_sfb,
        lQ, lK, lP, lV
    };
  }


  CUTLASS_DEVICE
  static void prefetch_tma_descriptors(Params const& params) {
    cute::prefetch_tma_descriptor(params.tma_load_q.get_tma_descriptor());
    cute::prefetch_tma_descriptor(params.tma_load_k.get_tma_descriptor());
    cute::prefetch_tma_descriptor(params.tma_load_v.get_tma_descriptor());

    cute::prefetch_tma_descriptor(params.tma_load_sfq.get_tma_descriptor());
    cute::prefetch_tma_descriptor(params.tma_load_sfk.get_tma_descriptor());
    cute::prefetch_tma_descriptor(params.tma_load_sfp.get_tma_descriptor());
    cute::prefetch_tma_descriptor(params.tma_load_sfv.get_tma_descriptor());
  }

  template<class BlkCoord, class ProblemShape, class ParamsProblemShape>
  CUTLASS_DEVICE void
  load(
      BlkCoord const& blk_coord_in, ProblemShape const& problem_shape,
      Params const& params, ParamsProblemShape const& params_problem_shape,
      TensorStorage& storage,
      PipelineQ& pipeline_q, typename PipelineQ::PipelineState& pipeline_q_producer_state,
      PipelineKV& pipeline_kv, typename PipelineKV::PipelineState& pipeline_kv_producer_state,
      PipelineSFP& pipeline_sfp, typename PipelineSFP::PipelineState& pipeline_sfp_producer_state) {

    BlkCoord blk_coord_q = blk_coord_in;
    BlkCoord blk_coord_kv = blk_coord_in;

    int mask_tile_count = Mask{}.get_trip_count(blk_coord_in, TileShape{}, problem_shape);

    using X = Underscore;

    auto sf_l_coord = [&](auto const& bc) {
      return make_coord(_0{}, crd2idx(get<2>(bc), get<3>(problem_shape)));
    };
    // using ClusterShape = Shape<_1, _1, _1>;
    // uint32_t block_rank_in_cluster_ = cute::block_rank_in_cluster();
    // // Define the CTA-in-cluster Layout and Coord
    // Layout cta_layout_mnk  = make_layout(ClusterShape{});
    // Layout cta_layout_vmnk = tiled_divide(cta_layout_mnk, make_tile(typename CollectiveMmaQK::TiledMma::AtomThrID{}));
    // auto cta_coord_vmnk  = cta_layout_vmnk.get_flat_coord(block_rank_in_cluster_);

    // Layout cta_layout_sfb_vmnk = tiled_divide(cta_layout_mnk, make_tile(typename CollectiveMmaQK::TiledMMA_SF::AtomThrID{}));
    // auto cta_coord_sfb_vmnk  = cta_layout_sfb_vmnk.get_flat_coord(block_rank_in_cluster_);
    
    // compute gQ, sQ 
    ThrMMA mma_qk = typename CollectiveMmaQK::TiledMma{}.get_slice(0);

    Tensor mQ_qdl_p = params.tma_load_q.get_tma_tensor(select<0,2,3>(problem_shape));
    Tensor mQ_qdl_p_sf = params.tma_load_sfq.get_tma_tensor(shape(params.lQ));

    int q_offs_0 = 0;

    if constexpr (is_variable_length_v<tuple_element_t<0, ParamsProblemShape>>) {
      auto cumulative_length_q = get<0>(params_problem_shape).cumulative_length;
      if (cumulative_length_q != nullptr) {
        q_offs_0 = cumulative_length_q[get<2,1>(blk_coord_q)];
        get<2,1>(blk_coord_q) = 0;
      }
    }

    Tensor mQ_qdl = domain_offset(make_coord(q_offs_0, _0{}, make_coord(_0{}, _0{})), mQ_qdl_p);
    Tensor mQ_qdl_sf = domain_offset(make_coord(q_offs_0, _0{}, make_coord(_0{}, _0{})), mQ_qdl_p_sf);

    Tensor gQ_qdl = local_tile(mQ_qdl, TileShapeQK{}, make_coord(_, _, _), Step<_1, X, _1>{});
    Tensor gQ_qdl_sf = local_tile(mQ_qdl_sf, TileShapeQK{}, make_coord(_, _, _), Step<_1, X, _1>{});

    Tensor tSgQ_qdl = mma_qk.partition_A(gQ_qdl);
    Tensor tSgQ_qdl_sf = mma_qk.partition_A(gQ_qdl_sf);

    Tensor sQ = make_tensor(make_smem_ptr(storage.smem_q.data()), SmemLayoutQ{});
    Tensor sQ_sf = make_tensor(make_smem_ptr(storage.smem_sfq.data()), SmemLayoutSFQ{});

    auto [tQgQ_qdl, tQsQ] = tma_partition(
      params.tma_load_q, _0{}, make_layout(_1{}),
      group_modes<0,3>(sQ), group_modes<0,3>(tSgQ_qdl)
    );
    auto [tQgQ_qdl_sf, tQsQ_sf] = tma_partition(
      params.tma_load_sfq, _0{}, make_layout(_1{}),
      group_modes<0,3>(sQ_sf), group_modes<0,3>(tSgQ_qdl_sf)
    );

    Tensor tQgQ = tQgQ_qdl(_, _, _0{}, get<2>(blk_coord_q));
    Tensor tQgQ_sf = tQgQ_qdl_sf(_, _, _0{}, sf_l_coord(blk_coord_q));
    
    // Tensor tQgQ_sf = tQgQ_qdl_sf(_, _, _0{}, sf_l_coord(blk_coord_q));

    // compute gK, sK
    Tensor mK_kdl_p = params.tma_load_k.get_tma_tensor(select<1,2,3>(problem_shape));
    Tensor mK_kdl_sfp = params.tma_load_sfk.get_tma_tensor(shape(params.lK));

    int kv_offs_0 = 0;

    if constexpr (is_variable_length_v<tuple_element_t<1, ParamsProblemShape>>) {
      auto cumulative_length = get<1>(params_problem_shape).cumulative_length;
      if (cumulative_length != nullptr) {
        kv_offs_0 = cumulative_length[get<2,1>(blk_coord_kv)];
        get<2,1>(blk_coord_kv) = 0;
      }
    }

    Tensor mK_kdl = domain_offset(make_coord(kv_offs_0, _0{}, make_coord(_0{}, _0{})), mK_kdl_p);
    Tensor mK_kdl_sf = domain_offset(make_coord(kv_offs_0, _0{}, make_coord(_0{}, _0{})), mK_kdl_sfp);

    Tensor gK_kdl = local_tile(mK_kdl, TileShapeQK{}, make_coord(_, _, _), Step<X, _1, _1>{});
    Tensor gK_kdl_sf = local_tile(mK_kdl_sf, TileShapeQK{}, make_coord(_, _, _), Step<X, _1, _1>{});

    // Tensor gK_kdl_sf = local_tile(mK_kdl_sf, typename CollectiveMmaQK::TileShape_SF{}, make_coord(_, _, _), Step<X, _1, _1>{});
    Tensor tSgK_kdl = mma_qk.partition_B(gK_kdl);
    Tensor tSgK_kdl_sf = mma_qk.partition_B(gK_kdl_sf);

    Tensor sK = make_tensor(make_smem_ptr(storage.smem_k.data()), SmemLayoutK{});
    Tensor sK_sf = make_tensor(make_smem_ptr(storage.smem_sfk.data()), SmemLayoutSFK{});
    auto [tKgK_kdl, tKsK] = tma_partition(
      params.tma_load_k, _0{}, make_layout(_1{}),
      group_modes<0,3>(sK), group_modes<0,3>(tSgK_kdl)
    );
    auto [tKgK_kdl_sf, tKsK_sf] = tma_partition(
      params.tma_load_sfk, _0{}, make_layout(_1{}),
      group_modes<0,3>(sK_sf), group_modes<0,3>(tSgK_kdl_sf)
    );
    Tensor tKgK = tKgK_kdl(_, _, _0{}, get<2>(blk_coord_kv));
    Tensor tKgK_sf = tKgK_kdl_sf(_, _, _0{}, sf_l_coord(blk_coord_kv));
    // Tensor tKgK_sf = tKgK_kdl_sf(_, _, _0{},sf_l_coord(blk_coord_kv));

    // compute gV, sV
    ThrMMA mma_pv = typename CollectiveMmaPV::TiledMma{}.get_slice(0);
    Tensor mV_dkl_p = params.tma_load_v.get_tma_tensor(select<2,1,3>(problem_shape));
    Tensor mV_dkl_p_sf = params.tma_load_sfv.get_tma_tensor(shape(params.lV));

    Tensor mV_dkl = domain_offset(make_coord(_0{}, kv_offs_0, make_coord(_0{}, _0{})), mV_dkl_p);
    Tensor mV_dkl_sf = domain_offset(make_coord(_0{}, kv_offs_0, make_coord(_0{}, _0{})), mV_dkl_p_sf);

    Tensor gV_dkl = local_tile(mV_dkl, TileShapePV{}, make_coord(_, _, _), Step<X, _1, _1>{});
    Tensor gV_dkl_sf = local_tile(mV_dkl_sf, TileShapePV{}, make_coord(_, _, _), Step<X, _1, _1>{});

    Tensor tOgV_dkl = mma_pv.partition_B(gV_dkl);
    Tensor tOgV_dkl_sf = mma_pv.partition_B(gV_dkl_sf);

    Tensor sV = make_tensor(make_smem_ptr(storage.smem_v.data()), SmemLayoutV{});
    Tensor sV_sf = make_tensor(make_smem_ptr(storage.smem_sfv.data()), SmemLayoutSFV{});

    auto [tVgV_dkl, tVsV] = tma_partition(
      params.tma_load_v, _0{}, make_layout(_1{}),
      group_modes<0,3>(sV), group_modes<0,3>(tOgV_dkl)
    );
    auto [tVgV_dkl_sf, tVsV_sf] = tma_partition(
      params.tma_load_sfv, _0{}, make_layout(_1{}),
      group_modes<0,3>(sV_sf), group_modes<0,3>(tOgV_dkl_sf)
    );

    auto tVgV = tVgV_dkl(_, _0{}, _, get<2>(blk_coord_kv));
    auto tVgV_sf = tVgV_dkl_sf(_, _0{}, _, sf_l_coord(blk_coord_kv));

    // ============================================================
    // Set up P SF TMA loading
    // P is the A operand of PV MMA, shape (Q, K)
    // P SF uses the SFA partition (since P is operand A)
    // P SF is loaded alongside V using the same KV pipeline staging
    // ============================================================
    Tensor mP_sf_p = params.tma_load_sfp.get_tma_tensor(shape(params.lP));

    int p_offs_0 = q_offs_0;  // P rows correspond to Q rows
    int p_offs_1 = kv_offs_0; // P cols correspond to K cols

    Tensor mP_sf = domain_offset(make_coord(p_offs_0, p_offs_1, make_coord(_0{}, _0{})), mP_sf_p);

    // Tile P SF over (Q_tiles, K_tiles)
    // P SF uses SFA layout (A operand of PV MMA uses TileShapePV)
    Tensor gP_sf = local_tile(mP_sf, TileShapePV{}, make_coord(_, _, _), Step<_1, X, _1>{});

    Tensor tSgP_sf = mma_pv.partition_A(gP_sf);

    Tensor sP_sf = make_tensor(make_smem_ptr(storage.smem_sfp.data()), SmemLayoutSFP{});

    auto [tPgP_sf, tPsP_sf] = tma_partition(
      params.tma_load_sfp, _0{}, make_layout(_1{}),
      group_modes<0,3>(sP_sf), group_modes<0,3>(tSgP_sf)
    );

    auto tPgP_sf_view = tPgP_sf(_, _, _0{}, sf_l_coord(blk_coord_kv));

    uint32_t lane_predicate = cute::elect_one_sync();

    // ============================================================
    // Q1
    // ============================================================
    int q0_index = 2 * get<0>(blk_coord_q);
    int q1_index = 2 * get<0>(blk_coord_q) + 1;
    pipeline_q.producer_acquire(pipeline_q_producer_state);

    if (lane_predicate) {
      auto tma_barrier = pipeline_q.producer_get_barrier(pipeline_q_producer_state);
      copy(params.tma_load_q.with(*tma_barrier, 0), tQgQ(_, q0_index), tQsQ(_, pipeline_q_producer_state.index()));
      copy(params.tma_load_sfq.with(*tma_barrier, 0), tQgQ_sf(_, q0_index), tQsQ_sf(_, pipeline_q_producer_state.index()));
    }
    ++pipeline_q_producer_state;
    // ===== DEBUG: Verify SFQ loaded into SMEM (disabled - uncomment for debugging) =====
    // __syncthreads();
    // if (threadIdx.x == 384) {
    //   printf("[LOAD][CTA=(%d,%d,%d)] After Q1 load: smem_sfq stage=%d first 8: ",
    //          blockIdx.x, blockIdx.y, blockIdx.z, pipeline_q_producer_state.index());
    //   auto sQ_sf_check = make_tensor(make_smem_ptr(storage.smem_sfq.data()), SmemLayoutSFQ{});
    //   for (int i = 0; i < 8; i++) printf("%.6f ", (float)sQ_sf_check(i));
    //   printf("\n");
    // }
    // __syncthreads();
    // =============================================
    // ============================================================
    // K1
    // ============================================================
    int k_index = 0;
    int sfp_index = 0;
    pipeline_kv.producer_acquire(pipeline_kv_producer_state);
    if (lane_predicate) {
      auto tma_barrier = pipeline_kv.producer_get_barrier(pipeline_kv_producer_state);
      copy(params.tma_load_k.with(*tma_barrier, 0), tKgK(_, k_index), tKsK(_, pipeline_kv_producer_state.index()));
      copy(params.tma_load_sfk.with(*tma_barrier, 0), tKgK_sf(_, k_index), tKsK_sf(_, pipeline_kv_producer_state.index()));
    }
    ++pipeline_kv_producer_state;

    // ============================================================
    // Q2
    // ============================================================
    pipeline_q.producer_acquire(pipeline_q_producer_state);
    if (lane_predicate) {
      auto tma_barrier = pipeline_q.producer_get_barrier(pipeline_q_producer_state);
      copy(params.tma_load_q.with(*tma_barrier, 0), tQgQ(_, q1_index), tQsQ(_, pipeline_q_producer_state.index()));
      copy(params.tma_load_sfq.with(*tma_barrier, 0), tQgQ_sf(_, q1_index), tQsQ_sf(_, pipeline_q_producer_state.index()));
    }
    ++pipeline_q_producer_state;

    // ============================================================
    // V1 + P SF 1
    // ============================================================
    pipeline_kv.producer_acquire(pipeline_kv_producer_state);
    if (lane_predicate) {
      auto tma_barrier = pipeline_kv.producer_get_barrier(pipeline_kv_producer_state);
      copy(params.tma_load_v.with(*tma_barrier, 0), tVgV(_, k_index), tVsV(_, pipeline_kv_producer_state.index()));
      copy(params.tma_load_sfv.with(*tma_barrier, 0), tVgV_sf(_, k_index), tVsV_sf(_, pipeline_kv_producer_state.index()));
      // P SF loaded alongside V (same KV pipeline stage)
    }

    ++pipeline_kv_producer_state;
    k_index += 1;

    pipeline_sfp.producer_acquire(pipeline_sfp_producer_state);
    if(lane_predicate){
      auto tma_barrier_sfp = pipeline_sfp.producer_get_barrier(pipeline_sfp_producer_state);
      copy(params.tma_load_sfp.with(*tma_barrier_sfp, 0), tPgP_sf_view(_, 0), tPsP_sf(_, pipeline_sfp_producer_state.index()));
    }

    ++pipeline_sfp_producer_state;
    sfp_index += 1; 

    // ============================================================
    // Main load loop: Ki, Vi + P_SF_i
    // ============================================================
    mask_tile_count -= 1;
    // mask_tile_count = 1;
    for (; mask_tile_count > 0; mask_tile_count -= 1) {

      pipeline_sfp.producer_acquire(pipeline_sfp_producer_state);
      if (lane_predicate) {
        auto tma_barrier_sfp = pipeline_sfp.producer_get_barrier(pipeline_sfp_producer_state);
        copy(params.tma_load_sfp.with(*tma_barrier_sfp, 0), tPgP_sf_view(_, 1), tPsP_sf(_, pipeline_sfp_producer_state.index()));
      }
      ++pipeline_sfp_producer_state;
      sfp_index += 1;

      // Ki + K SF
      pipeline_kv.producer_acquire(pipeline_kv_producer_state);
      if (lane_predicate) {
        auto tma_barrier = pipeline_kv.producer_get_barrier(pipeline_kv_producer_state);
        copy(params.tma_load_k.with(*tma_barrier, 0), tKgK(_, k_index), tKsK(_, pipeline_kv_producer_state.index()));
        copy(params.tma_load_sfk.with(*tma_barrier, 0), tKgK_sf(_, k_index), tKsK_sf(_, pipeline_kv_producer_state.index()));
      }
      ++pipeline_kv_producer_state;


      // Vi + V SF + P SF
      pipeline_kv.producer_acquire(pipeline_kv_producer_state);
      if (lane_predicate) {
        auto tma_barrier = pipeline_kv.producer_get_barrier(pipeline_kv_producer_state);
        copy(params.tma_load_v.with(*tma_barrier, 0), tVgV(_, k_index), tVsV(_, pipeline_kv_producer_state.index()));
        copy(params.tma_load_sfv.with(*tma_barrier, 0), tVgV_sf(_, k_index), tVsV_sf(_, pipeline_kv_producer_state.index()));
        // P SF loaded alongside V (same KV pipeline stage)
      }
      ++pipeline_kv_producer_state;
      k_index += 1;
      pipeline_sfp.producer_acquire(pipeline_sfp_producer_state);
      if (lane_predicate) {
        auto tma_barrier_sfp = pipeline_sfp.producer_get_barrier(pipeline_sfp_producer_state);
        copy(params.tma_load_sfp.with(*tma_barrier_sfp, 0), tPgP_sf_view(_, 0), tPsP_sf(_, pipeline_sfp_producer_state.index()));
      }
      ++pipeline_sfp_producer_state;
      sfp_index += 1;

      // if(blockIdx.x==0 && blockIdx.y==0){
      //   // printf("K_index: %d\n",k_index);
      //   printf("sp_index: %d\n",sfp_index);
      // }
    }

    pipeline_sfp.producer_acquire(pipeline_sfp_producer_state);
    if (lane_predicate) {
      auto tma_barrier_sfp = pipeline_sfp.producer_get_barrier(pipeline_sfp_producer_state);
      copy(params.tma_load_sfp.with(*tma_barrier_sfp, 0), tPgP_sf_view(_, 0), tPsP_sf(_, pipeline_sfp_producer_state.index()));
    }
    ++pipeline_sfp_producer_state;
    sfp_index += 1;
  }
};

}  // namespace cutlass::fmha::collective
