/***************************************************************************************************
 * ===== CtaN=128 single-stage variant (M3) — MAINLOOP TRANSFORMED =====
 * Single-softmax-warp, M=128, CtaN=128 rewrite of the verified CtaN=64
 * mainloop (sm100_fmha_fwd_mainloop_mxfp8.hpp, untouched). See
 * docs/OP6_M3_CTAN128_PLAN.md.
 * DONE here: ThreadShape<1,1,1>; single-stage TmemAllocation (single-buffered
 * S0 + O0 + SFA0 + SFB0, kEnd=320); single-stage mma() (one QK, one PV per KV
 * tile; pipeline op counts verified n+1 on s0, n on corr); correction() and
 * correction_empty() single-stage (one O, one epilogue signal); softmax_step
 * order_s removed (single softmax warp group).
 * STILL NEEDED to build/run (see plan): single-stage epilogue collective
 * (store() o0_index=blk, 1 wait/store/release); single-Q load; driver
 * -DMXFP8_N128 path. NOT YET BUILT/VERIFIED.
 * =====================================================================
 *
 * Operator 6 — Milestone 2 / Route C : MXFP8 FlashAttention
 * File 2 of 3 : block-scaled QK mainloop collective.
 *
 * Derived from CUTLASS example 77
 *   examples/77_blackwell_fmha/collective/sm100_fmha_fwd_mainloop_tma_warpspecialized.hpp
 * (pristine CUTLASS is never edited; this is a standalone modified copy).
 *
 * Route C changes vs the pristine collective — all marked `// [MXFP8]`:
 *   - QK CollectiveBuilder : OpClassTensorOp -> OpClassBlockScaledTensorOp
 *       Q,K become mx_float8_t<float_e4m3_t> = (e4m3 data, ue8m0 scale factor).
 *   - PV stays plain-FP8 (unchanged) — P is generated on-chip by softmax.
 *   - QK N-tile shrunk 128 -> 64 (driver passes TileShape with mode-1 == 64)
 *       so the SFA/SFB TMEM operands fit in the 512-column TMEM budget:
 *         S0,S1 = 64 cols each ; O0,O1 = 128 cols each ; SF region = 128 cols.
 *   - SF rides the existing Q / KV TMA pipelines (no new pipeline): each Q-slot
 *     also carries SFQ, each KV-slot also carries an SF-sized payload (real K
 *     scale factors on K-slots, ignored filler on V-slots) so the pipeline
 *     transaction-byte counts stay uniform.
 *   - mma(): before every QK UMMA, the relevant SF tile is copied smem->TMEM
 *     via tcgen05.cp (UTCCP), and the UMMA is issued as a block-scaled MMA
 *     `mma_qk.with(scaleC, tCtSFA, tCtSFB)`.
 *
 * The example-77 forward kernel header is generic over the mainloop collective
 * and needs no modification: the SF pointers/layouts are encapsulated inside
 * Load::Arguments and thread through automatically.
 ***************************************************************************************************/
#pragma once

__device__ __forceinline__ float fast_exp2f(float x) {
    float result;
    asm("ex2.approx.ftz.f32 %0, %1;" : "=f"(result) : "f"(x));
    return result;
}

#include "cutlass/cutlass.h"
#include "cutlass/arch/memory_sm80.h"
#include "cutlass/arch/barrier.h"                            // [MXFP8 N128 M3b] NamedBarrier
#include "cutlass/float8.h"                                  // [MXFP8] mx_float8_t / float_ue8m0_t
#include "cutlass/gemm/collective/collective_builder.hpp"
#include "cutlass/detail/sm100_blockscaled_layout.hpp"       // [MXFP8] Sm1xxBlockScaledConfig
#include "cutlass/detail/sm100_tmem_helper.hpp"              // [MXFP8] tmem helpers
#include "cute/arch/simd_sm100.hpp"
#include "cute/tensor.hpp"
#include "cute/layout.hpp"

#include "collective/fmha_common.hpp"
#include "collective/fmha_fusion.hpp"
#include "sm100_fmha_load_tma_mxfp8_n128.hpp"                // [MXFP8 N128] single-Q SF-aware load

#ifdef MXFP8_DBG
// Debug: capture the SF smem the MMA warp sees after the TMA load.
__device__ uint8_t g_dbg_sfk[2048];
__device__ uint8_t g_dbg_sfq[2048];
// Debug: capture raw QK scores for Q-row 0, first KV tile (64 values).
__device__ float   g_dbg_S[64];
__device__ int     g_dbg_S_got;
// Debug: capture raw QK score S[q,kv=0] for Q-rows 0..63.
__device__ float   g_dbg_Srow[64];
#endif

namespace cutlass::fmha::collective {

using namespace cute;

// [MXFP8] block-scaled QK gemm helper — like fmha_common.hpp gemm_zero_acc, but
// every k-block UMMA is issued as a block-scaled MMA referencing SF in TMEM.
template<class Atom, class TA, class TB, class TC, class TSFA, class TSFB>
CUTE_DEVICE void gemm_bs(Atom& atom, bool zero_acc,
                         TA const& tA, TB const& tB, TC&& tC,
                         TSFA const& tSFA, TSFB const& tSFB) {
  using ScaleOut = decltype(atom.accumulate_);
  atom.accumulate_ = zero_acc ? ScaleOut::Zero : ScaleOut::One;
  CUTLASS_PRAGMA_UNROLL
  for (int k_block = 0; k_block < size<2>(tA); k_block++) {
    cute::gemm(atom.with(atom.accumulate_, tSFA(_,_,k_block), tSFB(_,_,k_block)),
               tA(_,_,k_block), tB(_,_,k_block), tC);
    atom.accumulate_ = ScaleOut::One;
  }
}

template<
  class Element_,
  class ElementQK_,
  class ElementPV_,
  class TileShape_,
  class StrideQ_,
  class StrideK_,
  class StrideV_,
  class Mask_,
  class ThreadShape = Shape<_1, _1, _1>,   // [MXFP8 N128] single-stage: M not split
  class OrderLoadEpilogue = cute::false_type
>
struct Sm100FmhaFwdMainloopTmaWarpspecializedMxfp8 {

  using Element = Element_;          // storage element for Q/K/V smem (float_e4m3_t)
  using ElementQK = ElementQK_;
  using ElementPV = ElementPV_;
  using TileShape = TileShape_;
  using StrideQ = StrideQ_;
  using StrideK = StrideK_;
  using StrideV = StrideV_;
  using Mask = Mask_;

  static constexpr int StageCountQ = 2;
#ifdef MXFP8_KV_STAGES
  static constexpr int StageCountKV = MXFP8_KV_STAGES;
#else
  static constexpr int StageCountKV = sizeof(Element_) == 1 ? 4 : 3;
#endif

  using StagesQ = cutlass::gemm::collective::StageCount<StageCountQ>;
  using StagesKV = cutlass::gemm::collective::StageCount<StageCountKV>;

  using ClusterShape = Shape<_1, _1, _1>;

  static const int Alignment = 128 / sizeof_bits_v<Element>;

  using TileShapeQK = decltype(shape_div(TileShape{}, ThreadShape{}));

  using TileShapePV = decltype(select<0,2,1>(TileShapeQK{}));

  // [MXFP8] block-scaled element pair for QK: (e4m3 data, ue8m0 scale factor).
  using ElementBlockScaled = cutlass::mx_float8_t<cutlass::float_e4m3_t>;

  // [MXFP8] QK collective is now block-scaled MXFP8.
  using CollectiveMmaQK = typename cutlass::gemm::collective::CollectiveBuilder<
      cutlass::arch::Sm100, cutlass::arch::OpClassBlockScaledTensorOp,
      ElementBlockScaled, StrideQ, Alignment,
      ElementBlockScaled, StrideK, Alignment,
      ElementQK,
      TileShapeQK, ClusterShape, cutlass::gemm::collective::StageCount<3> /* changed later */,
      cutlass::gemm::KernelTmaWarpSpecialized1SmMxf8f6f4Sm100>::CollectiveOp;

  // [PVMX 2a.1] PV is now block-scaled MXFP8 SS: P,V = e4m3 + ue8m0 SF along seqlen_kv.
  using CollectiveMmaPV = typename cutlass::gemm::collective::CollectiveBuilder<
      cutlass::arch::Sm100, cutlass::arch::OpClassBlockScaledTensorOp,
      ElementBlockScaled, StrideK, Alignment,
      ElementBlockScaled, decltype(select<1,0,2>(StrideV{})), Alignment,
      ElementPV,
      TileShapePV, ClusterShape, cutlass::gemm::collective::StageCount<3> /* changed later */,
      cutlass::gemm::KernelTmaWarpSpecialized1SmMxf8f6f4Sm100>::CollectiveOp;

  using SmemLayoutQ = decltype(unstageSmemLayout(typename CollectiveMmaQK::SmemLayoutA{}, Int<StageCountQ>{}));
  using SmemLayoutK = decltype(unstageSmemLayout(typename CollectiveMmaQK::SmemLayoutB{}, Int<StageCountKV>{}));
  using SmemLayoutV = decltype(unstageSmemLayout(typename CollectiveMmaPV::SmemLayoutB{}, Int<StageCountKV>{}));
  // [PVMX 2a.0] P operand smem layout (block-scaled SS PV reads P from smem, not TMEM).
  // 2 stages, matching the double-buffered S/PV pipeline.
  static constexpr int StageCountP = 2;
  using SmemLayoutP = decltype(unstageSmemLayout(typename CollectiveMmaPV::SmemLayoutA{}, Int<StageCountP>{}));

  // [MXFP8] scale-factor types/layouts exposed by the block-scaled QK collective.
  using ElementSF = typename CollectiveMmaQK::ElementSF;                 // float_ue8m0_t
  using SmemLayoutAtomSFA = typename CollectiveMmaQK::SmemLayoutAtomSFA;
  using SmemLayoutAtomSFB = typename CollectiveMmaQK::SmemLayoutAtomSFB;
  using TiledMmaQK        = typename CollectiveMmaQK::TiledMma;
  // [PVMX 2a.1] block-scaled PV SF types (P=SFA, V=SFB; per-32 along seqlen_kv).
  using TiledMmaPV        = typename CollectiveMmaPV::TiledMma;
  using SmemLayoutAtomSFP = typename CollectiveMmaPV::SmemLayoutAtomSFA;  // P-SF atom
  using SmemLayoutAtomSFV = typename CollectiveMmaPV::SmemLayoutAtomSFB;  // V-SF atom
  using SmemLayoutSFP = decltype(unstageSmemLayout(typename CollectiveMmaPV::SmemLayoutSFA{}, Int<2>{}));
  // [PVMX 2a.1b] V-SF smem, staged on KV pipeline (loaded by TMA on V-slots).
  using SmemLayoutSFV = decltype(unstageSmemLayout(typename CollectiveMmaPV::SmemLayoutSFB{}, Int<StageCountKV>{}));

  // [MXFP8] SF smem layouts, re-staged onto the FMHA Q / KV pipelines.
  using SmemLayoutSFQ = decltype(unstageSmemLayout(typename CollectiveMmaQK::SmemLayoutSFA{}, Int<StageCountQ>{}));
  using SmemLayoutSFK = decltype(unstageSmemLayout(typename CollectiveMmaQK::SmemLayoutSFB{}, Int<StageCountKV>{}));

  // Reuse shared memory for V and O.
  static constexpr bool IsOrderLoadEpilogue = std::is_same_v<OrderLoadEpilogue, cute::true_type>;
  struct TensorStorage {
    cute::array_aligned<Element, cute::cosize_v<SmemLayoutQ>> smem_q;
    union {
      cute::array_aligned<Element, cute::cosize_v<SmemLayoutK>> smem_k;
      cute::array_aligned<Element, cute::cosize_v<SmemLayoutV>> smem_v;
    };
    // [MXFP8] scale-factor smem — one slot per Q / KV pipeline stage.
    cute::array_aligned<ElementSF, cute::cosize_v<SmemLayoutSFQ>> smem_sfq;
    cute::array_aligned<ElementSF, cute::cosize_v<SmemLayoutSFK>> smem_sfk;
    // [PVMX 2a.0] P data buffer in smem (e4m3), written by softmax, read by PV SS MMA.
    cute::array_aligned<Element, cute::cosize_v<SmemLayoutP>> smem_p;
    // [PVMX 2b] P-SF smem, double-buffered (softmax writes per tile, UTCCP stages per tile).
    cute::array_aligned<ElementSF, cute::cosize_v<SmemLayoutSFP>> smem_sfp;
    // [PVMX 2a.1b] V-SF smem, staged on KV pipeline (TMA loads per V-slot).
    cute::array_aligned<ElementSF, cute::cosize_v<SmemLayoutSFV>> smem_sfv;
  };

  // [MXFP8 N128 M2] single-stage, DOUBLE-buffered S TMEM map — QK N-tile 128.
  // Two QK accumulators S0/S1 (128 cols each) so QK(k+1)→S[(k+1)%2] can run
  // ahead while softmax reads S[k%2] — recovers the QK/softmax overlap that
  // single-buffered milestone-1 lost. One PV accumulator O (single M=128
  // subtile). One SFA (Q is fixed for all KV tiles → one shared slot); two SFB
  // (K changes per tile and QK(k)/QK(k+1) overlap, so the K scale-factor TMEM
  // operand must ping-pong too).
  //   S0=0..127  S1=128..255  O0=256..383
  //   SFA0=384..415  SFB0=416..447  SFB1=448..479   kEnd=480  (<=512)
  // P / V-stats are embedded in the S buffers: P0=S0+32, P1=S1+32 (e4m3 P tile
  // for the PV GEMM); V0=S0, V1=S1 (softmax→correction row stats, first 2 cols).
  // O1 aliases O0 — the *_1 name is still referenced inside the static
  // `stage==_0 ? _0 : _1` ternary in correction_epilogue, never the live branch
  // (there is only one O).
  enum class TmemAllocation : uint32_t {
    kSizeS = 128,
    kSizeO = 128,
    kSizeP = 32,                 // tilePlikeFP32 = N(128)/sizeof(float)*sizeof(Element)
    kSizeSF = 16,                // [PVMX 2a.1] tightened 32->16 (real SF footprint=4) to fit PV SF slots
    S0 = 0,
    S1 = S0 + kSizeS,            // [M2] second QK accumulator buffer
    V0 = S0,                     // stats storage from softmax to correction
    V1 = S1,
    P0 = S0 + kSizeP,
    P1 = S1 + kSizeP,
    O0 = S1 + kSizeS,
    SFA0 = O0 + kSizeO,          // [MXFP8] SFA (Q scale factors, shared)
    SFB0 = SFA0 + kSizeSF,       // [MXFP8] SFB (K scale factors, buffer 0)
    SFB1 = SFB0 + kSizeSF,       // [MXFP8] SFB (K scale factors, buffer 1)
    SFP0 = SFB1 + kSizeSF,       // [PVMX 2b] P-SF (PV SFA), buffer 0 — double-buffered like SFB(K)
    SFP1 = SFP0 + kSizeSF,       // [PVMX 2b] P-SF, buffer 1
    SFV0 = SFP1 + kSizeSF,       // [PVMX 2a.1b] V-SF (PV SFB), buffer 0 — double-buffered per-tile
    SFV1 = SFV0 + kSizeSF,       // [PVMX 2a.1b] V-SF, buffer 1
    kEnd = SFV1 + kSizeSF,       // = 496 <= 512
    O1 = O0,                     // [N128] alias (single O; *_1 never live)
    SFA1 = SFA0
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
    2,
    typename CollectiveMmaQK::AtomThrShapeMNK
  >;

  // from mma to softmax0/1 warp, protects S in tmem
  // [MXFP8 N128 M2] depth 2 — double-buffered S. The single softmax warp group
  // consumes ONE pipeline (pipeline_mma_s0); depth 2 maps its two stages onto
  // the two TMEM buffers S0/S1, selected by PipelineState::index(). QK(k)
  // commits stage k%2; softmax_step(k) / PV read the same buffer via index().
  using PipelineS = cutlass::PipelineUmmaAsync<2>;

  // from softmax0/1/ to correction wg
  using PipelineC = cutlass::PipelineAsync<1>;

  // from mma to correction
  // [MXFP8 N128] depth 1, NOT 2. The dual-stage design has TWO O accumulators
  // (O0/O1, one per M=128 subtile) so depth-2 PipelineO ⟺ 2 physical buffers.
  // The single-stage variant has ONE O accumulator (TmemAllocation::O0, with O1
  // aliased to it). With depth 2 the mma producer runs 2 PVs ahead: PV(i+1)'s
  // producer_acquire only waits for correction to release O #(i-1), so PV(i+1)
  // accumulates into the single O before correction has rescaled/consumed
  // PV(i)'s result — a WAR race on O (~1% corrupted outputs). Depth 1 forces
  // PV(i+1) to wait until correction has consumed PV(i)'s O.
  using PipelineO = cutlass::PipelineUmmaAsync<1>;

  // from corr to epilogue
  using PipelineE = cutlass::PipelineAsync<2>;

  using OrderBarrierSoftmax = cutlass::OrderedSequenceBarrier<
    /*stages*/ 1, /*groups*/ 2>;

  // [MXFP8] Q-pipeline transaction = Q tile bytes + SFQ tile bytes.
  static const int TransactionBytesLoadQ_data = cutlass::bits_to_bytes(cosize(take<0,3>(SmemLayoutQ{})) * cute::sizeof_bits_v<Element>);
  static const int TransactionBytesLoadSFQ   = cutlass::bits_to_bytes(cosize(take<0,3>(SmemLayoutSFQ{})) * cute::sizeof_bits_v<ElementSF>);
  static const int TransactionBytesLoadQ     = TransactionBytesLoadQ_data + TransactionBytesLoadSFQ;

  // [MXFP8] KV-pipeline transaction = K|V tile bytes + SF-sized payload (real
  // K scale factors on K-slots, ignored filler on V-slots) — kept equal.
  static const int TransactionBytesLoadK_data = cutlass::bits_to_bytes(cosize(take<0,3>(SmemLayoutK{})) * cute::sizeof_bits_v<Element>);
  static const int TransactionBytesLoadV_data = cutlass::bits_to_bytes(cosize(take<0,3>(SmemLayoutV{})) * cute::sizeof_bits_v<Element>);
  static const int TransactionBytesLoadSFK   = cutlass::bits_to_bytes(cosize(take<0,3>(SmemLayoutSFK{})) * cute::sizeof_bits_v<ElementSF>);
  static const int TransactionBytesLoadSFP = cutlass::bits_to_bytes(
      cosize(take<0,3>(SmemLayoutSFP{})) * cute::sizeof_bits_v<ElementSF>);
  static const int TransactionBytesLoadK     = TransactionBytesLoadK_data + TransactionBytesLoadSFK;
  static const int TransactionBytesLoadV     = TransactionBytesLoadV_data + TransactionBytesLoadSFK;

  static_assert(TransactionBytesLoadK == TransactionBytesLoadV, "K and V smem layouts must be of equal size");

  // [MXFP8] SF-aware load collective (file 3).
  using Load = Sm100FmhaLoadTmaWarpspecializedMxfp8<
    Element, ElementSF, StrideQ, StrideK, StrideV,
    CollectiveMmaQK, CollectiveMmaPV,
    SmemLayoutQ, SmemLayoutK, SmemLayoutV,
    SmemLayoutSFQ, SmemLayoutSFK, SmemLayoutSFP, SmemLayoutSFV,
    TensorStorage, PipelineQ, PipelineKV, PipelineSFP, Mask, TileShape
  >;

  struct Arguments {
    typename Load::Arguments load;

    // if zero, defaults to 1/sqrt(D)
    float scale_softmax = 0.0f;

    // scaling factors to dequantize QKV
    float scale_q = 1.0f;
    float scale_k = 1.0f;
    float scale_v = 1.0f;

    // scaling factor to quantize O
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

    Load load;
    load.load(blk_coord, problem_shape, params.load, params_problem_shape,
        storage,
        pipeline_q, pipeline_q_producer_state,
        pipeline_kv, pipeline_kv_producer_state,
        pipeline_sfp, pipeline_sfp_producer_state);
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
      PipelineS& pipeline_s0, typename PipelineS::PipelineState& pipeline_s0_producer_state,
      PipelineS& pipeline_s1, typename PipelineS::PipelineState& pipeline_s1_producer_state,
      PipelineO& pipeline_corr, typename PipelineO::PipelineState& pipeline_corr_producer_state) {

    auto pipeline_q_release_state = pipeline_q_consumer_state;
    auto pipeline_kv_release_state = pipeline_kv_consumer_state;
    auto pipeline_sfp_release_state = pipeline_sfp_consumer_state;

    int mask_tile_count = Mask{}.get_trip_count(blk_coord, TileShape{}, problem_shape);

    typename CollectiveMmaQK::TiledMma mma_qk;
    ThrMMA thr_mma_qk = mma_qk.get_slice(0);

    // [PVMX 2a.0] PV uses the plain SS atom directly (P read from smem, not TMEM).
    // Dropping to_tiled_mma_sm100_ts is the first half of moving toward the
    // block-scaled SS PV (which is SS-only — no TS form exists).
    typename CollectiveMmaPV::TiledMma mma_pv;
    ThrMMA thr_mma_pv = mma_pv.get_slice(0);

    Tensor sQ = make_tensor(make_smem_ptr(storage.smem_q.data()), SmemLayoutQ{});
    Tensor sK = make_tensor(make_smem_ptr(storage.smem_k.data()), SmemLayoutK{});
    Tensor sV = make_tensor(make_smem_ptr(storage.smem_v.data()), SmemLayoutV{});

    Tensor tSrQ = thr_mma_qk.make_fragment_A(sQ);
    Tensor tSrK = thr_mma_qk.make_fragment_B(sK);
    Tensor tOrV = thr_mma_pv.make_fragment_B(sV);

    // tmem layout: S0 S1 O0, S overlaps with P and V
    Tensor tStS = partition_fragment_C(mma_qk, select<0,1>(TileShapeQK{}));
    Tensor tOtO = partition_fragment_C(mma_pv, select<0,1>(TileShapePV{}));

    // [MXFP8 N128 M2] two S accumulators (ping-pong), one O accumulator.
    Tensor tStS0 = tStS;  tStS0.data() = tStS.data().get() + uint32_t(TmemAllocation::S0);
    Tensor tStS1 = tStS;  tStS1.data() = tStS.data().get() + uint32_t(TmemAllocation::S1);

    Tensor tOtO0 = tOtO;  tOtO0.data() = tOtO.data().get() + uint32_t(TmemAllocation::O0);

    // [PVMX 2a.0] P now lives in a real smem buffer (2-stage). SS PV reads A=P from smem.
    Tensor sP = make_tensor(make_smem_ptr(storage.smem_p.data()), SmemLayoutP{});
    Tensor tOrP = thr_mma_pv.make_fragment_A(sP);   // (MMA, M, K, STAGE)

    // ===================================================================
    // [MXFP8] block-scaled QK: scale-factor smem -> TMEM infrastructure.
    // [M2] one SFA operand (Q, fixed/shared), two SFB operands (K) so the
    // K scale-factor TMEM operand ping-pongs while QK(k)/QK(k+1) overlap.
    // ===================================================================
    Tensor sSFQ = make_tensor(make_smem_ptr(storage.smem_sfq.data()), SmemLayoutSFQ{});
    Tensor sSFK = make_tensor(make_smem_ptr(storage.smem_sfk.data()), SmemLayoutSFK{});

    // SF TMEM operands — the block-scaled UMMA reads SFA/SFB from TMEM.
    Tensor tCtSFA = make_tensor<typename TiledMmaQK::FrgTypeSFA>(shape(SmemLayoutAtomSFA{}));
    Tensor tCtSFB = make_tensor<typename TiledMmaQK::FrgTypeSFB>(shape(SmemLayoutAtomSFB{}));

    Tensor tCtSFA0 = tCtSFA;  tCtSFA0.data() = tCtSFA0.data().get() + uint32_t(TmemAllocation::SFA0);
    Tensor tCtSFB0 = tCtSFB;  tCtSFB0.data() = tCtSFB0.data().get() + uint32_t(TmemAllocation::SFB0);
    Tensor tCtSFB1 = tCtSFB;  tCtSFB1.data() = tCtSFB1.data().get() + uint32_t(TmemAllocation::SFB1);

    // UTCCP (tcgen05.cp) smem->TMEM copies, mirroring the block-scaled
    // GEMM collective's mma_init().
    using UtccpOp = SM100_UTCCP_4x32dp128bit_1cta;
    auto tCtSFA0_c = make_tensor(tCtSFA0.data(), filter_zeros(tCtSFA0.layout()));
    auto tCtSFB0_c = make_tensor(tCtSFB0.data(), filter_zeros(tCtSFB0.layout()));
    auto tCtSFB1_c = make_tensor(tCtSFB1.data(), filter_zeros(tCtSFB1.layout()));

    auto utccp_SFA = make_utccp_copy(UtccpOp{}, tCtSFA0_c);
    auto utccp_SFB = make_utccp_copy(UtccpOp{}, tCtSFB0_c);

    auto sSFQ_c = make_tensor(sSFQ.data(), filter_zeros(sSFQ.layout()));
    auto sSFK_c = make_tensor(sSFK.data(), filter_zeros(sSFK.layout()));

    auto thr_utccp_SFA = utccp_SFA.get_slice(0);
    auto thr_utccp_SFB = utccp_SFB.get_slice(0);

    auto sfq_s2t_src = get_utccp_smem_desc_tensor<UtccpOp>(thr_utccp_SFA.partition_S(sSFQ_c));
    auto sfk_s2t_src = get_utccp_smem_desc_tensor<UtccpOp>(thr_utccp_SFB.partition_S(sSFK_c));
    auto sfa0_s2t_dst = thr_utccp_SFA.partition_D(tCtSFA0_c);
    auto sfb0_s2t_dst = thr_utccp_SFB.partition_D(tCtSFB0_c);
    auto sfb1_s2t_dst = thr_utccp_SFB.partition_D(tCtSFB1_c);
    // ===================================================================

    // ===================================================================
    // [PVMX 2a.1b/2b] block-scaled PV SF: SFP (P, online) and SFV (V, from gmem)
    // both double-buffered + per-tile UTCCP. V-SF is staged by the load TMA into
    // smem_sfv (KV pipeline); SFP is computed by softmax into smem_sfp.
    // ===================================================================
    Tensor sSFP_full = make_tensor(make_smem_ptr(storage.smem_sfp.data()), SmemLayoutSFP{});
    Tensor sSFV_full = make_tensor(make_smem_ptr(storage.smem_sfv.data()), SmemLayoutSFV{});
    Tensor tCtSFP = make_tensor<typename TiledMmaPV::FrgTypeSFA>(shape(SmemLayoutAtomSFP{}));
    Tensor tCtSFV = make_tensor<typename TiledMmaPV::FrgTypeSFB>(shape(SmemLayoutAtomSFV{}));
    Tensor tCtSFP0 = tCtSFP;  tCtSFP0.data() = tCtSFP0.data().get() + uint32_t(TmemAllocation::SFP0);
    Tensor tCtSFP1 = tCtSFP;  tCtSFP1.data() = tCtSFP1.data().get() + uint32_t(TmemAllocation::SFP1);
    Tensor tCtSFV0 = tCtSFV;  tCtSFV0.data() = tCtSFV0.data().get() + uint32_t(TmemAllocation::SFV0);
    Tensor tCtSFV1 = tCtSFV;  tCtSFV1.data() = tCtSFV1.data().get() + uint32_t(TmemAllocation::SFV1);
    auto tCtSFP0_c = make_tensor(tCtSFP0.data(), filter_zeros(tCtSFP0.layout()));
    auto tCtSFP1_c = make_tensor(tCtSFP1.data(), filter_zeros(tCtSFP1.layout()));
    auto tCtSFV0_c = make_tensor(tCtSFV0.data(), filter_zeros(tCtSFV0.layout()));
    auto tCtSFV1_c = make_tensor(tCtSFV1.data(), filter_zeros(tCtSFV1.layout()));
    auto utccp_SFP = make_utccp_copy(UtccpOp{}, tCtSFP0_c);
    auto utccp_SFV = make_utccp_copy(UtccpOp{}, tCtSFV0_c);
    auto sSFP_c  = make_tensor(sSFP_full.data(), filter_zeros(sSFP_full.layout()));
    auto sSFV_c  = make_tensor(sSFV_full.data(), filter_zeros(sSFV_full.layout()));
    auto thr_utccp_SFP = utccp_SFP.get_slice(0);
    auto thr_utccp_SFV = utccp_SFV.get_slice(0);
    auto sfp_s2t_src = get_utccp_smem_desc_tensor<UtccpOp>(thr_utccp_SFP.partition_S(sSFP_c));
    auto sfv_s2t_src = get_utccp_smem_desc_tensor<UtccpOp>(thr_utccp_SFV.partition_S(sSFV_c));
    auto sfp0_s2t_dst = thr_utccp_SFP.partition_D(tCtSFP0_c);
    auto sfp1_s2t_dst = thr_utccp_SFP.partition_D(tCtSFP1_c);
    auto sfv0_s2t_dst = thr_utccp_SFV.partition_D(tCtSFV0_c);
    auto sfv1_s2t_dst = thr_utccp_SFV.partition_D(tCtSFV1_c);
    // [PVMX 2b] per-tile P-SF UTCCP (mirror load_sfb): smem_sfp[buf] -> SFP[buf] TMEM.
    auto load_sfp = [&](int buf) {
      if (cute::elect_one_sync()) {
        if (buf == 0) copy(utccp_SFP, sfp_s2t_src(_,_,_,_,0), sfp0_s2t_dst);
        else          copy(utccp_SFP, sfp_s2t_src(_,_,_,_,1), sfp1_s2t_dst);
      }
    };
    // [PVMX 2a.1b] per-tile V-SF UTCCP: smem_sfv[vidx] -> SFV[buf] TMEM.
    auto load_sfv = [&](int vidx, int buf) {
      if (cute::elect_one_sync()) {
        if (buf == 0) copy(utccp_SFV, sfv_s2t_src(_,_,_,_,vidx), sfv0_s2t_dst);
        else          copy(utccp_SFV, sfv_s2t_src(_,_,_,_,vidx), sfv1_s2t_dst);
      }
    };
    // ===================================================================

    int k_index = 0;
    int v_index = 0;
    int q_index = 0;
    int sp_index = 0;

    // wait for Q
    q_index = pipeline_q_consumer_state.index();
    pipeline_q.consumer_wait(pipeline_q_consumer_state);
    ++pipeline_q_consumer_state;

    Tensor tSrQ0 = tSrQ(_,_,_,q_index);

    int n = mask_tile_count;   // number of KV tiles

    // ---- helpers -----------------------------------------------------
    // SFA (Q scale factors) is loaded smem->TMEM exactly ONCE: Q is fixed for
    // all KV tiles, so a single SFA0 slot is reused — re-copying it per QK
    // would race the in-flight QK that reads it once QK tiles overlap.
    auto load_sfa = [&]() {
      if (cute::elect_one_sync())
        copy(utccp_SFA, sfq_s2t_src(_,_,_,_,q_index), sfa0_s2t_dst);
    };
    // SFB (K scale factors) ping-pongs by buf — QK(k)/QK(k+1) overlap, so the
    // K scale-factor TMEM operand must double-buffer like S.
    auto load_sfb = [&](int kidx, int buf) {
      if (cute::elect_one_sync()) {
        if (buf == 0) copy(utccp_SFB, sfk_s2t_src(_,_,_,_,kidx), sfb0_s2t_dst);
        else          copy(utccp_SFB, sfk_s2t_src(_,_,_,_,kidx), sfb1_s2t_dst);
      }
    };
    // issue QK(tile) -> S[buf], block-scaled MXFP8 with SFB[buf].
    // buf is warp-uniform (pipeline state); warp_uniform makes the TMEM
    // operand addresses explicitly uniform for the tcgen05 MMA.
    auto do_qk = [&](int kidx, int buf) {
      Tensor tStSk   = tStS;    tStSk.data()   = tStS.data().get()   + warp_uniform(uint32_t(buf ? TmemAllocation::S1   : TmemAllocation::S0));
      Tensor tCtSFBk = tCtSFB;  tCtSFBk.data() = tCtSFB.data().get() + warp_uniform(uint32_t(buf ? TmemAllocation::SFB1 : TmemAllocation::SFB0));
      load_sfb(kidx, buf);
      gemm_bs(mma_qk, /*zero_acc=*/true, tSrQ0, tSrK(_,_,_,kidx), tStSk, tCtSFA0, tCtSFBk);
    };
    // issue PV(tile) reading P[buf] -> O. first_pv zero-inits O; otherwise
    // accumulate (do NOT reset accumulate_ to Zero — see milestone-1 fix).
    auto do_pv = [&](int vidx, int buf, bool first_pv) {
      Tensor tOrPk   = tOrP(_,_,_,buf);   // [PVMX 2a.0] smem P, stage `buf`
      Tensor tCtSFPk = tCtSFP;
      tCtSFPk.data() = tCtSFP.data().get() + warp_uniform(uint32_t(buf ? TmemAllocation::SFP1 : TmemAllocation::SFP0));
      Tensor tCtSFVk = tCtSFV;
      tCtSFVk.data() = tCtSFV.data().get() + warp_uniform(uint32_t(buf ? TmemAllocation::SFV1 : TmemAllocation::SFV0));
      // [PVMX 2a.1b/2b] block-scaled SS PV with per-tile P-SF (SFP[buf]) AND per-tile V-SF (SFV[buf]).
      gemm_bs(mma_pv, /*zero_acc=*/first_pv, tOrPk, tOrV(_,_,_,vidx), tOtO0, tCtSFPk, tCtSFVk);
    };
    // ------------------------------------------------------------------

    // ---- prologue: QK0 -> S0 -----------------------------------------
    // wait K0
    k_index = pipeline_kv_consumer_state.index();
    pipeline_kv.consumer_wait(pipeline_kv_consumer_state);
    ++pipeline_kv_consumer_state;

#ifdef MXFP8_DBG
    if (blockIdx.x == 0 && blockIdx.y == 0 && blockIdx.z == 0 &&
        (threadIdx.x % 32) == 0) {
      auto* psfk = reinterpret_cast<uint8_t*>(storage.smem_sfk.data());
      auto* psfq = reinterpret_cast<uint8_t*>(storage.smem_sfq.data());
      for (int i = 0; i < 2048; ++i) { g_dbg_sfk[i] = psfk[i]; g_dbg_sfq[i] = psfq[i]; }
    }
#endif

    // Q scale factors -> SFA0 TMEM, once (Q is fixed for all KV tiles).
    load_sfa();

    {
      int buf = pipeline_s0_producer_state.index();          // 0
      pipeline_s0.producer_acquire(pipeline_s0_producer_state);
      do_qk(k_index, buf);                                   // QK0 -> S0
      pipeline_s0.producer_commit(pipeline_s0_producer_state);
      ++pipeline_s0_producer_state;
    }

    // wait V0 (held until PV0 in loop iter 1)
    int v_index_prev = pipeline_kv_consumer_state.index();
    pipeline_kv.consumer_wait(pipeline_kv_consumer_state);
    ++pipeline_kv_consumer_state;

    // release K0  (KV release order == slot order: K0,V0,K1,V1,...)
    pipeline_kv.consumer_release(pipeline_kv_release_state);
    ++pipeline_kv_release_state;

    // acquire S buffer 1 for QK1 (loop iter 1). depth-2 acquire #1 is granted
    // immediately (no prior occupant of buffer 1).
    pipeline_s0.producer_acquire(pipeline_s0_producer_state);

    // ---- loop: iter k = 1..n-1 issues QK(k) -> S[k%2] and PV(k-1) ------
    // Double-buffered S: QK(k) writes the buffer NOT being read by softmax,
    // so QK runs a tile ahead of softmax (the recovered overlap). PV(k-1) is
    // gated by producer_acquire #(k+1) which (depth 2) waits softmax
    // release #(k-1) ⟹ P[(k-1)%2] is ready.
    bool first_pv = true;
    for (int k = 1; k < n; ++k) {
      int qk_buf = pipeline_s0_producer_state.index();       // == k % 2
      int pv_buf = (k - 1) & 1;                              // == (k-1) % 2

      // wait K(k)
      k_index = pipeline_kv_consumer_state.index();
      pipeline_kv.consumer_wait(pipeline_kv_consumer_state);
      ++pipeline_kv_consumer_state;

      // QK(k) -> S[k%2]
      do_qk(k_index, qk_buf);
      pipeline_s0.producer_commit(pipeline_s0_producer_state);
      ++pipeline_s0_producer_state;

      // wait V(k) (held until PV(k) next iter)
      v_index = pipeline_kv_consumer_state.index();
      pipeline_kv.consumer_wait(pipeline_kv_consumer_state);
      ++pipeline_kv_consumer_state;

      // PV(k-1): P[(k-1)%2] * V(k-1) -> O
      // V(k-1) and its SF arrived before this iteration. Stage SFV into its
      // ping-pong TMEM slot before waiting for O or P, overlapping UTCCP
      // latency with both independent critical-path waits.
      load_sfv(v_index_prev, pv_buf);
      pipeline_corr.producer_acquire(pipeline_corr_producer_state);
      pipeline_s0.producer_acquire(pipeline_s0_producer_state);   // acquire #(k+1): P[(k-1)%2] ready
      
      sp_index = pipeline_sfp_consumer_state.index();
      pipeline_sfp.consumer_wait(pipeline_sfp_consumer_state);
      ++pipeline_sfp_consumer_state;

      load_sfp(pv_buf);                // [PVMX 2b] stage P-SF[pv_buf] -> SFP[pv_buf]

      do_pv(v_index_prev, pv_buf, first_pv);
      first_pv = false;

      pipeline_sfp.consumer_release(pipeline_sfp_release_state);
      ++pipeline_sfp_release_state;

      pipeline_corr.producer_commit(pipeline_corr_producer_state);
      ++pipeline_corr_producer_state;

      // release V(k-1) then K(k)  (slot order)
      pipeline_kv.consumer_release(pipeline_kv_release_state);
      ++pipeline_kv_release_state;
      pipeline_kv.consumer_release(pipeline_kv_release_state);
      ++pipeline_kv_release_state;

      v_index_prev = v_index;
    }

    // ---- tail: PV(n-1) -----------------------------------------------
    {
      int pv_buf = (n - 1) & 1;
      pipeline_corr.producer_acquire(pipeline_corr_producer_state);
      // balancing commit for the S buffer acquired at the end of the last loop
      // iteration (no QK follows); then acquire #(n+1) waits softmax
      // release #(n-1) ⟹ P[(n-1)%2] ready.
      pipeline_s0.producer_commit(pipeline_s0_producer_state);
      ++pipeline_s0_producer_state;
      pipeline_s0.producer_acquire(pipeline_s0_producer_state);

      sp_index = pipeline_sfp_consumer_state.index();
      pipeline_sfp.consumer_wait(pipeline_sfp_consumer_state);
      ++pipeline_sfp_consumer_state;

      load_sfp(pv_buf);                // [PVMX 2b] tail: stage P-SF[pv_buf] -> SFP[pv_buf]
      load_sfv(v_index_prev, pv_buf);  // [PVMX 2a.1b] tail: stage V-SF[v_index_prev] -> SFV[pv_buf]
      do_pv(v_index_prev, pv_buf, first_pv);
      pipeline_corr.producer_commit(pipeline_corr_producer_state);
      ++pipeline_corr_producer_state;

      pipeline_sfp.consumer_release(pipeline_sfp_release_state);
      ++pipeline_sfp_release_state;

      // release V(n-1)
      pipeline_kv.consumer_release(pipeline_kv_release_state);
      ++pipeline_kv_release_state;

      // final balancing commit (matches the trailing producer_acquire)
      pipeline_s0.producer_commit(pipeline_s0_producer_state);
      ++pipeline_s0_producer_state;
    }

    // release Q
    pipeline_q.consumer_release(pipeline_q_release_state);
    ++pipeline_q_release_state;
  }

  template<bool need_apply_mask, class Stage, class BlkCoord, class CoordTensor, class ProblemShape>
  CUTLASS_DEVICE auto
  softmax_step(
      float& row_max, float& row_sum,
      Stage stage, bool final_call,
      BlkCoord const& blk_coord, CoordTensor const& cS,
      Params const& params, ProblemShape const& problem_shape,
      TensorStorage& storage,    // [PVMX 2a.0] write P -> storage.smem_p
      PipelineS& pipeline_s, typename PipelineS::PipelineState& pipeline_s_consumer_state,
      PipelineC& pipeline_c, typename PipelineC::PipelineState& pipeline_c_producer_state,
      OrderBarrierSoftmax& order_s) {

    // [MXFP8 N128 M3b] split-N cooperative softmax. stage 0 (warps 0-3) owns
    // KV columns [0:64]; stage 1 (warps 4-7) owns [64:128]. Each group reduces
    // its 64-col half; row_max (every step) and final row_sum are combined
    // across groups via a smem exchange + NamedBarrier. Only group 0 (is_g0)
    // drives the mma->softmax / softmax->correction pipelines and writes the
    // V-stats; group 1 stays lockstepped purely through the NamedBarriers.
    const bool is_g0 = (stage == 0);
    const uint32_t nHalf = uint32_t(stage) * 64u;   // TMEM col offset of this group's score half
    const uint32_t pHalf = uint32_t(stage) * 16u;   // TMEM col offset of this group's P half (64 e4m3 = 16 fp32 cols)

    // smem exchange buffer for the cross-group row_max / row_sum reduction —
    // one float per (group, row). Function-static: both softmax groups execute
    // this same instantiation so they share the one allocation; other warp
    // roles never reach here.
    __shared__ float smem_sm_exch[2][128];

    Tensor tScS_full = typename CollectiveMmaQK::TiledMma{}.get_slice(0).partition_C(cS);

    // [MXFP8 N128 M2] double-buffered S: select the S/P/V buffer from the
    // S-pipeline consumer state index (0/1). softmax_step(k) consumes commit #k
    // → index == k%2 → buffer k%2, exactly the buffer QK(k) wrote. group 1
    // mirrors group 0's pipeline-state ++ so this index stays in lockstep.
    int sbuf = pipeline_s_consumer_state.index();

    // Build the full (128,128) C fragment exactly as the single-group variant,
    // then compose down to this group's 64-col half (the proven sub-view path
    // already used for tStS_v / tStS_P — avoids partitioning the N=128 TiledMma
    // over a 64-wide tile directly).
    Tensor tStS_full = partition_fragment_C(typename CollectiveMmaQK::TiledMma{}, select<0,1>(TileShapeQK{}));
    uint32_t s_base = (sbuf == 0 ? uint32_t(TmemAllocation::S0) : uint32_t(TmemAllocation::S1));

    Tensor tStS = tStS_full.compose(make_layout(make_shape(_128{}, _64{})));
    tStS.data() = warp_uniform(s_base + nHalf);     // this group's 64-col score half
    // [mask fix] group g owns score cols [g*64 : g*64+64) = kv [tile_base + g*64 : ...).
    // tStS.data() gets +nHalf (above), but the COORDINATE used for masking must too —
    // else ResidualMask::apply_mask (kv >= seqlen_kv) tests the wrong kv for group 1 and
    // leaves its half of the partial last tile unmasked (S%128!=0 -> biased low). Offset
    // the score-coordinate's kv by nHalf so both groups see global kv.
    Tensor tScS = domain_offset(make_coord(_0{}, nHalf),
                                tScS_full.compose(make_layout(make_shape(_128{}, _64{}))));

    // V-stats (old/new row_max, then final row_sum/max) live at the S-buffer
    // base cols [0:2] — NOT split; written by group 0 only.
    Tensor tStS_v = tStS_full.compose(make_layout(make_shape(_128{}, _2{})));
    tStS_v.data() = warp_uniform(s_base);
    Tensor tScS_v = tScS_full.compose(make_layout(make_shape(_128{}, _2{})));

    // per-group P half is 64 e4m3 = 16 fp32-words wide.
    auto tilePlikeFP32 = Int<64>{} / Int<sizeof(float)>{} * Int<sizeof(Element)>{};
    uint32_t p_base = (sbuf == 0 ? uint32_t(TmemAllocation::P0) : uint32_t(TmemAllocation::P1));
    Tensor tStS_P = tStS_full.compose(make_layout(make_shape(_128{}, tilePlikeFP32)));
    tStS_P.data() = warp_uniform(p_base + pHalf);
    Tensor tScS_P = tScS_full.compose(make_layout(make_shape(_128{}, tilePlikeFP32)));

    // Each thread owns a single row.
    #if defined CUTE_ARCH_TCGEN05_TMEM_STAT_ENABLED
      using TMEM_LOAD = SM100_TMEM_LOAD_STAT_32dp32b32x;
    #else
      // [MXFP8 N128 M3b] split-N: each group reads only its 64-col score half.
      // Use the 64-wide TMEM-load atom so the whole half lands in ONE tcgen05.ld
      // per thread (vs two with the 32x atom) — fewer MIO-queued instructions,
      // which NCU flagged as a top secondary stall (mio_throttle). STAT is not
      // enabled for sm_100a (SM103 only), so this #else is the live path.
      using TMEM_LOAD = SM100_TMEM_LOAD_32dp32b32x;
    #endif
    // [MXFP8] P-tile width follows TileShapeQK's N. With the Route-C N=64 tile
    // the P-tile is 16 fp32-words wide, so the store atom must be the 16x form
    // (the pristine N=128 collective used 32x).
    using TMEM_STORE = std::conditional_t<
        decltype(tilePlikeFP32)::value >= 32,
        SM100_TMEM_STORE_32dp32b32x, SM100_TMEM_STORE_32dp32b16x>;
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

    auto tiled_tmem_store = make_tmem_copy(TMEM_STORE{}, tStS_P);
    auto thr_tmem_store  = tiled_tmem_store.get_slice(thread_idx);

    Tensor tTMEM_STOREtS_x4 = thr_tmem_store.partition_D(tStS_P);
    tTMEM_STOREtS_x4.data() = warp_uniform(tTMEM_STOREtS_x4.data().get());
    Tensor tTMEM_STOREcS = thr_tmem_store.partition_S(tScS_P);
    // wait on tensor core pipe — only group 0 holds the mma->softmax pipeline;
    // it waits for QK(k) to land in TMEM, then B_SREADY lets group 1 (which
    // holds no mma pipeline) safely read its score half. Both groups arrive.
    if (is_g0) {
      pipeline_s.consumer_wait(pipeline_s_consumer_state);
    }
    cutlass::arch::NamedBarrier::arrive_and_wait(256, /*B_SREADY*/ 0u);

    // read all of S from tmem into reg mem
    Tensor tTMEM_LOADrS = make_tensor<ElementQK>(shape(tTMEM_LOADcS));
    copy(tiled_tmem_load, tTMEM_LOADtS, tTMEM_LOADrS);

#ifdef MXFP8_DBG
    // Dump raw QK scores, blk 0, stage 0, first KV tile.
    if (blockIdx.x == 0 && blockIdx.y == 0 && blockIdx.z == 0 &&
        stage == _0{} && get<1>(tTMEM_LOADcS(0)) == 0) {
      int row = get<0>(tTMEM_LOADcS(0));
      if (row == 0) {                              // S[0, kv=0..63]
        for (int i = 0; i < size(tTMEM_LOADrS) && i < 64; ++i)
          g_dbg_S[i] = tTMEM_LOADrS(i);
        g_dbg_S_got = (int)size(tTMEM_LOADrS);
      }
      if (row >= 0 && row < 64)                    // S[q=row, kv=0]
        g_dbg_Srow[row] = tTMEM_LOADrS(0);
    }
#endif

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
      // compute rowmax
      float row_max_0 = row_max;
      float row_max_1 = row_max;
      float row_max_2 = row_max;
      float row_max_3 = row_max;
      CUTLASS_PRAGMA_UNROLL
      for (int i = 0; i < size(tTMEM_LOADrS); i += 4) {
        row_max_0  = ::fmax(row_max_0, tTMEM_LOADrS(i));
        row_max_1 = ::fmax(row_max_1, tTMEM_LOADrS(i+1));
        row_max_2 = ::fmax(row_max_2, tTMEM_LOADrS(i+2));
        row_max_3 = ::fmax(row_max_3, tTMEM_LOADrS(i+3));
      }
      row_max = ::fmax(row_max_0, row_max_1);
      row_max = ::fmax(row_max, row_max_2);
      row_max = ::fmax(row_max, row_max_3);
    }

    // [MXFP8 N128 M3b] combine the per-group partial row_max across the two
    // N-halves so both groups hold the true global row_max before computing P.
    // (Both groups already finished reading their score halves into registers,
    // so it is now safe for group 1's P write to land in group 0's old score
    // region — see the P-in-S note below.)
    smem_sm_exch[stage][thread_idx] = row_max;
    cutlass::arch::NamedBarrier::arrive_and_wait(256, /*B_REDUCE*/ 1u);
    row_max = ::fmax(row_max, smem_sm_exch[stage ^ 1][thread_idx]);

    ElementQK row_max_safe = row_max == -INFINITY ? 0 : row_max;

    // V-stats (old/new row_max) — global after the exchange, identical on both
    // groups; write once, from group 0 only, to avoid a redundant store race.
    if (is_g0) {
      Tensor tTMEM_STOREVrS = make_tensor<ElementQK>(shape(tTMEM_STOREVcS));
      tTMEM_STOREVrS(kIdxOldRowMax) = old_row_max;
      tTMEM_STOREVrS(kIdxNewRowMax) = row_max_safe;
      copy(tiled_tmem_storev, tTMEM_STOREVrS, tTMEM_STOREVtS);

      pipeline_c.producer_commit(pipeline_c_producer_state);
      ++pipeline_c_producer_state;
    }

    ElementQK scale = params.scale_softmax_log2;
    ElementQK row_max_scale = row_max_safe * scale;

    float2 scale_fp32x2 = make_float2(scale, scale);
    float2 minus_row_max_scale_fp32x2 = make_float2(-row_max_scale, -row_max_scale);
    
    constexpr int kConversionsPerStep = 16;
    NumericArrayConverter<Element, ElementQK, kConversionsPerStep> convert;
    Tensor sP_st = make_tensor(make_smem_ptr(storage.smem_p.data()), SmemLayoutP{})(_, _, _, sbuf);
    Tensor sSFP_full = make_tensor(make_smem_ptr(storage.smem_sfp.data()), SmemLayoutSFP{});
    Tensor sSFP_st = sSFP_full(_, _, _, sbuf);
    const int p_row = thread_idx;
    const int p_kbase = int(nHalf);

    // [MXFP8 N128 M3b] the pristine inter-group OrderBarrier (order_s) is an
    // ordered (group0-then-group1) sequencer; split-N instead needs symmetric
    // rendezvous + reduction, done with NamedBarriers above. order_s unused.
    (void) order_s;

    // [PVMX 2b][opt 20260605] Phase 1+2 FUSED: scale+exp -> P (fp32) in tTMEM_LOADrS
    // AND accumulate per-32 amax in the SAME pass. P = exp2(...) >= 0 always, so the
    // old fabsf() was redundant; and the separate amax pass over 64 regs is folded in.
    // Thread owns its row's 64 cols (group half) = 2 SF blocks (size=64; 64/32=2).
    // stride-2 keeps i and i+1 in the same SF block (kSFVec=32 even), so one fmax/pair.
    CUTLASS_PRAGMA_UNROLL
    for (int i = 0; i < size(tTMEM_LOADrS); i += kConversionsPerStep) {
      CUTLASS_PRAGMA_UNROLL
      for (int j = 0; j < kConversionsPerStep; j += 2) {
        float2 in = make_float2(
          tTMEM_LOADrS(i + j + 0),
          tTMEM_LOADrS(i + j + 1)
        );
        float2 out;
        cute::fma(out, scale_fp32x2, in, minus_row_max_scale_fp32x2);
        tTMEM_LOADrS(i + j + 0) = fast_exp2f(out.x);
        tTMEM_LOADrS(i + j + 1) = fast_exp2f(out.y);
      }

      // Quantize FP32 → E4M3 and pack into register buffer (uint32_t per 4×FP8)
      int local_k = p_kbase + i;
      int sf_group = local_k / 32;
      auto sf_coord = make_coord(
          make_coord(make_coord(make_coord(p_row % 32, p_row / 32), _0{}),
                     make_coord(_0{}, _0{})),
          _0{}, make_coord(sf_group, _0{}));
      ElementQK inv_scale = ElementQK(1.0f) / ElementQK(sSFP_st(sf_coord));
      Array<ElementQK, kConversionsPerStep> in_conv;
      CUTLASS_PRAGMA_UNROLL
      for (int j = 0; j < kConversionsPerStep; j++) {
        ElementQK val_fp32 = tTMEM_LOADrS(i + j) * inv_scale;
        in_conv[j] = fminf(448.0f, fmaxf(-448.0f, val_fp32));
      }
      Array<Element, kConversionsPerStep> out_conv = convert(in_conv);
      auto out_words = reinterpret_cast<uint32_t const*>(&out_conv);
      Element& dst_byte0 = sP_st(make_coord(p_row, local_k % 32), _0{}, local_k / 32);
      uint4 packed = make_uint4(out_words[0], out_words[1], out_words[2], out_words[3]);
      *reinterpret_cast<uint4*>(&dst_byte0) = packed;
    }

    cutlass::arch::fence_view_async_shared();

    // [MXFP8 N128 M3b] P-in-S: P is embedded at S+32 (P0/P1). group 0 writes
    // P[0:64]→fp32 cols [32:48], group 1 writes P[64:128]→[48:64]. group 1's
    // P write lands inside group 0's old score region [0:64], so both groups
    // must have finished reading scores (done — past B_REDUCE) AND finished
    // writing their P half before group 0 releases the S buffer (mma then
    // reuses it / PV reads the full 128-wide P).
    cutlass::arch::NamedBarrier::arrive_and_wait(256, /*B_PDONE*/ 2u);

    // notify tensor core warp that P is ready (group 0 owns the pipeline);
    // both groups advance the consumer state so sbuf stays in lockstep.
    if (is_g0) {
      pipeline_s.consumer_release(pipeline_s_consumer_state);
    }
    ++pipeline_s_consumer_state;

    ElementQK acc_scale = (old_row_max == row_max_safe)
        ? 0.5f
        : 0.5f * fast_exp2f(scale * (old_row_max - row_max_safe));
    row_sum *= acc_scale;
    // Compute row-sum after releasing P so it overlaps the following PV.
    float2 local_row_sum_f32x2 = make_float2(row_sum, row_sum);
    float2 local_row_sum_1 = make_float2(0, 0);
    float2 local_row_sum_2 = make_float2(0, 0);
    float2 local_row_sum_3 = make_float2(0, 0);

    CUTLASS_PRAGMA_UNROLL
    for (int i = 0; i < size(tTMEM_LOADrS); i += 8) {
      float2 in = make_float2(tTMEM_LOADrS(i), tTMEM_LOADrS(i+1));
      cute::add(local_row_sum_f32x2, local_row_sum_f32x2, in);

      in = make_float2(tTMEM_LOADrS(i+2), tTMEM_LOADrS(i+3));
      cute::add(local_row_sum_1, local_row_sum_1, in);

      in = make_float2(tTMEM_LOADrS(i+4), tTMEM_LOADrS(i+5));
      cute::add(local_row_sum_2, local_row_sum_2, in);

      in = make_float2(tTMEM_LOADrS(i+6), tTMEM_LOADrS(i+7));
      cute::add(local_row_sum_3, local_row_sum_3, in);
    }

    cute::add(local_row_sum_f32x2, local_row_sum_f32x2, local_row_sum_1);
    cute::add(local_row_sum_2, local_row_sum_2, local_row_sum_3);
    cute::add(local_row_sum_f32x2, local_row_sum_f32x2, local_row_sum_2);
    float local_row_sum = local_row_sum_f32x2.x + local_row_sum_f32x2.y;

    row_sum = local_row_sum;

    // The next correction signal is not committed until the next softmax
    // step. Delay its depth-1 producer acquire until after this step's
    // independent row-sum work, overlapping correction consumption with the
    // reduction instead of blocking group 0 before it.
    if (is_g0) {
      pipeline_c.producer_acquire(pipeline_c_producer_state);
    }

    if (final_call) {
      // [MXFP8 N128 M3b] each group summed only its own 64-col half of every
      // P tile, so its running row_sum is a partial. Combine into the global
      // row_sum (the per-step acc_scale rescales are identical on both groups,
      // so the two partials add up correctly). Reuse the B_REDUCE barrier.
      smem_sm_exch[stage][thread_idx] = row_sum;
      cutlass::arch::NamedBarrier::arrive_and_wait(256, /*B_REDUCE*/ 1u);
      row_sum += smem_sm_exch[stage ^ 1][thread_idx];

      if (is_g0) {
        // re-acquire the S part in the final step
        pipeline_s.consumer_wait(pipeline_s_consumer_state);

        Tensor tTMEM_STOREVrS = make_tensor<ElementQK>(shape(tTMEM_STOREVcS));
        tTMEM_STOREVrS(kIdxFinalRowMax) = row_max;
        tTMEM_STOREVrS(kIdxFinalRowSum) = row_sum;
        copy(tiled_tmem_storev, tTMEM_STOREVrS, tTMEM_STOREVtS);
      }
    }
  }

  template<class Stage, class BlkCoord, class ProblemShape>
  CUTLASS_DEVICE auto
  softmax(
      Stage stage,
      BlkCoord const& blk_coord,
      Params const& params, ProblemShape const& problem_shape,
      TensorStorage& storage,    // [PVMX 2a.0] softmax writes P -> storage.smem_p
      PipelineSFP& pipeline_sfp, typename PipelineSFP::PipelineState& pipeline_sfp_consumer_state,
      PipelineS& pipeline_s, typename PipelineS::PipelineState& pipeline_s_consumer_state,
      PipelineC& pipeline_c, typename PipelineC::PipelineState& pipeline_c_producer_state,
      OrderBarrierSoftmax& order_s) {

    int mask_tile_count = Mask{}.get_unmasked_trip_count(blk_coord, TileShape{}, problem_shape);

    ElementQK row_max = -INFINITY;
    ElementQK row_sum = 0;

    Tensor cS_base = make_identity_tensor(select<0,1>(TileShapeQK{}));
    auto logical_offset = make_coord(
        get<0>(blk_coord) * get<0>(TileShape{}) + (stage % get<0>(ThreadShape{})) * get<0>(TileShapeQK{}),
        0 + (stage % get<1>(ThreadShape{})) * get<1>(TileShapeQK{})
    );
    Tensor cS = domain_offset(logical_offset, cS_base);

    // [MXFP8 N128 M3b] split-N: group 0 (stage 0) owns the mma->softmax /
    // softmax->correction pipelines; group 1 holds none and only rendezvouses
    // via the NamedBarriers inside softmax_step.
    const bool is_g0 = (stage == 0);

    if (is_g0) {
      pipeline_c.producer_acquire(pipeline_c_producer_state);
    }

    CUTLASS_PRAGMA_NO_UNROLL
    for (; mask_tile_count > 0; mask_tile_count -= 1) {
      if (is_g0) pipeline_sfp.consumer_wait(pipeline_sfp_consumer_state);
      softmax_step<false /* need_apply_mask */>(
          row_max, row_sum, stage,
          (mask_tile_count == 1) &&
              (Mask{}.get_masked_trip_count(blk_coord, TileShape{}, problem_shape) == 0),
          blk_coord, cS, params, problem_shape, storage,
          pipeline_s, pipeline_s_consumer_state,
          pipeline_c, pipeline_c_producer_state,
          order_s
      );
      if (is_g0) ++pipeline_sfp_consumer_state;

      cS.data() = cS.data() + E<1>{} * get<1>(ThreadShape{}) * get<1>(TileShapeQK{});
    }

    // Masked iterations
    mask_tile_count = Mask{}.get_masked_trip_count(blk_coord, TileShape{}, problem_shape);

    CUTLASS_PRAGMA_NO_UNROLL
    for (; mask_tile_count > 0; mask_tile_count -= 1) {
      if (is_g0) pipeline_sfp.consumer_wait(pipeline_sfp_consumer_state);
      softmax_step<true /* need_apply_mask */>(
          row_max, row_sum, stage, mask_tile_count == 1,
          blk_coord, cS, params, problem_shape, storage,
          pipeline_s, pipeline_s_consumer_state,
          pipeline_c, pipeline_c_producer_state,
          order_s
      );
      if (is_g0) ++pipeline_sfp_consumer_state;

      cS.data() = cS.data() + E<1>{} * get<1>(ThreadShape{}) * get<1>(TileShapeQK{});
    }

    // [MXFP8 N128 M3b] only group 0 drives the pipelines (group 1 produced no
    // s_corr signals and consumed no mma->softmax commits — its s1 pipelines
    // stay inert). The trailing balancing matches the single-stage variant.
    if (is_g0) {
      pipeline_c.producer_commit(pipeline_c_producer_state);
      ++pipeline_c_producer_state;

      pipeline_c.producer_acquire(pipeline_c_producer_state);
      // [MXFP8 N128 M2] empty steps to sync against pipe s. The double-buffered
      // mma() issues n+2 S-pipeline commits (n QK + 2 tail balancing commits, vs
      // n+1 in single-buffered milestone-1): n consumed by softmax_step, one by
      // the final_call re-wait, so TWO trailing empty wait/release pairs balance
      // it (milestone-1 needed only one).
      pipeline_s.consumer_release(pipeline_s_consumer_state);
      ++pipeline_s_consumer_state;
      pipeline_s.consumer_wait(pipeline_s_consumer_state);
      pipeline_s.consumer_release(pipeline_s_consumer_state);
      ++pipeline_s_consumer_state;
    }
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

    using TMEM_LOAD = std::conditional_t<kCorrectionTileSize == 32, SM100_TMEM_LOAD_32dp32b32x, SM100_TMEM_LOAD_32dp32b16x>;  // 4x32 threads with 64 cols of 32b elem

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

    const int kCorrectionTileSize = 8;

    using TMEM_LOAD = SM100_TMEM_LOAD_32dp32b8x;
    using TMEM_STORE = SM100_TMEM_STORE_32dp32b8x;

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
      PipelineE& pipeline_epi, typename PipelineE::PipelineState& pipeline_epi_producer_state,
      CollectiveEpilogue& epilogue) {

    int mask_tile_count = Mask{}.get_trip_count(blk_coord, TileShape{}, problem_shape);

    int thread_idx = threadIdx.x % (4 * cutlass::NumThreadsPerWarp);

    Tensor tStS = partition_fragment_C(typename CollectiveMmaQK::TiledMma{}, select<0,1>(TileShapeQK{}));

    Tensor cS = make_identity_tensor(select<0,1>(TileShapeQK{}));
    Tensor tScS = typename CollectiveMmaQK::TiledMma{}.get_slice(0).partition_C(cS);

    Tensor tStS_v = tStS.compose(make_layout(make_shape(_128{}, _2{})));
    Tensor tScS_v = tScS.compose(make_layout(make_shape(_128{}, _2{})));

    using TMEM_LOAD_V = SM100_TMEM_LOAD_32dp32b2x;   // 4x32 threads with 2 cols of 32b elem

    auto tiled_tmem_loadv = make_tmem_copy(TMEM_LOAD_V{}, tStS_v);
    auto thr_tmem_loadv  = tiled_tmem_loadv.get_slice(thread_idx);

    Tensor tTMEM_LOADVtS = thr_tmem_loadv.partition_S(tStS_v);
    Tensor tTMEM_LOADVcS = thr_tmem_loadv.partition_D(tScS_v);

    // [MXFP8 N128 M2] double-buffered S ⟹ the softmax→correction row stats V
    // are embedded in S0/S1 too (V0=S0, V1=S1). correction must read V[t%2]
    // where t is the KV tile the consumed softmax signal belongs to. PipelineC
    // is depth 1 so its state carries no parity — track the tile index here.
    int corr_tile = 0;
    auto loadv_stats = [&](int tile, auto& dst) {
      Tensor tv = tTMEM_LOADVtS;
      tv.data() = tTMEM_LOADVtS.data().get()
                + warp_uniform(uint32_t((tile & 1) ? TmemAllocation::V1 : TmemAllocation::V0));
      copy(tiled_tmem_loadv, tv, dst);
    };

    // [MXFP8 N128] single-stage: pipeline_s1_c is constructed by the kernel but
    // never produced (single softmax warp group signals only s0_corr).
    (void) pipeline_s1_c; (void) pipeline_s1_c_consumer_state;

    // ignore first signal from softmax — no O to rescale before the first PV
    // (this is softmax_step(0)'s signal; corr_tile stays 0).
    pipeline_s0_c.consumer_wait(pipeline_s0_c_consumer_state);
    pipeline_s0_c.consumer_release(pipeline_s0_c_consumer_state);
    ++pipeline_s0_c_consumer_state;

    mask_tile_count -= 1;

    CUTLASS_PRAGMA_NO_UNROLL
    for (; mask_tile_count > 0; mask_tile_count -= 1) {

      pipeline_s0_c.consumer_wait(pipeline_s0_c_consumer_state);

      ++corr_tile;   // this loop iteration processes softmax_step(corr_tile)
      Tensor tTMEM_LOADVrS = make_tensor<ElementQK>(shape(tTMEM_LOADVcS));

      loadv_stats(corr_tile, tTMEM_LOADVrS);

      float scale = fast_exp2f(params.scale_softmax_log2 * (tTMEM_LOADVrS(kIdxOldRowMax) - tTMEM_LOADVrS(kIdxNewRowMax)));

      // The softmax->correction signal protects the row stats in S/V. Once the
      // stats have been loaded into registers and scale is computed, correction
      // no longer needs that S buffer; release it before waiting on the single
      // O accumulator so softmax can prepare the next P/SFP tile earlier.
      pipeline_s0_c.consumer_release(pipeline_s0_c_consumer_state);
      ++pipeline_s0_c_consumer_state;

      pipeline_o.consumer_wait(pipeline_o_consumer_state);

      correction_rescale(scale, uint32_t(TmemAllocation::O0));

      cutlass::arch::fence_view_async_tmem_store();

      pipeline_o.consumer_release(pipeline_o_consumer_state);
      ++pipeline_o_consumer_state;
    }

    // tail: final normalize + epilogue for the single M=128 tile.
    // corr_tile == n-1 here — the final stats were written by the last
    // softmax_step (softmax_step(n-1)) into V[(n-1)%2].
    pipeline_s0_c.consumer_wait(pipeline_s0_c_consumer_state);

    Tensor tTMEM_LOADVrS = make_tensor<ElementQK>(shape(tTMEM_LOADVcS));
    loadv_stats(corr_tile, tTMEM_LOADVrS);

    pipeline_s0_c.consumer_release(pipeline_s0_c_consumer_state);
    ++pipeline_s0_c_consumer_state;

    pipeline_epi.producer_acquire(pipeline_epi_producer_state);

    pipeline_o.consumer_wait(pipeline_o_consumer_state);

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

    // [MXFP8 N128] single-stage: one O sub-tile, one epilogue signal.
    cutlass::arch::fence_view_async_shared();
    pipeline_epi.producer_commit(pipeline_epi_producer_state);
    ++pipeline_epi_producer_state;
  }

};

}  // namespace cutlass::fmha::collective
