/***************************************************************************************************
 * MXFP8 Flash Attention Forward Reference Implementation
 *
 * === MXFP8 Scaling Factor Convention =============================================
 *
 *   32 consecutive E4M3 elements share one scaling factor (FP32):
 *       value[i][j] = data_e4m3[i][j] * sf[i][j / 32]
 *
 *   Per head, per batch:
 *     Q: (SQ, D)   → SF_Q: (SQ, D/32)
 *     K: (SK, D)   → SF_K: (SK, D/32)
 *     V: (SK, D)   → SF_V: (SK, D/32)
 *
 *   SF_P: treated as scalar 1.0 — P stays in FP32, no quantization.
 *
 * === CUTLASS SF Layout ===========================================================
 *
 *   D=128 → D/32=4 → K_tiling=1 → CUTLASS layout IS simple row-major.
 *   SF tensors are created with plain strided (row, sf_group, hb) layout.
 *
 * === Usage in verify() ===========================================================
 *
 *   #include "fmha_fwd_reference.hpp"
 *
 *   int hb_q  = H * B;
 *   int hb_kv = H_KV * B;
 *
 *   auto sfQ = make_mxfp8_sf_tensor(buffer.block_SFQ.get(), SQ, D/32, hb_q);
 *   auto sfK = make_mxfp8_sf_tensor(buffer.block_SFK.get(), SK, D/32, hb_kv);
 *   auto sfV = make_mxfp8_sf_tensor(buffer.block_SFV.get(), SK, D/32, hb_kv);
 *
 *   fmha_reference_mxfp8(problem_shape_ref,
 *       mQ, sfQ, mK, sfK, mV, sfV, mO, mLSE, ActiveMask{});
 *
 **************************************************************************************************/
#pragma once

#include "cute/tensor.hpp"
#include "collective/fmha_fusion.hpp"

/////////////////////////////////////////////////////////////////////////////////////////////////

static constexpr int kMXFP8GroupSize_sfp = 32;

/// Create a simple 3-mode SF tensor: (rows, sf_groups, hb).
/// D=128 → D/32=4 → CUTLASS layout ≡ simple row-major.
// template <class T>
// auto make_mxfp8_sf_tensor(T* dev_ptr, int rows, int sf_groups, int hb) {
//     return cute::make_tensor(
//         cute::make_gmem_ptr(dev_ptr),
//         cute::make_layout(
//             cute::make_shape(rows, sf_groups, hb),
//             cute::make_stride(sf_groups, cute::_1{}, sf_groups*rows)));
// }

/////////////////////////////////////////////////////////////////////////////////////////////////
// MXFP8 Reference Kernel
//   SF_Q, SF_K, SF_V are used.  P stays in FP32 (SF_P ≡ 1.0).
/////////////////////////////////////////////////////////////////////////////////////////////////

template<
  class ProblemShapeIn,
  class TensorQ,  class TensorSFQ,
  class TensorK,  class TensorSFK,
  class TensorV,  class TensorSFV,
  class TensorO,  class TensorLSE,
  class Mask
>
void __global__ fmha_reference_mxfp8_kernel_sfp(
    ProblemShapeIn problem_shape_in,
    TensorQ  mQ,   TensorSFQ mSFQ,
    TensorK  mK,   TensorSFK mSFK,
    TensorV  mV,   TensorSFV mSFV,
    TensorO  mO,   TensorLSE mLSE,
    Mask mask) {

  using namespace cute;
  using namespace cutlass::fmha::collective;

  using Element = typename TensorO::value_type;
  using ElementAccumulator = typename TensorLSE::value_type;

  extern __shared__ char mS_mem[];
  ElementAccumulator* mS = reinterpret_cast<ElementAccumulator*>(mS_mem);
  // mS 之后：每组 32 个 K 元素共享一个 scale 值
  int total_k = size<0>(mK);
  int sf_groups = (total_k + kMXFP8GroupSize_sfp - 1) / kMXFP8GroupSize_sfp;
  float* sf_group_data = reinterpret_cast<float*>(mS_mem + total_k * sizeof(ElementAccumulator));
  // sf_group_data 之后：E4M3 类型数组，与 mS 等大小
  cutlass::float_e4m3_t* mS_e4m3 = reinterpret_cast<cutlass::float_e4m3_t*>(
      reinterpret_cast<char*>(sf_group_data) + sf_groups * sizeof(float));
  // mS_e4m3 之后：UE8M0 类型数组，每组一个 scale
  cutlass::float_ue8m0_t* sf_ue8m0 = reinterpret_cast<cutlass::float_ue8m0_t*>(
      reinterpret_cast<char*>(mS_e4m3) + total_k * sizeof(cutlass::float_e4m3_t));

  ElementAccumulator softmax_scale =
      static_cast<ElementAccumulator>(1.0 / sqrt(1.0 * size<1>(mQ)));

  auto id = make_identity_tensor(make_shape(1, 1));

  for (int idx_L = blockIdx.y; idx_L < size<4>(problem_shape_in); idx_L += gridDim.y) {
    for (int idx_Q = blockIdx.x; idx_Q < size<0>(problem_shape_in); idx_Q += gridDim.x) {

      auto coord_L = idx2crd(idx_L, shape<4>(problem_shape_in));
      auto get_coord_in = [&]() {
        if constexpr (rank_v<decltype(get<2>(ProblemShapeIn{}))> == 2) {
          return cute::make_tuple(idx_Q, _0{}, cute::make_tuple(_0{}, _0{}),
                                  cute::make_tuple(_0{}, _0{}), coord_L);
        } else {
          return cute::make_tuple(idx_Q, _0{}, _0{}, _0{}, coord_L);
        }
      };
      auto coord_in = get_coord_in();
      auto [problem_shape, coord] = apply_variable_length(
          problem_shape_in, coord_in, get<4,1>(coord_in));

      int head_qk, head_v;
      if constexpr (rank_v<decltype(get<2>(problem_shape))> == 2) {
        head_qk = size<2,0>(problem_shape) + size<2,1>(problem_shape);
        head_v  = size<2,0>(problem_shape);
      } else {
        head_qk = size<3>(problem_shape);
        head_v  = head_qk;
      }

      if (get<0,0>(coord) >= get<0>(problem_shape)) continue;

      int offset_Q = 0;
      if constexpr (rank<0>(decltype(coord){}) == 2)
        offset_Q = get<0,1>(coord);

      int offset_K = 0;
      if constexpr (rank<1>(decltype(coord){}) == 2)
        offset_K = get<1,1>(coord);

      if (get<1>(problem_shape) == 0) {
        {
          int d_chunk = (head_qk + blockDim.x - 1) / blockDim.x;
          int d_begin = threadIdx.x * d_chunk;
          int d_end   = min(d_begin + d_chunk, head_qk);
          for (int d = d_begin; d < d_end; d++)
            mO(idx_Q + offset_Q, d, coord_L) = Element(0);
        }
        if (threadIdx.x == 0 && mLSE.data() != nullptr)
          mLSE(idx_Q + offset_Q, coord_L) = -INFINITY;
        continue;
      }

      // 初始化 sf_group_data 为 0
      {
        int g_chunk = (sf_groups + blockDim.x - 1) / blockDim.x;
        int g_begin = threadIdx.x * g_chunk;
        int g_end   = min(g_begin + g_chunk, sf_groups);
        for (int g = g_begin; g < g_end; g++)
          sf_group_data[g] = 0.0f;
      }
      __syncthreads();

      // --- Phase 1: Q * K^T  (MXFP8: E4M3 × SF → FP32) ---
      {
        int total_k = size<1>(problem_shape);
        int k_chunk = (total_k + blockDim.x - 1) / blockDim.x;
        int k_begin = threadIdx.x * k_chunk;
        int k_end   = min(k_begin + k_chunk, total_k);
        for (int k = k_begin; k < k_end; k++) {
          ElementAccumulator acc = 0;
          for (int d = 0; d < head_qk; d++) {
            int g = d / kMXFP8GroupSize_sfp;
            ElementAccumulator q = ElementAccumulator(mQ(idx_Q + offset_Q, d, coord_L))
                                 * ElementAccumulator(mSFQ(idx_Q + offset_Q, g, idx_L));
            ElementAccumulator kv = ElementAccumulator(mK(k + offset_K, d, coord_L))
                                  * ElementAccumulator(mSFK(k + offset_K, g, idx_L));
            acc += q * kv;
          }
          auto frag = make_tensor<ElementAccumulator>(Shape<_1, _1>{});
          frag(0) = acc;
          mask.apply_mask(frag,
              make_tensor(id.data() + make_arithmetic_tuple(idx_Q, k), id.layout()),
              problem_shape);
          mS[k] = frag(0);
        }
      }

      __syncthreads();

      //       // ===== DEBUG =====
      // if (blockIdx.x == 0 && blockIdx.y == 0 && threadIdx.x == 0) {
      //   printf("\n[REF] Q[0][d]  first 8:  ");
      //   for (int _d = 0; _d < 16; _d++) printf("%.3f ", (float)ElementAccumulator(mQ(0, _d, coord_L)));
      //   printf("\n[REF] K[0][d]  first 8:  ");
      //   for (int _d = 0; _d < 16; _d++) printf("%.3f ", (float)ElementAccumulator(mK(0, _d, coord_L)));
      //   printf("\n[REF] K[1][d]  first 8:  ");
      //   for (int _d = 0; _d < 16; _d++) printf("%.3f ", (float)ElementAccumulator(mK(1, _d, coord_L)));
      //   printf("\n[REF] SF_Q[0][g] g=0..3: ");
      //   for (int _g = 0; _g < 8; _g++) printf("%.6f ", (float)ElementAccumulator(mSFQ(0, _g, 0)));
      //   printf("\n[REF] SF_K[0][g] g=0..3: ");
      //   for (int _g = 0; _g < 8; _g++) printf("%.6f ", (float)ElementAccumulator(mSFK(0, _g, 0)));
      //   printf("\n[REF] SF_K[1][g] g=0..3: ");
      //   for (int _g = 0; _g < 8; _g++) printf("%.6f ", (float)ElementAccumulator(mSFK(1, _g, 0)));
      //   printf("\n[REF] S[0][k] k=0..15: ");
      //   for (int _k = 0; _k < 32 && _k < size<1>(problem_shape); _k++) {
      //     float _sum = 0;
      //     for (int _d = 0; _d < head_qk; _d++) {
      //       int _g = _d / kMXFP8GroupSize_sfp;
      //       _sum += (float)ElementAccumulator(mQ(idx_Q + offset_Q, _d, coord_L))
      //             * (float)ElementAccumulator(mSFQ(idx_Q + offset_Q, _g, idx_L))
      //             * (float)ElementAccumulator(mK(_k, _d, coord_L))
      //             * (float)ElementAccumulator(mSFK(_k + offset_K, _g, idx_L));
      //     }
      //     printf("%.6f ", _sum);
      //   }
      //   printf("\n\n");
      // }
      // // ==================

      // --- Phase 2: Softmax (FP32) ---
      ElementAccumulator maxS = -std::numeric_limits<ElementAccumulator>::infinity();
      for (int k = 0; k < size<1>(problem_shape); k++)
        maxS = std::max<ElementAccumulator>(maxS, mS[k]);
      if (maxS == -std::numeric_limits<ElementAccumulator>::infinity()) maxS = 0;

      __syncthreads();

      ElementAccumulator sum = 0;

      // Reference correctness matters more than parallelism here.  Keep the
      // softmax and MXFP8 P quantization serial so each 32-wide scale group has
      // a deterministic max; the previous parallel max update raced between
      // threads writing the same sf_group_data entry.
      if (threadIdx.x == 0) {
        int total_k = size<1>(problem_shape);

        for (int g = 0; g < sf_groups; g++) {
          sf_group_data[g] = 0.0f;
        }

        for (int k = 0; k < total_k; k++) {
          mS[k] = expf(softmax_scale * (mS[k] - maxS));
          sum += mS[k];
          int g = k / kMXFP8GroupSize_sfp;
          sf_group_data[g] = max(sf_group_data[g], float(mS[k]));
        }

        for (int g = 0; g < sf_groups; g++) {
          int k_begin = g * kMXFP8GroupSize_sfp;
          int k_end = min(k_begin + kMXFP8GroupSize_sfp, total_k);
          for (int k = k_begin; k < k_end; k++) {
            ElementAccumulator val_fp32 = mS[k];
            val_fp32 = fminf(448.0f, fmaxf(-448.0f, val_fp32));
            mS_e4m3[k] = static_cast<cutlass::float_e4m3_t>(val_fp32);
          }
          sf_ue8m0[g] = static_cast<cutlass::float_ue8m0_t>(1.0f);
        }

        sf_group_data[0] = float(sum);
      }
      __syncthreads();

      sum = ElementAccumulator(sf_group_data[0]);
      ElementAccumulator inv_sum = 1.0f / sum;

      // // ===== Dump all mS (softmax probabilities) =====
      // if (threadIdx.x == 0) {
      //   printf("\n[REF] mS[0..%d] after softmax (idx_Q=%d, idx_L=%d):\n  ",
      //          size<1>(problem_shape) - 1, idx_Q, coord_L);
      //   for (int k = 0; k < size<1>(problem_shape); k++) {
      //     printf("%.6f ", (float)mS[k]);
      //     if ((k + 1) % 16 == 0 && k + 1 < size<1>(problem_shape))
      //       printf("\n  ");
      //   }
      //   printf("\n");
      // }
      // // ===============================================
      // --- Phase 3: P * V (P=fp32, V=MXFP8 dequantized) ---
      // SFV groups along K-seqlen, matching MMA hardware layout
      {
        int d_chunk = (head_v + blockDim.x - 1) / blockDim.x;
        int d_begin = threadIdx.x * d_chunk;
        int d_end   = min(d_begin + d_chunk, head_v);
        for (int d = d_begin; d < d_end; d++) {
          ElementAccumulator acc = 0;
          for (int k = 0; k < size<1>(problem_shape); k++) {
            int gk = (k + offset_K) / kMXFP8GroupSize_sfp;
            ElementAccumulator p_fp32 =
                ElementAccumulator(mS_e4m3[k])
              * ElementAccumulator(sf_ue8m0[k / kMXFP8GroupSize_sfp]);

            ElementAccumulator v_fp32 =
                ElementAccumulator(mV(k + offset_K, d, coord_L))
              * ElementAccumulator(mSFV(d, gk, idx_L));
            acc += p_fp32 * v_fp32;
          }
          mO(idx_Q + offset_Q, d, coord_L) =
              static_cast<typename TensorO::value_type>(acc * inv_sum);
        }
      }

      if (threadIdx.x == 0 && mLSE.data() != nullptr) {
        mLSE(idx_Q + offset_Q, coord_L) = log(sum) + softmax_scale * maxS;
      }
    }
  }
}

/////////////////////////////////////////////////////////////////////////////////////////////////
// Host wrapper
/////////////////////////////////////////////////////////////////////////////////////////////////

template<
  class ProblemShapeIn,
  class TensorQ,  class TensorSFQ,
  class TensorK,  class TensorSFK,
  class TensorV,  class TensorSFV,
  class TensorO,  class TensorLSE,
  class Mask
>
void fmha_reference_mxfp8_sfp(
    ProblemShapeIn problem_shape_in,
    TensorQ  mQ,   TensorSFQ mSFQ,
    TensorK  mK,   TensorSFK mSFK,
    TensorV  mV,   TensorSFV mSFV,
    TensorO  mO,   TensorLSE mLSE,
    Mask mask) {

  using namespace cute;

  dim3 grid(size<0>(mO), size<2>(mO), 1);
  dim3 block(256);
  int shared_mem = size<0>(mK) * int(sizeof(typename TensorLSE::value_type))       // mS (float)
                 + ((size<0>(mK) + kMXFP8GroupSize_sfp - 1) / kMXFP8GroupSize_sfp) * sizeof(float)  // sf_group_data
                 + size<0>(mK) * sizeof(cutlass::float_e4m3_t)                              // mS_e4m3
                 + ((size<0>(mK) + kMXFP8GroupSize_sfp - 1) / kMXFP8GroupSize_sfp) * sizeof(cutlass::float_ue8m0_t); // sf_ue8m0
  cudaError_t result;
  if (shared_mem >= (48 << 10)) {
    result = cudaFuncSetAttribute(
        &fmha_reference_mxfp8_kernel_sfp<
            ProblemShapeIn,
            TensorQ, TensorSFQ, TensorK, TensorSFK,
            TensorV, TensorSFV,
            TensorO, TensorLSE, Mask>,
        cudaFuncAttributeMaxDynamicSharedMemorySize, shared_mem);
    if (cudaSuccess != result) {
      cudaGetLastError();
      throw std::runtime_error("Failed to allocate " +
          std::to_string(shared_mem >> 10) +
          " KB dynamic smem for MXFP8 ref. check");
    }
  }
  fmha_reference_mxfp8_kernel_sfp<<<grid, block, shared_mem>>>(
      problem_shape_in,
      mQ, mSFQ, mK, mSFK, mV, mSFV,
      mO, mLSE, mask);
}

/////////////////////////////////////////////////////////////////////////////////////////////////
