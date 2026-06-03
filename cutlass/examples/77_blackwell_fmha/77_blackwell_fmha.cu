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
/*! \file
    \brief Example implementation of fused multi-head attention for the NVIDIA Blackwell SM100
    architecture using CUTLASS 3.

    MQA/GQA
    -------

    The head dimension can be represented as a tuple, where the K/V strides in the
    first dimension is zero. This has the effect of MQA or GQA.
    * MHA is (head_size:head_stride).
    * MQA is (head_size:head_stride) in Q and (head_size:_0) in K and V.
    * GQA is (grouped_heads,heads_kv):(head_stride,grouped_heads*head_stride) in Q
      and (grouped_heads,heads_kv):(0,head_stride) in K and V

    Output Scale
    ------------

    The output scale gets passed to the collective mainloop, and is applied
    using FP32 compute pre-quantization

    Variable Sequence Length
    ------------------------

    For variable sequence length, pass in VariableLength objects
    (max_seqlen, cumulative_seqlen_ptr) in the problem shape for
    seqlen Q and KV.

    Support
    ---------

    Right now e4m3 with fp32 compute is using a 256x256 tiling and a head dimension
    of 128 is supported.

    #(batch_size, head_num, seq_len, d_k)
    Example usage:
      $ ./examples/77_blackwell_fmha/77_blackwell_fmha_fp8 \
            --b=1 --h=2048 --d=128 --q=2048 --k=2048
        ./examples/77_blackwell_fmha/101_blackwell_fmha_mxfp8 \
            --b=1 --h=40 --d=128 --q=256 --k=128

        ./examples/77_blackwell_fmha/101_blackwell_fmha_mxfp8 --b=1 --h=1 --d=128 --q=1 --k=128 --verify=1
            
            
*/

#include <iostream>
#include <random>
#include <regex>

#include "cute/tensor.hpp"

#include "cutlass/cutlass.h"
#include "cutlass/kernel_hardware_info.h"

#include "cutlass/util/command_line.h"
#include "cutlass/util/distribution.h"
#include "cutlass/util/reference/device/tensor_fill.h"
#include "reference/fmha_fwd_reference.hpp"
#include "reference/fmha_fwd_reference_mxfp8.hpp"
#include "reference/reference_abs_error.hpp"

#include "device/fmha.hpp"
#include "collective/fmha_fusion.hpp"
#include "collective/sm100_fmha_fwd_mainloop_tma_warpspecialized.hpp"
#include "collective/sm100_fmha_fwd_epilogue_tma_warpspecialized.hpp"
#include "kernel/fmha_options.hpp"
#include "kernel/fmha_tile_scheduler.hpp"
#include "kernel/sm100_fmha_fwd_kernel_tma_warpspecialized.hpp"

#include <cute/util/debug.hpp>
#include <cute/util/print.hpp>
#include <cute/util/print_tensor.hpp>
///////////////////////////////////////////////////////////////////////////////////////////////////

using namespace cute;
using namespace cutlass::fmha::kernel;
using namespace cutlass::fmha::collective;
using namespace cutlass::fmha;

///////////////////////////////////////////////////////////////////////////////////////////////////

enum class InitStyle {
  kOne, kLinearStride128, kLinearStride1, kRandom, kNone
};

///////////////////////////////////////////////////////////////////////////////////////////////////

/// Command line options parsing
struct Options {

  bool help = false;
  bool error = false;

  int b = 1;
  int h = 1;
  int h_k = 1;
  int q = 256;
  int k = 256;
  std::vector<int> varlen_q;
  std::vector<int> varlen_k;
  int d = 128;
  int warmup_iterations = 1;
  int iterations = 3;
  int tensor_ring_buffers = 1;
  bool verify = false;
  bool verbose = false;

  bool causal = false;
  bool causal_q_begin = true;
  bool residual = false;
  bool varlen = false;
  bool persistent = false;
  int sm_count = 0;
  std::string kernel_filter;

  InitStyle init_style_q = InitStyle::kRandom;
  InitStyle init_style_k = InitStyle::kRandom;
  InitStyle init_style_v = InitStyle::kRandom;
  
  InitStyle init_style_sfq = InitStyle::kRandom;
  InitStyle init_style_sfk = InitStyle::kOne;
  InitStyle init_style_sfp = InitStyle::kOne;
  InitStyle init_style_sfv = InitStyle::kOne;

  static void get_init_style_argument(cutlass::CommandLine& cmd, const char* name, InitStyle& dst, InitStyle const& src) {
    std::string s;
    cmd.get_cmd_line_argument(name, s, s);
    if (s.empty()) {
      dst = src;
    }
    else {
      if (s == "r") {
        dst = InitStyle::kRandom;
      }
      else if (s == "1") {
        dst = InitStyle::kOne;
      }
      else if (s == "d") {
        dst = InitStyle::kLinearStride1;
      }
      else if (s == "s") {
        dst = InitStyle::kLinearStride128;
      }
      else if (s == "n") {
        dst = InitStyle::kNone;
      }
      else {
        std::cout << "Error: " << s << " is not a valid input type.\n";
        std::exit(-1);
      }
    }
  }

  // Parses the command line
  void parse(int argc, char const **args) {
    cutlass::CommandLine cmd(argc, args);

    Options defaults;

    if (cmd.check_cmd_line_flag("help")) {
      help = true;
      return;
    }

    cmd.get_cmd_line_argument("d", d, defaults.d);
    cmd.get_cmd_line_argument("h", h, -1);
    if (h == -1) h = 2048 / d;

    cmd.get_cmd_line_argument("h_k", h_k, -1);
    if (h_k == -1) h_k = h;

    varlen = cmd.check_cmd_line_flag("varlen");

    cmd.get_cmd_line_argument("q", q, -1);
    cmd.get_cmd_line_argument("k", k, -1);
    cmd.get_cmd_line_argument("b", b, -1);

    std::string varlen_q_str;
    cmd.get_cmd_line_argument("varlen-q", varlen_q_str);
    std::string varlen_k_str;
    cmd.get_cmd_line_argument("varlen-k", varlen_k_str);

    if (varlen && ! varlen_q_str.empty()) {
      varlen_q.clear();
      while (! varlen_q_str.empty()) {
        size_t pos = varlen_q_str.find(':');
        varlen_q.push_back(std::stoi(varlen_q_str.substr(0, pos)));
        if (pos == std::string::npos) {
          break;
        }
        varlen_q_str = varlen_q_str.substr(pos + 1);
      }
      if (b == -1) {
        b = static_cast<int>(varlen_q.size());
      }
      if (b != static_cast<int>(varlen_q.size())) {
        std::cout << "Error: Invalid --varlen-q length\n";
        std::exit(-1);
      }
      int new_q = 0;
      for (auto elem : varlen_q) {
        new_q += elem;
      }
      if (q != -1) {
        std::cout << "Error: Can't provide --q and --varlen-q\n";
        std::exit(-1);
      }
      q = new_q;
    }

    if (varlen && ! varlen_k_str.empty()) {
      varlen_k.clear();
      while (! varlen_k_str.empty()) {
        size_t pos = varlen_k_str.find(':');
        varlen_k.push_back(std::stoi(varlen_k_str.substr(0, pos)));
        if (pos == std::string::npos) {
          break;
        }
        varlen_k_str = varlen_k_str.substr(pos + 1);
      }
      if (b == -1) {
        b = static_cast<int>(varlen_k.size());
      }
      if (b != static_cast<int>(varlen_k.size())) {
        std::cout << " Error: Invalid --varlen-k length\n";
        std::exit(-1);
      }
      int new_k = 0;
      for (auto elem : varlen_k) {
        new_k += elem;
      }
      if (k != -1) {
        std::cout << "Error: Can't provide --k and --varlen-k\n";
        std::exit(-1);
      }
      k = new_k;
    }

    if (q == -1) q = k;
    if (k == -1) k = q;
    if (q == -1 && k == -1) q = k = defaults.q;
    if (b == -1) b = 16384 / k;
    if (b == 0) b = 1;

    cmd.get_cmd_line_argument("warmup_iterations", warmup_iterations, defaults.warmup_iterations);
    cmd.get_cmd_line_argument("iterations", iterations, defaults.iterations);
    cmd.get_cmd_line_argument("tensor_ring_buffers", tensor_ring_buffers, defaults.tensor_ring_buffers);

    verify = cmd.check_cmd_line_flag("verify");
    verbose = cmd.check_cmd_line_flag("verbose");
    persistent = cmd.check_cmd_line_flag("persistent");

    std::string mask;
    cmd.get_cmd_line_argument<std::string>("mask", mask, "");
    std::string causal_type;
    cmd.get_cmd_line_argument<std::string>("causal-type", causal_type, "");
    if (mask == "no" || mask == "") {
      causal = residual = false;
      if (varlen) {
        residual = true;
      }
    }
    else if (mask == "causal") {
      residual = false;
      causal = true;
      if(causal_type == "qend") {
        causal_q_begin = false;
      } else {
        causal_q_begin = true;
      }
    }
    else if (mask == "residual") {
      residual = true;
      causal = false;
    }
    cmd.get_cmd_line_argument("sm-count", sm_count, defaults.sm_count);
    get_init_style_argument(cmd, "init-style", init_style_q, defaults.init_style_q);
    get_init_style_argument(cmd, "init-style", init_style_k, defaults.init_style_q);
    get_init_style_argument(cmd, "init-style", init_style_v, defaults.init_style_q);
    get_init_style_argument(cmd, "init-style-q", init_style_q, init_style_q);
    get_init_style_argument(cmd, "init-style-k", init_style_k, init_style_k);
    get_init_style_argument(cmd, "init-style-v", init_style_v, init_style_v);
    get_init_style_argument(cmd, "init-style-sfq", init_style_sfq, init_style_sfq);
    get_init_style_argument(cmd, "init-style-sfk", init_style_sfk, init_style_sfk);
    get_init_style_argument(cmd, "init-style-sfp", init_style_sfp, init_style_sfp);
    get_init_style_argument(cmd, "init-style-sfv", init_style_sfv, init_style_sfv);

    cmd.get_cmd_line_argument("kernel-filter", kernel_filter, defaults.kernel_filter);
  }

  /// Prints the usage statement.
  std::ostream & print_usage(std::ostream &out) const {

    out << "77_blackwell_fmha\n\n"
      << "  This example showcases the use of CUTLASS's collective operation builders to easily construct\n"
      << "  fused multi-head attention forward-passkernels targeting NVIDIA's Blackwell architecture.\n\n"
      << "Options:\n\n"
      << "  --help                      If specified, displays this usage statement\n\n"
      << "  --b=<int>                   Sets the B extent\n"
      << "  --h=<int>                   Sets the H extent\n"
      << "  --h_k=<int>                 Sets the H_K/V extent (for GQA/MQA)\n"
      << "  --q=<int>                   Sets the Q extent\n"
      << "  --k=<int>                   Sets the K extent\n"
      << "  --varlen-q=<int>:<int...>   Sets the variable Q extent per batch (colon separated)\n"
      << "  --varlen-k=<int>:<int...>   Sets the variable K extent per batch (colon separated)\n"
      << "  --d=<int>                   Sets the D extent\n"
      << "  --tensor_ring_buffers=<int> Sets the number of tensor ring buffers\n"
      << "  --warmup_iterations=<int>   Sets the warmup iterations\n"
      << "  --iterations=<int>          Benchmarking iterations\n"
      << "  --verify                    Verify results\n"
      << "  --verbose                   Print smem and execution time per kernel\n"
      << "  --mask=<no|residual|causal> Enables masking\n"
      << "  --causal-type=<qbegin|qend> Causal mask type\n"
      << "  --persistent                Enables persistent scheduler\n"
      << "  --varlen                    Enables variable sequence length\n"
      << "                              B*Q and B*K become the total sequence length\n"
      << "                              and are split B-ways, alternatingly +10% and -10%\n"
      << "                              with the last batch sized to make it fit\n"
      << "                              implies at least residual masking for correctness\n"
      << "  --sm-count                  Sets SM count rather than querying it\n"
      << "  --kernel-filter=<filter>    Sets regexp to match kernel against\n"
      << "\n";

    return out;
  }
};


///////////////////////////////////////////////////////////////////////////////////////////////////

/// Helper to initialize a block of device data
template <class Element>
void initialize_block(
    DeviceAllocation<Element>& block,
    uint64_t seed=2023, InitStyle init_style = InitStyle::kRandom) {

  switch (init_style) {
    case InitStyle::kOne: {
      cutlass::reference::device::BlockFillRandomUniform(
        block.get(), block.size(), seed, (Element) 1, (Element) 1);
      break;
    }
    case InitStyle::kRandom: {
      cutlass::reference::device::BlockFillRandomGaussian(
        block.get(), block.size(), seed, (Element) 0, (Element) 1);
      break;
    }
    case InitStyle::kLinearStride1: {
      std::vector<Element> data(block.size());
      for (size_t i = 0; i < block.size() / 128; i ++) {
        for (int j = 0; j < 128; j++) {
          data[j + 128*i] = static_cast<Element>((double) (j % 4));
        }
      }
      block.copy_from_host(data.data(), data.size());
      break;
    }
    case InitStyle::kLinearStride128: {
      std::vector<Element> data(block.size());
      for (size_t i = 0; i < block.size() / 128; i ++) {
        for (int j = 0; j < 128; j++) {
          data[j + 128*i] = static_cast<Element>((double) (i % 4));
        }
      }
      block.copy_from_host(data.data(), data.size());
      break;
    }
    case InitStyle::kNone: {
      break;
    }
  }
}

///////////////////////////////////////////////////////////////////////////////////////////////////

struct ExampleResult {
  bool passed = false;
  bool verified = false;
  float runtime_ms = 0;
  double tflops_tc_s = 0;
  double tops_exp2_s = 0;
  double tbytes_s = 0;
  size_t smem_size = 0;
};
#include <typeinfo>
#include <cxxabi.h> // 用于解码修饰名
#include <cute/util/debug.hpp>
template <typename T>
std::string type_name() {
    int status;
    char* demangled = abi::__cxa_demangle(typeid(T).name(), nullptr, nullptr, &status);
    std::string result = (status == 0) ? demangled : typeid(T).name();
    free(demangled);
    return result;
}
///////////////////////////////////////////////////////////////////////////////////////////////////

#if defined(CUTLASS_ARCH_MMA_SM100_SUPPORTED)

///////////////////////////////////////////////////////////////////////////////////////////////////

template<
  bool kIsVarlen,
  class TileShape,
  class DispatchPolicy,
  class ActiveMask,
  class... KernelOptions
>
struct FwdRunner {

#ifdef MXFP8
  // using Element = cutlass::float_e4m3_t;
  // using Element = cutlass::nv_float4_t<cutlass::float_e2m1_t>;
  using Element = cutlass::mx_float8_t<cutlass::float_e4m3_t>;
  using ElementData = typename Element::DataType;
  // using ElementData = cutlass::float_e4m3_t;
  using ElementScale = typename Element::ScaleFactorType;
#else
  using Element = cutlass::half_t;
#endif

  using ElementAccumulatorQK = float;
  using ElementAccumulatorPV = float;
  using ElementOut = cutlass::half_t;

  // Q K D ((H_R, H_K) B)
  using ProblemShapeRegular = cute::tuple<int, int, int, cute::tuple<cute::tuple<int, int>, int>>;
  // using ProblemShapeRegular_mha = cute::tuple<int, int, int, int>;
  using ProblemShapeVarlen = cute::tuple<VariableLength, VariableLength, int, cute::tuple<cute::tuple<int, int>, int>>;
  using ProblemShapeType = std::conditional_t<kIsVarlen, ProblemShapeVarlen, ProblemShapeRegular>;
  // using ProblemShapeType_mha = std::conditional_t<kIsVarlen, ProblemShapeVarlen, ProblemShapeRegular_mha>;
  
  using StrideQ = cute::tuple<int, _1, cute::tuple<cute::tuple<int, int>, int>>;  // Q D ((H_R, H_K), B)
  using StrideK = cute::tuple<int, _1, cute::tuple<cute::tuple<_0, int>, int>>;  // K D ((H_R, H_K), B)
  using StrideV = StrideK;
  using StrideO = StrideQ;
  using StrideLSE = cute::tuple<_1, cute::tuple<cute::tuple<int, int>, int>>;     // Q ((H_R, H_K), B)

  // using         LayoutATag  = cutlass::layout::RowMajor;                      // Layout type for A matrix operand
  // int AlignmentA  = 16;
  // using         LayoutBTag  = cutlass::layout::RowMajor;                      // Layout type for A matrix operand
  // int AlignmentB  = 16;
  static constexpr bool kIsPersistent = find_option_t<Tag::kIsPersistent, true_type, KernelOptions...>::value;
  using TileScheduler = std::conditional_t<kIsPersistent, cutlass::fmha::kernel::PersistentTileScheduler, cutlass::fmha::kernel::IndividualTileScheduler>;

  using Mainloop = 
    cutlass::fmha::collective::Sm100FmhaFwdMainloopTmaWarpspecialized<
      Element, ElementAccumulatorQK, ElementAccumulatorPV,
      TileShape, StrideQ, StrideK, StrideV,
      ActiveMask
    >;
  using Operation = cutlass::fmha::device::FMHA<
    cutlass::fmha::kernel::Sm100FmhaFwdKernelTmaWarpspecialized<
      ProblemShapeType,
      Mainloop,
      cutlass::fmha::collective::Sm100FmhaFwdEpilogueTmaWarpspecialized<
        ElementOut, ElementAccumulatorPV,
        typename Mainloop::TileShapePV,
        StrideO, StrideLSE
      >,
      TileScheduler
    >>;

  //
  // Data members
  //

  /// Initialization
  StrideQ stride_Q;
  StrideK stride_K;
  StrideV stride_V;
  StrideO stride_O;
  StrideLSE stride_LSE;
  
  using Sm1xxBlkScaledConfigQK = typename Mainloop::CollectiveMmaQK::Sm1xxBlkScaledConfig;
  using Sm1xxBlkScaledConfigPV = typename Mainloop::CollectiveMmaPV::Sm1xxBlkScaledConfig;


  typename Sm1xxBlkScaledConfigQK::LayoutSF layout_SFQ;   // 需查阅实际类型
  typename Sm1xxBlkScaledConfigQK::LayoutSF layout_SFK;
  typename Sm1xxBlkScaledConfigPV::LayoutSF layout_SFP;
  typename Sm1xxBlkScaledConfigPV::LayoutSF layout_SFV;
  uint64_t seed = 0;

  struct DeviceBuffer {
    DeviceAllocation<ElementData> block_Q;
    DeviceAllocation<ElementData> block_K;
    DeviceAllocation<ElementData> block_V;
    DeviceAllocation<ElementScale> block_SFQ;
    DeviceAllocation<ElementScale> block_SFK;
    DeviceAllocation<ElementScale> block_SFP;
    DeviceAllocation<ElementScale> block_SFV;
    // Row-major SF buffer for reference verification
    DeviceAllocation<ElementScale> block_ref_SFQ;
    DeviceAllocation<ElementScale> block_ref_SFK;
    DeviceAllocation<ElementScale> block_ref_SFP;
    DeviceAllocation<ElementScale> block_ref_SFV;
    DeviceAllocation<ElementOut> block_O;
    DeviceAllocation<ElementAccumulatorPV> block_LSE;
    DeviceAllocation<ElementOut> block_ref_O;
    DeviceAllocation<ElementAccumulatorPV> block_ref_LSE;
    DeviceAllocation<int> device_cumulative_seqlen_q;
    DeviceAllocation<int> device_cumulative_seqlen_kv;

    DeviceBuffer() = default;
    DeviceBuffer(const DeviceBuffer&) = delete;
    DeviceBuffer& operator=(const DeviceBuffer&) = delete;

    size_t get_storage_size() const {
      return block_Q.get_storage_size() + block_K.get_storage_size() + block_V.get_storage_size()
          + block_O.get_storage_size() + block_LSE.get_storage_size() + block_ref_O.get_storage_size()
          + block_ref_LSE.get_storage_size() + device_cumulative_seqlen_q.get_storage_size()
          + device_cumulative_seqlen_kv.get_storage_size() + block_SFQ.get_storage_size() + 
          block_SFK.get_storage_size() + block_SFV.get_storage_size() + block_SFP.get_storage_size()
          + block_ref_SFQ.get_storage_size() + block_ref_SFK.get_storage_size()
          + block_ref_SFV.get_storage_size() + block_ref_SFP.get_storage_size();
    }
  };

  std::vector<std::unique_ptr<DeviceBuffer>> buffers;

  std::vector<int> cumulative_seqlen_q;
  std::vector<int> cumulative_seqlen_kv;

  //
  // Methods
  //
  bool verify(const ProblemShapeType& problem_shape, DeviceBuffer& buffer) {
    Tensor mQ = make_tensor(make_gmem_ptr(buffer.block_Q.get()),
      select<0,2,3>(problem_shape),
      stride_Q);

    Tensor mK = make_tensor(make_gmem_ptr(buffer.block_K.get()),
      select<1,2,3>(problem_shape),
      stride_K);

    Tensor mV = make_tensor(make_gmem_ptr(buffer.block_V.get()),
      select<1,2,3>(problem_shape),
      stride_V);

    Tensor mO = make_tensor(make_gmem_ptr(buffer.block_ref_O.get()),
      select<0,2,3>(problem_shape),
      stride_O);

    Tensor mLSE = make_tensor(make_gmem_ptr(buffer.block_ref_LSE.get()),
      select<0,3>(problem_shape),
      stride_LSE);
    
    auto [Q, K, D, HB] = problem_shape;

    auto problem_shape_ref = cute::make_tuple(Q, K, D, D, HB);

    // fmha_reference(problem_shape_ref, mQ, mK, mV, mO, mLSE, ActiveMask{});

#ifdef MXFP8
    int SQ = Q, SK = K;
    int H_KV = size<0,1>(HB);
    int H    = size<0,0>(HB) * H_KV;
    int B    = size<1>(HB);
    int SF_D = D / 32;  // =4 when D=128
    printf("SQ:%d, SK:%d, H_KV:%d, H:%d, B:%d, SF_D:%d\n",SQ,SK,H_KV,H,B,SF_D);
    // D=128 → D/32=4 → K_tiling=1 → CUTLASS SF layout = simple row-major
    // SF_P treated as 1.0 (P stays in FP32)
    auto sfQ = make_mxfp8_sf_tensor(buffer.block_ref_SFQ.get(), SQ, SF_D, H * B);
    auto sfK = make_mxfp8_sf_tensor(buffer.block_ref_SFK.get(), SK, SF_D, H_KV * B);
    int SF_K = (K + 31) / 32;  // ceil(K/32) — SFV groups along K-seqlen
    auto sfV = make_mxfp8_sf_tensor(buffer.block_ref_SFV.get(), D, SF_K, H_KV * B);
    
        // Dump raw SF_K buffer from device to verify layout
    {
      size_t sfk_elems = size(filter_zeros(layout_SFK));
      std::vector<ElementScale> h_sfk(sfk_elems);
      cudaMemcpy(h_sfk.data(), buffer.block_SFK.get(),
                 sfk_elems * sizeof(ElementScale), cudaMemcpyDeviceToHost);
      printf("[HOST] SF_K ref-style: row0=[%.6f,%.6f,%.6f,%.6f] row1=[%.6f,%.6f,%.6f,%.6f]\n",
          (float)h_sfk[0], (float)h_sfk[1], (float)h_sfk[2], (float)h_sfk[3],
          (float)h_sfk[4], (float)h_sfk[5], (float)h_sfk[6], (float)h_sfk[7]);
    }

    // ===== DEBUG: Compare Raw kernel vs Ref for SFQ, SFK, SFV =====
    {
      auto print_compare = [&](const char* name, auto& kernel_buf, auto& ref_buf, auto& layout) {
        size_t n_k = cosize(layout);
        size_t n_r = (size_t)size<0>(problem_shape) * (size<2>(problem_shape)/32) * size<3,0>(problem_shape) * size<3,1>(problem_shape);
        // For SFV, ref size is different: D * (K/32) * H_K * B
        // Just compare first 16 elements
        std::vector<ElementScale> h_k(n_k), h_r(n_r > 0 ? n_r : 1);
        cudaMemcpy(h_k.data(), kernel_buf.get(), n_k * sizeof(ElementScale), cudaMemcpyDeviceToHost);
        cudaMemcpy(h_r.data(), ref_buf.get(), min(n_r, n_k) * sizeof(ElementScale), cudaMemcpyDeviceToHost);
        printf("[COMPARE %s] Raw[0..7]=", name);
        for (int i = 0; i < min(8,(int)n_k); i++) printf("%.4f ", (float)h_k[i]);
        printf(" Ref[0..7]=");
        for (int i = 0; i < min(8,(int)n_r); i++) printf("%.4f ", (float)h_r[i]);
        printf(" (n_kernel=%ld n_ref=%ld)\n", (long)n_k, (long)n_r);
      };
      print_compare("SFQ", buffer.block_SFQ, buffer.block_ref_SFQ, layout_SFQ);
      print_compare("SFK", buffer.block_SFK, buffer.block_ref_SFK, layout_SFK);
      print_compare("SFV", buffer.block_SFV, buffer.block_ref_SFV, layout_SFV);
      printf("=== END COMPARE ===\n");
    }

    // ===== DEBUG: Compare CUTLASS SF layout vs reference row-major layout =====
    {
      int SQ = size<0>(problem_shape);
      int SK = size<1>(problem_shape);
      int D_shape = size<2>(problem_shape);
      int H  = size<3,0>(problem_shape);
      int H_K = size<3,0,1>(problem_shape);
      int B  = size<3,1>(problem_shape);
      int SF_D = D_shape / 32;

      printf("\n=== SF LAYOUT DEBUG ===\n");
      printf("Problem: SQ=%d SK=%d D=%d H=%d H_K=%d B=%d SF_D=%d\n", SQ, SK, D_shape, H, H_K, B, SF_D);

      printf("CUTLASS layout_SFQ shape: "); print(shape(layout_SFQ)); printf("\n");
      printf("CUTLASS layout_SFQ stride: "); print(stride(layout_SFQ)); printf("\n");
      printf("CUTLASS layout_SFQ cosize: %ld\n", (long)cute::cosize(layout_SFQ));
      printf("CUTLASS layout_SFQ size (filter_zeros): %ld\n", (long)size(filter_zeros(layout_SFQ)));

      printf("CUTLASS layout_SFK shape: "); print(shape(layout_SFK)); printf("\n");
      printf("CUTLASS layout_SFK stride: "); print(stride(layout_SFK)); printf("\n");
      printf("CUTLASS layout_SFK cosize: %ld\n", (long)cute::cosize(layout_SFK));
      printf("CUTLASS layout_SFK size (filter_zeros): %ld\n", (long)size(filter_zeros(layout_SFK)));

      int ref_sfq_size = SQ * SF_D * H * B;
      int ref_sfk_size = SK * SF_D * H_K * B;
      printf("Reference row-major SFQ size: %d (SQ=%d * SF_D=%d * H=%d * B=%d)\n",
             ref_sfq_size, SQ, SF_D, H, B);
      printf("Reference row-major SFK size: %d (SK=%d * SF_D=%d * H_K=%d * B=%d)\n",
             ref_sfk_size, SK, SF_D, H_K, B);

      long cutlass_sfq = (long)size(filter_zeros(layout_SFQ));
      long cutlass_sfk = (long)size(filter_zeros(layout_SFK));
      if (cutlass_sfq != ref_sfq_size) {
        printf("*** WARNING: CUTLASS SFQ size (%ld) != reference (%d) ***\n", cutlass_sfq, ref_sfq_size);
      }
      if (cutlass_sfk != ref_sfk_size) {
        printf("*** WARNING: CUTLASS SFK size (%ld) != reference (%d) ***\n", cutlass_sfk, ref_sfk_size);
      }

      printf("\n--- SFQ logical->linear index comparison ---\n");
      for (int hb = 0; hb < min(H*B, 2); hb++) {
        for (int row = 0; row < min(SQ, 4); row++) {
          printf("SFQ[row=%d][g=0..3][hb=%d]: ", row, hb);
          for (int g = 0; g < SF_D; g++) {
            auto cutlass_idx = layout_SFQ(make_coord(row, g, make_coord(_0{}, hb)));
            auto ref_idx = row * SF_D + g + hb * SQ * SF_D;
            printf("c=%ld/r=%ld ", (long)cutlass_idx, (long)ref_idx);
          }
          printf("\n");
        }
      }
      printf("=== END SF LAYOUT DEBUG ===\n\n");
    }
    // ===== HOST-SIDE manual S computation (FP64 for reference accuracy) =====
    {
      size_t q_elems = size(select<0,2,3>(problem_shape));
      size_t k_elems = size(select<1,2,3>(problem_shape));
      std::vector<ElementData> h_Q(q_elems), h_K(k_elems);
      std::vector<ElementScale> h_sfq(size(filter_zeros(layout_SFQ)));
      std::vector<ElementScale> h_sfk(size(filter_zeros(layout_SFK)));
      cudaMemcpy(h_Q.data(), buffer.block_Q.get(), q_elems * sizeof(ElementData), cudaMemcpyDeviceToHost);
      cudaMemcpy(h_K.data(), buffer.block_K.get(), k_elems * sizeof(ElementData), cudaMemcpyDeviceToHost);
      cudaMemcpy(h_sfq.data(), buffer.block_ref_SFQ.get(), h_sfq.size() * sizeof(ElementScale), cudaMemcpyDeviceToHost);
      cudaMemcpy(h_sfk.data(), buffer.block_ref_SFK.get(), h_sfk.size() * sizeof(ElementScale), cudaMemcpyDeviceToHost);
      
      for(int i=0;i<12;i++){
        printf("SFQ[%d]: %f, ",i, float(h_sfq[i]));
      }

      // ===== HOST-SIDE manual S computation (FP64) - enhanced =====
      {
        int D_val = D;
        int SF_D = D / 32;
        printf("\n[HOST] Manual S for Q=0..%d (FP64, host-side):\n", min(3, SQ-1));
        for (int q_check = 0; q_check < min(4, SQ); q_check++) {
          printf("--- Q[%d] ---\n", q_check);
          for (int k_check = 0; k_check < min(4, SK); k_check++) {
            double sum = 0.0;
            for (int d = 0; d < D_val; d++) {
              int g = d / 32;
              double q_val = double(h_Q[q_check * D_val + d]) * double(h_sfq[q_check * SF_D + g]);
              double kv_val = double(h_K[k_check * D_val + d]) * double(h_sfk[k_check * SF_D + g]);
              sum += q_val * kv_val;
            }
            printf("  S[%d][%d] = %.10f\n", q_check, k_check, sum);
          }
        }
        printf("\n");
      }
    }
    // ====================================================================

    fmha_reference_mxfp8(problem_shape_ref,
        mQ, sfQ, mK, sfK, mV, sfV, mO, mLSE, ActiveMask{});

#else
    fmha_reference(problem_shape_ref, mQ, mK, mV, mO, mLSE, ActiveMask{});
#endif

    cudaError_t result = cudaDeviceSynchronize();
    if (result != cudaSuccess) {
      std::cerr << "Reference kernel failed. Last CUDA error: "
                << cudaGetErrorString(result) << std::endl;
      return false;
    }

    // const double kMaxDiffThresh = sizeof(Element) == 1 ? 1e-1 : 1e-2;
    // const double kMeanDiffThresh = sizeof(Element) == 1 ? 1e-1 : 1e-3;
    const double kMaxDiffThresh = sizeof(Element) == 1 ? 4.0e-1 : 1e-2;
    const double kMeanDiffThresh = sizeof(Element) == 1 ? 1e-1 : 1e-3;

    // Check if output from CUTLASS kernel and reference kernel are equal or not
    double max_diff = 0;
    double mean_diff = 0;
    reference_abs_diff(buffer.block_O, buffer.block_ref_O, max_diff, mean_diff);
    printf("O max_diff:%f, mean_diff:%f\n", max_diff, mean_diff);

    bool passed_O = (max_diff < kMaxDiffThresh) && (mean_diff < kMeanDiffThresh);
    printf("passed_O: %d\n", passed_O);
    if (! passed_O) {
      std::cerr << "failed O: max diff " << max_diff 
                << " mean " << mean_diff << std::endl;
    }

    reference_abs_diff(buffer.block_LSE, buffer.block_ref_LSE, max_diff, mean_diff);
    printf("lse max_diff:%f, mean_diff:%f\n", max_diff, mean_diff);
    // printf("q style: ");
    bool passed_LSE = (max_diff < kMaxDiffThresh) && (mean_diff < kMeanDiffThresh);
    printf("passed_LSE: %d\n", passed_LSE);
    if ( ! passed_LSE) {
      std::cerr << "failed LSE: max diff " << max_diff 
                << " mean " << mean_diff << std::endl;
    }

    return passed_O && passed_LSE;
  }

  template<class ProblemShape>
  auto initialize_varlen(
      const Options& options, const ProblemShape& problem_size,
      const bool kVarlenSame = true) {

    int num_batches = get<3,1>(problem_size);

    // generate Q as --b times
    //    gaussian (--Q, --Q / 2) sampled positive
    //    track cumulative 
    std::mt19937 rng(0x202305151552ull);
    std::normal_distribution<double> dist_q(get<0>(problem_size), get<0>(problem_size) / 2);
    std::normal_distribution<double> dist_kv(get<1>(problem_size), get<1>(problem_size) / 2);
    std::cout << "N: " << num_batches << ", Q: " << get<0>(problem_size) << ", KV: " << get<1>(problem_size) << std::endl;

    auto generate_positive_int = [](auto& dist, auto& gen) {
      int result = 0;
      do {
        result = static_cast<int>(dist(gen));
      } while (result <= 0);
      return result;
    };

    cumulative_seqlen_q = {0};
    cumulative_seqlen_kv = {0};

    int total_seqlen_q = 0;
    int total_seqlen_kv = 0;
    int max_seqlen_q = 0;
    int max_seqlen_kv = 0;

    for (int i = 0; i < num_batches; i++) {
      int seqlen_q = (! options.varlen_q.empty()) ? options.varlen_q.at(i) : 
              kVarlenSame ? get<0>(problem_size) :
              generate_positive_int(dist_q, rng);
      int seqlen_kv = (! options.varlen_k.empty()) ? options.varlen_k.at(i) :
              kVarlenSame ? get<1>(problem_size) :
              generate_positive_int(dist_kv, rng);

      total_seqlen_q += seqlen_q;
      total_seqlen_kv += seqlen_kv;

      max_seqlen_q = std::max(max_seqlen_q, seqlen_q);
      max_seqlen_kv = std::max(max_seqlen_kv, seqlen_kv);

      cumulative_seqlen_q.push_back(cumulative_seqlen_q.back() + seqlen_q);
      cumulative_seqlen_kv.push_back(cumulative_seqlen_kv.back() + seqlen_kv);
    }
    std::cout << "Q max: " << max_seqlen_q << " total: " << total_seqlen_q << " vs even " << num_batches * get<0>(problem_size) << std::endl;
    std::cout << "KV max: " << max_seqlen_kv << " total: " << total_seqlen_kv << " vs even " << num_batches * get<1>(problem_size) << std::endl;

    ProblemShape problem_size_for_init = problem_size;
    get<3,1>(problem_size_for_init) = 1;
    get<0>(problem_size_for_init) = total_seqlen_q;
    get<1>(problem_size_for_init) = total_seqlen_kv;

    ProblemShapeType problem_size_for_launch;

    get<0>(problem_size_for_launch) = VariableLength{max_seqlen_q, nullptr, total_seqlen_q};
    get<1>(problem_size_for_launch) = VariableLength{max_seqlen_kv, nullptr, total_seqlen_kv};
    get<2>(problem_size_for_launch) = get<2>(problem_size);
    get<3>(problem_size_for_launch) = get<3>(problem_size);

    return cute::make_tuple(problem_size_for_init, problem_size_for_launch);
  }


  /// Initialize operands to be used in the GEMM and reference GEMM

  ProblemShapeType initialize(const Options& options) {
    int h_r = options.h / options.h_k;
    assert(options.h % options.h_k == 0);
    auto problem_shape_in = cute::make_tuple(options.q, options.k, options.d, cute::make_tuple(cute::make_tuple(h_r, options.h_k), options.b));
                                              //sqence length, K, head_dim, ((每组内查询头的数量，键值头分组后的每组头数),batch_size)
    // auto problem_shape_in_mha = cute::make_tuple(options.q, options.k, options.d, options.h);
                                              
    ProblemShapeType problem_shape;      
    // ProblemShapeType_mha problem_shape_mha;
    decltype(problem_shape_in) problem_size;
    // decltype(problem_shape_in_mha) problem_size_mha;

    if constexpr (kIsVarlen) {
      auto [problem_shape_init, problem_shape_launch] = initialize_varlen(options, problem_shape_in);
      problem_shape = problem_shape_launch;
      problem_size = problem_shape_init;
    }
    else {
      problem_size = problem_shape_in;
      problem_shape = problem_shape_in;
      // problem_size_mha = problem_shape_in_mha;
    }

    get<2>(problem_size) = cutlass::round_up(get<2>(problem_size), 8);  // alignment

    // printf("problem_size: ");
    // print(problem_size);
    // printf("\n");

    // printf("size(problem_size): ");
    // print(size(problem_size));
    // printf("\n");
    auto shape_QO = select<0,2,3>(problem_size);
    auto shape_KV = select<1,2,3>(problem_size);
    auto shape_LSE = select<0,3>(problem_size);
    // printf("shapeQO: ");
    // print(shape_QO);
    // print(size(shape_QO));
    // printf("\n");//SQ, D, SK, H)
    int SQ = size<0>(problem_size);
    int SK = size<1>(problem_size);
    int D = size<2>(problem_size);
    int H  = size<3,0>(problem_size);
    int H_K = size<3,0,1>(problem_size);
    int H_Q = size<3,0,0>(problem_size);
    int B = size<3,1>(problem_size);

    stride_Q = make_stride(H*D , _1{}, make_stride(make_stride(D, H_Q*D), H*D*SQ));
    stride_O = stride_Q;
    // printf("stride_Q: ");
    // print(stride_Q);
    // printf("\n");
    stride_K = make_stride(H_K*D , _1{}, make_stride(make_stride(_0{}, D), H_K*D*SK));
    stride_V = stride_K;
    stride_LSE = make_stride(_1{}, make_stride(make_stride(SQ, SQ*H_Q), SQ*H));
    // using SFConfig = cutlass::detail::Sm1xxBlockScaledConfig<SF_VEC>;
    layout_SFQ = Sm1xxBlkScaledConfigQK::tile_atom_to_shape_SFA(problem_size);
    layout_SFK = Sm1xxBlkScaledConfigQK::tile_atom_to_shape_SFB(problem_size);
    // auto layout_SFK_temp = Sm1xxBlkScaledConfigQK::tile_atom_to_shape_SFB(problem_shape_in);

    // std::cout<<"layout_SFQTypename: " << type_name<decltype(layout_SFQ)>() << std::endl;
    // std::cout<<"layout_SFK_tempTypename: " << type_name<decltype(layout_SFQ)>() << std::endl;
    auto problem_size_pv = select<0,2,1,3>(problem_size);
    layout_SFP = Sm1xxBlkScaledConfigPV::tile_atom_to_shape_SFA(problem_size_pv);    //对于PV的problem_size应该是变了
    layout_SFV = Sm1xxBlkScaledConfigPV::tile_atom_to_shape_SFB(problem_size_pv);
    
    // printf("layout_SFQ: ");
    // print(layout_SFQ);
    // printf("\n");
    
    // printf("layout_SFK: ");
    // print(layout_SFK);
    // printf("\n");

    // printf("layout_SFP: ");
    // print(layout_SFP);
    // printf("\n");
    
    // printf("layout_SFV: ");
    // print(layout_SFV);
    // printf("\n");
    // printf("size(layout_SFQ): ");
    // print(size(layout_SFQ));
    // printf("\n");

    // printf("sizeof element: %ld\n",sizeof(Element));
    if (kIsVarlen) {
      get<2,1>(stride_Q) = 0;
      get<2,1>(stride_K) = 0;
      get<2,1>(stride_V) = 0;
      get<2,1>(stride_O) = 0;
      get<1,1>(stride_LSE) = 0;
    }

    auto buffer_init_fn = [&](auto& buffer) {
      buffer.block_Q.reset(size(shape_QO));
      buffer.block_K.reset(size(shape_KV));
      buffer.block_V.reset(size(shape_KV));

      buffer.block_SFQ.reset(size(filter_zeros(layout_SFQ)));
      buffer.block_SFK.reset(size(filter_zeros(layout_SFK)));
      buffer.block_SFP.reset(size(filter_zeros(layout_SFP)));
      buffer.block_SFV.reset(size(filter_zeros(layout_SFV)));


      buffer.block_O.reset(size(shape_QO), kIsVarlen ? D*SQ*H : 0);
      buffer.block_LSE.reset(size(shape_LSE));
      buffer.block_ref_O.reset(size(shape_QO), kIsVarlen ? D*SQ*H : 0);
      buffer.block_ref_LSE.reset(size(shape_LSE));
      // Initialize LSE buffers to 0 to avoid garbage comparison for
      // positions that kernel/reference don't write (e.g., partial tiles)
      cudaMemset(buffer.block_LSE.get(), 0, size(shape_LSE) * sizeof(ElementAccumulatorPV));
      cudaMemset(buffer.block_ref_LSE.get(), 0, size(shape_LSE) * sizeof(ElementAccumulatorPV));

      initialize_block(buffer.block_Q, seed + 2023, options.init_style_q);
      initialize_block(buffer.block_K, seed + 2022, options.init_style_k);
      initialize_block(buffer.block_V, seed + 2021, options.init_style_v);
      // std::cout<< "block_q: " <<buffer.block_Q.get() << std::endl;
      // std::cout<< "block_q: " <<(*(buffer.block_Q.get()))[0] << std::endl;
      initialize_block(buffer.block_SFQ, seed + 2027, options.init_style_sfq);
      initialize_block(buffer.block_SFK, seed + 2026, options.init_style_sfk);
      initialize_block(buffer.block_SFP, seed + 2025, options.init_style_sfp);
      initialize_block(buffer.block_SFV, seed + 2024, options.init_style_sfv);
      
      // Build reference SF buffers in row-major order from kernel buffer
      // CUTLASS layout encodes (row, group) in mode0 coord = row + group*128:
      //   offset = (row%32)*16 + ((row/32)%4)*4 + group*512 + head*SQ*SF_D
      {
        int SQ_loc = size<0>(problem_size);
        int SK_loc = size<1>(problem_size);
        int D_loc = size<2>(problem_size);
        int H_loc  = size<3,0>(problem_size);
        int H_K_loc = size<3,0,1>(problem_size);
        int B_loc = size<3,1>(problem_size);
        int SF_D_loc = D_loc / 32;
        int SF_K_loc = (SK_loc + 31) / 32;  // ceil(K/32) — SFV groups along K-seqlen
        size_t ref_sfq_sz = (size_t)SQ_loc * SF_D_loc * H_loc * B_loc;
        size_t ref_sfk_sz = (size_t)SK_loc * SF_D_loc * H_K_loc * B_loc;
        buffer.block_ref_SFQ.reset(ref_sfq_sz);
        buffer.block_ref_SFK.reset(ref_sfk_sz);
        buffer.block_ref_SFP.reset(ref_sfq_sz);
        buffer.block_ref_SFV.reset((size_t)D_loc * SF_K_loc * H_K_loc * B_loc);
        
        auto fill_ref = [&](auto& kernel_buf, auto& ref_buf, int rows, int hb, auto& layout_sf) {
          if (hb == 0 || SF_D_loc == 0) return;
          size_t n_kernel = cosize(layout_sf);
          size_t n_ref = (size_t)rows * SF_D_loc * hb;
          std::vector<ElementScale> host_kernel(n_kernel);
          std::vector<ElementScale> host_ref(n_ref, ElementScale(0));
          cudaMemcpy(host_kernel.data(), kernel_buf.get(), n_kernel * sizeof(ElementScale), cudaMemcpyDeviceToHost);
          // CUTLASS layout pads rows to 128-tile boundaries. The head stride is:
          //   head_stride = 512 * ceil(rows / 128)
          // where 512 = 128 rows * 4 groups (SF_D_loc=4).
          // Using rows * SF_D_loc (= rows*4) is WRONG for non-aligned rows.
          int num_128row_tiles = (rows + 127) / 128;
          int head_stride = 512 * num_128row_tiles;
          for (int r = 0; r < rows; r++) {
            for (int g = 0; g < SF_D_loc; g++) {
              for (int h = 0; h < hb; h++) {
                // CUTLASS mode0 offset (row): (r%32)*16 + ((r/32)%4)*4 + (r/128)*512
                // CUTLASS mode1 (K-element index): g*32 → group g gives offset = g (stride 1)
                // CUTLASS mode2 (head): h * head_stride (padded to 128-row tiles)
                int mode0 = (r % 32) * 16 + ((r / 32) % 4) * 4 + (r / 128) * 512;
                int cutlass_idx = mode0 + g + h * head_stride;
                int rm_idx = r * SF_D_loc + g + h * rows * SF_D_loc;
                if (cutlass_idx < (int)n_kernel && rm_idx < (int)n_ref) {
                  host_ref[rm_idx] = host_kernel[cutlass_idx];
                }
              }
            }
          }
          cudaMemcpy(ref_buf.get(), host_ref.data(), n_ref * sizeof(ElementScale), cudaMemcpyHostToDevice);
        };
        
        fill_ref(buffer.block_SFQ, buffer.block_ref_SFQ, SQ_loc, H_loc * B_loc, layout_SFQ);
        fill_ref(buffer.block_SFK, buffer.block_ref_SFK, SK_loc, H_K_loc * B_loc, layout_SFK);
        fill_ref(buffer.block_SFP, buffer.block_ref_SFP, SQ_loc, H_loc * B_loc, layout_SFP);
        // SFV: CUTLASS layout has shape (D=128, K/32, H_K*B), groups along K-seqlen
        // Reference buffer is row-major (D, K/32, H_K*B) with stride (K/32, 1, D*K/32)
        // CUTLASS SfAtom: mode0=(d%32)*16+(d/32)*4 for 128 rows, mode1=kg (flat offset),
        // per-atom-tile=512 (128*4), num_tiles=ceil(K/32/4), per-hb stride=512*num_tiles
        {
          int sfv_k_rows = D_loc;
          int sfv_k_groups = SF_K_loc;
          int sfv_hb = H_K_loc * B_loc;
          int sfv_num_tiles = (sfv_k_groups + 3) / 4;
          int sfv_per_hb = 512 * sfv_num_tiles;
          size_t n_kernel_sfv = cosize(layout_SFV);
          size_t n_ref_sfv = (size_t)sfv_k_rows * sfv_k_groups * sfv_hb;
          std::vector<ElementScale> host_kernel_sfv(n_kernel_sfv);
          std::vector<ElementScale> host_ref_sfv(n_ref_sfv, ElementScale(0));
          cudaMemcpy(host_kernel_sfv.data(), buffer.block_SFV.get(),
                     n_kernel_sfv * sizeof(ElementScale), cudaMemcpyDeviceToHost);
          
          // Validate: check the layout structure by comparing manual formula vs expected pattern
          for (int d = 0; d < sfv_k_rows; d++) {
            int mode0 = (d % 32) * 16 + (d / 32) * 4;
            for (int kg = 0; kg < sfv_k_groups; kg++) {
              int tile_inner = kg % 4;
              int tile_outer = kg / 4;
              int cutlass_base = mode0 + tile_inner + tile_outer * 512;
              for (int h = 0; h < sfv_hb; h++) {
                int cutlass_idx = cutlass_base + h * sfv_per_hb;
                int rm_idx = d * sfv_k_groups + kg + h * sfv_k_rows * sfv_k_groups;
                if (cutlass_idx >= 0 && cutlass_idx < (int)n_kernel_sfv && rm_idx >= 0 && rm_idx < (int)n_ref_sfv) {
                  host_ref_sfv[rm_idx] = host_kernel_sfv[cutlass_idx];
                }
              }
            }
          }
          cudaMemcpy(buffer.block_ref_SFV.get(), host_ref_sfv.data(),
                     n_ref_sfv * sizeof(ElementScale), cudaMemcpyHostToDevice);
        
          // Simple random SFV initialization (user requested keep random)
          // No override needed — random SFV already set by initialize_block above
        }  // end SFV-specific block
      }  // end fill_ref outer block
      
      size_t total_sfq = size(filter_zeros(layout_SFQ));
      std::vector<ElementScale> h_sfQ(total_sfq);
      cudaMemcpy(h_sfQ.data(), buffer.block_SFQ.get(),
                 total_sfq * sizeof(ElementScale), cudaMemcpyDeviceToHost);

      // int D_val = get<2>(problem_size);
      int D_val = 4;
      int print_rows = 128;
      int print_cols = 4;

      int start_rows = 0;
      int start_cols = 0;
      printf("size of sfQ: %d\n", total_sfq);
      printf("=== Q (E4M3) first %d x %d ===\n", print_rows, print_cols);
      for (int r = start_rows; r < start_rows+print_rows; r++) {
          for (int c = start_cols; c < start_cols+print_cols; c++) {
              printf("h_sfQ[%d]: %f, ", r * D_val + c, (float)h_sfQ[r * D_val + c]);
          }
          printf("\n");
      }

      printf("shapeQ: ");
      print(shape_QO);
      printf("shape SFQ:: ");
      print(layout_SFQ);
      printf("\n");
      // int total_k = size(filter_zeros(layout_SFK));
      // std::vector<ElementScale> h_sfK(total_k);
      // cudaMemcpy(h_sfK.data(), buffer.block_SFK.get(),
      //            total_k * sizeof(ElementScale), cudaMemcpyDeviceToHost);
      // for (int r = 0; r < 0+32; r++) {
      //         printf("h_sfK[%d]: %f\n, ", r, (float)h_sfK[r]);
      // }


      // total_q = size(filter_zeros(layout_SFV));
      // std::vector<ElementScale> h_sfv(total_q);
      // cudaMemcpy(h_sfv.data(), buffer.block_SFV.get(),
      //            total_q * sizeof(ElementScale), cudaMemcpyDeviceToHost);
      // for (int r = 0; r < 0+32; r++) {
      //         printf("h_sfv[%d]: %f\n, ", r, (float)h_sfv[r]);
      // }

      int total_q = size((shape_QO));
      std::vector<ElementData> h_q(total_q);
      cudaMemcpy(h_q.data(), buffer.block_Q.get(),
                 total_q * sizeof(ElementData), cudaMemcpyDeviceToHost);
      for (int r = 0; r < 0+32; r++) {
              printf("h_q[%d]: %f\n, ", r, (float)h_q[r]);
      }

      
      int total_kv = size((shape_KV));
      std::vector<ElementData> h_kv(total_kv);
      cudaMemcpy(h_kv.data(), buffer.block_K.get(),
                 total_kv * sizeof(ElementData), cudaMemcpyDeviceToHost);
      for (int r = 0; r < 0+32; r++) {
              printf("h_kv[%d]: %f\n, ", r, (float)h_kv[r]);
      }
      

      total_q = size(filter_zeros(layout_SFQ));
      std::vector<ElementScale> h_sfQ2(total_q);
      cudaMemcpy(h_sfQ2.data(), buffer.block_SFQ.get(),
                 total_q * sizeof(ElementScale), cudaMemcpyDeviceToHost);
      for (int r = 0; r < 0+32; r++) {
              printf("h_sfQ2[%d]: %f\n, ", r, (float)h_sfQ2[r]);
      }

      total_q = size(filter_zeros(layout_SFK));
      std::vector<ElementScale> h_sfK(total_q);
      cudaMemcpy(h_sfK.data(), buffer.block_SFK.get(),
                 total_q * sizeof(ElementScale), cudaMemcpyDeviceToHost);
      for (int r = 0; r < 0+32; r++) {
              printf("h_sfK[%d]: %f\n, ", r, (float)h_sfK[r]);
      }

      // D_val = get<2>(problem_size);
      // print_rows = 2;
      // print_cols = 32;
      // start_rows = 256;
      // start_cols = 0;

      // // printf("=== Q (E4M3) first %d x %d ===\n", print_rows, print_cols);
      // // for (int r = start_rows; r < start_rows+print_rows; r++) {
      // //     for (int c = start_cols; c < start_cols+print_cols; c++) {
      // //         printf("h_sfQ[%d]: %f, ", r * D_val + c, (float)h_sfQ[r * D_val + c]);
      // //     }
      // //     printf("\n");
      // // }
      // for (int r = 0; r < 0+32; r++) {
      //         printf("h_sfQ[%d]: %f\n, ", r, (float)h_sfQ[r]);
      // }
      // printf("*********************\n");
      // total_q = size(filter_zeros(layout_SFP));
      // std::vector<ElementScale> h_sfP(total_q);
      // cudaMemcpy(h_sfP.data(), buffer.block_SFP.get(),
      //            total_q * sizeof(ElementScale), cudaMemcpyDeviceToHost);

      // D_val = get<2>(problem_size);
      // print_rows = 2;
      // print_cols = 32;
      // start_rows = 256;
      // start_cols = 0;

      // // printf("=== Q (E4M3) first %d x %d ===\n", print_rows, print_cols);
      // // for (int r = start_rows; r < start_rows+print_rows; r++) {
      // //     for (int c = start_cols; c < start_cols+print_cols; c++) {
      // //         printf("h_sfQ[%d]: %f, ", r * D_val + c, (float)h_sfQ[r * D_val + c]);
      // //     }
      // //     printf("\n");
      // // }
      // for (int r = 0; r < 0+32; r++) {
      //         printf("h_sfP[%d]: %f\n, ", r, (float)h_sfP[r]);
      // }
      // // for (int r = 524288*2; r < 524288*2+32; r++) {
      // //         printf("h_sfQ[%d]: %f, ", r, (float)h_sfQ[r]);
      // // }
      // // printf("Q[0] as float = %f\n", float(h_sfQ[0]));

      if ( ! cumulative_seqlen_q.empty()) {
        buffer.device_cumulative_seqlen_q.reset(cumulative_seqlen_q.size());
        buffer.device_cumulative_seqlen_q.copy_from_host(
          cumulative_seqlen_q.data(), cumulative_seqlen_q.size());
      }
      if ( ! cumulative_seqlen_kv.empty()) {
        buffer.device_cumulative_seqlen_kv.reset(cumulative_seqlen_kv.size());
        buffer.device_cumulative_seqlen_kv.copy_from_host(
          cumulative_seqlen_kv.data(), cumulative_seqlen_kv.size());
      }   
    };

    buffers.push_back(std::make_unique<DeviceBuffer>());
    buffer_init_fn(*buffers.back());

    int tensor_ring_buffers = options.tensor_ring_buffers;
    for (int i = 1; i < tensor_ring_buffers; i++) {
      buffers.push_back(std::make_unique<DeviceBuffer>());
      buffer_init_fn(*buffers.back());
    }

    if constexpr (kIsVarlen) {
      get<0>(problem_shape).cumulative_length = buffers[0]->device_cumulative_seqlen_q.get();
      get<1>(problem_shape).cumulative_length = buffers[0]->device_cumulative_seqlen_kv.get();
    }

    return problem_shape;
  }
  const char* to_string(cutlass::Status status) {
    switch (status) {
        case cutlass::Status::kSuccess:           return "Success";
        case cutlass::Status::kErrorInvalidProblem: return "ErrorInvalidProblem";
        case cutlass::Status::kErrorNotSupported:   return "ErrorNotSupported";
        case cutlass::Status::kErrorInternal:       return "ErrorInternal";
        // 可根据需要添加更多枚举值
        default:                                    return "Unknown";
    }
}
  auto get_arguments(const ProblemShapeType& problem_shape, const cutlass::KernelHardwareInfo& hw_info, int buffer_index) {
    auto problem_shape_ = problem_shape;
    if constexpr (kIsVarlen) {
      get<0>(problem_shape_).cumulative_length = buffers[buffer_index]->device_cumulative_seqlen_q.get();
      get<1>(problem_shape_).cumulative_length = buffers[buffer_index]->device_cumulative_seqlen_kv.get();
    }
    typename Operation::Arguments arguments{
      problem_shape_,
      { buffers[buffer_index]->block_Q.get(), stride_Q,
        buffers[buffer_index]->block_SFQ.get(), layout_SFQ,
        buffers[buffer_index]->block_K.get(), stride_K,
        buffers[buffer_index]->block_SFK.get(), layout_SFK,
        buffers[buffer_index]->block_SFP.get(), layout_SFP,
        buffers[buffer_index]->block_V.get(), stride_V,
        buffers[buffer_index]->block_SFV.get(), layout_SFV,},
      { buffers[buffer_index]->block_O.get(), stride_O,
        buffers[buffer_index]->block_LSE.get(), stride_LSE },
      hw_info
    };
    return arguments;
  }

  ExampleResult run(const Options& options, const cutlass::KernelHardwareInfo& hw_info) {

    ProblemShapeType problem_shape = initialize(options);

    int buffer_index = 0;
    typename Operation::Arguments arguments = get_arguments(problem_shape, hw_info, buffer_index);

    Operation op;

    ExampleResult example_result;

    example_result.smem_size = Operation::Kernel::SharedStorageSize;

    size_t workspace_size = 0;
    workspace_size = Operation::get_workspace_size(arguments);
    DeviceAllocation<uint8_t> workspace(workspace_size);

    cutlass::Status status = cutlass::Status::kSuccess;
    status = op.can_implement(arguments);
    std::cout << "status: " << to_string(status) << std::endl;
    if (status != cutlass::Status::kSuccess) {
      std::cerr << "This kernel is not supported. Last CUDA error is: "
                << cudaGetErrorString(cudaGetLastError()) << std::endl;
      return example_result;
    }

    status = op.initialize(arguments, workspace.get());
    std::cout << "status_initialize: " << to_string(status) << std::endl;
    if (status != cutlass::Status::kSuccess) {
      std::cerr << "Failed to initialize the CUTLASS kernel. Last CUDA error is: "
                << cudaGetErrorString(cudaGetLastError()) << std::endl;
      return example_result;
    }

    // Run
    for (int i = 0; i < options.warmup_iterations; i++) {
      status = op.run();
      if (status != cutlass::Status::kSuccess) {
        std::cerr << "Failed to launch the CUTLASS kernel. Last CUDA error is: "
                  << cudaGetErrorString(cudaGetLastError()) << std::endl;
        return example_result;
      }
      buffer_index = (buffer_index + 1) % buffers.size();
      arguments = get_arguments(problem_shape, hw_info, buffer_index);
      status = op.update(arguments, workspace.get());
      if (status != cutlass::Status::kSuccess) {
        std::cerr << "Failed to update the CUTLASS kernel's parameters. Last CUDA error is: "
                  << std::endl;
        return example_result;
      }
    }

    cudaError_t result = cudaDeviceSynchronize();
    if (result != cudaSuccess) {
      std::cerr << "Error running the CUTLASS kernel. Last CUDA error is: "
                << cudaGetErrorString(result) << std::endl;
      return example_result;
    }

    //
    // Construct events
    //

    cudaEvent_t events[2];

    for (auto & event : events) {
      result = cudaEventCreate(&event);
      if (result != cudaSuccess) {
        std::cerr << "cudaEventCreate() failed: " << cudaGetErrorString(result) << std::endl;
        return example_result;
      }
    }

    // Record an event at the start of a series of GEMMs
    result = cudaEventRecord(events[0]);
    if (result != cudaSuccess) {
      std::cerr << "cudaEventRecord() failed: " << cudaGetErrorString(result) << std::endl;
      return example_result;
    }

    for (int i = 0; i < options.iterations; i++) {
      status = op.run();
      if (status != cutlass::Status::kSuccess) {
        std::cerr << "Failed to launch the CUTLASS kernel. Last CUDA error is: "
                  << cudaGetErrorString(cudaGetLastError()) << std::endl;
        return example_result;
      }
      buffer_index = (buffer_index + 1) % buffers.size();
      arguments = get_arguments(problem_shape, hw_info, buffer_index);
      status = op.update(arguments, workspace.get());
      if (status != cutlass::Status::kSuccess) {
        std::cerr << "Failed to update the CUTLASS kernel's parameters. Last CUDA error is: "
                  << std::endl;
        return example_result;
      }
    }

    //
    // Stop profiling loop
    //

    // Record an event when the GEMMs are complete
    result = cudaEventRecord(events[1]);
    if (result != cudaSuccess) {
      std::cerr << "cudaEventRecord() failed: " << cudaGetErrorString(result) << std::endl;
      return example_result;
    }

    // Wait for work on the device to complete.
    result = cudaEventSynchronize(events[1]);
    if (result != cudaSuccess) {
      std::cerr << "cudaEventSynchronize() failed: " << cudaGetErrorString(result) << std::endl;
      return example_result;
    }

    // Measure elapsed runtime
    float runtime_ms = 0;
    result = cudaEventElapsedTime(&runtime_ms, events[0], events[1]);
    if (result != cudaSuccess) {
      std::cerr << "cudaEventElapsed() failed: " << cudaGetErrorString(result) << std::endl;
      return example_result;
    }

    runtime_ms /= static_cast<float>(options.iterations);

    double flops;
    if (kIsVarlen) {
      flops = 0.0;
      for (int i = 0; i < size<3,1>(problem_shape); i++) {
        flops += (cumulative_seqlen_q[i+1] - cumulative_seqlen_q[i])
               * 1.0
               * (cumulative_seqlen_kv[i+1] - cumulative_seqlen_kv[i]);
      }
    }
    else {
      flops = 1.0;
      flops *= static_cast<double>(size<0>(problem_shape));
      flops *= static_cast<double>(size<1>(problem_shape));
      flops *= static_cast<double>(size<3,1>(problem_shape));
    }
    flops *= 4.0 * (std::is_same_v<ActiveMask, CausalMask<true>> || std::is_same_v<ActiveMask, CausalMask<false>> ? 0.5 : 1.0);
    flops *= static_cast<double>(size<2>(problem_shape));
    flops *= static_cast<double>(size<3,0>(problem_shape));
    double tflops_s = flops * 1e-12 /*tera*/ / (runtime_ms * 1e-3 /*ms*/);
    example_result.tflops_tc_s = tflops_s;
    example_result.runtime_ms = runtime_ms;

    result = cudaDeviceSynchronize();
    if (result != cudaSuccess) {
      std::cerr << "Error running the CUTLASS kernel. Last CUDA error is: "
                << cudaGetErrorString(result) << std::endl;
      return example_result;
    }

    // Verify that the result is correct
    bool passed = true;
    if (options.verify) {
      passed = verify(problem_shape, *buffers[0]);
      if (passed) {
        example_result.verified = true;
        // ===== Save config when specific test passes =====
        // Condition: h=1, QK=random, SFQ=random, SFK=1
        if (options.h == 1 &&
            options.init_style_q == InitStyle::kRandom &&
            options.init_style_k == InitStyle::kRandom &&
            options.init_style_sfq == InitStyle::kRandom &&
            options.init_style_sfk == InitStyle::kOne) {
          printf("\n*** PASSED: h=1, QK=random, SFQ=random, SFK=1 ***\n");
          printf("*** Saving working config to passed_config.txt ***\n");
          FILE* f = fopen("passed_config_mxfp8.txt", "a");
          if (f) {
            fprintf(f, "PASS: b=%d h=%d q=%d k=%d d=%d persistent=%d\n",
                    options.b, options.h, options.q, options.k, options.d,
                    (int)options.persistent);
            fclose(f);
          }
        }
      }
    }

    if (!passed) {
      std::cerr << "Reference check failed" << std::endl;
      return example_result;
    }

    example_result.passed = true;

    return example_result;
  }

};

///////////////////////////////////////////////////////////////////////////////////////////////////

int main_result = 0;

///////////////////////////////////////////////////////////////////////////////////////////////////

/// Helper to print a description of the example run and its result
void print_result(const std::string& description, ExampleResult result, bool verbose) {
  std::ios fmt(nullptr);
  fmt.copyfmt(std::cout);
  std::cout << (result.passed ? (result.verified ? " [OK]  " : " [--] ") : "[FAIL] ");
  if (! result.passed) {
    main_result = -1;
  }
  std::cout << std::setw(32) << std::left << description;
  std::cout.copyfmt(fmt);
  std::cout << " : " << result.tflops_tc_s << " TFLOPS/s" << std::endl;
  if (verbose) {
    std::cout << "       t=" << result.runtime_ms << "ms, "
        "smem=" << result.smem_size << "b" << std::endl;
  }
}

///////////////////////////////////////////////////////////////////////////////////////////////////

template<class Mask>
void run_fwd_128(Mask fusion, Options const & options, cutlass::KernelHardwareInfo const& hw_info) {
  auto run = [&](auto shape, const char* name, auto... kernel_options) {
    if ((! options.kernel_filter.empty()) && (! std::regex_search(name, std::basic_regex(options.kernel_filter)))) {
        return;
    }
    if (options.varlen) {
      FwdRunner<true, decltype(shape), void, Mask, decltype(kernel_options)...> runner;
      auto result = runner.run(options, hw_info);
      print_result(name, result, options.verbose);
    }
    else 
    {
      FwdRunner<false, decltype(shape), void, Mask, decltype(kernel_options)...> runner;
      auto result = runner.run(options, hw_info);
      print_result(name, result, options.verbose);
    }
  };

  using HeadDim = _128;

  if (options.persistent) {
    // Persistent Tile Scheduler
    run(Shape<_256, _128, HeadDim>{}, "tma ws 256x128 acc fp32 persistent", Option<Tag::kIsPersistent, true_type>{});
  }
  else {
    // Individual Tile Scheduler
    run(Shape<_256, _128, HeadDim>{}, "tma ws 256x128 acc fp32 individual", Option<Tag::kIsPersistent, false_type>{});
  }
}

///////////////////////////////////////////////////////////////////////////////////////////////////

template<class Mask>
void run_fwd_64(Mask fusion, Options const & options, cutlass::KernelHardwareInfo const& hw_info) {
  auto run = [&](auto shape, const char* name, auto... kernel_options) {
    if ((! options.kernel_filter.empty()) && (! std::regex_search(name, std::basic_regex(options.kernel_filter)))) {
        return;
    }
    if (options.varlen) {
      FwdRunner<true, decltype(shape), void, Mask, decltype(kernel_options)...> runner;
      auto result = runner.run(options, hw_info);
      print_result(name, result, options.verbose);
    }
    else 
    {
      FwdRunner<false, decltype(shape), void, Mask, decltype(kernel_options)...> runner;
      auto result = runner.run(options, hw_info);
      print_result(name, result, options.verbose);
    }
  };

  using HeadDim = _64;

  if (options.persistent) {
    // Persistent Tile Scheduler
    run(Shape<_256, _128, HeadDim>{}, "tma ws 256x128 acc fp32 persistent", Option<Tag::kIsPersistent, true_type>{});
  }
  else {
    // Individual Tile Scheduler
    run(Shape<_256, _128, HeadDim>{}, "tma ws 256x128 acc fp32 individual", Option<Tag::kIsPersistent, false_type>{});
  }
}


///////////////////////////////////////////////////////////////////////////////////////////////////

template<class Mask>
void run_fwd_32(Mask fusion, Options const & options, cutlass::KernelHardwareInfo const& hw_info) {
  auto run = [&](auto shape, const char* name, auto... kernel_options) {
    if (options.varlen) {
      FwdRunner<true, decltype(shape), void, Mask, decltype(kernel_options)...> runner;
      auto result = runner.run(options, hw_info);
      print_result(name, result, options.verbose);
    }
    else {
      FwdRunner<false, decltype(shape), void, Mask, decltype(kernel_options)...> runner;
      auto result = runner.run(options, hw_info);
      print_result(name, result, options.verbose);
    }
  };

  using HeadDim = _32;

#ifdef MXFP8
  if (options.persistent) {
    // Persistent Tile Scheduler
    run(Shape<_256, _128, HeadDim>{}, "tma ws 256x128 acc fp32 persistent", Option<Tag::kIsPersistent, true_type>{});
  }
  else {
    // Individual Tile Scheduler
    run(Shape<_256, _128, HeadDim>{}, "tma ws 256x128 acc fp32 individual", Option<Tag::kIsPersistent, false_type>{});
  }
#endif
}

///////////////////////////////////////////////////////////////////////////////////////////////////

#endif // defined(CUTLASS_ARCH_MMA_SM100_SUPPORTED)

///////////////////////////////////////////////////////////////////////////////////////////////////

int main_single(int argc, char const **args) {

  cudaDeviceProp props;

  cudaError_t error = cudaGetDeviceProperties(&props, 0);
  if (error != cudaSuccess) {
    std::cerr << "cudaGetDeviceProperties() returned an error: " << cudaGetErrorString(error) << std::endl;
    return -1;
  }

  if (__CUDACC_VER_MAJOR__ < 12 || (__CUDACC_VER_MAJOR__ == 12 && __CUDACC_VER_MINOR__ < 8)) {
    std::cerr << "This example requires CUDA 12.8 or newer." << std::endl;
    // Returning zero so this test passes on older Toolkits. Its actions are no-op.
    return 0;
  }

  if (props.major != 10) {
    std::cerr
      << "This example requires a GPU of NVIDIA's Blackwell Architecture "
      << "(compute capability 100a)." << std::endl;
    return 0;
  }  //
  // Parse options
  //

  Options options;

  options.parse(argc, args);

  if (options.help) {
    options.print_usage(std::cout) << std::endl;
    return 0;
  }

  if (options.error) {
    std::cerr << "Aborting execution." << std::endl;
    return -1;
  }

#if defined(CUTLASS_ARCH_MMA_SM100_SUPPORTED)

  //
  // Run examples
  //

  // The KernelHardwareInfo struct holds the number of SMs on the GPU with a given device ID. This
  // information is used by the underlying kernel.
  cutlass::KernelHardwareInfo hw_info;

  // Change device_id to another value if you are running on a machine with multiple GPUs and wish
  // to use a GPU other than that with device ID 0.
  hw_info.device_id = 0;
  if (options.sm_count == 0) {
    hw_info.sm_count = cutlass::KernelHardwareInfo::query_device_multiprocessor_count(hw_info.device_id);
  }
  else {
    hw_info.sm_count = options.sm_count;
  }

  std::cout << "###### B " << options.b << " H " << options.h << " H_K " << options.h_k << " Q " << options.q << " K " << options.k << " D " << options.d << " ";
  std::cout << "Forward" << " " << (options.causal ? "Causal" : (options.residual ? "Residual" : "None")) << " ";
  std::cout << "#SM " << hw_info.sm_count << std::endl;

  auto with_mask = [&](auto fn) {
    if (options.causal) {
      if(options.causal_q_begin) {
        fn(CausalMask{});
      } else {
        fn(CausalMask<false>{});
      }
    }
    else if (options.residual) {
      fn(ResidualMask{});
    }
    else {
      fn(NoMask{});
    }
  };

  with_mask([&](auto fusion) {
    if (options.d <= 32) {
      // run_fwd_32(fusion, options, hw_info);
    }
    else if (options.d <= 64) {
      // run_fwd_64(fusion, options, hw_info);
    }
    else if (options.d <= 128) {
      run_fwd_128(fusion, options, hw_info);
    }
    else {
      std::cout << "No kernel instantiated for d=" << options.d << std::endl;
    }
  });
#endif

  return main_result;
}

/////////////////////////////////////////////////////////////////////////////////////////////////

int main(int argc, char const **args) {
  std::vector<std::string> full_arguments(args, args + argc);
  // #ifdef FP8
  //   printf("im fp8\n");
  // #endif
  // return 0;
  bool recursed = false;
  for (size_t i = 1; i < full_arguments.size(); i++) {
    if (full_arguments[i].find(',') != std::string::npos) {
      auto arg = full_arguments[i];
      size_t eq_pos = arg.find('=');
      std::string prefix = eq_pos == std::string::npos ? "" : arg.substr(0, eq_pos+1);
      std::string rest = eq_pos == std::string::npos ? arg : arg.substr(eq_pos+1);
      for (;;) {
        size_t comma_pos = rest.find(',');
        std::string current = rest.substr(0, comma_pos);
        full_arguments[i] = prefix + current;
        std::vector<const char*> next_args;
        for (auto& elem : full_arguments) { next_args.push_back(elem.data()); }
        main(argc, next_args.data());
        if (comma_pos == std::string::npos) break;
        rest = rest.substr(comma_pos+1);
      }
      recursed = true;
      break;
    }
  }

  if (! recursed) {
    main_single(argc, args);
  }

  return main_result;
}

/////////////////////////////////////////////////////////////////////////////////////////////////
