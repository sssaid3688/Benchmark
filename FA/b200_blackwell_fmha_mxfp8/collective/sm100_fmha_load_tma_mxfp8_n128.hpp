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

// [2SM BEACON] load-side progress (slot 1); g_bcn defined in the mainloop file.
#if defined(MXFP8_2SM_BEACON)
__device__ unsigned* g_bcn;   // [2SM] definition (this header is parsed first)
#define BCNL(val) do { if (cute::elect_one_sync()) { unsigned _r = cute::block_rank_in_cluster(); if (_r<4) { atomicMax(&g_bcn[_r*16+1], (unsigned)(val)); __threadfence_system(); } } } while(0)
#else
#define BCNL(val) do {} while(0)
#endif

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
  class SmemLayoutSFV,             // [PVMX 2a.1b] V-SF smem layout, staged on KV pipeline
  class TensorStorage,
  class PipelineQ,
  class PipelineKV,
  class Mask,
  class TileShape,
  class ClusterShape = cute::Shape<cute::_1, cute::_1, cute::_1>   // [2SM] cluster for TMA multicast masks
>
struct Sm100FmhaLoadTmaWarpspecializedMxfp8 {

  using TileShapeQK = typename CollectiveMmaQK::TileShape;
  using TileShapePV = typename CollectiveMmaPV::TileShape;

  // [MXFP8] SF layout / tile types exposed by the block-scaled QK collective.
  using LayoutSFA = typename CollectiveMmaQK::LayoutSFA;
  using LayoutSFB = typename CollectiveMmaQK::LayoutSFB;
  // [PVMX 2a.1] PV-side SF layouts (PV problem: M=seqlen_q, N=D, K=seqlen_kv).
  using LayoutSFP = typename CollectiveMmaPV::LayoutSFA;   // P-SF (dummy gmem; on-chip real)
  using LayoutSFV = typename CollectiveMmaPV::LayoutSFB;   // V-SF (real)

  // [续19an] per-CTA per-tile transaction bytes (Q+SFQ, K+SFK), replicated from the
  // mainloop's formula. NOTE the 续15g "self-only expect-tx" model these once served is
  // DEAD (it made every consumer_wait vacuous): all our TMA atoms are cta_group::2
  // (UTMALDG.2CTA) whose complete-tx ALWAYS lands on the pair-leader CTA's barrier, so
  // arming is now stock-faithful in the KERNEL pipeline params (leader-CTA-only
  // arrive_and_expect_tx with 2x these bytes). Kept for reference/1-SM formulas.
  static constexpr int TxBytesQ_2sm =
      cutlass::bits_to_bytes(cute::cosize(cute::take<0,3>(SmemLayoutQ{})) * cute::sizeof_bits_v<Element>) +
      cutlass::bits_to_bytes(cute::cosize(cute::take<0,3>(SmemLayoutSFQ{})) * cute::sizeof_bits_v<ElementSF>);
  static constexpr int TxBytesKV_2sm =
      cutlass::bits_to_bytes(cute::cosize(cute::take<0,3>(SmemLayoutK{})) * cute::sizeof_bits_v<Element>) +
      cutlass::bits_to_bytes(cute::cosize(cute::take<0,3>(SmemLayoutSFK{})) * cute::sizeof_bits_v<ElementSF>);

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
  using TMA_SFV = typename CollectiveMmaPV::Params::TMA_SFB;   // [PVMX 2a.1] V-SF TMA (PV SFB)

  struct Params {
    TMA_Q tma_load_q;
    TMA_K tma_load_k;
    TMA_V tma_load_v;
    TMA_SFA tma_load_sfa;          // [MXFP8]
    TMA_SFB tma_load_sfb;          // [MXFP8]
    LayoutSFA layout_SFA;          // [MXFP8] needed for get_tma_tensor(shape(...))
    LayoutSFB layout_SFB;          // [MXFP8]
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
            args.ptr_SFP, args.layout_SFP,   // [PVMX 2a.1] P-SF (dummy gmem; TMA built but unused)
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
    cute::prefetch_tma_descriptor(params.tma_load_sfv.get_tma_descriptor());   // [PVMX 2a.1b]
  }

  template<class BlkCoord, class ProblemShape, class ParamsProblemShape>
  CUTLASS_DEVICE void
  load(
      BlkCoord const& blk_coord_in, ProblemShape const& problem_shape,
      Params const& params, ParamsProblemShape const& params_problem_shape,
      TensorStorage& storage,
      PipelineQ& pipeline_q, typename PipelineQ::PipelineState& pipeline_q_producer_state,
      PipelineKV& pipeline_kv, typename PipelineKV::PipelineState& pipeline_kv_producer_state) {

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
    // [2SM 续19an ROOT FIX — THE peer-S=0 bug] Q DATA load MUST be cluster-aware like
    // SFA/K/V/SFB/SFV (it was the LAST self-only load). TileShapeQK here is the MMA
    // tile (M=256): with the old get_slice(0) + self-only tma_partition + q0_index,
    //   (a) the PEER's own TMA box selected gmem rows q0_index*256 = rows 256-383
    //       (OOB for s=256 -> TMA ZERO-fills peer smem stage0), and
    //   (b) the LEADER's cta_group::2 TMA box-split delivered the real m128-255 rows
    //       into the PEER's smem at box-offset +16KB = STAGE 1 (measured: PEERQ_LOC
    //       peer stage0_nz=0 stage1_nz=16384).
    //   => the MMA's peer-half A operand (stage 0) read ZEROS -> peer S = exact 0.
    //   (FORCE_PEERQ proved the peer-half A READ works: forcing smem_q=1.0 gave S!=0.
    //    PEERSFA_RD proved peer SFA TMEM=0x78 GOOD — all prior SF theories were red
    //    herrings downstream of this Q-load bug.)
    // Mirror stock sm100_blockscaled L757-772: get_slice(block_rank % AtomThrID)
    // .partition_A + tma_partition projected along the n-modes; gmem m-tile index
    // = q0_index / AtomThrID (256-row MMA-tile units) at the copy site.
    auto cta_layout_vmnk_a = tiled_divide(make_layout(ClusterShape{}),
        make_tile(typename CollectiveMmaQK::TiledMma::AtomThrID{}));
    auto cta_coord_vmnk_a  = cta_layout_vmnk_a.get_flat_coord(cute::block_rank_in_cluster());
    ThrMMA mma_qk_a = typename CollectiveMmaQK::TiledMma{}.get_slice(
        cute::block_rank_in_cluster() % cute::size(typename CollectiveMmaQK::TiledMma::AtomThrID{}));
    Tensor tSgQ_qdl = mma_qk_a.partition_A(gQ_qdl);
    Tensor sQ = make_tensor(make_smem_ptr(storage.smem_q.data()), SmemLayoutQ{});
    auto [tQgQ_qdl, tQsQ] = tma_partition(
      params.tma_load_q, get<2>(cta_coord_vmnk_a), make_layout(cute::size<2>(cta_layout_vmnk_a)),
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
    // [2SM 续19d FIX] K is the QK B-operand and the 2-SM block-scaled MMA N-SPLITS it
    // across the cluster pair: the per-CTA B smem is only 64-N (SmemLayoutK), so the
    // leader must load kv[0-63] and the peer kv[64-127]. partition_B with a hardcoded
    // get_slice(0) made BOTH CTAs select kv[0-63] -> the cooperative MMA read kv[0-63]
    // DUPLICATED for n=64-127 (the entire PV/O failure + d/d+64 alias). Mirror stock:
    // slice by block_rank so each CTA selects its own N-half.
    ThrMMA mma_qk_b = typename CollectiveMmaQK::TiledMma{}.get_slice(
        cute::block_rank_in_cluster() % cute::size(typename CollectiveMmaQK::TiledMma::AtomThrID{}));
    Tensor tSgK_kdl = mma_qk_b.partition_B(gK_kdl);
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
    // [2SM 续19d FIX] V is the PV B-operand, N-split the same way (per-CTA D smem 64).
    ThrMMA mma_pv_b = typename CollectiveMmaPV::TiledMma{}.get_slice(
        cute::block_rank_in_cluster() % cute::size(typename CollectiveMmaPV::TiledMma::AtomThrID{}));
    Tensor tOgV_dkl = mma_pv_b.partition_B(gV_dkl);
    Tensor sV = make_tensor(make_smem_ptr(storage.smem_v.data()), SmemLayoutV{});
    auto [tVgV_dkl, tVsV] = tma_partition(
      params.tma_load_v, _0{}, make_layout(_1{}),
      group_modes<0,3>(sV), group_modes<0,3>(tOgV_dkl)
    );
#ifdef MXFP8_OSPLIT
    // [刀12 OSPLIT] keep the D-tile mode free: each 128-D V stage is delivered as
    // TWO N=64 sub-tiles (D-tiles 0/1 -> smem sub-slots 2*slot / 2*slot+1),
    // matching the N=64 PV sub-MMA's per-CTA 32-row N-split (exact K mirror).
    auto tVgV = tVgV_dkl(_, _, _, get<2>(blk_coord_kv));
#else
    auto tVgV = tVgV_dkl(_, _0{}, _, get<2>(blk_coord_kv));
#endif

    // ===================================================================
    // [MXFP8] SFA (Q scale factors) — rides the Q pipeline.
    // ===================================================================
    Tensor mSFA = params.tma_load_sfa.get_tma_tensor(shape(params.layout_SFA));
    Tensor gSFA = local_tile(mSFA, TileShapeQK{}, make_coord(_, _, _), Step<_1, X, _1>{});
    // [2SM 续19p FIX] SFA (Q-scale) load MUST be cluster-aware — mirror stock
    // sm100_blockscaled_mma_array_warpspecialized.hpp:756-792. Was get_slice(0) +
    // self-only tma_partition(_0,size1): the PEER's Q-scale never landed (peer
    // smem_sfq=0) => cooperative block-scaled MMA scaled the peer's m128-255 S by
    // ue8m0 0 = 2^-127 ~ 0 => peer S = exact 0 (the ENTIRE peer-row failure). Stock
    // uses cta_mma=get_slice(block_rank).partition_A + tma_partition projected along
    // the n-modes (get<2>/size<2> of the main-MMA cta_layout_vmnk).
    // [续19an] cta_layout_vmnk_a / cta_coord_vmnk_a / mma_qk_a now defined at the Q
    // DATA load above (Q uses the same cluster-aware A-side projection).
    Tensor tSgSFA = mma_qk_a.partition_A(gSFA);
    Tensor sSFQ = make_tensor(make_smem_ptr(storage.smem_sfq.data()), SmemLayoutSFQ{});
    auto [tQgSFA_qdl, tQsSFQ] = tma_partition(
      params.tma_load_sfa, get<2>(cta_coord_vmnk_a), make_layout(cute::size<2>(cta_layout_vmnk_a)),
      group_modes<0,3>(sSFQ), group_modes<0,3>(tSgSFA)
    );
    Tensor tQgSFA = tQgSFA_qdl(_, _, _0{}, sf_l_coord(blk_coord_q));
#ifdef MXFP8_DBG
    if (false && blockIdx.x==1 && blockIdx.y==0 && blockIdx.z==0 && cute::elect_one_sync()) {
      int _q0 = get<0>(blk_coord_q);
      cute::print("[SFATILE bx=%d rank=%d q0_index=%d blkc0=%d] ", (int)blockIdx.x, (int)cute::block_rank_in_cluster(), _q0, (int)get<0>(blk_coord_q)); cute::print("\n");
      cute::print("[SFATILE bx=%d] tQgQ      =", (int)blockIdx.x); cute::print(tQgQ.layout()); cute::print("\n");
      cute::print("[SFATILE bx=%d] tQgSFA    =", (int)blockIdx.x); cute::print(tQgSFA.layout()); cute::print("\n");
      cute::print("[SFATILE bx=%d] gSFA      =", (int)blockIdx.x); cute::print(gSFA.layout()); cute::print("\n");
      cute::print("[SFATILE bx=%d] gQ        =", (int)blockIdx.x); cute::print(gQ_qdl.layout()); cute::print("\n");
      cute::print("[SFATILE bx=%d] tSgSFA    =", (int)blockIdx.x); cute::print(tSgSFA.layout()); cute::print("\n");
      cute::print("[SFATILE bx=%d] tSgQ_qdl  =", (int)blockIdx.x); cute::print(tSgQ_qdl.layout()); cute::print("\n");
      cute::print("[SFATILE bx=%d] tQgQ_qdl  =", (int)blockIdx.x); cute::print(tQgQ_qdl.layout()); cute::print("\n");
      cute::print("[SFATILE bx=%d] tQgSFA_qdl=", (int)blockIdx.x); cute::print(tQgSFA_qdl.layout()); cute::print("\n");
      // crd: the chosen Q vs SFA box origin coord for THIS cta's q0_index
      cute::print("[SFATILE bx=%d] tQgQ(_,q0)  coord="  , (int)blockIdx.x); cute::print(tQgQ(_, _q0).layout()); cute::print(" data="); cute::print(tQgQ(_, _q0).data()); cute::print("\n");
      cute::print("[SFATILE bx=%d] tQgSFA(_,q0) coord=" , (int)blockIdx.x); cute::print(tQgSFA(_, _q0).layout()); cute::print(" data="); cute::print(tQgSFA(_, _q0).data()); cute::print("\n");
    }
#endif

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
    // [2SM 续19] SFB load MUST be cluster-aware (mirror stock collective). The SF
    // rides a SEPARATE TiledMMA_SF; for a 2-SM main MMA the SFB is multicast along
    // the M-cluster dim (mcast_sfb covers both CTAs). Hardcoding the tma_partition
    // cta-projection to (_0, size 1) — like the self-only K/V/Q loads — mis-sizes
    // the multicast smem destination so the SFB lands only in the EVEN-16-N-blocks
    // of TMEM, leaving the ODD-16-N-blocks zero => QK scores exact-0 at kv odd-16
    // (the 续16 numeric bug). Use the SF cluster coord/layout instead.
    auto cta_layout_sfb_vmnk_ld = tiled_divide(make_layout(ClusterShape{}),
        make_tile(typename CollectiveMmaQK::TiledMMA_SF::AtomThrID{}));
    auto cta_coord_sfb_vmnk_ld  = cta_layout_sfb_vmnk_ld.get_flat_coord(cute::block_rank_in_cluster());
    ThrMMA mma_sfb = typename CollectiveMmaQK::TiledMMA_SF{}.get_slice(
        cute::block_rank_in_cluster() % cute::size(typename CollectiveMmaQK::TiledMMA_SF::AtomThrID{}));
    Tensor tSgSFB = mma_sfb.partition_B(gSFB);
    Tensor sSFK = make_tensor(make_smem_ptr(storage.smem_sfk.data()), SmemLayoutSFK{});
    auto [tKgSFB_kdl, tKsSFK] = tma_partition(
      params.tma_load_sfb, get<1>(cta_coord_sfb_vmnk_ld), make_layout(cute::size<1>(cta_layout_sfb_vmnk_ld)),
      group_modes<0,3>(sSFK), group_modes<0,3>(tSgSFB)
    );
    Tensor tKgSFB = tKgSFB_kdl(_, _, _0{}, sf_l_coord(blk_coord_kv));

    // ===================================================================
    // [PVMX 2a.1b] SFV (V scale factors) — rides the KV pipeline.
    // PV problem: (M=seqlen_q, N=D, K=seqlen_kv).
    // SFV layout shape: (D, K_blocks, L); partition_B selects (N, K_tile).
    // ===================================================================
#ifdef MXFP8_OSPLIT
    // [刀12 OSPLIT] PV N-tile is 64 => IsCtaN64: mirror the mSFB stride-0 pair
    // reshape (sub0/sub1 D-tile boxes are IDENTICAL — each carries the FULL
    // 128-D-row SF atom, one copy per V stage serves BOTH sub-MMAs; the odd
    // sub reads the same TMEM slot at +2 columns).
    auto mSFV = [&]() {
      if constexpr (CollectiveMmaPV::IsCtaN64) {
        Tensor t = params.tma_load_sfv.get_tma_tensor(shape(params.layout_SFV));
        auto new_shape  = make_shape (make_shape(shape<0,0>(t),
                                      make_shape(_2{}, shape<0,1>(t))), shape<1>(t), shape<2>(t));
        auto new_stride = make_stride(make_stride(stride<0,0>(t),
                                      make_stride(_0{}, stride<0,1>(t))), stride<1>(t), stride<2>(t));
        return make_tensor(t.data(), make_layout(new_shape, new_stride));
      }
      else {
        return params.tma_load_sfv.get_tma_tensor(shape(params.layout_SFV));
      }
    }();
#else
    Tensor mSFV = params.tma_load_sfv.get_tma_tensor(shape(params.layout_SFV));
#endif
    Tensor gSFV = local_tile(mSFV, typename CollectiveMmaPV::TileShape_SF{}, make_coord(_, _, _), Step<X, _1, _1>{});
    // [2SM 续19d] SFV: PV V scale. O dump (s=128) shows ODD-16-D-blocks of O =
    // EXACT ZERO (d 16-31, 48-63) — identical signature to the SFB odd-16-N-block
    // bug. Cause: self-only SFV leaves the odd-16-blocks of the SFV TMEM unfilled.
    // FIX: cluster-aware partition mirroring SFB but with PV's TiledMMA_SF. The
    // PREVIOUS attempt (84f512d) used cluster-aware partition but kept the OLD
    // self-only OUTPUT index (_, _0{}, _, l) — wrong slice. The index is being
    // DETERMINED EMPIRICALLY via the cute::print below (compare to tKgSFB_kdl).
    auto cta_layout_sfv_vmnk_ld = tiled_divide(make_layout(ClusterShape{}),
        make_tile(typename CollectiveMmaPV::TiledMMA_SF::AtomThrID{}));
    auto cta_coord_sfv_vmnk_ld  = cta_layout_sfv_vmnk_ld.get_flat_coord(cute::block_rank_in_cluster());
    ThrMMA mma_sfv = typename CollectiveMmaPV::TiledMMA_SF{}.get_slice(
        cute::block_rank_in_cluster() % cute::size(typename CollectiveMmaPV::TiledMMA_SF::AtomThrID{}));
    Tensor tSgSFV = mma_sfv.partition_B(gSFV);
    Tensor sSFV = make_tensor(make_smem_ptr(storage.smem_sfv.data()), SmemLayoutSFV{});
    auto [tVgSFV_kdl, tVsSFV] = tma_partition(
      params.tma_load_sfv, get<1>(cta_coord_sfv_vmnk_ld), make_layout(cute::size<1>(cta_layout_sfv_vmnk_ld)),
      group_modes<0,3>(sSFV), group_modes<0,3>(tSgSFV)
    );
    // [2SM 续19j FIX] The kv-TILE dimension is at a DIFFERENT mode for SFV vs SFB,
    // because their local_tile tiles different dims: SFB tiles (N=seqlen_kv, K=D) so
    // the kv-tiles are mode1; SFV (PV B-operand) tiles (N=D, K=seqlen_kv) so the
    // kv-tiles are mode2 (SWAPPED). cute::print @ s=256 PROVES it:
    //   tKgSFB_kdl=((_512,_32),2,1,...)  kv-tile=mode1
    //   tVgSFV_kdl=((_512,_32),1,2,...)  kv-tile=mode2
    // So SFV MUST index (_, _0{}, _, l) — select D-tile=0, ITERATE mode2 (kv-tiles).
    // My earlier (_, _, _0{}, l) (copied from SFB) selected kv-tile 0 ALWAYS, so
    // tile>=1's V-SF was never loaded (smem_sfv stage 3 empty) -> PV(1) scaled V by
    // ~0 -> tile-1 contribution lost -> the s>=192 verify FAIL. (s=128 has 1 kv-tile
    // so the wrong index was harmless.) The 84f512d index was right; its failure was
    // the then-unfixed K/V N-split (续19d).
    Tensor tVgSFV = tVgSFV_kdl(_, _0{}, _, sf_l_coord(blk_coord_kv));

    uint32_t lane_predicate = cute::elect_one_sync();

    // [2SM] TMA multicast masks. The TMA atoms (built by the 2-SM collective) are
    // multicast-capable; passing mask 0 = NO destination => the TMA never arrives
    // on the mbarrier => the MMA's consumer_wait hangs forever. Replicate the stock
    // collective's mask calc: A-side (Q/SFA) along mode 2, B-side (K/V/SFB/SFV)
    // along mode 1. For ClusterShape<2,1,1> the mask is self-only (1<<rank) — still
    // non-zero, which is exactly what the multicast TMA needs. 1-SM => mask 1 (self).
    auto cta_layout_vmnk = tiled_divide(make_layout(ClusterShape{}),
        make_tile(typename CollectiveMmaQK::TiledMma::AtomThrID{}));
    auto cta_coord_vmnk  = cta_layout_vmnk.get_flat_coord(cute::block_rank_in_cluster());
    uint16_t mcast_a = cute::create_tma_multicast_mask<2>(cta_layout_vmnk, cta_coord_vmnk);
    uint16_t mcast_b = cute::create_tma_multicast_mask<1>(cta_layout_vmnk, cta_coord_vmnk);
    // [2SM] SF uses a SEPARATE MMA (TiledMMA_SF) whose AtomThrID may differ from the
    // main MMA's (often 1-SM even when the main MMA is 2-SM). So SFB/SFV multicast
    // along its own cluster layout — can be 0b11 (both CTAs) when the SF atom is 1-SM,
    // unlike the self-only mcast_b. Using mcast_b for SF was why K0 (K+SFB) hung.
    auto cta_layout_sfb_vmnk = tiled_divide(make_layout(ClusterShape{}),
        make_tile(typename CollectiveMmaQK::TiledMMA_SF::AtomThrID{}));
    auto cta_coord_sfb_vmnk  = cta_layout_sfb_vmnk.get_flat_coord(cute::block_rank_in_cluster());
    uint16_t mcast_sfb = cute::create_tma_multicast_mask<1>(cta_layout_sfb_vmnk, cta_coord_sfb_vmnk);
#if defined(MXFP8_2SM_BEACON)
    if (cute::elect_one_sync()) { unsigned _r=cute::block_rank_in_cluster(); if(_r<4){ g_bcn[_r*16+2]=mcast_a; g_bcn[_r*16+3]=mcast_b; g_bcn[_r*16+4]=mcast_sfb; __threadfence_system(); } }
#endif

    // [MXFP8 N128] single-stage: the CTA loads ONE Q tile (M=128). The dual-
    // stage q0/q1 = 2*blk, 2*blk+1 split is gone — q0_index is the CTA's tile.
    int q0_index = get<0>(blk_coord_q);
    // [2SM 续19s FIX] SFA (Q-scale) M-tile index. Q DATA uses get_slice(0)+self-only
    // tma_partition, so its M-split rides q0_index (per-CTA 128-row tiles): peer
    // q0_index=1 steps to m128-255. But SFA uses get_slice(block_rank).partition_A
    // (stock block-scaled style) which ALREADY does the leader/peer M-atom split
    // (peer base = m128-255 atom). Indexing the SF m-tile mode with the per-CTA
    // q0_index ON TOP of that double-counts: peer base(atom1=basis1 1) + q0_index*2
    // = basis1 3 -> OOB SF region -> TMA zero-fill -> peer smem_sfq=0 -> cooperative
    // block-scaled QK MMA scales peer m128-255 by ue8m0 0 = 2^-127 ~ 0 -> peer S = 0
    // (the WHOLE peer-row failure). The SF m-tile mode is in MMA-tile (256-row) units,
    // so its index is q0_index / AtomThrID (matches stock cta_coord_M / AtomThrID).
    int sfa_m_index = q0_index / (int)cute::size(typename CollectiveMmaQK::TiledMma::AtomThrID{});
    BCNL(1);   // [2SM] load: about to producer_acquire Q
    pipeline_q.producer_acquire(pipeline_q_producer_state);
    // [续19an] manual self-only expect-tx REMOVED: arming now happens inside
    // producer_acquire (stock-faithful: leader-CTA-only arrive_and_expect_tx with 2x
    // bytes; all 2CTA-TMA tx land on the leader's barrier). See kernel pipeline params.
    // [2SM 续15c] async-proxy cluster fence: order the producer_acquire's expect-tx
    // arming (generic-proxy smem write to the cluster mbarrier) BEFORE the TMA issue
    // (async proxy). Without it, in 2-SM the TMA's byte-count may race the expect-tx
    // arm on the CLUSTER mbarrier -> full barrier never completes -> MMA consumer_wait
    // hangs (the deterministic s>=256 Q-wait hang; beacon's __threadfence_system here
    // accidentally masked it). 1-SM unaffected (no cluster mbarrier).
#ifndef MXFP8_2SM_NOQFENCE
    // [刀18 E2] oyhj has NO fence here (and our own KV-loop copies of this fence were
    // already removed in 续19an as vestigial — they only ordered the DELETED manual
    // self-only expect-tx). This Q-side one is the last survivor of the same dead
    // model; under MXFP8_2SM_NOQFENCE it is removed to match oyhj exactly.
    if constexpr (cute::size(ClusterShape{}) > 1) {
      asm volatile("fence.proxy.async.shared::cluster;" ::: "memory");
    }
#endif
    BCNL(2);   // [2SM] load: Q producer_acquire passed, about to issue Q TMA
    if (lane_predicate) {
      auto tma_barrier = pipeline_q.producer_get_barrier(pipeline_q_producer_state);
      // [续19an] Q DATA m-tile index is now /AtomThrID (256-row MMA-tile units), same as
      // SFA — the cluster-aware partition_A selects each CTA's own M-half of the tile.
      copy(params.tma_load_q.with(*tma_barrier, mcast_a), tQgQ(_, sfa_m_index), tQsQ(_, pipeline_q_producer_state.index()));
      copy(params.tma_load_sfa.with(*tma_barrier, mcast_a), tQgSFA(_, sfa_m_index), tQsSFQ(_, pipeline_q_producer_state.index()));  // [MXFP8] [续19s] /AtomThrID
    }
    ++pipeline_q_producer_state;
    BCNL(3);   // [2SM] load: Q TMA issued

    // K1 (+ SFB1)
    // [M3 sub-tile] each 128-row K stage is delivered as TWO N=64 sub-tiles
    // (gmem 64-tiles 2k / 2k+1 -> smem sub-slots 2*stage / 2*stage+1), matching
    // the N=64 sub-MMA's per-CTA 32-row N-split partitioning. Same total bytes.
    int k_index = 0;
    pipeline_kv.producer_acquire(pipeline_kv_producer_state);
    // [续19an-perf] cluster fence removed: it only ordered the (deleted) manual expect-tx; arming now rides producer_acquire's arrive_and_expect_tx (stock has no fence here).
    if (lane_predicate) {
      auto tma_barrier = pipeline_kv.producer_get_barrier(pipeline_kv_producer_state);
      int kv_slot = pipeline_kv_producer_state.index();
#if defined(MXFP8_2SM_N128SINGLE)
      // [刀27 N128SINGLE] N128 QK collective: ONE K copy/stage (per-CTA B = 64-N),
      // ONE slot. SFB tile is now N128 -> index k_index (not 2*k_index). The
      // IsCtaN64 SFB reshape still holds (gates the SF mcast). (oyhj 406-407.)
      copy(params.tma_load_k.with(*tma_barrier, mcast_b), tKgK(_, k_index), tKsK(_, kv_slot));
      copy(params.tma_load_sfb.with(*tma_barrier, mcast_sfb), tKgSFB(_, k_index), tKsSFK(_, kv_slot));  // [MXFP8]
#else
      copy(params.tma_load_k.with(*tma_barrier, mcast_b), tKgK(_, 2*k_index    ), tKsK(_, 2*kv_slot    ));
      copy(params.tma_load_k.with(*tma_barrier, mcast_b), tKgK(_, 2*k_index + 1), tKsK(_, 2*kv_slot + 1));
      // [M3 sub-tile] SFB: ONE copy per stage — the IsCtaN64 gmem view's sub0/sub1
      // boxes are identical (stride-0 pair), each = the full 128-row SF atom.
      copy(params.tma_load_sfb.with(*tma_barrier, mcast_sfb), tKgSFB(_, 2*k_index), tKsSFK(_, kv_slot));  // [MXFP8]
#endif
    }
    ++pipeline_kv_producer_state;
    BCNL(4);   // [2SM] load: K0 TMA issued

    // [MXFP8 N128] single-stage: no second Q tile is loaded.

    // V1 (+ SFV1) — V-slot now carries real V-SF (was K-SF filler).
    pipeline_kv.producer_acquire(pipeline_kv_producer_state);
    // [续19an-perf] cluster fence removed: it only ordered the (deleted) manual expect-tx; arming now rides producer_acquire's arrive_and_expect_tx (stock has no fence here).
    if (lane_predicate) {
      auto tma_barrier = pipeline_kv.producer_get_barrier(pipeline_kv_producer_state);
#ifdef MXFP8_OSPLIT
      int v_slot = pipeline_kv_producer_state.index();
      copy(params.tma_load_v.with(*tma_barrier, mcast_b), tVgV(_, 0, k_index), tVsV(_, 2*v_slot    ));
      copy(params.tma_load_v.with(*tma_barrier, mcast_b), tVgV(_, 1, k_index), tVsV(_, 2*v_slot + 1));
      // [刀12] SFV: ONE copy per stage — the IsCtaN64 stride-0 pair's D-tile-0 box
      // is the full 128-D-row SF atom (serves both sub-MMAs).
      copy(params.tma_load_sfv.with(*tma_barrier, mcast_sfb), tVgSFV(_, k_index), tVsSFV(_, v_slot));
#else
      copy(params.tma_load_v.with(*tma_barrier, mcast_b), tVgV(_, k_index), tVsV(_, pipeline_kv_producer_state.index()));
      copy(params.tma_load_sfv.with(*tma_barrier, mcast_sfb), tVgSFV(_, k_index), tVsSFV(_, pipeline_kv_producer_state.index()));  // [PVMX 2a.1b]
#endif
    }
    ++pipeline_kv_producer_state;
    k_index += 1;
    BCNL(5);   // [2SM] load: V0 TMA issued, entering KV loop

    // loop:
    mask_tile_count -= 1;
    for (; mask_tile_count > 0; mask_tile_count -= 1) {

      // Ki (+ SFBi) — [M3 sub-tile] 2 sub-tile copies per stage (see K1 above).
      pipeline_kv.producer_acquire(pipeline_kv_producer_state);
      // [续19an-perf] cluster fence removed: it only ordered the (deleted) manual expect-tx; arming now rides producer_acquire's arrive_and_expect_tx (stock has no fence here).
      if (lane_predicate) {
        auto tma_barrier = pipeline_kv.producer_get_barrier(pipeline_kv_producer_state);
        int kv_slot = pipeline_kv_producer_state.index();
#if defined(MXFP8_2SM_N128SINGLE)
        // [刀27 N128SINGLE] ONE K copy + ONE SFB copy / stage (oyhj 433-434).
        copy(params.tma_load_k.with(*tma_barrier, mcast_b), tKgK(_, k_index), tKsK(_, kv_slot));
        copy(params.tma_load_sfb.with(*tma_barrier, mcast_sfb), tKgSFB(_, k_index), tKsSFK(_, kv_slot));  // [MXFP8]
#else
        copy(params.tma_load_k.with(*tma_barrier, mcast_b), tKgK(_, 2*k_index    ), tKsK(_, 2*kv_slot    ));
        copy(params.tma_load_k.with(*tma_barrier, mcast_b), tKgK(_, 2*k_index + 1), tKsK(_, 2*kv_slot + 1));
        copy(params.tma_load_sfb.with(*tma_barrier, mcast_sfb), tKgSFB(_, 2*k_index), tKsSFK(_, kv_slot));  // [MXFP8] [M3] one SF atom/stage
#endif
#if defined(MXFP8_2SM_VPREFETCH)
        // [刀r4 RANK-1] Blackwell MLA load-pipeline hint (oyhj load:440): warm the
        // current V tile's L2 lookup while K is in flight so the following V
        // producer_acquire+TMA does not pay full L2 latency on its critical path.
        // Pure non-semantic TMA prefetch (bit-exact). px27 non-OSPLIT V box =
        // tVgV(_, k_index).
#ifdef MXFP8_OSPLIT
        cute::prefetch(params.tma_load_v, tVgV(_, 0, k_index));
#else
        cute::prefetch(params.tma_load_v, tVgV(_, k_index));
#endif
#endif
      }
      ++pipeline_kv_producer_state;

      // Vi (+ SFVi) — V-slot now carries real V-SF.
      pipeline_kv.producer_acquire(pipeline_kv_producer_state);
      // [续19an-perf] cluster fence removed: it only ordered the (deleted) manual expect-tx; arming now rides producer_acquire's arrive_and_expect_tx (stock has no fence here).
      if (lane_predicate) {
        auto tma_barrier = pipeline_kv.producer_get_barrier(pipeline_kv_producer_state);
#ifdef MXFP8_OSPLIT
        int v_slot = pipeline_kv_producer_state.index();
        copy(params.tma_load_v.with(*tma_barrier, mcast_b), tVgV(_, 0, k_index), tVsV(_, 2*v_slot    ));
        copy(params.tma_load_v.with(*tma_barrier, mcast_b), tVgV(_, 1, k_index), tVsV(_, 2*v_slot + 1));
        copy(params.tma_load_sfv.with(*tma_barrier, mcast_sfb), tVgSFV(_, k_index), tVsSFV(_, v_slot));  // [刀12] one SF atom/stage
#else
        copy(params.tma_load_v.with(*tma_barrier, mcast_b), tVgV(_, k_index), tVsV(_, pipeline_kv_producer_state.index()));
        copy(params.tma_load_sfv.with(*tma_barrier, mcast_sfb), tVgSFV(_, k_index), tVsSFV(_, pipeline_kv_producer_state.index()));  // [PVMX 2a.1b]
#endif
      }
      ++pipeline_kv_producer_state;
      k_index += 1;
    }
  }
};

}  // namespace cutlass::fmha::collective
