// Standalone B200 microbenchmark for the complete P = softmax(S - LSE) path.
//
// This intentionally mirrors the production FP8 backward kernel's per-thread
// work for one 128x128 P tile across eight compute warps:
//   4 x LDTM.x16 (64 FP32 S values/thread)
//   16 x shared-memory vector loads (64 FP32 LSE values/thread)
//   32 x packed FP32 FMA (scale and LSE adjustment)
//   48 x hardware EX2 + 8 packed two-value software EX2 groups
//   32 x FP32-to-E4M3 pack instructions
//   one 256-thread barrier
//   4 x STTM.x4 (16 packed E4M3 registers/thread)
// It is diagnostic code only and is not linked into the attention kernel.

#include <cuda_runtime.h>

#include <cute/arch/copy_sm100.hpp>
#include <cute/arch/tmem_allocator_sm100.hpp>
#include <cutlass/arch/barrier.h>
#include <cutlass/array.h>
#include <cutlass/float8.h>
#include <cutlass/numeric_conversion.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace {

constexpr int kWarps = 8;
constexpr int kThreads = kWarps * 32;
constexpr int kTrials = 15;
constexpr int kIterations = 512;

enum class Mode : int {
  TransportOnly = 0,
  AllHardwareEx2 = 1,
  Split3To1 = 2,
  AllSoftwareEx2 = 3,
  Split1To1 = 4,
  Split7To1 = 5,
  Split15To1 = 6,
};

struct Result {
  unsigned long long cycles[kWarps];
  unsigned int sink[kWarps];
};

inline void check(cudaError_t status, const char* what) {
  if (status != cudaSuccess) {
    std::fprintf(stderr, "%s: %s\n", what, cudaGetErrorString(status));
    std::exit(EXIT_FAILURE);
  }
}

__device__ __forceinline__ void load_x16(uint32_t addr, uint32_t (&dst)[16]) {
  cute::SM100::TMEM::LOAD::SM100_TMEM_LOAD_32dp32b16x op;
  op.copy(addr, dst[0], dst[1], dst[2], dst[3], dst[4], dst[5], dst[6], dst[7],
          dst[8], dst[9], dst[10], dst[11], dst[12], dst[13], dst[14], dst[15]);
}

__device__ __forceinline__ void store_x4(
    uint32_t addr, uint32_t a, uint32_t b, uint32_t c, uint32_t d) {
  cute::SM100::TMEM::STORE::SM100_TMEM_STORE_32dp32b4x op;
  op.copy(a, b, c, d, addr);
}

__device__ __forceinline__ float hw_ex2(float x) {
  float out;
  asm volatile("ex2.approx.ftz.f32 %0, %1;" : "=f"(out) : "f"(x));
  return out;
}

// Same degree-3 Cody-Waite/minimax implementation used by the production
// FAG_EX2_EMU_STRIDE=4 path.  One call processes two FP32 values.
__device__ __forceinline__ float2 sw_ex2_pair(float x, float y) {
  uint32_t out0;
  uint32_t out1;
  asm volatile(
      "{\n\t"
      ".reg .f32 f1, f2, f3, f4, f5, f6, f7;\n\t"
      ".reg .b64 l1, l2, l3, l4, l5, l6, l7, l8, l9, l10;\n\t"
      ".reg .s32 r1, r2, r3, r4, r5, r6, r7, r8;\n\t"
      "max.ftz.f32 f1, %2, 0fC2FE0000;\n\t"
      "max.ftz.f32 f2, %3, 0fC2FE0000;\n\t"
      "mov.b64 l1, {f1, f2};\n\t"
      "mov.f32 f3, 0f4B400000;\n\t"
      "mov.b64 l2, {f3, f3};\n\t"
      "add.rm.ftz.f32x2 l7, l1, l2;\n\t"
      "sub.rn.ftz.f32x2 l8, l7, l2;\n\t"
      "sub.rn.ftz.f32x2 l9, l1, l8;\n\t"
      "mov.f32 f7, 0f3D9DF09D;\n\t"
      "mov.b64 l6, {f7, f7};\n\t"
      "mov.f32 f6, 0f3E6906A4;\n\t"
      "mov.b64 l5, {f6, f6};\n\t"
      "mov.f32 f5, 0f3F31F519;\n\t"
      "mov.b64 l4, {f5, f5};\n\t"
      "mov.f32 f4, 0f3F800000;\n\t"
      "mov.b64 l3, {f4, f4};\n\t"
      "fma.rn.ftz.f32x2 l10, l9, l6, l5;\n\t"
      "fma.rn.ftz.f32x2 l10, l10, l9, l4;\n\t"
      "fma.rn.ftz.f32x2 l10, l10, l9, l3;\n\t"
      "mov.b64 {r1, r2}, l7;\n\t"
      "mov.b64 {r3, r4}, l10;\n\t"
      "shl.b32 r5, r1, 23;\n\t"
      "add.s32 r7, r5, r3;\n\t"
      "shl.b32 r6, r2, 23;\n\t"
      "add.s32 r8, r6, r4;\n\t"
      "mov.b32 %0, r7;\n\t"
      "mov.b32 %1, r8;\n\t"
      "}\n"
      : "=r"(out0), "=r"(out1)
      : "f"(x), "f"(y));
  return make_float2(__uint_as_float(out0), __uint_as_float(out1));
}

__device__ __forceinline__ void load_lse4(
    uint32_t shared_addr, float& a, float& b, float& c, float& d) {
  uint32_t ra, rb, rc, rd;
  asm volatile("ld.shared.v4.b32 {%0, %1, %2, %3}, [%4];"
               : "=r"(ra), "=r"(rb), "=r"(rc), "=r"(rd)
               : "r"(shared_addr));
  a = __uint_as_float(ra);
  b = __uint_as_float(rb);
  c = __uint_as_float(rc);
  d = __uint_as_float(rd);
}

__device__ __forceinline__ void fma_pair(
    float& x, float& y, float scale, float lse_x, float lse_y) {
  union Pack {
    uint64_t u64;
    float2 f2;
  } a, b, c, out;
  a.f2 = make_float2(scale, scale);
  b.f2 = make_float2(x, y);
  c.f2 = make_float2(lse_x, lse_y);
  asm volatile("fma.rn.f32x2 %0, %1, %2, %3;"
               : "=l"(out.u64)
               : "l"(a.u64), "l"(b.u64), "l"(c.u64));
  x = out.f2.x;
  y = out.f2.y;
}

__device__ __forceinline__ uint32_t quantize4(
    float a, float b, float c, float d) {
  cutlass::Array<float, 4> input;
  input[0] = a;
  input[1] = b;
  input[2] = c;
  input[3] = d;
  cutlass::NumericArrayConverter<cutlass::float_e4m3_t, float, 4> convert;
  cutlass::Array<cutlass::float_e4m3_t, 4> output = convert(input);
  return *reinterpret_cast<uint32_t const*>(&output);
}

template <Mode TestMode>
__global__ void p_chain(Result* result) {
  __shared__ uint32_t tmem_base;
  __shared__ alignas(16) float lse[128];

  const int warp = threadIdx.x >> 5;
  const int lane = threadIdx.x & 31;

  cute::TMEM::Allocator1Sm allocator;
  if (warp == 0) {
    allocator.allocate(cute::TMEM::Allocator1Sm::Sm100TmemCapacityColumns, &tmem_base);
  }
  for (int i = threadIdx.x; i < 128; i += blockDim.x) {
    lse[i] = -1.0f - float(i & 7) * 0.03125f;
  }
  __syncthreads();
  if (warp == 0) {
    allocator.release_allocation_lock();
  }
  __syncthreads();

  const uint32_t src_addr = tmem_base;
  const uint32_t dst_addr = tmem_base + 0x100;

  // Initialize the source columns with finite FP32 values outside the clock
  // window.  Every warp writes the same bit pattern, so duplicate stores are
  // benign and avoid introducing a separate producer mapping.
  uint32_t init[16];
  #pragma unroll
  for (int i = 0; i < 16; ++i) {
    init[i] = __float_as_uint(-0.25f - 0.015625f * float(i));
  }
  cute::SM100::TMEM::STORE::SM100_TMEM_STORE_32dp32b16x init_store;
  init_store.copy(init[0], init[1], init[2], init[3], init[4], init[5], init[6], init[7],
                  init[8], init[9], init[10], init[11], init[12], init[13], init[14], init[15],
                  src_addr + 0);
  init_store.copy(init[0], init[1], init[2], init[3], init[4], init[5], init[6], init[7],
                  init[8], init[9], init[10], init[11], init[12], init[13], init[14], init[15],
                  src_addr + 16);
  init_store.copy(init[0], init[1], init[2], init[3], init[4], init[5], init[6], init[7],
                  init[8], init[9], init[10], init[11], init[12], init[13], init[14], init[15],
                  src_addr + 32);
  init_store.copy(init[0], init[1], init[2], init[3], init[4], init[5], init[6], init[7],
                  init[8], init[9], init[10], init[11], init[12], init[13], init[14], init[15],
                  src_addr + 48);
  cutlass::arch::fence_view_async_tmem_store();
  __syncthreads();

  // Warm up the instruction and TMEM pipes.
  uint32_t warm[16];
  load_x16(src_addr, warm);
  cutlass::arch::fence_view_async_tmem_load();
  __syncthreads();

  uint32_t sink = 0;
  unsigned long long start = clock64();

  #pragma unroll 1
  for (int iteration = 0; iteration < kIterations; ++iteration) {
    uint32_t raw0[16], raw1[16], raw2[16], raw3[16];
    load_x16(src_addr + 0, raw0);
    load_x16(src_addr + 16, raw1);
    load_x16(src_addr + 32, raw2);
    load_x16(src_addr + 48, raw3);

    uint32_t packed[16];
    if constexpr (TestMode == Mode::TransportOnly) {
      #pragma unroll
      for (int i = 0; i < 4; ++i) {
        packed[i + 0] = raw0[i];
        packed[i + 4] = raw1[i];
        packed[i + 8] = raw2[i];
        packed[i + 12] = raw3[i];
      }
    } else {
      float values[64];
      #pragma unroll
      for (int i = 0; i < 16; ++i) {
        values[i + 0] = __uint_as_float(raw0[i]);
        values[i + 16] = __uint_as_float(raw1[i]);
        values[i + 32] = __uint_as_float(raw2[i]);
        values[i + 48] = __uint_as_float(raw3[i]);
      }

      constexpr float kScaleLog2e = 1.4426950408889634f;
      #pragma unroll
      for (int group = 0; group < 16; ++group) {
        float l0, l1, l2, l3;
        // A fixed multicast address per group matches the highly reused LSE
        // access pattern rather than charging 512 unique bytes per LDS.128.
        uint32_t shared_addr = static_cast<uint32_t>(
            __cvta_generic_to_shared(&lse[group * 4]));
        load_lse4(shared_addr, l0, l1, l2, l3);
        int i = group * 4;
        fma_pair(values[i], values[i + 1], kScaleLog2e, l0, l1);
        fma_pair(values[i + 2], values[i + 3], kScaleLog2e, l2, l3);
      }

      #pragma unroll
      for (int pair = 0; pair < 32; ++pair) {
        int i = pair * 2;
        if constexpr (TestMode != Mode::AllHardwareEx2) {
          constexpr int kEmulationStride =
              TestMode == Mode::AllSoftwareEx2 ? 1 :
              TestMode == Mode::Split1To1 ? 2 :
              TestMode == Mode::Split3To1 ? 4 :
              TestMode == Mode::Split7To1 ? 8 : 16;
          if ((pair % kEmulationStride) == kEmulationStride - 1) {
            float2 out = sw_ex2_pair(values[i], values[i + 1]);
            values[i] = out.x;
            values[i + 1] = out.y;
          } else {
            values[i] = hw_ex2(values[i]);
            values[i + 1] = hw_ex2(values[i + 1]);
          }
        } else {
          values[i] = hw_ex2(values[i]);
          values[i + 1] = hw_ex2(values[i + 1]);
        }
      }

      #pragma unroll
      for (int group = 0; group < 16; ++group) {
        int i = group * 4;
        packed[group] = quantize4(values[i], values[i + 1], values[i + 2], values[i + 3]);
      }
    }

    cutlass::arch::fence_view_async_tmem_load();
    __syncthreads();
    store_x4(dst_addr + 0, packed[0], packed[1], packed[2], packed[3]);
    store_x4(dst_addr + 8, packed[4], packed[5], packed[6], packed[7]);
    store_x4(dst_addr + 16, packed[8], packed[9], packed[10], packed[11]);
    store_x4(dst_addr + 24, packed[12], packed[13], packed[14], packed[15]);
    cutlass::arch::fence_view_async_tmem_store();
    sink ^= packed[(iteration + lane) & 15];
  }

  unsigned long long stop = clock64();
  if (lane == 0) {
    result->cycles[warp] = stop - start;
    result->sink[warp] = sink;
  }
  __syncthreads();
  if (warp == 0) {
    allocator.free(tmem_base, cute::TMEM::Allocator1Sm::Sm100TmemCapacityColumns);
  }
}

template <Mode TestMode>
double run(Result* device_result, Result& host_result) {
  std::vector<double> samples;
  samples.reserve(kTrials);
  for (int trial = 0; trial < kTrials; ++trial) {
    check(cudaMemset(device_result, 0, sizeof(Result)), "clear result");
    p_chain<TestMode><<<1, kThreads>>>(device_result);
    check(cudaGetLastError(), "launch P-chain benchmark");
    check(cudaMemcpy(&host_result, device_result, sizeof(Result), cudaMemcpyDeviceToHost),
          "copy P-chain result");
    unsigned long long max_cycles = 0;
    for (int warp = 0; warp < kWarps; ++warp) {
      max_cycles = std::max(max_cycles, host_result.cycles[warp]);
    }
    samples.push_back(double(max_cycles) / double(kIterations));
  }
  std::sort(samples.begin(), samples.end());
  return samples[samples.size() / 2];
}

}  // namespace

int main() {
  cudaDeviceProp prop{};
  check(cudaGetDeviceProperties(&prop, 0), "cudaGetDeviceProperties");
  if (prop.major != 10) {
    std::fprintf(stderr, "This benchmark requires a Blackwell SM100 GPU.\n");
    return EXIT_FAILURE;
  }

  Result* device_result = nullptr;
  Result host_result{};
  check(cudaMalloc(&device_result, sizeof(Result)), "allocate result");

  p_chain<Mode::TransportOnly><<<1, kThreads>>>(device_result);
  check(cudaDeviceSynchronize(), "warmup");

  double transport = run<Mode::TransportOnly>(device_result, host_result);
  double hardware = run<Mode::AllHardwareEx2>(device_result, host_result);
  double software = run<Mode::AllSoftwareEx2>(device_result, host_result);
  double split1 = run<Mode::Split1To1>(device_result, host_result);
  double split = run<Mode::Split3To1>(device_result, host_result);
  double split7 = run<Mode::Split7To1>(device_result, host_result);
  double split15 = run<Mode::Split15To1>(device_result, host_result);

  std::printf("GPU: %s, %d compute warps, one 128x128 P tile\n", prop.name, kWarps);
  std::printf("transport only (LDTM + barrier + STTM): %.2f cycles/tile\n", transport);
  std::printf("complete P chain, all hardware EX2:       %.2f cycles/tile\n", hardware);
  std::printf("complete P chain, 100%% software EX2:     %.2f cycles/tile\n", software);
  std::printf("complete P chain, 1:1 hardware/software: %.2f cycles/tile\n", split1);
  std::printf("complete P chain, current 3:1 split:     %.2f cycles/tile\n", split);
  std::printf("complete P chain, 7:1 hardware/software: %.2f cycles/tile\n", split7);
  std::printf("complete P chain, 15:1 hardware/software: %.2f cycles/tile\n", split15);
  std::printf("split arithmetic increment over transport: %.2f cycles/tile\n", split - transport);

  check(cudaFree(device_result), "free result");
  return EXIT_SUCCESS;
}
