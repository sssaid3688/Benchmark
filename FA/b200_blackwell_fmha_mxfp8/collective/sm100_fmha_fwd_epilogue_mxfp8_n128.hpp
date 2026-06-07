/***************************************************************************************************
 * Operator 6 — M3 / CtaN=128 single-stage variant : FMHA forward epilogue.
 *
 * Standalone modified copy of CUTLASS example 77's
 *   collective/sm100_fmha_fwd_epilogue_tma_warpspecialized.hpp
 * (pristine CUTLASS is never edited).
 *
 * Single-stage change vs the pristine epilogue — marked `// [MXFP8 N128]`:
 *   the pristine `store()` is dual-stage (the CTA owns M=256 = two 128-row O
 *   sub-tiles, driven by two pipeline signals from `correction`). The CtaN=128
 *   single-softmax-warp kernel has one M=128 tile per CTA, so `store()` issues
 *   ONE TMA store of sub-tile 0 driven by ONE pipeline signal, and indexes the
 *   global O tile directly by `get<0>(blk_coord)` instead of `2*blk_coord`.
 *
 * SmemLayoutO keeps the pristine 2-sub-tile shape (the mainloop's
 * correction_epilogue still writes sub-tile 0 via `sO_01(_,_,_0{})`); only
 * sub-tile 0 is stored. The unused sub-tile costs smem only in the union with
 * the (larger) mainloop storage, so it is free in practice.
 ***************************************************************************************************/
#pragma once

#include "cutlass/cutlass.h"
#include "cute/layout.hpp"
#include "cutlass/epilogue/collective/collective_builder.hpp"
#include "cutlass/gemm/collective/collective_builder.hpp"

namespace cutlass::fmha::collective {

template<
  class Element,
  class ElementAcc,
  class TileShape,  // Q, D, _
  class StrideO,    // Q, D, B
  class StrideLSE_,   // Q, B
  class OrderLoadEpilogue = cute::false_type
>
struct Sm100FmhaFwdEpilogueTmaWarpspecialized {

  using Pipeline = cutlass::PipelineAsync<2>;

  using SmemLayoutAtomO = decltype(cutlass::gemm::collective::detail::sm100_smem_selector<
        cute::UMMA::Major::K, Element, tuple_element_t<0, TileShape>, tuple_element_t<1, TileShape>>());
  using SmemLayoutO = decltype(tile_to_shape(SmemLayoutAtomO{}, replace<2>(TileShape{}, _2{}), Step<_2, _1, _3>{}));
  using SmemLayoutO_ = SmemLayoutO;
  using StrideLSE = StrideLSE_;
  using ElementOut = Element;

  static const int NumWarpsEpilogue = 1;
  static const int NumWarpsLoad = 1;

  struct TensorStorage {

    using SmemLayoutO = SmemLayoutO_;
    cute::array_aligned<Element, cute::cosize_v<SmemLayoutO>> smem_o;

  };

  struct Arguments {
    Element* ptr_O;
    StrideO dO;

    ElementAcc* ptr_LSE;
    StrideLSE dLSE;
  };

  using TMA_O = decltype(make_tma_copy(
    SM90_TMA_STORE{},
    make_tensor((Element*) nullptr, repeat_like(StrideO{}, 0), StrideO{}),
    SmemLayoutO{}(_,_,_0{})
  ));


  struct Params {
    TMA_O tma_store_o;

    ElementAcc* ptr_LSE;
    StrideLSE dLSE;
  };

  // FMHA and MLA have different input ProblemShapes;
  // get problem_shape_O according to the input ProblemShape.
  template<class ProblemShape>
  CUTLASS_DEVICE static constexpr
  auto get_problem_shape_O (
    ProblemShape const& problem_shape) {
    if constexpr (rank_v<decltype(get<2>(ProblemShape{}))> == 2) {
      return replace<1>(select<0,2,3>(problem_shape), get<2, 0>(problem_shape));
    } else {
      return select<0,2,3>(problem_shape);
    }
  }

  template<class ProblemShape>
  static Params to_underlying_arguments(
      ProblemShape const& problem_shape,
      Arguments const& args,
      void* workspace = nullptr) {

    auto ptr_O = args.ptr_O;
    StrideO dO = args.dO;

    auto problem_shape_O = get_problem_shape_O(problem_shape);

    if constexpr (is_variable_length_v<tuple_element_t<0, ProblemShape>>) {
      auto cumulative_length_q = get<0>(problem_shape).cumulative_length;
      if (cumulative_length_q != nullptr) {
          int max_length_q = get<0>(problem_shape).max_length;
          // for variable sequence lenght, the batch is in units of row_stride
          get<2,1>(dO) = get<0>(dO);
          get<2,1>(problem_shape_O) = max_length_q * (1 + get<2,1>(problem_shape_O));
          // offset ptr by the amount we add back in later
          ptr_O -= max_length_q * get<0>(dO);
      }
    }

    auto tma_store_o = make_tma_copy(
      SM90_TMA_STORE{},
      make_tensor(ptr_O, problem_shape_O, dO),
      SmemLayoutO{}(_,_,_0{})
    );

    return {
      tma_store_o,
      args.ptr_LSE,
      args.dLSE
    };
  }

  CUTLASS_DEVICE
  static void prefetch_tma_descriptors(Params const& params) {
    cute::prefetch_tma_descriptor(params.tma_store_o.get_tma_descriptor());
  }

  const Params& params;

  CUTLASS_DEVICE Sm100FmhaFwdEpilogueTmaWarpspecialized(const Params& params) : params(params) {}

  template<class BlkCoord, class ProblemShape, class ParamsProblemShape>
  CUTLASS_DEVICE auto
  store(
      BlkCoord const& blk_coord_in, ProblemShape const& problem_shape,
      Params const& params, ParamsProblemShape const& params_problem_shape,
      TensorStorage& shared_storage,
      Pipeline& pipeline, typename Pipeline::PipelineState& pipeline_consumer_state) {

    BlkCoord blk_coord = blk_coord_in;
    uint32_t lane_predicate = cute::elect_one_sync();

    using X = Underscore;

    // [MXFP8 N128] single-stage: the CTA owns exactly one M=128 O tile.
    int o0_index = get<0>(blk_coord);

    Tensor mO_qdl_p = params.tma_store_o.get_tma_tensor(get_problem_shape_O(problem_shape));
    int offs_0 = 0;
    int offs_2_1 = 0;

    if constexpr (is_variable_length_v<tuple_element_t<0, ParamsProblemShape>>) {
      auto cumulative_length_q = get<0>(params_problem_shape).cumulative_length;
      if (cumulative_length_q != nullptr) {
        int max_length_q = get<0>(params_problem_shape).max_length;
        offs_0 = max_length_q - get<0>(problem_shape);
        offs_2_1 = cumulative_length_q[get<2,1>(blk_coord)] + get<0>(problem_shape);
        get<2,1>(blk_coord) = 0;
      }
    }

    Tensor mO_qdl = domain_offset(make_coord(offs_0, _0{}, make_coord(_0{}, offs_2_1)), mO_qdl_p);

    Tensor gO_qdl = local_tile(mO_qdl, TileShape{}, make_coord(_, _, _), Step<_1, _1, X>{});
    Tensor gO = gO_qdl(_, _, _, _0{}, get<2>(blk_coord));
    Tensor sO = make_tensor(make_smem_ptr(shared_storage.smem_o.data()), SmemLayoutO{});
    auto block_tma = params.tma_store_o.get_slice(0);
    Tensor tOsO = block_tma.partition_S(sO);
    Tensor tOgO = block_tma.partition_D(gO);

    auto pipeline_release_state = pipeline_consumer_state;

    // [MXFP8 N128] one pipeline signal from correction, one O sub-tile stored.
    pipeline.consumer_wait(pipeline_consumer_state);
    ++pipeline_consumer_state;

    if (lane_predicate) {
      copy(params.tma_store_o, tOsO(_,_,_,_0{}), tOgO(_,_,_,o0_index));
    }
    tma_store_arrive();
    tma_store_wait<0>();

    if constexpr (cute::is_same_v<OrderLoadEpilogue, cute::true_type>) {
      cutlass::arch::NamedBarrier::arrive((NumWarpsLoad + NumWarpsEpilogue) * NumThreadsPerWarp,
                                          cutlass::arch::ReservedNamedBarriers::EpilogueBarrier);
    }

    pipeline.consumer_release(pipeline_release_state);
    ++pipeline_release_state;

  }

};

}  // namespace cutlass::fmha::collective
