/***************************************************************************************************
 * Copyright (c) 2025 - 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
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
    \brief Example implementation of fused multi-head attention for Blackwell using CUTLASS 3.

    This example showcases the use of CUTLASS to build backward fused
    multi-head attention (FMHA) collectives from existing CUTLASS collectives targeting
    the NVIDIA Blackwell architecture.

    Background and motivation
    -------------------------
    CUTLASS is a highly flexible library that provides open-source building blocks
    for tensor core programming for GEMM or GEMM-like problems. Fused multi-head
    attention (FMHA) is a foundational kernel for large language models (LLMs) since it
    makes long sequence lengths feasible from a memory-usage perspective. It also
    improves computational efficiency since it transforms an outer-product-like and
    a matrix-vector-like GEMM into a fused operation with much higher arithmetic
    intensity. For more details, see Dao et al, 2022; Dao, 2023.
    Implementing this kernel in CUTLASS enabled easy customization and high
    performance.

    Introduction
    ------------
    The example targets the NVIDIA Blackwell architecture, and takes advantage of
    5th gen tensor cores and the Tensor Memory Accelerator (TMA), just like
    GEMMs do. It provides a backward pass (often abbreviated
    bwd in the code).
    The code is structured into three layers: The runner (and the reference kernels)
    takes care of initialization, measurement, and testing; the device layer
    orchestrates kernel calls and partitions workspace; and the kernel layer (just
    like the CUTLASS kernel layer.

    Support
    ---------

    We support fp16 and fp8 data types with a head dimension of 128.

    Example usage:
    $ ./examples/77_blackwell_fmha/77_blackwell_fmha_bwd_fp16 \
            --b=2048 --h=2048 --d=2048 --q=2048 --k=2048
*/

#include <algorithm>
#include <iostream>
#include <fstream>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <random>
#include <regex>
#include <string>
#include <type_traits>
#include <vector>

#include "cute/tensor.hpp"

#include "cutlass/cutlass.h"
#include "cutlass/kernel_hardware_info.h"

#include "cutlass/util/command_line.h"
#include "cutlass/util/distribution.h"
#include "cutlass/util/reference/device/tensor_fill.h"

#include "reference/fmha_fwd_reference.hpp"
#include "reference/fmha_bwd_reference.hpp"
#include "reference/reference_abs_error.hpp"

#include "collective/fmha_fusion.hpp"
#ifdef BWD_2SM
#include "device/fmha_device_bwd_2sm_probe.hpp"
#else
#include "device/fmha_device_bwd.hpp"
#endif

///////////////////////////////////////////////////////////////////////////////////////////////////

using namespace cute;
using namespace cutlass::fmha::kernel;
using namespace cutlass::fmha::collective;
using namespace cutlass::fmha;

///////////////////////////////////////////////////////////////////////////////////////////////////

enum class InitStyle {
  kOne, kZero, kLinearStride128, kLinearStride1, kRandom, kNone
};

///////////////////////////////////////////////////////////////////////////////////////////////////

/// Command line options parsing
struct Options {

  bool help = false;
  bool error = false;

  int b = 16;
  int h = 16;
  int h_k = 1;
  int q = 1024;
  int k = 1024;
  std::vector<int> varlen_q;
  std::vector<int> varlen_k;
  int d = 128;
  int d_vo = 128;
  int iterations = 3;
  bool verify = false;
  bool verbose = false;

  bool causal = false;
  bool residual = false;
  bool varlen = false;
  int sm_count = 0;

  std::string kernel_filter;

  InitStyle init_style_q = InitStyle::kRandom;
  InitStyle init_style_k = InitStyle::kRandom;
  InitStyle init_style_v = InitStyle::kRandom;
  InitStyle init_style_do = InitStyle::kRandom;
  bool skip_reference = false;

  // Strict gradient validation. A 1-SM run writes the snapshot and a 2-SM
  // run consumes it, so the comparison is performed on the actual dQ/dK/dV
  // arrays rather than on the example's historical loose [OK] threshold.
  std::string dump_gradients;
  std::string compare_gradients;
  double strict_max_diff = 0.1;
  double strict_mean_diff = 0.02;
  int stability_runs = 1;

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
      else if (s == "0") {
        dst = InitStyle::kZero;
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
    cmd.get_cmd_line_argument("d_vo", d_vo, d);
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

    cmd.get_cmd_line_argument("iterations", iterations, defaults.iterations);
    verify = cmd.check_cmd_line_flag("verify");
    verbose = cmd.check_cmd_line_flag("verbose");
    std::string mask;
    cmd.get_cmd_line_argument<std::string>("mask", mask, "");
    if (mask == "causal") {
      causal = true;
    }
    else if (mask == "residual") {
      residual = true;
    }
    else {
      causal = defaults.causal;
    }
    if (varlen) {
      residual = true;
    }

    skip_reference = cmd.check_cmd_line_flag("skip-reference");
    cmd.get_cmd_line_argument("dump-gradients", dump_gradients, std::string{});
    cmd.get_cmd_line_argument("compare-gradients", compare_gradients, std::string{});
    cmd.get_cmd_line_argument("strict-max-diff", strict_max_diff, 0.1);
    cmd.get_cmd_line_argument("strict-mean-diff", strict_mean_diff, 0.02);
    cmd.get_cmd_line_argument("stability-runs", stability_runs, 1);
    if (!dump_gradients.empty() && !compare_gradients.empty()) {
      std::cerr << "Error: --dump-gradients and --compare-gradients are mutually exclusive\n";
      error = true;
    }
    if (stability_runs < 1) {
      std::cerr << "Error: --stability-runs must be at least 1\n";
      error = true;
    }
    if (skip_reference &&
        (!dump_gradients.empty() || !compare_gradients.empty() || stability_runs > 1)) {
      std::cerr << "Error: strict gradient snapshot validation cannot be combined with "
                   "--skip-reference\n";
      error = true;
    }
    cmd.get_cmd_line_argument("sm-count", sm_count, defaults.sm_count);

    get_init_style_argument(cmd, "init-style", init_style_q, defaults.init_style_q);
    get_init_style_argument(cmd, "init-style", init_style_k, defaults.init_style_k);
    get_init_style_argument(cmd, "init-style", init_style_v, defaults.init_style_v);
    get_init_style_argument(cmd, "init-style", init_style_do, defaults.init_style_do);
    get_init_style_argument(cmd, "init-style-q", init_style_q, init_style_q);
    get_init_style_argument(cmd, "init-style-k", init_style_k, init_style_k);
    get_init_style_argument(cmd, "init-style-v", init_style_v, init_style_v);
    get_init_style_argument(cmd, "init-style-do", init_style_do, init_style_do);

    cmd.get_cmd_line_argument("kernel-filter", kernel_filter, defaults.kernel_filter);
  }

  /// Prints the usage statement.
  std::ostream & print_usage(std::ostream &out) const {

    out << "77_blackwell_fmha_bwd\n\n"
      << "  This example showcases the use of CUTLASS's collective operation builders to easily construct\n"
      << "  fused multi-head attention kernels for the backward pass targeting NVIDIA's Blackwell architecture.\n\n"
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
      << "  --d_vo=<int>                  Sets the D_VO extent\n"
      << "  --iterations=<int>          Benchmarking iterations\n"
      << "  --verify                    Verify results\n"
      << "  --dump-gradients=<file>     Write raw dQ/dK/dV and input fingerprint\n"
      << "  --compare-gradients=<file>  Compare raw dQ/dK/dV with a 1-SM snapshot\n"
      << "  --strict-max-diff=<float>   Strict max-absolute-difference limit (default 0.1)\n"
      << "  --strict-mean-diff=<float>  Strict mean-absolute-difference limit (default 0.02)\n"
      << "  --stability-runs=<int>      Clean validation launches; hashes must be stable (default 1)\n"
      << "  --verbose                   Print smem and execution time per kernel\n"
      << "  --mask=<no|residual|causal> Enables masking\n"
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
    case InitStyle::kZero: {
      cutlass::reference::device::BlockFillRandomUniform(
        block.get(), block.size(), seed, (Element) 0, (Element) 0);
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

struct GradientSnapshotHeader {
  char magic[8]{};
  uint32_t version = 3;
  uint32_t header_bytes = 0;
  uint32_t endian_tag = 0x01020304u;
  uint32_t producer = 0; // 1 = 1-SM baseline, 2 = 2-SM candidate
  uint32_t element_bytes = 0;
  int32_t b = 0;
  int32_t h = 0;
  int32_t h_k = 0;
  int32_t q = 0;
  int32_t k = 0;
  int32_t d = 0;
  int32_t d_vo = 0;
  uint32_t mask_flags = 0;
  uint32_t init_q = 0;
  uint32_t init_k = 0;
  uint32_t init_v = 0;
  uint32_t init_do = 0;
  uint64_t seed = 0;
  uint64_t input_hash = 0;
  uint64_t dq_count = 0;
  uint64_t dk_count = 0;
  uint64_t dv_count = 0;
  uint64_t dq_hash = 0;
  uint64_t dk_hash = 0;
  uint64_t dv_hash = 0;
  uint64_t payload_hash = 0;
};

static_assert(std::is_trivially_copyable_v<GradientSnapshotHeader>);

struct GradientDiffStats {
  double max_abs = 0.0;
  double mean_abs = 0.0;
  double rms = 0.0;
  size_t worst_index = 0;
  double actual_at_worst = 0.0;
  double expected_at_worst = 0.0;
  bool finite = true;
};

inline uint64_t fnv1a_append(uint64_t hash, void const* data, size_t bytes) {
  auto ptr = static_cast<uint8_t const*>(data);
  for (size_t i = 0; i < bytes; ++i) {
    hash ^= ptr[i];
    hash *= 1099511628211ull;
  }
  return hash;
}

template <class T>
uint64_t raw_payload_hash(std::vector<T> const& data) {
  return fnv1a_append(
      14695981039346656037ull, data.data(), data.size() * sizeof(T));
}

template <class T>
bool copy_device_vector(DeviceAllocation<T> const& block, std::vector<T>& host) {
  host.resize(block.size());
  if (host.empty()) {
    return true;
  }
  cudaError_t status = cudaMemcpy(
      host.data(), block.get(), host.size() * sizeof(T), cudaMemcpyDeviceToHost);
  if (status != cudaSuccess) {
    std::cerr << "cudaMemcpy(DeviceToHost) failed: " << cudaGetErrorString(status) << "\n";
    return false;
  }
  return true;
}

template <class T>
bool hash_device_allocation(DeviceAllocation<T> const& block, uint64_t& hash) {
  std::vector<T> host;
  if (!copy_device_vector(block, host)) {
    return false;
  }
  hash = fnv1a_append(hash, host.data(), host.size() * sizeof(T));
  return true;
}

template <class T>
GradientDiffStats gradient_diff(std::vector<T> const& actual, std::vector<T> const& expected) {
  GradientDiffStats stats;
  if (actual.size() != expected.size()) {
    stats.finite = false;
    stats.max_abs = std::numeric_limits<double>::infinity();
    return stats;
  }

  long double sum = 0.0;
  long double sum_sq = 0.0;
  for (size_t i = 0; i < actual.size(); ++i) {
    double lhs = static_cast<double>(actual[i]);
    double rhs = static_cast<double>(expected[i]);
    if (!std::isfinite(lhs) || !std::isfinite(rhs)) {
      stats.finite = false;
      stats.max_abs = std::numeric_limits<double>::infinity();
      stats.mean_abs = std::numeric_limits<double>::infinity();
      stats.rms = std::numeric_limits<double>::infinity();
      stats.worst_index = i;
      stats.actual_at_worst = lhs;
      stats.expected_at_worst = rhs;
      return stats;
    }
    double diff = std::fabs(lhs - rhs);
    sum += diff;
    sum_sq += diff * diff;
    if (diff > stats.max_abs) {
      stats.max_abs = diff;
      stats.worst_index = i;
      stats.actual_at_worst = lhs;
      stats.expected_at_worst = rhs;
    }
  }
  if (!actual.empty()) {
    stats.mean_abs = static_cast<double>(sum / actual.size());
    stats.rms = std::sqrt(static_cast<double>(sum_sq / actual.size()));
  }
  return stats;
}

///////////////////////////////////////////////////////////////////////////////////////////////////

struct ExampleResult {
  bool passed = false;
  bool verified = false;
  float runtime_ms = 0;
  double tflops_tc_s = 0;
  size_t smem_size = 0;
};

///////////////////////////////////////////////////////////////////////////////////////////////////

#if defined(CUTLASS_ARCH_MMA_SM100_SUPPORTED)

///////////////////////////////////////////////////////////////////////////////////////////////////

template<
  bool kIsVarlen,
  bool kIsMla,
  class TileShape,
  class DispatchPolicy,
  class ActiveMask,
  class... KernelOptions
>
struct BwdRunner {

#ifdef FP8
  using Element = cutlass::float_e4m3_t;
#else
  using Element = cutlass::half_t;
#endif
  using ElementAccumulator = float;

  // Q K D D_VO ((H_R, H_K) B)
  using ProblemShape = std::conditional_t<
      kIsVarlen,
      cute::tuple<VariableLength, VariableLength, int, int, cute::tuple<cute::tuple<int, int>, int>>,
      cute::tuple<int, int, int, int, cute::tuple<cute::tuple<int, int>, int>>
  >;
  
  using StrideQ = Stride<int, _1, Stride<Stride<int, int>, int>>; // Q D    ((H_R, H_K), B)
  using StrideK = Stride<int, _1, Stride<Stride<_0, int>, int>>;  // K D    ((H_R, H_K), B)
  using StrideV = StrideK;                                        // K D_VO ((H_R, H_K), B)
  using StrideO = StrideQ;                                        // Q D_VO ((H_R, H_K), B)
  using StrideLSE = Stride<_1, Stride<Stride<int, int>, int>>;    // Q      ((H_R, H_K), B)

  // Backwards specific
  using StrideDQ = StrideQ;
  using StrideDK = StrideK;
  using StrideDV = StrideV;
  using StrideDO = StrideO;

  //
  // Data members
  //

  /// Initialization
  StrideQ stride_Q;
  StrideK stride_K;
  StrideV stride_V;
  StrideO stride_O;
  StrideLSE stride_LSE;

  StrideDQ stride_dQ;
  StrideDK stride_dK;
  StrideDV stride_dV;
  StrideDO stride_dO;

  uint64_t seed = 0;

  DeviceAllocation<Element> block_Q;
  DeviceAllocation<Element> block_K;
  DeviceAllocation<Element> block_V;
  DeviceAllocation<Element> block_O;
  DeviceAllocation<ElementAccumulator> block_LSE;

  DeviceAllocation<int> block_cumulative_seqlen_q;
  DeviceAllocation<int> block_cumulative_seqlen_kv;

  DeviceAllocation<Element> block_dQ;
  DeviceAllocation<Element> block_dK;
  DeviceAllocation<Element> block_dV;
  DeviceAllocation<Element> block_dO;

  DeviceAllocation<Element> block_ref_dQ;
  DeviceAllocation<Element> block_ref_dK;
  DeviceAllocation<Element> block_ref_dV;

  // Populated by verify(). Keeping the full reference arrays lets the strict
  // 2-SM-vs-1-SM report show all three values at the direct-comparison worst
  // coordinate, rather than relying on an unrelated scalar [OK] summary.
  std::vector<Element> host_ref_dQ;
  std::vector<Element> host_ref_dK;
  std::vector<Element> host_ref_dV;
  uint64_t strict_input_hash = 0;
  bool strict_input_hash_valid = false;

  enum class GradientKind { kDQ, kDK, kDV };

  struct HostGradients {
    std::vector<Element> dq;
    std::vector<Element> dk;
    std::vector<Element> dv;
  };

  struct GradientHashes {
    uint64_t dq = 0;
    uint64_t dk = 0;
    uint64_t dv = 0;
  };

  struct GradientCoordinate {
    size_t batch = 0;
    size_t h_q = 0;
    size_t h_k = 0;
    size_t h_r = 0;
    size_t row = 0;
    size_t col = 0;
  };

  struct TileDiffAggregate {
    size_t plane = 0;
    size_t row_tile = 0;
    size_t count = 0;
    size_t over_threshold = 0;
    size_t worst_index = 0;
    long double sum = 0.0;
    long double sum_sq = 0.0;
    double max_abs = 0.0;
  };

  //
  // Methods
  //
  bool capture_gradients(HostGradients& host) const {
    return copy_device_vector(block_dQ, host.dq) &&
           copy_device_vector(block_dK, host.dk) &&
           copy_device_vector(block_dV, host.dv);
  }

  static GradientHashes gradient_hashes(HostGradients const& host) {
    return {raw_payload_hash(host.dq), raw_payload_hash(host.dk), raw_payload_hash(host.dv)};
  }

  static void print_gradient_hashes(char const* role, GradientHashes const& hashes) {
    std::cerr << "[RAW_HASH] " << role
              << " dQ=0x" << std::hex << hashes.dq
              << " dK=0x" << hashes.dk
              << " dV=0x" << hashes.dv << std::dec << "\n";
  }

  static GradientCoordinate decode_gradient_index(
      GradientKind kind, size_t index, Options const& options, int rows, int cols) {
    GradientCoordinate coord{};
    size_t const plane_elements = static_cast<size_t>(rows) * cols;
    size_t const outer = plane_elements == 0 ? 0 : index / plane_elements;
    size_t const in_plane = plane_elements == 0 ? 0 : index % plane_elements;
    coord.row = cols == 0 ? 0 : in_plane / static_cast<size_t>(cols);
    coord.col = cols == 0 ? 0 : in_plane % static_cast<size_t>(cols);

    size_t const h_k_count = static_cast<size_t>(std::max(options.h_k, 1));
    if (kind == GradientKind::kDQ) {
      size_t const h_r_count = static_cast<size_t>(
          std::max(options.h / std::max(options.h_k, 1), 1));
      coord.h_r = outer % h_r_count;
      size_t const outer_hk = outer / h_r_count;
      coord.h_k = outer_hk % h_k_count;
      coord.batch = outer_hk / h_k_count;
      coord.h_q = coord.h_k * h_r_count + coord.h_r;
    }
    else {
      coord.h_k = outer % h_k_count;
      coord.batch = outer / h_k_count;
    }
    return coord;
  }

  static void print_gradient_coordinate(
      std::ostream& out, GradientKind kind, GradientCoordinate const& coord) {
    if (kind == GradientKind::kDQ) {
      out << "b=" << coord.batch << ",hq=" << coord.h_q
          << ",hk=" << coord.h_k << ",hr=" << coord.h_r;
    }
    else {
      out << "b=" << coord.batch << ",hk=" << coord.h_k;
    }
    out << ",row=" << coord.row << ",d=" << coord.col;
  }

  bool compute_input_hash(Options const& options, uint64_t& hash) const {
    hash = 14695981039346656037ull;
    hash = fnv1a_append(hash, &options.b, sizeof(options.b));
    hash = fnv1a_append(hash, &options.h, sizeof(options.h));
    hash = fnv1a_append(hash, &options.h_k, sizeof(options.h_k));
    hash = fnv1a_append(hash, &options.q, sizeof(options.q));
    hash = fnv1a_append(hash, &options.k, sizeof(options.k));
    hash = fnv1a_append(hash, &options.d, sizeof(options.d));
    hash = fnv1a_append(hash, &options.d_vo, sizeof(options.d_vo));
    return hash_device_allocation(block_Q, hash) &&
           hash_device_allocation(block_K, hash) &&
           hash_device_allocation(block_V, hash) &&
           hash_device_allocation(block_dO, hash) &&
           hash_device_allocation(block_O, hash) &&
           hash_device_allocation(block_LSE, hash);
  }

  GradientSnapshotHeader make_snapshot_header(Options const& options, uint64_t input_hash) const {
    GradientSnapshotHeader header{};
    std::memcpy(header.magic, "FAGBWD1", sizeof(header.magic));
    header.version = 3;
    header.header_bytes = sizeof(GradientSnapshotHeader);
    header.endian_tag = 0x01020304u;
#ifdef BWD_2SM
    header.producer = 2;
#else
    header.producer = 1;
#endif
    header.element_bytes = sizeof(Element);
    header.b = options.b;
    header.h = options.h;
    header.h_k = options.h_k;
    header.q = options.q;
    header.k = options.k;
    header.d = options.d;
    header.d_vo = options.d_vo;
    header.mask_flags = uint32_t(options.causal) |
                        (uint32_t(options.residual) << 1) |
                        (uint32_t(options.varlen) << 2);
    header.init_q = static_cast<uint32_t>(options.init_style_q);
    header.init_k = static_cast<uint32_t>(options.init_style_k);
    header.init_v = static_cast<uint32_t>(options.init_style_v);
    header.init_do = static_cast<uint32_t>(options.init_style_do);
    header.seed = seed;
    header.input_hash = input_hash;
    header.dq_count = block_dQ.size();
    header.dk_count = block_dK.size();
    header.dv_count = block_dV.size();
    return header;
  }

  static bool same_snapshot_problem(
      GradientSnapshotHeader const& lhs, GradientSnapshotHeader const& rhs) {
    return std::memcmp(lhs.magic, rhs.magic, sizeof(lhs.magic)) == 0 &&
           lhs.version == rhs.version && lhs.header_bytes == rhs.header_bytes &&
           lhs.endian_tag == rhs.endian_tag && lhs.element_bytes == rhs.element_bytes &&
           lhs.b == rhs.b && lhs.h == rhs.h && lhs.h_k == rhs.h_k &&
           lhs.q == rhs.q && lhs.k == rhs.k && lhs.d == rhs.d && lhs.d_vo == rhs.d_vo &&
           lhs.mask_flags == rhs.mask_flags &&
           lhs.init_q == rhs.init_q && lhs.init_k == rhs.init_k &&
           lhs.init_v == rhs.init_v && lhs.init_do == rhs.init_do &&
           lhs.seed == rhs.seed && lhs.input_hash == rhs.input_hash &&
           lhs.dq_count == rhs.dq_count && lhs.dk_count == rhs.dk_count &&
           lhs.dv_count == rhs.dv_count;
  }

  static void print_gradient_stats(
      char const* tag, GradientKind kind, GradientDiffStats const& stats,
      Options const& options, int rows, int cols,
      std::vector<Element> const* reference = nullptr) {
    GradientCoordinate const coord =
        decode_gradient_index(kind, stats.worst_index, options, rows, cols);
    std::cerr << "[STRICT] " << tag
              << ": max=" << stats.max_abs
              << " mean=" << stats.mean_abs
              << " rms=" << stats.rms
              << " finite=" << (stats.finite ? "yes" : "no")
              << " worst=(";
    print_gradient_coordinate(std::cerr, kind, coord);
    std::cerr << ")"
              << " actual=" << stats.actual_at_worst
              << " expected=" << stats.expected_at_worst;
    if (reference != nullptr && stats.worst_index < reference->size()) {
      std::cerr << " reference=" << static_cast<double>((*reference)[stats.worst_index]);
    }
    std::cerr << "\n";
  }

  static void print_top_row_tiles(
      char const* tag, GradientKind kind,
      std::vector<Element> const& actual, std::vector<Element> const& expected,
      Options const& options, int rows, int cols, double threshold,
      std::vector<Element> const* reference = nullptr) {
    constexpr size_t kTileRows = 128;
    constexpr size_t kTopTiles = 8;
    size_t const plane_elements = static_cast<size_t>(rows) * cols;
    if (actual.size() != expected.size() || plane_elements == 0 ||
        actual.size() % plane_elements != 0) {
      std::cerr << "[STRICT_TILE] " << tag << ": invalid tensor geometry\n";
      return;
    }

    size_t const plane_count = actual.size() / plane_elements;
    size_t const tiles_per_plane = (static_cast<size_t>(rows) + kTileRows - 1) / kTileRows;
    std::vector<TileDiffAggregate> tiles(plane_count * tiles_per_plane);
    for (size_t plane = 0; plane < plane_count; ++plane) {
      for (size_t tile = 0; tile < tiles_per_plane; ++tile) {
        TileDiffAggregate& aggregate = tiles[plane * tiles_per_plane + tile];
        aggregate.plane = plane;
        aggregate.row_tile = tile;
      }
    }

    for (size_t index = 0; index < actual.size(); ++index) {
      size_t const in_plane = index % plane_elements;
      size_t const row = in_plane / static_cast<size_t>(cols);
      size_t const plane = index / plane_elements;
      TileDiffAggregate& aggregate =
          tiles[plane * tiles_per_plane + row / kTileRows];
      double const lhs = static_cast<double>(actual[index]);
      double const rhs = static_cast<double>(expected[index]);
      double const diff = std::isfinite(lhs) && std::isfinite(rhs)
          ? std::fabs(lhs - rhs)
          : std::numeric_limits<double>::infinity();
      if (aggregate.count == 0) {
        aggregate.worst_index = index;
      }
      ++aggregate.count;
      aggregate.sum += diff;
      aggregate.sum_sq += diff * diff;
      if (diff > threshold) {
        ++aggregate.over_threshold;
      }
      if (diff > aggregate.max_abs) {
        aggregate.max_abs = diff;
        aggregate.worst_index = index;
      }
    }

    std::vector<size_t> order;
    order.reserve(tiles.size());
    for (size_t i = 0; i < tiles.size(); ++i) {
      if (tiles[i].count != 0) {
        order.push_back(i);
      }
    }
    std::sort(order.begin(), order.end(), [&](size_t lhs, size_t rhs) {
      TileDiffAggregate const& a = tiles[lhs];
      TileDiffAggregate const& b = tiles[rhs];
      if (a.max_abs != b.max_abs) {
        return a.max_abs > b.max_abs;
      }
      long double const mean_a = a.sum / a.count;
      long double const mean_b = b.sum / b.count;
      return mean_a > mean_b;
    });

    size_t const top_count = std::min(kTopTiles, order.size());
    std::cerr << "[STRICT_TILE] " << tag << " top=" << top_count
              << " tile_rows=" << kTileRows << "\n";
    for (size_t rank = 0; rank < top_count; ++rank) {
      TileDiffAggregate const& aggregate = tiles[order[rank]];
      double const mean = static_cast<double>(aggregate.sum / aggregate.count);
      double const rms = std::sqrt(
          static_cast<double>(aggregate.sum_sq / aggregate.count));
      size_t const row_begin = aggregate.row_tile * kTileRows;
      size_t const row_end = std::min(
          row_begin + kTileRows, static_cast<size_t>(rows));
      GradientCoordinate const coord = decode_gradient_index(
          kind, aggregate.worst_index, options, rows, cols);
      std::cerr << "  rank=" << rank
                << " rows=[" << row_begin << "," << row_end << ") "
                << "max=" << aggregate.max_abs
                << " mean=" << mean
                << " rms=" << rms
                << " over=" << aggregate.over_threshold << "/" << aggregate.count
                << " worst=(";
      print_gradient_coordinate(std::cerr, kind, coord);
      std::cerr << ") actual="
                << static_cast<double>(actual[aggregate.worst_index])
                << " expected="
                << static_cast<double>(expected[aggregate.worst_index]);
      if (reference != nullptr && aggregate.worst_index < reference->size()) {
        std::cerr << " reference="
                  << static_cast<double>((*reference)[aggregate.worst_index]);
      }
      std::cerr << "\n";
    }
  }

  static bool check_stability_tensor(
      char const* tag, GradientKind kind,
      std::vector<Element> const& actual, std::vector<Element> const& baseline,
      uint64_t actual_hash, uint64_t baseline_hash,
      Options const& options, int rows, int cols) {
    if (actual_hash == baseline_hash) {
      return true;
    }
    GradientDiffStats const stats = gradient_diff(actual, baseline);
    print_gradient_stats(tag, kind, stats, options, rows, cols);
    print_top_row_tiles(
        tag, kind, actual, baseline, options, rows, cols,
        options.strict_max_diff);
    return false;
  }

  bool write_gradient_snapshot(Options const& options) const {
    if (!strict_input_hash_valid) {
      std::cerr << "Strict input fingerprint was not captured before the backward launch\n";
      return false;
    }
    uint64_t input_hash = strict_input_hash;
    GradientSnapshotHeader header = make_snapshot_header(options, input_hash);
    if (header.producer != 1) {
      std::cerr << "Refusing to write a baseline snapshot from a 2-SM build; "
                   "use the 1-SM executable\n";
      return false;
    }
    std::vector<Element> dq, dk, dv;
    if (!copy_device_vector(block_dQ, dq) ||
        !copy_device_vector(block_dK, dk) ||
        !copy_device_vector(block_dV, dv)) {
      return false;
    }
    header.dq_hash = raw_payload_hash(dq);
    header.dk_hash = raw_payload_hash(dk);
    header.dv_hash = raw_payload_hash(dv);
    header.payload_hash = 14695981039346656037ull;
    header.payload_hash = fnv1a_append(
        header.payload_hash, dq.data(), dq.size() * sizeof(Element));
    header.payload_hash = fnv1a_append(
        header.payload_hash, dk.data(), dk.size() * sizeof(Element));
    header.payload_hash = fnv1a_append(
        header.payload_hash, dv.data(), dv.size() * sizeof(Element));

    std::ofstream out(options.dump_gradients, std::ios::binary | std::ios::trunc);
    if (!out) {
      std::cerr << "Failed to open gradient snapshot for writing: "
                << options.dump_gradients << "\n";
      return false;
    }
    out.write(reinterpret_cast<char const*>(&header), sizeof(header));
    out.write(reinterpret_cast<char const*>(dq.data()), dq.size() * sizeof(Element));
    out.write(reinterpret_cast<char const*>(dk.data()), dk.size() * sizeof(Element));
    out.write(reinterpret_cast<char const*>(dv.data()), dv.size() * sizeof(Element));
    if (!out) {
      std::cerr << "Failed while writing gradient snapshot: " << options.dump_gradients << "\n";
      return false;
    }
    std::cerr << "[STRICT] wrote 1-SM gradient snapshot " << options.dump_gradients
              << " input_hash=0x" << std::hex << input_hash << std::dec << "\n";
    print_gradient_hashes(
        "1SM-baseline", {header.dq_hash, header.dk_hash, header.dv_hash});
    return true;
  }

  bool compare_gradient_snapshot(Options const& options) const {
    if (!strict_input_hash_valid) {
      std::cerr << "Strict input fingerprint was not captured before the backward launch\n";
      return false;
    }
    uint64_t input_hash = strict_input_hash;
    GradientSnapshotHeader expected_header = make_snapshot_header(options, input_hash);
    GradientSnapshotHeader file_header{};
    std::ifstream in(options.compare_gradients, std::ios::binary);
    if (!in) {
      std::cerr << "Failed to open gradient baseline: " << options.compare_gradients << "\n";
      return false;
    }
    in.read(reinterpret_cast<char*>(&file_header), sizeof(file_header));
    if (!in || !same_snapshot_problem(expected_header, file_header) || file_header.producer != 1) {
      std::cerr << "Gradient baseline metadata/input fingerprint mismatch: "
                << options.compare_gradients << "\n";
      return false;
    }

    std::vector<Element> base_dq(file_header.dq_count);
    std::vector<Element> base_dk(file_header.dk_count);
    std::vector<Element> base_dv(file_header.dv_count);
    in.read(reinterpret_cast<char*>(base_dq.data()), base_dq.size() * sizeof(Element));
    in.read(reinterpret_cast<char*>(base_dk.data()), base_dk.size() * sizeof(Element));
    in.read(reinterpret_cast<char*>(base_dv.data()), base_dv.size() * sizeof(Element));
    if (!in || in.peek() != std::ifstream::traits_type::eof()) {
      std::cerr << "Gradient baseline is truncated or has trailing data: "
                << options.compare_gradients << "\n";
      return false;
    }
    GradientHashes const baseline_hashes{
        raw_payload_hash(base_dq), raw_payload_hash(base_dk), raw_payload_hash(base_dv)};
    if (baseline_hashes.dq != file_header.dq_hash ||
        baseline_hashes.dk != file_header.dk_hash ||
        baseline_hashes.dv != file_header.dv_hash) {
      std::cerr << "Gradient baseline per-tensor payload hash mismatch: "
                << options.compare_gradients << "\n";
      print_gradient_hashes("1SM-baseline-file", baseline_hashes);
      print_gradient_hashes(
          "1SM-baseline-header",
          {file_header.dq_hash, file_header.dk_hash, file_header.dv_hash});
      return false;
    }
    uint64_t payload_hash = 14695981039346656037ull;
    payload_hash = fnv1a_append(
        payload_hash, base_dq.data(), base_dq.size() * sizeof(Element));
    payload_hash = fnv1a_append(
        payload_hash, base_dk.data(), base_dk.size() * sizeof(Element));
    payload_hash = fnv1a_append(
        payload_hash, base_dv.data(), base_dv.size() * sizeof(Element));
    if (payload_hash != file_header.payload_hash) {
      std::cerr << "Gradient baseline payload hash mismatch: "
                << options.compare_gradients << "\n";
      return false;
    }

    std::vector<Element> actual_dq, actual_dk, actual_dv;
    if (!copy_device_vector(block_dQ, actual_dq) ||
        !copy_device_vector(block_dK, actual_dk) ||
        !copy_device_vector(block_dV, actual_dv)) {
      return false;
    }
    GradientHashes const candidate_hashes{
        raw_payload_hash(actual_dq), raw_payload_hash(actual_dk), raw_payload_hash(actual_dv)};
    print_gradient_hashes("2SM-candidate", candidate_hashes);
    print_gradient_hashes("1SM-baseline", baseline_hashes);
    bool const reference_available =
        host_ref_dQ.size() == actual_dq.size() &&
        host_ref_dK.size() == actual_dk.size() &&
        host_ref_dV.size() == actual_dv.size();
    if (reference_available) {
      print_gradient_hashes(
          "reference",
          {raw_payload_hash(host_ref_dQ), raw_payload_hash(host_ref_dK),
           raw_payload_hash(host_ref_dV)});
    }
    else {
      std::cerr << "[RAW_HASH] reference unavailable\n";
    }
    GradientDiffStats dq = gradient_diff(actual_dq, base_dq);
    GradientDiffStats dk = gradient_diff(actual_dk, base_dk);
    GradientDiffStats dv = gradient_diff(actual_dv, base_dv);
    print_gradient_stats(
        "dQ 2SM-vs-1SM", GradientKind::kDQ, dq, options,
        options.q, options.d, &host_ref_dQ);
    print_gradient_stats(
        "dK 2SM-vs-1SM", GradientKind::kDK, dk, options,
        options.k, options.d, &host_ref_dK);
    print_gradient_stats(
        "dV 2SM-vs-1SM", GradientKind::kDV, dv, options,
        options.k, options.d_vo, &host_ref_dV);

    auto passes = [&](GradientDiffStats const& stats) {
      return stats.finite && stats.max_abs <= options.strict_max_diff &&
             stats.mean_abs <= options.strict_mean_diff;
    };
    bool const passed_dq = passes(dq);
    bool const passed_dk = passes(dk);
    bool const passed_dv = passes(dv);
    if (!passed_dq) {
      print_top_row_tiles(
          "dQ 2SM-vs-1SM", GradientKind::kDQ, actual_dq, base_dq,
          options, options.q, options.d, options.strict_max_diff, &host_ref_dQ);
    }
    if (!passed_dk) {
      print_top_row_tiles(
          "dK 2SM-vs-1SM", GradientKind::kDK, actual_dk, base_dk,
          options, options.k, options.d, options.strict_max_diff, &host_ref_dK);
    }
    if (!passed_dv) {
      print_top_row_tiles(
          "dV 2SM-vs-1SM", GradientKind::kDV, actual_dv, base_dv,
          options, options.k, options.d_vo, options.strict_max_diff, &host_ref_dV);
    }
    bool passed = passed_dq && passed_dk && passed_dv;
    std::cerr << "[STRICT] direct gradient comparison "
              << (passed ? "PASSED" : "FAILED")
              << " (max<=" << options.strict_max_diff
              << ", mean<=" << options.strict_mean_diff << ")\n";
    return passed;
  }

  bool verify(const ProblemShape& problem_shape, Options const& options) {
    auto [Q, K, D, D_VO, HB] = problem_shape;
    auto [H, B] = HB;

    Tensor mQ = make_tensor(make_gmem_ptr(block_Q.get()), make_shape(Q, D, HB), stride_Q);
    Tensor mK = make_tensor(make_gmem_ptr(block_K.get()), make_shape(K, D, HB), stride_K);
    Tensor mV = make_tensor(make_gmem_ptr(block_V.get()), make_shape(K, D_VO, HB), stride_V);
    Tensor mO = make_tensor(make_gmem_ptr(block_O.get()), make_shape(Q, D_VO, HB), stride_O);
    Tensor mLSE = make_tensor(make_gmem_ptr(block_LSE.get()), make_shape(Q, HB), stride_LSE);
    Tensor mDQ = make_tensor(make_gmem_ptr(block_ref_dQ.get()), make_shape(Q, D, HB), stride_dQ);
    Tensor mDK = make_tensor(make_gmem_ptr(block_ref_dK.get()), make_shape(K, D, HB), stride_dK);
    Tensor mDV = make_tensor(make_gmem_ptr(block_ref_dV.get()), make_shape(K, D_VO, HB), stride_dV);
    Tensor mDO = make_tensor(make_gmem_ptr(block_dO.get()), make_shape(Q, D_VO, HB), stride_dO);

    fmha_bwd_reference(problem_shape, mQ, mK, mV, mO, mLSE, mDO, mDQ, mDK, mDV, ActiveMask{});

#ifdef BWD_2SM_DEBUG
    // [HOST mid dump] Compute expected S=Q@K^T, P=softmax(S), dP=dO@V^T for
    // head0/batch0 (coord_HB = ((0,0),0)) and print the SAME logical elements the
    // device dumps, so we can compare. scale = 1/sqrt(D). LSE is in mLSE.
    {
      ElementAccumulator scale = ElementAccumulator(1) / std::sqrt(ElementAccumulator(D));
      ElementAccumulator log2e = ElementAccumulator(M_LOG2E);
      // head0/batch0 == H_K index 0, B 0. With H_R=H_K=16,B=1, head0 batch0 lives
      // at the start of each gmem tensor. We copy the first head's Q/K/V/dO/LSE to host.
      // Logical index for mQ(q,d,((H_R,H_K),B)): stride_Q = (D,1,((D*Q,D*Q*H_R),...)).
      // head0batch0 offset = 0 for Q/K/V/dO (H_K=0,B=0).
      auto H_R = get<4,0,0>(problem_shape); auto H_Kv = get<4,0,1>(problem_shape);
      long long qelem = (long long)Q * D;
      long long kelem = (long long)K * D;
      long long velem = (long long)K * D_VO;
      long long doelem = (long long)Q * D_VO;
      std::vector<Element> hQ(qelem), hK(kelem), hV(velem), hDO(doelem);
      // O is head0/batch0: stride_O = (D_VO,1,((D_VO*Q, D_VO*Q*H_R),...)). head0batch0 offset 0.
      long long oelem = (long long)Q * D_VO;
      std::vector<Element> hO(oelem);
      std::vector<ElementAccumulator> hLSE(Q);
      cudaMemcpy(hQ.data(),  block_Q.get(),  qelem*sizeof(Element), cudaMemcpyDefault);
      cudaMemcpy(hK.data(),  block_K.get(),  kelem*sizeof(Element), cudaMemcpyDefault);
      cudaMemcpy(hV.data(),  block_V.get(),  velem*sizeof(Element), cudaMemcpyDefault);
      cudaMemcpy(hDO.data(), block_dO.get(), doelem*sizeof(Element), cudaMemcpyDefault);
      cudaMemcpy(hO.data(),  block_O.get(),  oelem*sizeof(Element), cudaMemcpyDefault);
      cudaMemcpy(hLSE.data(), block_LSE.get(), Q*sizeof(ElementAccumulator), cudaMemcpyDefault);
      std::cerr << "[HOSTMID] expected S/P/dP/dS for head0/batch0 (scale=" << scale << ")\n";
      // Q row-major (q,d): hQ[q*D+d]; K (k,d): hK[k*D+d]; V (k,dvo): hV[k*D_VO+dvo]; dO (q,dvo); O(q,dvo)
      // sum_OdO_pos[q] = sum_dvo dO[q,dvo]*O[q,dvo]  (reference uses acc_doo = dO.O, positive)
      auto sum_odo_pos = [&](int q){
        ElementAccumulator s = 0;
        for (int dvo = 0; dvo < D_VO; ++dvo) s += ElementAccumulator(hDO[q*D_VO+dvo]) * ElementAccumulator(hO[q*D_VO+dvo]);
        return s;
      };
      auto print_elem = [&](int q, int k){
        ElementAccumulator acc_qk = 0;
        for (int d = 0; d < D; ++d) acc_qk += ElementAccumulator(hQ[q*D+d]) * ElementAccumulator(hK[k*D+d]);
        ElementAccumulator S = acc_qk;
        ElementAccumulator lse = hLSE[q];
        ElementAccumulator P = std::exp2(scale * log2e * S - log2e * lse);
        ElementAccumulator acc_dp = 0;
        for (int dvo = 0; dvo < D_VO; ++dvo)
          acc_dp += ElementAccumulator(hDO[q*D_VO+dvo]) * ElementAccumulator(hV[k*D_VO+dvo]);
        ElementAccumulator sop = sum_odo_pos(q);
        // kernel dS = P * (dP + sum_OdO_neg) = P * (dP - sum_OdO_pos)
        ElementAccumulator dS = P * (acc_dp - sop);
        std::cerr << "[HOSTMID] (q=" << q << ",k=" << k << ") S=" << S
                  << " LSE=" << lse << " P=" << P << " dP=" << acc_dp
                  << " sumOdO+=" << sop << " dS=" << dS << "\n";
      };
      print_elem(0, 0); print_elem(0, 32); print_elem(0, 64); print_elem(0, 96);
      print_elem(1, 0); print_elem(1, 32); print_elem(1, 64); print_elem(1, 96);
      print_elem(2, 0); print_elem(2, 32); print_elem(2, 64); print_elem(2, 96);
    }
#endif

    cudaError_t result = cudaDeviceSynchronize();
    if (result != cudaSuccess) {
      std::cerr << "Reference kernel failed. Last CUDA error: "
                << cudaGetErrorString(result) << std::endl;
      return false;
    }

    const double kMaxDiffThresh = sizeof(Element) == 1 ? options.strict_max_diff : 1e-2;
    const double kMeanDiffThresh = sizeof(Element) == 1 ? options.strict_mean_diff : 1e-3;

    // Strict verification is also a full-array comparison. Cache the reference
    // arrays so the subsequent 2-SM-vs-1-SM report can print a three-way value at
    // exactly the direct-comparison worst coordinate.
    std::vector<Element> actual_dq, actual_dk, actual_dv;
    if (!copy_device_vector(block_dQ, actual_dq) ||
        !copy_device_vector(block_dK, actual_dk) ||
        !copy_device_vector(block_dV, actual_dv) ||
        !copy_device_vector(block_ref_dQ, host_ref_dQ) ||
        !copy_device_vector(block_ref_dK, host_ref_dK) ||
        !copy_device_vector(block_ref_dV, host_ref_dV)) {
      return false;
    }
    GradientDiffStats dq = gradient_diff(actual_dq, host_ref_dQ);
    GradientDiffStats dk = gradient_diff(actual_dk, host_ref_dK);
    GradientDiffStats dv = gradient_diff(actual_dv, host_ref_dV);
    print_gradient_hashes(
        "candidate",
        {raw_payload_hash(actual_dq), raw_payload_hash(actual_dk), raw_payload_hash(actual_dv)});
    print_gradient_hashes(
        "reference",
        {raw_payload_hash(host_ref_dQ), raw_payload_hash(host_ref_dK),
         raw_payload_hash(host_ref_dV)});
    print_gradient_stats(
        "dQ candidate-vs-reference", GradientKind::kDQ, dq, options, Q, D);
    print_gradient_stats(
        "dK candidate-vs-reference", GradientKind::kDK, dk, options, K, D);
    print_gradient_stats(
        "dV candidate-vs-reference", GradientKind::kDV, dv, options, K, D_VO);
    auto passes_reference = [&](GradientDiffStats const& stats) {
      return stats.finite && stats.max_abs <= kMaxDiffThresh &&
             stats.mean_abs <= kMeanDiffThresh;
    };
    bool passed_dQ = passes_reference(dq);
    bool passed_dK = passes_reference(dk);
    bool passed_dV = passes_reference(dv);

#ifdef BWD_2SM_DEBUG
    // ============== dV large-shape locator (head0/batch0) ==============
    // dV layout (K, D_VO, HB), stride_dV = (D_VO, 1, ...). head0/batch0 offset 0.
    // These scans cover ALL K rows of head0/batch0 to find where the max diff lives
    // (the per-element dumps below only look at k=0..8 / k=128..136).
    {
      std::vector<Element> dv_a(block_dV.size()), dvr_a(block_ref_dV.size());
      cudaMemcpy(dv_a.data(),  block_dV.get(),     dv_a.size()*sizeof(Element), cudaMemcpyDefault);
      cudaMemcpy(dvr_a.data(), block_ref_dV.get(), dvr_a.size()*sizeof(Element), cudaMemcpyDefault);

      // (1) Per-row max abs diff across the full K extent of head0/batch0,
      //     bucketed by 128-row K-block (== MMA tile / cluster pair).
      const int k_block = 128;
      const int n_kblk = K / k_block;
      std::vector<double> blk_maxdiff(n_kblk, 0.0);
      std::vector<int>    blk_worst_k(n_kblk, -1);
      std::vector<int>    blk_worst_dvo(n_kblk, -1);
      double overall_max = 0; int overall_k = -1, overall_dvo = -1;
      for (int k = 0; k < K; ++k) {
        int b = k / k_block;
        for (int dvo = 0; dvo < D_VO; ++dvo) {
          int idx = k*D_VO + dvo;
          double d = std::fabs((double)dv_a[idx] - (double)dvr_a[idx]);
          if (d > blk_maxdiff[b]) { blk_maxdiff[b]=d; blk_worst_k[b]=k; blk_worst_dvo[b]=dvo; }
          if (d > overall_max)    { overall_max=d; overall_k=k; overall_dvo=dvo; }
        }
      }
      std::cerr << "[dVSCAN] head0/batch0 overall max abs diff = " << overall_max
                << " at k=" << overall_k << " dvo=" << overall_dvo << "\n";
      std::cerr << "[dVSCAN] per-K-block(128) maxdiff head0/batch0:\n";
      for (int b = 0; b < n_kblk; ++b) {
        std::cerr << "  K-blk " << b << " (k=" << b*k_block << ".." << (b+1)*k_block-1
                  << "): maxdiff=" << blk_maxdiff[b]
                  << " at k=" << blk_worst_k[b] << " dvo=" << blk_worst_dvo[b] << "\n";
      }

      // (2) For the worst K-block, show max-diff per dvo-column to reveal any N-axis
      //     (CTA0 rank0 vs CTA1 rank1, split at dvo=64) pattern.
      {
        int worst_b = 0;
        for (int b = 1; b < n_kblk; ++b) if (blk_maxdiff[b] > blk_maxdiff[worst_b]) worst_b = b;
        int kbase = worst_b * k_block;
        std::cerr << "[dVSCAN] worst K-blk " << worst_b << " (k=" << kbase
                  << ".." << kbase+k_block-1 << ") per-dvo maxdiff + a sample k row:\n";
        std::vector<double> col_max(D_VO, 0.0);
        for (int kk = 0; kk < k_block; ++kk)
          for (int dvo = 0; dvo < D_VO; ++dvo) {
            int idx = (kbase+kk)*D_VO + dvo;
            double d = std::fabs((double)dv_a[idx] - (double)dvr_a[idx]);
            if (d > col_max[dvo]) col_max[dvo] = d;
          }
        for (int dvo = 0; dvo < D_VO; dvo += 8) {
          std::cerr << "  dvo=" << dvo << " colmax:";
          for (int j = 0; j < 8; ++j) std::cerr << " " << col_max[dvo+j];
          std::cerr << "\n";
        }
        // a concrete worst row of this block, full D_VO
        int wk = blk_worst_k[worst_b];
        std::cerr << "  sample k=" << wk << " full D_VO kern/ref:\n";
        for (int dvo = 0; dvo < D_VO; dvo += 8) {
          std::cerr << "    dvo=" << dvo << ":";
          for (int j = 0; j < 8; ++j) {
            int idx = wk*D_VO + dvo + j;
            std::cerr << " " << (double)dv_a[idx] << "/" << (double)dvr_a[idx];
          }
          std::cerr << "\n";
        }
      }
    }
    // [dV element dump] dV layout (K, D_VO, HB), stride_dV = (D_VO, 1, ((0, D_VO*K), ...)).
    // Linear index for head0 batch0 = k*D_VO + dvo. Print K-block 0 and K-block 1
    // (8 rows x 8 cols) to see whether the error follows the M-split boundary.
    {
      std::vector<Element> dv(block_dV.size()), dvr(block_ref_dV.size());
      cudaMemcpy(dv.data(),  block_dV.get(),     dv.size()*sizeof(Element), cudaMemcpyDefault);
      cudaMemcpy(dvr.data(), block_ref_dV.get(), dvr.size()*sizeof(Element), cudaMemcpyDefault);
      auto dump_block = [&](int kbase, const char* tag){
        std::cerr << "[dVDUMP] " << tag << " (k=" << kbase << ".." << kbase+8
                  << ", dvo=0..7) kern vs ref:\n";
        for (int kk = 0; kk < 8; ++kk) {
          std::cerr << "  k=" << (kbase+kk) << ":";
          for (int dvo = 0; dvo < 8; ++dvo) {
            int idx = (kbase+kk)*D_VO + dvo;
            std::cerr << " " << (double)dv[idx] << "/" << (double)dvr[idx];
          }
          std::cerr << "\n";
        }
      };
      dump_block(0, "K-block0");
      dump_block(128, "K-block1");
      // [dV accumulation ratio] mean |kern|/|ref| over K-block0 head0/batch0.
      // If dV only accumulates a fraction of Q-tiles, ratio ~ (#tiles kept)/(total).
      {
        double sum_k = 0, sum_r = 0;
        for (int k = 0; k < 128; ++k)
          for (int dvo = 0; dvo < D_VO; ++dvo) {
            int idx = k*D_VO + dvo;
            sum_k += std::fabs((double)dv[idx]);
            sum_r += std::fabs((double)dvr[idx]);
          }
        std::cerr << "[dVRATIO] K-block0 mean|kern|/mean|ref| = " << (sum_k/sum_r)
                  << "  (Q-tiles this shape = " << (Q/128) << ")\n";
      }
      // [dV N-axis pattern] full D_VO row for k=0 (head0/batch0) to see N-split boundary.
      {
        int k = 0;
        std::cerr << "[dVROW] k=" << k << " full D_VO (kern vs ref), N-split boundary at dvo=64:\n";
        for (int dvo = 0; dvo < D_VO; dvo += 8) {
          std::cerr << "  dvo=" << dvo << ":";
          for (int j = 0; j < 8; ++j) {
            int idx = k*D_VO + dvo + j;
            std::cerr << " " << (double)dv[idx] << "/" << (double)dvr[idx];
          }
          std::cerr << "\n";
        }
      }
      // [dK element dump] dK layout (K, D, HB). Compare K-block0 vs K-block1 to see
      // if the CTA1 M-split error is specific to dV or also hits dK (= dS^T @ Q).
      {
        std::vector<Element> dk(block_dK.size()), dkr(block_ref_dK.size());
        cudaMemcpy(dk.data(),  block_dK.get(),     dk.size()*sizeof(Element), cudaMemcpyDefault);
        cudaMemcpy(dkr.data(), block_ref_dK.get(), dkr.size()*sizeof(Element), cudaMemcpyDefault);
        auto dump_dk = [&](int kbase, const char* tag){
          std::cerr << "[dKDUMP] " << tag << " (k=" << kbase << ".." << kbase+8
                    << ", d=0..7) kern vs ref:\n";
          for (int kk = 0; kk < 8; ++kk) {
            std::cerr << "  k=" << (kbase+kk) << ":";
            for (int d = 0; d < 8; ++d) {
              int idx = (kbase+kk)*D + d;
              std::cerr << " " << (double)dk[idx] << "/" << (double)dkr[idx];
            }
            std::cerr << "\n";
          }
        };
        dump_dk(0, "K-block0");
        dump_dk(128, "K-block1");
      }
    }
#endif

    return passed_dQ && passed_dK && passed_dV;
  }

  auto initialize_problem_shape(Options const& options) {
    int h_r = options.h / options.h_k;
    assert(options.h % options.h_k == 0);

    if constexpr (kIsVarlen) {
      int num_batches = options.b;

      // generate Q as --b times
      //    gaussian (--Q, --Q / 2) sampled positive
      //    track cumulative 
      std::mt19937 rng(0x202305151552ull);
      std::normal_distribution<double> dist_q(options.q, options.q / 2);
      std::normal_distribution<double> dist_kv(options.k, options.k / 2);

      auto generate_positive_int = [](auto& dist, auto& gen) {
        // "0" is a valid value we test here
        return std::max(0, static_cast<int>(dist(gen)));
      };

      std::vector<int> cumulative_seqlen_q = {0};
      std::vector<int> cumulative_seqlen_kv = {0};

      int total_seqlen_q = 0;
      int total_seqlen_kv = 0;
      int max_seqlen_q = 0;
      int max_seqlen_kv = 0;

      const bool kVarlenSame = false;
      for (int i = 0; i < num_batches; i++) {
        int seqlen_q = (! options.varlen_q.empty()) ? options.varlen_q.at(i) : 
                kVarlenSame ? options.q :
                generate_positive_int(dist_q, rng);
        int seqlen_kv = (! options.varlen_k.empty()) ? options.varlen_k.at(i) :
                kVarlenSame ? options.k :
                generate_positive_int(dist_kv, rng);

        total_seqlen_q += seqlen_q;
        total_seqlen_kv += seqlen_kv;

        max_seqlen_q = std::max(max_seqlen_q, seqlen_q);
        max_seqlen_kv = std::max(max_seqlen_kv, seqlen_kv);

        cumulative_seqlen_q.push_back(cumulative_seqlen_q.back() + seqlen_q);
        cumulative_seqlen_kv.push_back(cumulative_seqlen_kv.back() + seqlen_kv);
      }

      block_cumulative_seqlen_q.reset(cumulative_seqlen_q.size());
      block_cumulative_seqlen_q.copy_from_host(cumulative_seqlen_q.data(), cumulative_seqlen_q.size());
      block_cumulative_seqlen_kv.reset(cumulative_seqlen_kv.size());
      block_cumulative_seqlen_kv.copy_from_host(cumulative_seqlen_kv.data(), cumulative_seqlen_kv.size());

      ProblemShape problem_shape{
          {max_seqlen_q, block_cumulative_seqlen_q.get(), total_seqlen_q},
          {max_seqlen_kv, block_cumulative_seqlen_kv.get(), total_seqlen_kv},
          options.d, options.d_vo, {{h_r, options.h_k}, options.b}
      };
      auto tensor_shape = make_shape(total_seqlen_q, total_seqlen_kv, options.d, options.d_vo, make_shape(make_shape(h_r, options.h_k), 1));

      return cute::make_tuple(problem_shape, tensor_shape);
    }
    else {
      ProblemShape problem_shape{options.q, options.k, options.d, options.d_vo, {{h_r, options.h_k}, options.b}};
      return cute::make_tuple(problem_shape, problem_shape);
    }
  }

  /// Initialize operands to be used in the GEMM and reference GEMM
  ProblemShape initialize(Options const& options) {
    auto [problem_shape, tensor_shape] = initialize_problem_shape(options);
    auto [Q, K, D, D_VO, HB] = tensor_shape;
    auto [H, B] = HB;
    auto [H_R, H_K] = H;
    D = cutlass::round_up(D, 8);  // Alignment

    // for varlen, Q == total_Q, K == total_K, B = 1
    // but in problem_shape, they've got to be max_Q/max_K, and B = B

    auto shape_Q = make_shape(Q, D, HB);
    auto shape_K = make_shape(K, D, HB);
    auto shape_V = make_shape(K, D_VO, HB);
    auto shape_O = make_shape(Q, D_VO, HB);
    auto shape_LSE = make_shape(Q, HB);

    stride_Q = make_stride(D, _1{}, make_stride(make_stride(D*Q, D*Q*H_R), B == 1 ? 0 : D*Q*H_R*H_K));
    stride_K = make_stride(D, _1{}, make_stride(make_stride(_0{}, D*K), B == 1 ? 0 : D*K*H_K));
    stride_V = make_stride(D_VO, _1{}, make_stride(make_stride(_0{},D_VO*K), B == 1 ? 0 : D_VO*K*H_K));
    stride_O = make_stride(D_VO, _1{}, make_stride(make_stride(D_VO*Q, D_VO*Q*H_R), B == 1 ? 0 : D_VO*Q*H_R*H_K));
    stride_LSE = make_stride(_1{}, make_stride(make_stride(Q, Q*H_R), B == 1 ? 0 : Q*H_R*H_K));

    stride_dQ = stride_Q;
    stride_dK = stride_K;
    stride_dV = stride_V;
    stride_dO = stride_O;

    auto lsize = [](auto shape) {
      return size(make_shape(1ull, shape));
    };

    auto size_K = lsize(K * D * H_K * B);
    auto size_V = lsize(K * D_VO * H_K * B);

    block_Q.reset(lsize(shape_Q));
    block_K.reset(size_K);
    block_V.reset(size_V);
    block_O.reset(lsize(shape_O));
    block_LSE.reset(lsize(shape_LSE));

    block_dQ.reset(lsize(shape_Q));
    block_dK.reset(size_K);
    block_dV.reset(size_V);
    block_dO.reset(lsize(shape_O));

    block_ref_dQ.reset(lsize(shape_Q));
    block_ref_dK.reset(size_K);
    block_ref_dV.reset(size_V);

    initialize_block(block_Q, seed + 2023, options.init_style_q);
    initialize_block(block_K, seed + 2022, options.init_style_k);
    initialize_block(block_V, seed + 2021, options.init_style_v);
    initialize_block(block_dO, seed + 2020, options.init_style_do);

    initialize_block(block_dQ, seed + 2030, InitStyle::kOne);
    initialize_block(block_dK, seed + 2031, InitStyle::kOne);
    initialize_block(block_dV, seed + 2032, InitStyle::kOne);
    initialize_block(block_ref_dQ, seed + 2033);
    initialize_block(block_ref_dK, seed + 2034);
    initialize_block(block_ref_dV, seed + 2035);

    Tensor mQ = make_tensor(make_gmem_ptr(block_Q.get()),
      select<0,2,4>(problem_shape),
      stride_Q);

    Tensor mK = make_tensor(make_gmem_ptr(block_K.get()),
      select<1,2,4>(problem_shape),
      stride_K);

    Tensor mV = make_tensor(make_gmem_ptr(block_V.get()),
      select<1,3,4>(problem_shape),
      stride_V);

    Tensor mO = make_tensor(make_gmem_ptr(block_O.get()),
      select<0,3,4>(problem_shape),
      stride_O);

    Tensor mLSE = make_tensor(make_gmem_ptr(block_LSE.get()),
      select<0,4>(problem_shape),
      stride_LSE);

    if (not options.skip_reference) {
      fmha_reference(problem_shape, mQ, mK, mV, mO, mLSE, ActiveMask{});
    }

    return problem_shape;
  }

  ExampleResult run(const Options& options, const cutlass::KernelHardwareInfo& hw_info) {
    auto problem_shape = initialize(options);

    bool const strict_mode = !options.dump_gradients.empty() ||
                             !options.compare_gradients.empty() ||
                             options.stability_runs > 1;
    strict_input_hash_valid = false;
    if (strict_mode) {
      strict_input_hash_valid = compute_input_hash(options, strict_input_hash);
      if (!strict_input_hash_valid) {
        std::cerr << "Failed to capture the pre-backward input fingerprint\n";
        return {};
      }
      std::cerr << "[STRICT] pre-backward input_hash=0x" << std::hex
                << strict_input_hash << std::dec << "\n";
    }

    ElementAccumulator softmax_scale = 1.0f / sqrtf(options.d);

    ExampleResult example_result;

#ifdef BWD_2SM
    using Operation = cutlass::fmha::device::Sm100FmhaBwd2Sm<ProblemShape, Element, ElementAccumulator, TileShape, kIsMla, ActiveMask>;
#else
    using Operation = cutlass::fmha::device::Sm100FmhaBwd<ProblemShape, Element, ElementAccumulator, TileShape, kIsMla, ActiveMask>;
#endif

    typename Operation::Arguments arguments{
      problem_shape,
      block_Q.get(), stride_Q,
      block_K.get(), stride_K,
      block_V.get(), stride_V,
      block_O.get(), stride_O,
      block_LSE.get(), stride_LSE,
      block_dO.get(), stride_dO,
      block_dQ.get(), stride_dQ,
      block_dK.get(), stride_dK,
      block_dV.get(), stride_dV,
      softmax_scale,
      hw_info
    };

    Operation op;

    example_result.smem_size = Operation::Kernel::SharedStorageSize;

    size_t workspace_size = 0;
    workspace_size = Operation::get_workspace_size(arguments);
    DeviceAllocation<uint8_t> workspace(workspace_size);

    cutlass::Status status = cutlass::Status::kSuccess;
    status = op.can_implement(arguments);
    if (status != cutlass::Status::kSuccess) {
      std::cerr << "This kernel is not supported. Last CUDA error is: "
                << cudaGetErrorString(cudaGetLastError()) << std::endl;
      return example_result;
    }

    status = op.initialize(arguments, workspace.get());
    if (status != cutlass::Status::kSuccess) {
      std::cerr << "Failed to initialize the CUTLASS kernel. Last CUDA error is: "
                << cudaGetErrorString(cudaGetLastError()) << std::endl;
      return example_result;
    }

    // Run
    status = op.run();
    if (status != cutlass::Status::kSuccess) {
      std::cerr << "Failed to launch the CUTLASS kernel. Last CUDA error is: "
                << cudaGetErrorString(cudaGetLastError()) << std::endl;
      return example_result;
    }

    cudaError_t result = cudaDeviceSynchronize();
    if (result != cudaSuccess) {
      std::cerr << "Error running the CUTLASS kernel. Last CUDA error is: "
                << cudaGetErrorString(result) << std::endl;
      return example_result;
    }

#ifdef BWD_2SM_DEBUG
    // [LSE timing] block_LSE[0..3] right AFTER the CUTLASS bwd kernel (before any
    // re-verify). Tells us whether the kernel corrupted/wrote LSE.
    {
      ElementAccumulator tmp[4];
      cudaMemcpy(tmp, block_LSE.get(), 4*sizeof(ElementAccumulator), cudaMemcpyDefault);
      std::cerr << "[LSETIMING] block_LSE[0..3] post-bwd-kernel = "
                << tmp[0] << " " << tmp[1] << " " << tmp[2] << " " << tmp[3] << "\n";
    }
#endif

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

    if (options.iterations > 0) {
      runtime_ms /= static_cast<float>(options.iterations);
    }
    else {
      runtime_ms = 0.0f;
    }

    double flops = 2.0 * (std::is_same_v<ActiveMask, CausalForBackwardMask<false>> || std::is_same_v<ActiveMask, CausalForBackwardMask<true>> ? 0.5 : 1.0);
    flops *= static_cast<double>(get<0>(problem_shape));
    flops *= static_cast<double>(get<1>(problem_shape));
    flops *= (3 * static_cast<double>(get<2>(problem_shape)) + 2 * static_cast<double>(get<3>(problem_shape)));
    flops *= static_cast<double>(get<4,0,0>(problem_shape));
    flops *= static_cast<double>(get<4,0,1>(problem_shape));
    flops *= static_cast<double>(get<4,1>(problem_shape));
    double tflops_s = runtime_ms > 0.0f
        ? flops * 1e-12 /*tera*/ / (runtime_ms * 1e-3 /*ms*/)
        : 0.0;
    example_result.tflops_tc_s = tflops_s;
    example_result.runtime_ms = runtime_ms;

    result = cudaDeviceSynchronize();
    if (result != cudaSuccess) {
      std::cerr << "Error running the CUTLASS kernel. Last CUDA error is: "
                << cudaGetErrorString(result) << std::endl;
      return example_result;
    }

    // Timed launches must never be the source of correctness data. Stability runs
    // are clean launches whose complete raw dQ/dK/dV payloads are compared with
    // run 1. The expensive high-precision reference is launched only once below,
    // after all clean runs have completed.
    bool stability_ok = true;
    if (options.verify || strict_mode) {
      HostGradients stability_baseline;
      HostGradients stability_current;
      GradientHashes stability_baseline_hashes{};
      for (int stability_run = 0; stability_run < options.stability_runs; ++stability_run) {
        status = op.run();
        if (status != cutlass::Status::kSuccess) {
          std::cerr << "Failed to launch clean validation run " << (stability_run + 1)
                    << ". Last CUDA error is: "
                    << cudaGetErrorString(cudaGetLastError()) << std::endl;
          return example_result;
        }
        result = cudaDeviceSynchronize();
        if (result != cudaSuccess) {
          std::cerr << "Clean validation run " << (stability_run + 1)
                    << " failed. Last CUDA error is: "
                    << cudaGetErrorString(result) << std::endl;
          return example_result;
        }

        if (options.stability_runs > 1) {
          HostGradients& captured = stability_run == 0
              ? stability_baseline : stability_current;
          if (!capture_gradients(captured)) {
            return example_result;
          }
          GradientHashes const hashes = gradient_hashes(captured);
          std::string const role =
              "stability-run-" + std::to_string(stability_run + 1);
          print_gradient_hashes(role.c_str(), hashes);

          if (stability_run == 0) {
            stability_baseline_hashes = hashes;
          }
          else {
            std::string const dq_tag =
                "dQ " + role + "-vs-run-1";
            std::string const dk_tag =
                "dK " + role + "-vs-run-1";
            std::string const dv_tag =
                "dV " + role + "-vs-run-1";
            bool const stable_dq = check_stability_tensor(
                dq_tag.c_str(), GradientKind::kDQ,
                stability_current.dq, stability_baseline.dq,
                hashes.dq, stability_baseline_hashes.dq,
                options, options.q, options.d);
            bool const stable_dk = check_stability_tensor(
                dk_tag.c_str(), GradientKind::kDK,
                stability_current.dk, stability_baseline.dk,
                hashes.dk, stability_baseline_hashes.dk,
                options, options.k, options.d);
            bool const stable_dv = check_stability_tensor(
                dv_tag.c_str(), GradientKind::kDV,
                stability_current.dv, stability_baseline.dv,
                hashes.dv, stability_baseline_hashes.dv,
                options, options.k, options.d_vo);
            stability_ok = stability_ok && stable_dq && stable_dk && stable_dv;
          }
        }
      }
      if (options.stability_runs > 1) {
        std::cerr << "[STABILITY] runs=" << options.stability_runs << " "
                  << (stability_ok ? "PASSED" : "FAILED") << "\n";
      }
    }

    // Detect input clobbering independently from gradient correctness. Preserve
    // the pre-launch fingerprint for baseline matching, and do not suppress the
    // direct dQ/dK/dV report if integrity or reference validation fails.
    bool input_integrity_ok = true;
    if (strict_mode) {
      uint64_t post_input_hash = 0;
      input_integrity_ok = compute_input_hash(options, post_input_hash) &&
                           post_input_hash == strict_input_hash;
      std::cerr << "[STRICT] post-backward input_hash=0x" << std::hex
                << post_input_hash << std::dec << " integrity="
                << (input_integrity_ok ? "PASSED" : "FAILED") << "\n";
    }

    bool ref_ok = true;
    if (options.verify || strict_mode) {
      ref_ok = verify(problem_shape, options);
    }

    bool snapshot_ok = true;
    if (!options.dump_gradients.empty()) {
      // A 1-SM baseline is only useful if it has independently passed the strict
      // high-precision reference gate.
      snapshot_ok = ref_ok && write_gradient_snapshot(options);
    }

    bool direct_ok = true;
    if (!options.compare_gradients.empty()) {
      direct_ok = compare_gradient_snapshot(options);
    }

    bool passed = input_integrity_ok && stability_ok && ref_ok && snapshot_ok && direct_ok;
    example_result.verified = passed && (options.verify || strict_mode);
    
    if (!passed) {
      std::cerr << "Strict gradient validation failed" << std::endl;
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

struct KernelCoop {};

///////////////////////////////////////////////////////////////////////////////////////////////////

template<class Fn>
auto dispatch_bool(bool value, Fn fn) {
  if (value) {
    return fn(std::true_type{});
  }
  else {
    return fn(std::false_type{});
  }
}

//////////////////////////////////////////////////////////////////////////////////////////////////

template<class Mask>
void run_bwd_64(Mask fusion, Options const & options, cutlass::KernelHardwareInfo const& hw_info) {
  auto run = [&](auto shape, auto kernel, const char* name, auto... kernel_options) {
    dispatch_bool(options.varlen, [&](auto is_varlen) {
      BwdRunner<decltype(is_varlen)::value, false,decltype(shape), decltype(kernel), Mask, decltype(kernel_options)...> runner;
      auto result = runner.run(options, hw_info);
      print_result(name, result, options.verbose);
    });
  };

  using HeadDim = _64;

  run(Shape<_128, _128, HeadDim, HeadDim>{}, KernelCoop{}, "tma");
}

///////////////////////////////////////////////////////////////////////////////////////////////////

template<class Mask>
void run_bwd_128(Mask fusion, Options const & options, cutlass::KernelHardwareInfo const& hw_info) {
  auto run = [&](auto shape, auto kernel, const char* name, auto... kernel_options) {
    dispatch_bool(options.varlen, [&](auto is_varlen) {
      BwdRunner<decltype(is_varlen)::value, false, decltype(shape), decltype(kernel), Mask, decltype(kernel_options)...> runner;
      auto result = runner.run(options, hw_info);
      print_result(name, result, options.verbose);
    });
  };

  using HeadDim = _128;

  run(Shape<_128, _128, HeadDim, HeadDim>{}, KernelCoop{}, "tma");
}

template<class Mask>
void run_bwd_mla_192(Mask fusion, Options const & options, cutlass::KernelHardwareInfo const& hw_info) {
  auto run = [&](auto shape, auto kernel, const char* name, auto... kernel_options) {
    dispatch_bool(options.varlen, [&](auto is_varlen) {
      BwdRunner<decltype(is_varlen)::value, true, decltype(shape), decltype(kernel), Mask, decltype(kernel_options)...> runner;
      auto result = runner.run(options, hw_info);
      print_result(name, result, options.verbose);
    });
  };

  using HeadDim = _192;

  run(Shape<_64, _128, HeadDim, _128>{}, KernelCoop{}, "tma");
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
  }  
  //
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

  std::cout << "###### B " << options.b << " H " << options.h << " H_K " << options.h_k << " Q " << options.q << " K " << options.k << " D " << options.d << " D_VO " << options.d_vo << " ";
  std::cout << "Backward" << " " << (options.causal ? "Causal" : "Full") << " ";
  std::cout << "#SM " << hw_info.sm_count << std::endl;

  auto with_causal = [&](auto fn) {
    if (options.causal) {
      fn(CausalForBackwardMask{});
    }
    else if (options.residual) {
      fn(ResidualMaskForBackward{});
    }
    else {
      fn(NoMask{});
    }
  };

  with_causal([&](auto fusion) {
    if (options.d <= 64 && options.d_vo == options.d) {
      run_bwd_64(fusion, options, hw_info);
    }
    else if (options.d <= 128 && options.d_vo == options.d) {
      run_bwd_128(fusion, options, hw_info);
    }
    else if (options.d == 192 && options.d_vo == 128) {
      run_bwd_mla_192(fusion, options, hw_info);
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
    int result = main_single(argc, args);
    if (result != 0) {
      main_result = result;
    }
  }

  return main_result;
}

/////////////////////////////////////////////////////////////////////////////////////////////////
