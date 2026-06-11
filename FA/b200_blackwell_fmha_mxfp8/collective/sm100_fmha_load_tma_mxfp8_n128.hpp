/***************************************************************************************************
 * Operator 6 — Milestone 2 / Route C : MXFP8 FlashAttention
 * File 3 of 3 : SF-aware TMA load collective.
 *
 * Derived from CUTLASS example 77
 *   examples/77_blackwell_fmha/collective/sm100_fmha_load_tma_warpspecialized.hpp
 * (pristine CUTLASS is never edited; this is a standalone modified copy).
 *
 * Route C changes vs the pristine load collective — all marked `// [MXFP8]`:
 *   - Two extra TMA tensormaps: SFA (Q scale factors) and SFB (K scale factors),
 *     obtained from the block-scaled QK CollectiveMma's Params.
 *   - SFA rides the Q pipeline (one SF tile per Q slot).
 *   - SFB rides the KV pipeline: real K scale factors on K-slots, and the same
 *     SF-sized payload re-loaded as ignored filler on V-slots, so every KV-slot
 *     TMA transaction delivers an identical byte count (K|V data + SF).
 *   - SFB partitioning mirrors the block-scaled GEMM collective's load_init,
 *     including the IsCtaN64 reshape (QK N-tile is 64 in Route C).
 ***************************************************************************************************/
#pragma once

#include "cutlass/cutlass.h"
#include "cutlass/arch/memory_sm80.h"
#include "cutlass/gemm/collective/collective_builder.hpp"
#include "cute/tensor.hpp"
#include "cute/layout.hpp"

#include "collective/fmha_common.hpp"
#include "collective/fmha_fusion.hpp"

namespace cutlass::fmha::collective {

using namespace cute;

template<
  class Element,
  class ElementSF,                 // [MXFP8] scale-factor element (float_ue8m0_t)
  class StrideQ,
  class StrideK,
  class StrideV,
  class CollectiveMmaQK,
  class CollectiveMmaPV,
  class SmemLayoutQ,
  class SmemLayoutK,
  class SmemLayoutV,
  class SmemLayoutSFQ,             // [MXFP8] SF smem layout, staged on Q pipeline
  class SmemLayoutSFK,             // [MXFP8] SF smem layout, staged on KV pipeline
  class SmemLayoutSFP,
  class SmemLayoutSFV,             // [PVMX 2a.1b] V-SF smem layout, staged on KV pipeline
  class TensorStorage,
  class PipelineQ,
  class PipelineKV,
  class PipelineSFP,
  class Mask,
  class TileShape
>
struct Sm100FmhaLoadTmaWarpspecializedMxfp8 {

  using TileShapeQK = typename CollectiveMmaQK::TileShape;
  using TileShapePV = typename CollectiveMmaPV::TileShape;

  // [MXFP8] SF layout / tile types exposed by the block-scaled QK collective.
  using LayoutSFA = typename CollectiveMmaQK::LayoutSFA;
  using LayoutSFB = typename CollectiveMmaQK::LayoutSFB;
  // [PVMX 2a.1] PV-side SF layouts (PV problem: M=seqlen_q, N=D, K=seqlen_kv).
  using LayoutSFP = typename CollectiveMmaPV::LayoutSFA;   // P-SF loaded from global memory
  using LayoutSFV = typename CollectiveMmaPV::LayoutSFB;   // V-SF (real)

  struct Arguments {
    const Element* ptr_Q;
    StrideQ dQ;
    const Element* ptr_K;
    StrideK dK;
    const Element* ptr_V;
    StrideV dV;
    // [MXFP8] scale factors for the block-scaled QK GEMM.
    const ElementSF* ptr_SFA;
    LayoutSFA layout_SFA;
    const ElementSF* ptr_SFB;
    LayoutSFB layout_SFB;
    // [PVMX 2a.1] scale factors for the block-scaled PV GEMM.
    const ElementSF* ptr_SFP;
    LayoutSFP layout_SFP;
    const ElementSF* ptr_SFV;
    LayoutSFV layout_SFV;
  };

  using TMA_Q = typename CollectiveMmaQK::Params::TMA_A;
  using TMA_K = typename CollectiveMmaQK::Params::TMA_B;
  using TMA_V = typename CollectiveMmaPV::Params::TMA_B;
  using TMA_SFA = typename CollectiveMmaQK::Params::TMA_SFA;   // [MXFP8]
  using TMA_SFB = typename CollectiveMmaQK::Params::TMA_SFB;   // [MXFP8]
  using TMA_SFP = typename CollectiveMmaPV::Params::TMA_SFA;   // [MXFP8]
  using TMA_SFV = typename CollectiveMmaPV::Params::TMA_SFB;   // [PVMX 2a.1] V-SF TMA (PV SFB)

  struct Params {
    TMA_Q tma_load_q;
    TMA_K tma_load_k;
    TMA_V tma_load_v;
    TMA_SFA tma_load_sfa;          // [MXFP8]
    TMA_SFB tma_load_sfb;          // [MXFP8]
    LayoutSFA layout_SFA;          // [MXFP8] needed for get_tma_tensor(shape(...))
    LayoutSFB layout_SFB;          // [MXFP8]
    TMA_SFP tma_load_sfp;          
    LayoutSFP layout_SFP;          
    TMA_SFV tma_load_sfv;          // [PVMX 2a.1] V-SF
    LayoutSFV layout_SFV;          // [PVMX 2a.1]
  };

  template<class ProblemShape>
  static Params to_underlying_arguments(
      ProblemShape const& problem_shape,
      Arguments const& args,
      void* workspace) {

    auto ptr_Q = args.ptr_Q;
    auto ptr_K = args.ptr_K;
    auto ptr_V = args.ptr_V;
    auto dQ = args.dQ;
    auto dK = args.dK;
    auto dV = args.dV;

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

    // [MXFP8] block-scaled QK collective Arguments carry the SF pointers/layouts;
    // its to_underlying_arguments builds the SFA / SFB TMA tensormaps for us.
    auto params_qk = CollectiveMmaQK::to_underlying_arguments(
        problem_shape_qk,
        typename CollectiveMmaQK::Arguments {
            ptr_Q, dQ,
            ptr_K, dK,
            args.ptr_SFA, args.layout_SFA,
            args.ptr_SFB, args.layout_SFB
        }, /*workspace=*/ nullptr);

    auto problem_shape_pv = select<0,2,1,3>(problem_shape_qk);
    auto params_pv = CollectiveMmaPV::to_underlying_arguments(
        problem_shape_pv,
        typename CollectiveMmaPV::Arguments {
            ptr_K, dK,  // dummy A (P is produced on-chip, not loaded)
            ptr_V, select<1,0,2>(dV),
            args.ptr_SFP, args.layout_SFP,   // [PVMX 2a.1] P-SF
            args.ptr_SFV, args.layout_SFV    // [PVMX 2a.1] V-SF (real)
        }, /*workspace=*/ nullptr);

    return Params{
        params_qk.tma_load_a,
        params_qk.tma_load_b,
        params_pv.tma_load_b,
        params_qk.tma_load_sfa,
        params_qk.tma_load_sfb,
        args.layout_SFA,
        args.layout_SFB,
        params_pv.tma_load_sfa,          // [PVMX 2a.1] V-SF TMA (PV SFB)
        args.layout_SFP,
        params_pv.tma_load_sfb,          // [PVMX 2a.1] V-SF TMA (PV SFB)
        args.layout_SFV
    };
  }


  CUTLASS_DEVICE
  static void prefetch_tma_descriptors(Params const& params) {
    cute::prefetch_tma_descriptor(params.tma_load_q.get_tma_descriptor());
    cute::prefetch_tma_descriptor(params.tma_load_k.get_tma_descriptor());
    cute::prefetch_tma_descriptor(params.tma_load_v.get_tma_descriptor());
    cute::prefetch_tma_descriptor(params.tma_load_sfa.get_tma_descriptor());   // [MXFP8]
    cute::prefetch_tma_descriptor(params.tma_load_sfb.get_tma_descriptor());   // [MXFP8]
    cute::prefetch_tma_descriptor(params.tma_load_sfp.get_tma_descriptor());   // [PVMX 2b]
    cute::prefetch_tma_descriptor(params.tma_load_sfv.get_tma_descriptor());   // [PVMX 2a.1b]
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

    // [MXFP8] The SF tensors carry a FLAT L = (_1, H*B): tile_atom_to_shape_SF*
    // collapses the (head,batch) tuple into one axis (leading sub-mode is a
    // static (_1):(_0) dummy). The kernel's block coord carries the nested L
    // coord of problem_shape's L. crd2idx colex-linearises it into the flat
    // (head + H*batch) index the driver repacks the SF buffers with, so each
    // head/batch hits its own SF slot.
    auto sf_l_coord = [&](auto const& bc) {
      return make_coord(_0{}, crd2idx(get<2>(bc), get<3>(problem_shape)));
    };

    ThrMMA mma_qk = typename CollectiveMmaQK::TiledMma{}.get_slice(0);
    Tensor mQ_qdl_p = params.tma_load_q.get_tma_tensor(select<0,2,3>(problem_shape));

    int q_offs_0 = 0;

    if constexpr (is_variable_length_v<tuple_element_t<0, ParamsProblemShape>>) {
      auto cumulative_length_q = get<0>(params_problem_shape).cumulative_length;
      if (cumulative_length_q != nullptr) {
        q_offs_0 = cumulative_length_q[get<2,1>(blk_coord_q)];
        get<2,1>(blk_coord_q) = 0;
      }
    }

    Tensor mQ_qdl = domain_offset(make_coord(q_offs_0, _0{}, make_coord(_0{}, _0{})), mQ_qdl_p);

    Tensor gQ_qdl = local_tile(mQ_qdl, TileShapeQK{}, make_coord(_, _, _), Step<_1, X, _1>{});
    Tensor tSgQ_qdl = mma_qk.partition_A(gQ_qdl);
    Tensor sQ = make_tensor(make_smem_ptr(storage.smem_q.data()), SmemLayoutQ{});
    auto [tQgQ_qdl, tQsQ] = tma_partition(
      params.tma_load_q, _0{}, make_layout(_1{}),
      group_modes<0,3>(sQ), group_modes<0,3>(tSgQ_qdl)
    );
    Tensor tQgQ = tQgQ_qdl(_, _, _0{}, get<2>(blk_coord_q));

    // compute gK, sK
    Tensor mK_kdl_p = params.tma_load_k.get_tma_tensor(select<1,2,3>(problem_shape));

    int kv_offs_0 = 0;

    if constexpr (is_variable_length_v<tuple_element_t<1, ParamsProblemShape>>) {
      auto cumulative_length = get<1>(params_problem_shape).cumulative_length;
      if (cumulative_length != nullptr) {
        kv_offs_0 = cumulative_length[get<2,1>(blk_coord_kv)];
        get<2,1>(blk_coord_kv) = 0;
      }
    }

    Tensor mK_kdl = domain_offset(make_coord(kv_offs_0, _0{}, make_coord(_0{}, _0{})), mK_kdl_p);

    Tensor gK_kdl = local_tile(mK_kdl, TileShapeQK{}, make_coord(_, _, _), Step<X, _1, _1>{});
    Tensor tSgK_kdl = mma_qk.partition_B(gK_kdl);
    Tensor sK = make_tensor(make_smem_ptr(storage.smem_k.data()), SmemLayoutK{});
    auto [tKgK_kdl, tKsK] = tma_partition(
      params.tma_load_k, _0{}, make_layout(_1{}),
      group_modes<0,3>(sK), group_modes<0,3>(tSgK_kdl)
    );
    Tensor tKgK = tKgK_kdl(_, _, _0{}, get<2>(blk_coord_kv));

    // compute gV, sV
    ThrMMA mma_pv = typename CollectiveMmaPV::TiledMma{}.get_slice(0);
    Tensor mV_dkl_p = params.tma_load_v.get_tma_tensor(select<2,1,3>(problem_shape));

    Tensor mV_dkl = domain_offset(make_coord(_0{}, kv_offs_0, make_coord(_0{}, _0{})), mV_dkl_p);

    Tensor gV_dkl = local_tile(mV_dkl, TileShapePV{}, make_coord(_, _, _), Step<X, _1, _1>{});
    Tensor tOgV_dkl = mma_pv.partition_B(gV_dkl);
    Tensor sV = make_tensor(make_smem_ptr(storage.smem_v.data()), SmemLayoutV{});
    auto [tVgV_dkl, tVsV] = tma_partition(
      params.tma_load_v, _0{}, make_layout(_1{}),
      group_modes<0,3>(sV), group_modes<0,3>(tOgV_dkl)
    );
    auto tVgV = tVgV_dkl(_, _0{}, _, get<2>(blk_coord_kv));

    // ===================================================================
    // [MXFP8] SFA (Q scale factors) — rides the Q pipeline.
    // ===================================================================
    Tensor mSFA = params.tma_load_sfa.get_tma_tensor(shape(params.layout_SFA));
    Tensor gSFA = local_tile(mSFA, TileShapeQK{}, make_coord(_, _, _), Step<_1, X, _1>{});
    Tensor tSgSFA = mma_qk.partition_A(gSFA);
    Tensor sSFQ = make_tensor(make_smem_ptr(storage.smem_sfq.data()), SmemLayoutSFQ{});
    auto [tQgSFA_qdl, tQsSFQ] = tma_partition(
      params.tma_load_sfa, _0{}, make_layout(_1{}),
      group_modes<0,3>(sSFQ), group_modes<0,3>(tSgSFA)
    );
    Tensor tQgSFA = tQgSFA_qdl(_, _, _0{}, sf_l_coord(blk_coord_q));

    // ===================================================================
    // [MXFP8] SFB (K scale factors) — rides the KV pipeline.
    // QK N-tile is 64 in Route C => IsCtaN64; mirror the block-scaled GEMM
    // collective's load_init reshape of the SFB tma tensor.
    // ===================================================================
    auto mSFB = [&]() {
      if constexpr (CollectiveMmaQK::IsCtaN64) {
        Tensor t = params.tma_load_sfb.get_tma_tensor(shape(params.layout_SFB));
        auto new_shape  = make_shape (make_shape(shape<0,0>(t),
                                      make_shape(_2{}, shape<0,1>(t))), shape<1>(t), shape<2>(t));
        auto new_stride = make_stride(make_stride(stride<0,0>(t),
                                      make_stride(_0{}, stride<0,1>(t))), stride<1>(t), stride<2>(t));
        return make_tensor(t.data(), make_layout(new_shape, new_stride));
      }
      else {
        return params.tma_load_sfb.get_tma_tensor(shape(params.layout_SFB));
      }
    }();
    Tensor gSFB = local_tile(mSFB, typename CollectiveMmaQK::TileShape_SF{}, make_coord(_, _, _), Step<X, _1, _1>{});
    ThrMMA mma_sfb = typename CollectiveMmaQK::TiledMMA_SF{}.get_slice(0);
    Tensor tSgSFB = mma_sfb.partition_B(gSFB);
    Tensor sSFK = make_tensor(make_smem_ptr(storage.smem_sfk.data()), SmemLayoutSFK{});
    auto [tKgSFB_kdl, tKsSFK] = tma_partition(
      params.tma_load_sfb, _0{}, make_layout(_1{}),
      group_modes<0,3>(sSFK), group_modes<0,3>(tSgSFB)
    );
    Tensor tKgSFB = tKgSFB_kdl(_, _, _0{}, sf_l_coord(blk_coord_kv));

    // ===================================================================
    // [PVMX 2a.1b] SFV (V scale factors) — rides the KV pipeline.
    // PV problem: (M=seqlen_q, N=D, K=seqlen_kv).
    // SFV layout shape: (D, K_blocks, L); partition_B selects (N, K_tile).
    // ===================================================================
    Tensor mSFV = params.tma_load_sfv.get_tma_tensor(shape(params.layout_SFV));
    Tensor gSFV = local_tile(mSFV, typename CollectiveMmaPV::TileShape_SF{}, make_coord(_, _, _), Step<X, _1, _1>{});
    ThrMMA mma_sfv = typename CollectiveMmaPV::TiledMMA_SF{}.get_slice(0);
    Tensor tSgSFV = mma_sfv.partition_B(gSFV);
    Tensor sSFV = make_tensor(make_smem_ptr(storage.smem_sfv.data()), SmemLayoutSFV{});
    auto [tVgSFV_kdl, tVsSFV] = tma_partition(
      params.tma_load_sfv, _0{}, make_layout(_1{}),
      group_modes<0,3>(sSFV), group_modes<0,3>(tSgSFV)
    );
    Tensor tVgSFV = tVgSFV_kdl(_, _0{}, _, sf_l_coord(blk_coord_kv));

    Tensor mP_sf_p = params.tma_load_sfp.get_tma_tensor(shape(params.layout_SFP));

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

    uint32_t lane_predicate = cute::elect_one_sync();

    // [MXFP8 N128] single-stage: the CTA loads ONE Q tile (M=128). The dual-
    // stage q0/q1 = 2*blk, 2*blk+1 split is gone — q0_index is the CTA's tile.
    int q0_index = get<0>(blk_coord_q);
    auto tPgP_sf_view = tPgP_sf(_, _, _0{}, make_coord(_0{}, _0{}));
    pipeline_q.producer_acquire(pipeline_q_producer_state);
    if (lane_predicate) {
      auto tma_barrier = pipeline_q.producer_get_barrier(pipeline_q_producer_state);
      copy(params.tma_load_q.with(*tma_barrier, 0), tQgQ(_, q0_index), tQsQ(_, pipeline_q_producer_state.index()));
      copy(params.tma_load_sfa.with(*tma_barrier, 0), tQgSFA(_, q0_index), tQsSFQ(_, pipeline_q_producer_state.index()));  // [MXFP8]
    }
    ++pipeline_q_producer_state;

    // K1 (+ SFB1)
    int k_index = 0;
    int sfp_index = 0;
    pipeline_kv.producer_acquire(pipeline_kv_producer_state);
    if (lane_predicate) {
      auto tma_barrier = pipeline_kv.producer_get_barrier(pipeline_kv_producer_state);
      copy(params.tma_load_k.with(*tma_barrier, 0), tKgK(_, k_index), tKsK(_, pipeline_kv_producer_state.index()));
      copy(params.tma_load_sfb.with(*tma_barrier, 0), tKgSFB(_, k_index), tKsSFK(_, pipeline_kv_producer_state.index()));  // [MXFP8]
    }
    ++pipeline_kv_producer_state;

    // [MXFP8 N128] single-stage: no second Q tile is loaded.

    // V1 (+ SFV1) — V-slot now carries real V-SF (was K-SF filler).
    pipeline_kv.producer_acquire(pipeline_kv_producer_state);
    if (lane_predicate) {
      auto tma_barrier = pipeline_kv.producer_get_barrier(pipeline_kv_producer_state);
      copy(params.tma_load_v.with(*tma_barrier, 0), tVgV(_, k_index), tVsV(_, pipeline_kv_producer_state.index()));
      copy(params.tma_load_sfv.with(*tma_barrier, 0), tVgSFV(_, k_index), tVsSFV(_, pipeline_kv_producer_state.index()));  // [PVMX 2a.1b]
    }
    ++pipeline_kv_producer_state;
    k_index += 1;

    // loop:
    mask_tile_count -= 1;
    for (; mask_tile_count > 0; mask_tile_count -= 1) {

      // Ki (+ SFBi)
      pipeline_kv.producer_acquire(pipeline_kv_producer_state);
      if (lane_predicate) {
        auto tma_barrier = pipeline_kv.producer_get_barrier(pipeline_kv_producer_state);
        copy(params.tma_load_k.with(*tma_barrier, 0), tKgK(_, k_index), tKsK(_, pipeline_kv_producer_state.index()));
        copy(params.tma_load_sfb.with(*tma_barrier, 0), tKgSFB(_, k_index), tKsSFK(_, pipeline_kv_producer_state.index()));  // [MXFP8]

        // Match the Blackwell MLA load pipeline: warm the current V tile while
        // K is in flight so the following V TMA does not pay the full L2
        // lookup/request latency on its critical path.
        cute::prefetch(params.tma_load_v, tVgV(_, k_index));
      }
      ++pipeline_kv_producer_state;

      // Vi (+ SFVi) — V-slot now carries real V-SF.
      pipeline_kv.producer_acquire(pipeline_kv_producer_state);
      if (lane_predicate) {
        auto tma_barrier = pipeline_kv.producer_get_barrier(pipeline_kv_producer_state);
        copy(params.tma_load_v.with(*tma_barrier, 0), tVgV(_, k_index), tVsV(_, pipeline_kv_producer_state.index()));
        copy(params.tma_load_sfv.with(*tma_barrier, 0), tVgSFV(_, k_index), tVsSFV(_, pipeline_kv_producer_state.index()));  // [PVMX 2a.1b]

      }
      ++pipeline_kv_producer_state;
      k_index += 1;

      pipeline_sfp.producer_acquire(pipeline_sfp_producer_state);
      if (lane_predicate) {
        auto tma_barrier_sfp = pipeline_sfp.producer_get_barrier(pipeline_sfp_producer_state);
        copy(params.tma_load_sfp.with(*tma_barrier_sfp, 0), tPgP_sf_view(_, sfp_index), tPsP_sf(_, pipeline_sfp_producer_state.index()));
      }
      ++pipeline_sfp_producer_state;
      ++sfp_index;
    }
    pipeline_sfp.producer_acquire(pipeline_sfp_producer_state);
      if (lane_predicate) {
        auto tma_barrier_sfp = pipeline_sfp.producer_get_barrier(pipeline_sfp_producer_state);
        copy(params.tma_load_sfp.with(*tma_barrier_sfp, 0), tPgP_sf_view(_, sfp_index), tPsP_sf(_, pipeline_sfp_producer_state.index()));
      }
      ++pipeline_sfp_producer_state;
      ++sfp_index;
  }
};

}  // namespace cutlass::fmha::collective
