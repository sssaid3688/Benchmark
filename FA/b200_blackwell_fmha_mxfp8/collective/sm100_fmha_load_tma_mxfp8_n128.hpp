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
    // [real static SFP] raw pointer for the softmax warps' direct (L2) reads of
    // the static P scale factors (the smem copy feeds the UTCCP/MMA path).
    const ElementSF* ptr_SFP;
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
        params_pv.tma_load_sfa,          // [PVMX 2a.1] P-SF TMA (PV SFA)
        args.layout_SFP,
        params_pv.tma_load_sfb,          // [PVMX 2a.1] V-SF TMA (PV SFB)
        args.layout_SFV,
        args.ptr_SFP                     // [real static SFP] raw gmem pointer
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

    // ===================================================================
    // [2-CTA] cluster-coordinate setup. For 1-CTA (AtomThrID == 1) every quantity
    // below reduces to the original single-CTA behaviour (rank 0, trivial layout,
    // self-only mcast mask), so this path is shared by both builds.
    //   - QK/PV atoms split the M=256 accumulator across the CTA pair (AtomThrID=2).
    //   - A operands (Q, SFA, SFP) are M-split   -> project tma along n-mode get<2>.
    //   - B operands (K, V) are N-split halves   -> project tma along m-mode get<1>.
    //   - B-side SFs (SFK, SFV) are FULL tiles in both CTAs' smem via the SF MMA's
    //     own (AtomThrID=1) layout + pair-wide multicast (pristine SFB pattern).
    // ===================================================================
    using AtomThrIDQK = typename CollectiveMmaQK::TiledMma::AtomThrID;
    using AtomThrIDSF = typename CollectiveMmaQK::TiledMMA_SF::AtomThrID;
    constexpr int kMmaThr = cute::size(AtomThrIDQK{});
    int mma_rank = int(cute::block_rank_in_cluster()) % kMmaThr;  // 0/1 = which M-half this CTA owns
    auto cluster_shape_mnk = make_shape(Int<kMmaThr>{}, _1{}, _1{});
    auto cta_layout_vmnk     = tiled_divide(make_layout(cluster_shape_mnk), make_tile(AtomThrIDQK{}));
    auto cta_coord_vmnk      = cta_layout_vmnk.get_flat_coord(cute::block_rank_in_cluster());
    auto cta_layout_sf_vmnk  = tiled_divide(make_layout(cluster_shape_mnk), make_tile(AtomThrIDSF{}));
    auto cta_coord_sf_vmnk   = cta_layout_sf_vmnk.get_flat_coord(cute::block_rank_in_cluster());
    // multicast masks: A/B data ops are non-multicast under (2,1,1) (mask ignored);
    // the SF B-side ops are true pair multicasts (mask = both pair bits).
    uint16_t mcast_a   = cutlass::create_tma_multicast_mask<2>(cta_layout_vmnk, cta_coord_vmnk);
    uint16_t mcast_b   = cutlass::create_tma_multicast_mask<1>(cta_layout_vmnk, cta_coord_vmnk);
    uint16_t mcast_sfb = cutlass::create_tma_multicast_mask<1>(cta_layout_sf_vmnk, cta_coord_sf_vmnk);

    ThrMMA mma_qk = typename CollectiveMmaQK::TiledMma{}.get_slice(mma_rank);
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
      params.tma_load_q, get<2>(cta_coord_vmnk), make_layout(size<2>(cta_layout_vmnk)),
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
      params.tma_load_k, get<1>(cta_coord_vmnk), make_layout(size<1>(cta_layout_vmnk)),
      group_modes<0,3>(sK), group_modes<0,3>(tSgK_kdl)
    );
    Tensor tKgK = tKgK_kdl(_, _, _0{}, get<2>(blk_coord_kv));

    // compute gV, sV
    ThrMMA mma_pv = typename CollectiveMmaPV::TiledMma{}.get_slice(mma_rank);
    Tensor mV_dkl_p = params.tma_load_v.get_tma_tensor(select<2,1,3>(problem_shape));

    Tensor mV_dkl = domain_offset(make_coord(_0{}, kv_offs_0, make_coord(_0{}, _0{})), mV_dkl_p);

    Tensor gV_dkl = local_tile(mV_dkl, TileShapePV{}, make_coord(_, _, _), Step<X, _1, _1>{});
    Tensor tOgV_dkl = mma_pv.partition_B(gV_dkl);
    Tensor sV = make_tensor(make_smem_ptr(storage.smem_v.data()), SmemLayoutV{});
    auto [tVgV_dkl, tVsV] = tma_partition(
      params.tma_load_v, get<1>(cta_coord_vmnk), make_layout(size<1>(cta_layout_vmnk)),
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
      params.tma_load_sfa, get<2>(cta_coord_vmnk), make_layout(size<2>(cta_layout_vmnk)),
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
    ThrMMA mma_sfb = typename CollectiveMmaQK::TiledMMA_SF{}.get_slice(int(cute::block_rank_in_cluster()) % int(cute::size(AtomThrIDSF{})));
    Tensor tSgSFB = mma_sfb.partition_B(gSFB);
    Tensor sSFK = make_tensor(make_smem_ptr(storage.smem_sfk.data()), SmemLayoutSFK{});
    auto [tKgSFB_kdl, tKsSFK] = tma_partition(
      params.tma_load_sfb, get<1>(cta_coord_sf_vmnk), make_layout(size<1>(cta_layout_sf_vmnk)),
      group_modes<0,3>(sSFK), group_modes<0,3>(tSgSFB)
    );
    Tensor tKgSFB = tKgSFB_kdl(_, _, _0{}, sf_l_coord(blk_coord_kv));

    // ===================================================================
    // [PVMX 2a.1b] SFV (V scale factors) — rides the KV pipeline.
    // PV problem: (M=seqlen_q, N=D, K=seqlen_kv).
    // SFV layout shape: (D, K_blocks, L); partition_B selects (N, K_tile).
    // ===================================================================
    // [2-CTA] SFV is the PV MMA's B-side scale factor: like SFK it is NOT N-split —
    // each CTA needs the FULL SFV tile (the merged-B MMA reads all of it), delivered
    // by the pair-wide SF multicast. Partition with the SF MMA's own (AtomThrID=1)
    // cluster layout — NOT the data-B layout — mirroring the SFK path above.
    using AtomThrIDSFV = typename CollectiveMmaPV::TiledMMA_SF::AtomThrID;
    auto cta_layout_sfv_vmnk = tiled_divide(make_layout(cluster_shape_mnk), make_tile(AtomThrIDSFV{}));
    auto cta_coord_sfv_vmnk  = cta_layout_sfv_vmnk.get_flat_coord(cute::block_rank_in_cluster());
    uint16_t mcast_sfv = cutlass::create_tma_multicast_mask<1>(cta_layout_sfv_vmnk, cta_coord_sfv_vmnk);
    Tensor mSFV = params.tma_load_sfv.get_tma_tensor(shape(params.layout_SFV));
    Tensor gSFV = local_tile(mSFV, typename CollectiveMmaPV::TileShape_SF{}, make_coord(_, _, _), Step<X, _1, _1>{});
    ThrMMA mma_sfv = typename CollectiveMmaPV::TiledMMA_SF{}.get_slice(int(cute::block_rank_in_cluster()) % int(cute::size(AtomThrIDSFV{})));
    Tensor tSgSFV = mma_sfv.partition_B(gSFV);
    Tensor sSFV = make_tensor(make_smem_ptr(storage.smem_sfv.data()), SmemLayoutSFV{});
    auto [tVgSFV_kdl, tVsSFV] = tma_partition(
      params.tma_load_sfv, get<1>(cta_coord_sfv_vmnk), make_layout(size<1>(cta_layout_sfv_vmnk)),
      group_modes<0,3>(sSFV), group_modes<0,3>(tSgSFV)
    );
    Tensor tVgSFV = tVgSFV_kdl(_, _0{}, _, sf_l_coord(blk_coord_kv));

    // [SFP-on-K-slot] the per-tile SFP rides the K(k) (and, as an equal-bytes
    // filler, the V(k)) TMA transaction into smem_sfp[k%2]. softmax(k) is gated
    // by the QK(k) commit, which the MMA only issues after consumer_wait(K(k)) —
    // so the SFP bytes are transitively visible to softmax with NO extra
    // pipeline. The dedicated SFP pipeline stays unused.
    (void) pipeline_sfp; (void) pipeline_sfp_producer_state;
    Tensor mP_sf_p = params.tma_load_sfp.get_tma_tensor(shape(params.layout_SFP));
    Tensor mP_sf = domain_offset(make_coord(q_offs_0, kv_offs_0, make_coord(_0{}, _0{})), mP_sf_p);
    Tensor gP_sf = local_tile(mP_sf, TileShapePV{}, make_coord(_, _, _), Step<_1, X, _1>{});
    Tensor tSgP_sf = mma_pv.partition_A(gP_sf);   // [2-CTA] M-split: own 128 P rows per CTA
    Tensor sP_sf = make_tensor(make_smem_ptr(storage.smem_sfp.data()), SmemLayoutSFP{});
    auto [tPgP_sf, tPsP_sf] = tma_partition(
      params.tma_load_sfp, get<2>(cta_coord_vmnk), make_layout(size<2>(cta_layout_vmnk)),
      group_modes<0,3>(sP_sf), group_modes<0,3>(tSgP_sf)
    );

    uint32_t lane_predicate = cute::elect_one_sync();

    // [MXFP8 N128] single-stage: the CTA loads ONE Q tile (M=128). The dual-
    // stage q0/q1 = 2*blk, 2*blk+1 split is gone — q0_index is the CTA's tile.
    int q0_index = get<0>(blk_coord_q);
    // [SFP-on-K-slot] this q-tile's SFP, per-KV-tile slices, head/batch coord.
    auto tPgSFP = tPgP_sf(_, q0_index, _, sf_l_coord(blk_coord_q));
    pipeline_q.producer_acquire(pipeline_q_producer_state);
    if (lane_predicate) {
      auto tma_barrier = pipeline_q.producer_get_barrier(pipeline_q_producer_state);
      copy(params.tma_load_q.with(*tma_barrier, mcast_a), tQgQ(_, q0_index), tQsQ(_, pipeline_q_producer_state.index()));
      copy(params.tma_load_sfa.with(*tma_barrier, mcast_a), tQgSFA(_, q0_index), tQsSFQ(_, pipeline_q_producer_state.index()));  // [MXFP8]
    }
    ++pipeline_q_producer_state;

    // K1 (+ SFB1 + SFP1)
    int k_index = 0;
    pipeline_kv.producer_acquire(pipeline_kv_producer_state);
    if (lane_predicate) {
      auto tma_barrier = pipeline_kv.producer_get_barrier(pipeline_kv_producer_state);
      copy(params.tma_load_k.with(*tma_barrier, mcast_b), tKgK(_, k_index), tKsK(_, pipeline_kv_producer_state.index()));
      copy(params.tma_load_sfb.with(*tma_barrier, mcast_sfb), tKgSFB(_, k_index), tKsSFK(_, pipeline_kv_producer_state.index()));  // [MXFP8]
      copy(params.tma_load_sfp.with(*tma_barrier, mcast_a), tPgSFP(_, k_index), tPsP_sf(_, k_index % 3));  // [SFP-on-K-slot]
    }
    ++pipeline_kv_producer_state;

    // [MXFP8 N128] single-stage: no second Q tile is loaded.

    // V1 (+ SFV1 + SFP filler for equal slot bytes — rewrites identical data).
    pipeline_kv.producer_acquire(pipeline_kv_producer_state);
    if (lane_predicate) {
      auto tma_barrier = pipeline_kv.producer_get_barrier(pipeline_kv_producer_state);
      copy(params.tma_load_v.with(*tma_barrier, mcast_b), tVgV(_, k_index), tVsV(_, pipeline_kv_producer_state.index()));
      copy(params.tma_load_sfv.with(*tma_barrier, mcast_sfv), tVgSFV(_, k_index), tVsSFV(_, pipeline_kv_producer_state.index()));  // [PVMX 2a.1b]
      copy(params.tma_load_sfp.with(*tma_barrier, mcast_a), tPgSFP(_, k_index), tPsP_sf(_, k_index % 3));  // [SFP-on-K-slot]
    }
    ++pipeline_kv_producer_state;
    k_index += 1;

    // loop:
    mask_tile_count -= 1;
    for (; mask_tile_count > 0; mask_tile_count -= 1) {

      // Ki (+ SFBi + SFPi)
      pipeline_kv.producer_acquire(pipeline_kv_producer_state);
      if (lane_predicate) {
        auto tma_barrier = pipeline_kv.producer_get_barrier(pipeline_kv_producer_state);
        copy(params.tma_load_k.with(*tma_barrier, mcast_b), tKgK(_, k_index), tKsK(_, pipeline_kv_producer_state.index()));
        copy(params.tma_load_sfb.with(*tma_barrier, mcast_sfb), tKgSFB(_, k_index), tKsSFK(_, pipeline_kv_producer_state.index()));  // [MXFP8]
        copy(params.tma_load_sfp.with(*tma_barrier, mcast_a), tPgSFP(_, k_index), tPsP_sf(_, k_index % 3));  // [SFP-on-K-slot]

        // Match the Blackwell MLA load pipeline: warm the current V tile while
        // K is in flight so the following V TMA does not pay the full L2
        // lookup/request latency on its critical path.
        cute::prefetch(params.tma_load_v, tVgV(_, k_index));
      }
      ++pipeline_kv_producer_state;

      // Vi (+ SFVi + SFP filler) — V-slot now carries real V-SF.
      pipeline_kv.producer_acquire(pipeline_kv_producer_state);
      if (lane_predicate) {
        auto tma_barrier = pipeline_kv.producer_get_barrier(pipeline_kv_producer_state);
        copy(params.tma_load_v.with(*tma_barrier, mcast_b), tVgV(_, k_index), tVsV(_, pipeline_kv_producer_state.index()));
        copy(params.tma_load_sfv.with(*tma_barrier, mcast_sfv), tVgSFV(_, k_index), tVsSFV(_, pipeline_kv_producer_state.index()));  // [PVMX 2a.1b]
        copy(params.tma_load_sfp.with(*tma_barrier, mcast_a), tPgSFP(_, k_index), tPsP_sf(_, k_index % 3));  // [SFP-on-K-slot]
      }
      ++pipeline_kv_producer_state;
      k_index += 1;

    }
    // [real static SFP] no per-tile SFP TMA — softmax publishes the SF bytes
    // it used directly into smem_sfp (see softmax_step).
  }
};

}  // namespace cutlass::fmha::collective
