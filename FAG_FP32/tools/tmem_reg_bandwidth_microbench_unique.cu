// Standalone B200 TMEM -> register microbenchmark.
// It does not call the attention kernel or any FMHA pipeline.
#include <cuda_runtime.h>

#include <cute/arch/copy_sm100.hpp>
#include <cute/arch/tmem_allocator_sm100.hpp>
#include <cutlass/arch/barrier.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace {

constexpr int kAtomBytes = 32 * 16 * 4;  // .32x32b.x16.b32 = 2 KiB / warp instruction
constexpr int kLoadsPerTilePerWarp = 4;   // 64 FP32 values / thread in the FMHA P path
constexpr int kTrials = 15;

struct Result {
  unsigned long long cycles[32];
  unsigned int sink[32];
};

inline void check(cudaError_t status, const char* what) {
  if (status != cudaSuccess) {
    std::fprintf(stderr, "%s: %s\n", what, cudaGetErrorString(status));
    std::exit(EXIT_FAILURE);
  }
}

// Keep every destination live through the caller.  In particular, do not fold
// a result into `sink` inside this helper: that would force each individual
// load to complete before the next one can be issued.
__device__ __forceinline__ void load_32x32b_x16(uint32_t taddr, uint32_t (&r)[16]) {
  cute::SM100::TMEM::LOAD::SM100_TMEM_LOAD_32dp32b16x op;
  op.copy(taddr, r[0], r[1], r[2], r[3], r[4], r[5], r[6], r[7],
          r[8], r[9], r[10], r[11], r[12], r[13], r[14], r[15]);
}

__device__ __forceinline__ uint32_t consume_fragment(const uint32_t (&r)[16]) {
  return r[0] ^ r[3] ^ r[7] ^ r[11] ^ r[15];
}

// Return the base address of one non-overlapping 32-lane x 16-column rectangle
// in a 128-lane x 128-column FP32 tile.  Eight warps x four atoms cover the
// tile exactly once:
//   warp % 4       -> lane groups {0, 32, 64, 96}
//   warp / 4       -> 64-column halves {0, 64}
//   atom in [0, 3] -> 16-column quarters within a half
__device__ __forceinline__ uint32_t unique_tile_atom_addr(
    uint32_t base, int warp, int atom) {
  const uint32_t lane = uint32_t(warp & 3) * 32u;
  const uint32_t column = uint32_t(warp >> 2) * 64u + uint32_t(atom) * 16u;
  return base + (lane << 16) + column;
}

// Saturated test: every active warp repeatedly issues the exact x16.b32 atom
// used by the FMHA S -> register path.  UniqueAddresses=true makes eight
// warps cover one real 128x128 FP32 tile (64 KiB) without address overlap.
// UniqueAddresses=false preserves the old broadcast/duplicate-address test as
// a control; its logical-byte rate must not be called unique TMEM bandwidth.
template <int Warps, int Iterations, bool UniqueAddresses>
__global__ void tmem_reg_bandwidth(Result* out) {
  static_assert(Warps >= 1 && Warps <= 32);
  static_assert(!UniqueAddresses || Warps <= 8,
                "the unique 128x128 mapping contains eight warp slices");
  __shared__ uint32_t tmem_base;
  const int warp = threadIdx.x >> 5;
  const int lane = threadIdx.x & 31;

  cute::TMEM::Allocator1Sm allocator;
  if (warp == 0) {
    allocator.allocate(cute::TMEM::Allocator1Sm::Sm100TmemCapacityColumns, &tmem_base);
  }
  __syncthreads();
  if (warp == 0) {
    allocator.release_allocation_lock();
  }
  __syncthreads();

  const uint32_t taddr0 = UniqueAddresses
      ? unique_tile_atom_addr(tmem_base, warp, 0) : tmem_base;
  const uint32_t taddr1 = UniqueAddresses
      ? unique_tile_atom_addr(tmem_base, warp, 1) : tmem_base;
  const uint32_t taddr2 = UniqueAddresses
      ? unique_tile_atom_addr(tmem_base, warp, 2) : tmem_base;
  const uint32_t taddr3 = UniqueAddresses
      ? unique_tile_atom_addr(tmem_base, warp, 3) : tmem_base;
  uint32_t sink = 0;
  uint32_t warm[16];
  // Warm the instruction pipe; allocation/warmup are outside the clock window.
  #pragma unroll
  for (int i = 0; i < 8; ++i) {
    load_32x32b_x16(taddr0, warm);
    sink ^= consume_fragment(warm);
  }
  cutlass::arch::fence_view_async_tmem_load();
  __syncthreads();

  const unsigned long long start = clock64();
  #pragma unroll 1
  for (int iter = 0; iter < Iterations; ++iter) {
    // This is deliberately four independent real loads followed by use of
    // their register results.  It matches the P-path's four x16 loads per
    // compute warp without placing a consumer between consecutive loads.
    uint32_t a0[16], a1[16], a2[16], a3[16];
    load_32x32b_x16(taddr0, a0);
    load_32x32b_x16(taddr1, a1);
    load_32x32b_x16(taddr2, a2);
    load_32x32b_x16(taddr3, a3);
    sink ^= consume_fragment(a0) ^ consume_fragment(a1)
         ^  consume_fragment(a2) ^ consume_fragment(a3);
  }
  cutlass::arch::fence_view_async_tmem_load();
  const unsigned long long stop = clock64();

  if (lane == 0) {
    out->cycles[warp] = stop - start;
    out->sink[warp] = sink;
  }
  __syncthreads();
  if (warp == 0) {
    allocator.free(tmem_base, cute::TMEM::Allocator1Sm::Sm100TmemCapacityColumns);
  }
}

// Dependency-chain test.  r0 controls the next address but is masked to zero,
// so every iteration accesses the same legal allocation.  This prevents the
// compiler/scheduler from treating adjacent loads as independent.
template <int Iterations>
__global__ void tmem_reg_dependent_latency(Result* out) {
  __shared__ uint32_t tmem_base;
  const int lane = threadIdx.x & 31;
  cute::TMEM::Allocator1Sm allocator;
  allocator.allocate(cute::TMEM::Allocator1Sm::Sm100TmemCapacityColumns, &tmem_base);
  __syncwarp();
  allocator.release_allocation_lock();
  __syncwarp();

  uint32_t taddr = tmem_base;
  uint32_t sink = 0;
  uint32_t warm[16];
  #pragma unroll
  for (int i = 0; i < 8; ++i) {
    load_32x32b_x16(taddr, warm);
    sink ^= consume_fragment(warm);
  }
  cutlass::arch::fence_view_async_tmem_load();
  __syncwarp();

  const unsigned long long start = clock64();
  #pragma unroll 1
  for (int iter = 0; iter < Iterations; ++iter) {
    cute::SM100::TMEM::LOAD::SM100_TMEM_LOAD_32dp32b16x op;
    uint32_t r0, r1, r2, r3, r4, r5, r6, r7;
    uint32_t r8, r9, r10, r11, r12, r13, r14, r15;
    op.copy(taddr, r0, r1, r2, r3, r4, r5, r6, r7,
            r8, r9, r10, r11, r12, r13, r14, r15);
    uint32_t zero;
    asm volatile("and.b32 %0, %1, 0;" : "=r"(zero) : "r"(r0));
    taddr = tmem_base + zero;
    sink ^= r3 ^ r7 ^ r11 ^ r15;
  }
  cutlass::arch::fence_view_async_tmem_load();
  const unsigned long long stop = clock64();

  if (lane == 0) {
    out->cycles[0] = stop - start;
    out->sink[0] = sink;
  }
  __syncwarp();
  allocator.free(tmem_base, cute::TMEM::Allocator1Sm::Sm100TmemCapacityColumns);
}

template <int Warps, bool UniqueAddresses>
double run_bandwidth(Result* d_result, Result& h_result) {
  constexpr int kIterations = 4096;
  std::vector<double> samples;
  samples.reserve(kTrials);
  for (int trial = 0; trial < kTrials; ++trial) {
    check(cudaMemset(d_result, 0, sizeof(Result)), "cudaMemset bandwidth result");
    tmem_reg_bandwidth<Warps, kIterations, UniqueAddresses>
        <<<1, Warps * 32>>>(d_result);
    check(cudaGetLastError(), "launch bandwidth kernel");
    check(cudaMemcpy(&h_result, d_result, sizeof(Result), cudaMemcpyDeviceToHost), "copy bandwidth result");
    unsigned long long cycles = 0;
    for (int warp = 0; warp < Warps; ++warp) {
      cycles = std::max(cycles, h_result.cycles[warp]);
    }
    const double bytes = double(Warps) * kIterations * kLoadsPerTilePerWarp * kAtomBytes;
    samples.push_back(bytes / double(cycles));
  }
  std::sort(samples.begin(), samples.end());
  return samples[samples.size() / 2];
}

double run_dependent(Result* d_result, Result& h_result) {
  constexpr int kIterations = 8192;
  std::vector<double> samples;
  samples.reserve(kTrials);
  for (int trial = 0; trial < kTrials; ++trial) {
    check(cudaMemset(d_result, 0, sizeof(Result)), "cudaMemset latency result");
    tmem_reg_dependent_latency<kIterations><<<1, 32>>>(d_result);
    check(cudaGetLastError(), "launch latency kernel");
    check(cudaMemcpy(&h_result, d_result, sizeof(Result), cudaMemcpyDeviceToHost), "copy latency result");
    samples.push_back(double(h_result.cycles[0]) / kIterations);
  }
  std::sort(samples.begin(), samples.end());
  return samples[samples.size() / 2];
}

}  // namespace

int main() {
  cudaDeviceProp prop{};
  check(cudaGetDeviceProperties(&prop, 0), "cudaGetDeviceProperties");
  std::printf("GPU: %s, compute capability %d.%d\n", prop.name, prop.major, prop.minor);
  if (prop.major != 10) {
    std::fprintf(stderr, "This test requires a Blackwell SM100 GPU.\n");
    return EXIT_FAILURE;
  }

  Result* d_result = nullptr;
  Result h_result{};
  check(cudaMalloc(&d_result, sizeof(Result)), "cudaMalloc result");

  // JIT/context warmup before collecting trials.
  tmem_reg_bandwidth<1, 32, true><<<1, 32>>>(d_result);
  check(cudaDeviceSynchronize(), "warmup");

  std::printf("\nTMEM -> REG, tcgen05.ld.sync.aligned.32x32b.x16.b32\n");
  std::printf("logical payload: %d B / warp instruction; 4 instructions / warp / S tile\n", kAtomBytes);
  std::printf("\nNon-overlapping address mode (unique bytes):\n");
  std::printf("warps  median unique B/cycle  cycles per 64KiB-equivalent tile\n");
  for (int warps : {1, 2, 4, 8}) {
    double bw = 0.0;
    if (warps == 1) bw = run_bandwidth<1, true>(d_result, h_result);
    if (warps == 2) bw = run_bandwidth<2, true>(d_result, h_result);
    if (warps == 4) bw = run_bandwidth<4, true>(d_result, h_result);
    if (warps == 8) bw = run_bandwidth<8, true>(d_result, h_result);
    std::printf("%5d  %26.2f  %31.2f\n", warps, bw, 65536.0 / bw);
  }

  std::printf("\nDuplicate-address control (logical bytes, not unique bandwidth):\n");
  std::printf("warps  median logical B/cycle  cycles per 64KiB-equivalent tile\n");
  for (int warps : {1, 2, 4, 8, 16, 32}) {
    double bw = 0.0;
    if (warps == 1) bw = run_bandwidth<1, false>(d_result, h_result);
    if (warps == 2) bw = run_bandwidth<2, false>(d_result, h_result);
    if (warps == 4) bw = run_bandwidth<4, false>(d_result, h_result);
    if (warps == 8) bw = run_bandwidth<8, false>(d_result, h_result);
    if (warps == 16) bw = run_bandwidth<16, false>(d_result, h_result);
    if (warps == 32) bw = run_bandwidth<32, false>(d_result, h_result);
    std::printf("%5d  %26.2f  %31.2f\n", warps, bw, 65536.0 / bw);
  }
  const double dependent_cycles = run_dependent(d_result, h_result);
  std::printf("\ndependent single-warp atom latency: %.2f cycles / 2KiB atom\n", dependent_cycles);
  std::printf("Interpretation: use the 8-warp B/cycle row for the FMHA P-path throughput roofline;\n");
  std::printf("the dependent number is latency, not sustainable bandwidth.\n");

  check(cudaFree(d_result), "cudaFree");
  return EXIT_SUCCESS;
}
