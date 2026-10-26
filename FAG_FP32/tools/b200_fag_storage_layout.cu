#include <cstddef>
#include <cstdio>

#include "collective/fmha_fusion.hpp"
#include "kernel/sm100_fmha_bwd_kernel_tma_2sm_warpspecialized.hpp"

using ProblemShape = cute::tuple<int, int, int, int,
                                 cute::tuple<cute::tuple<int, int>, int>>;
using TileShape = cute::tuple<cute::_128, cute::_128, cute::_128, cute::_128>;
using Kernel = cutlass::fmha::kernel::Sm100FmhaBwdKernelTma2SmWarpSpecialized<
    ProblemShape, cutlass::float_e4m3_t, float, TileShape,
    cutlass::fmha::collective::NoMask>;

#define PRINT_MEMBER(Type, member)                                             \
  std::printf("%-28s offset=%6zu size=%6zu end=%6zu\n", #member,             \
              offsetof(Type, member),                                         \
              sizeof(((Type*)nullptr)->member),                               \
              offsetof(Type, member) + sizeof(((Type*)nullptr)->member))

int main() {
  using T = Kernel::TensorStorage;
  using P = Kernel::PipelineStorage;
  using S = Kernel::SharedStorage;

  std::printf("TensorStorage sizeof=%zu align=%zu\n", sizeof(T), alignof(T));
  PRINT_MEMBER(T, smem_k);
  PRINT_MEMBER(T, smem_k_t);
  PRINT_MEMBER(T, smem_v);
  PRINT_MEMBER(T, smem_q);
  PRINT_MEMBER(T, smem_q_t);
  PRINT_MEMBER(T, smem_do);
  PRINT_MEMBER(T, smem_ds);
  PRINT_MEMBER(T, smem_ds_xchg);
  PRINT_MEMBER(T, smem_do_dv);
  PRINT_MEMBER(T, smem_dq);
  PRINT_MEMBER(T, smem_lse);
  PRINT_MEMBER(T, smem_sum_odo);
  PRINT_MEMBER(T, ds_full);
  PRINT_MEMBER(T, ds_leader);

  std::printf("\nPipelineStorage sizeof=%zu align=%zu\n", sizeof(P),
              alignof(P));
  PRINT_MEMBER(P, load_mma_q);
  PRINT_MEMBER(P, load_mma_qt);
  PRINT_MEMBER(P, load_mma_kt);
  PRINT_MEMBER(P, load_mma_do);
  PRINT_MEMBER(P, load_compute_lse);
  PRINT_MEMBER(P, load_compute_sum_odo);
  PRINT_MEMBER(P, mma_compute_s);
  PRINT_MEMBER(P, mma_compute_dp);
  PRINT_MEMBER(P, mma_reduce_dq);
  PRINT_MEMBER(P, compute_mma_p);
  PRINT_MEMBER(P, compute_mma_ds);
  PRINT_MEMBER(P, mma_compute_dkdv);
  PRINT_MEMBER(P, tmem_alloc_ready);

  std::printf("\nSharedStorage sizeof=%zu align=%zu declared_size=%d\n",
              sizeof(S), alignof(S), Kernel::SharedStorageSize);
  PRINT_MEMBER(S, tensors);
  PRINT_MEMBER(S, pipelines);
  PRINT_MEMBER(S, tmem_base_ptr);
  return 0;
}
