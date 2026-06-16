/*
 * ============================================================================
 * Operator (6) — Milestone 2 / Route C : MXFP8 FlashAttention (driver + quant)
 * ============================================================================
 *
 * Route C = adapt CUTLASS example 77 FMHA forward; QK^T GEMM becomes
 * block-scaled MXFP8 (Q,K = e4m3 + ue8m0 scale factors, SF vec size 32 along
 * headdim). PV stays plain-FP8. See docs/OP6_ROUTE_C_PLAN.md.
 *
 * THIS FILE = file 1 of 4: the driver + MXFP8 quantization.
 *   - MXFP8 quantization (Q,K -> e4m3 data + ue8m0 SF) is complete & self-testable.
 *   - The FMHA runner needs the modified collective headers (files 2-4):
 *       sm100_fmha_fwd_mainloop_mxfp8.hpp / _load_tma_mxfp8.hpp / _kernel_mxfp8.hpp
 *     and is therefore guarded behind -DMXFP8_FULL.
 *
 * Build modes:
 *   nvcc -arch=sm_100a fmha_mxfp8.cu -o fmha_mxfp8_quant      # quant self-test (NOW)
 *   nvcc -arch=sm_100a -DMXFP8_FULL ... fmha_mxfp8.cu         # full FMHA (after files 2-4)
 *
 * SF layout note: the quantizer emits SF in a plain row-major [rows][D/32]
 * uint8 buffer (ue8m0). Re-packing into CUTLASS's swizzled block-scaled SF
 * layout (Sm1xxBlockScaledConfig<32>, 128x4 chunk) is done at the collective
 * wiring step (file 3), where the exact CuTe layout type is available.
 * ============================================================================
 */

#include <cuda_runtime.h>
#include <cuda_fp8.h>
#include <cstdio>
#include <cstdint>
#include <cmath>
#include <vector>
#include <thread>
#include <chrono>
#include <string>
#include <random>

#include "common.h"

// ---------------------------------------------------------------------------
// MXFP8 format constants
// ---------------------------------------------------------------------------
#define SF_VEC      32        // MX block-scale vector size (elements per SF)
#define E4M3_MAX    448.0f    // max finite magnitude of float_e4m3

// ===========================================================================
// e8m0 scale-factor helpers
//
// ue8m0: 8-bit, pure biased exponent. value = 2^(byte - 127). byte in [0,254].
// A faithful MX scale is a power of two, so it round-trips exactly through e8m0.
// ===========================================================================

// Pick the e8m0 byte so that  amax / 2^(byte-127) <= E4M3_MAX.
__host__ __device__ __forceinline__ uint8_t mx_choose_e8m0(float amax) {
#ifdef MXFP8_UNIT_SF
    // Bisection build: force SF == 2^0 == 1, turning the block-scaled QK path
    // into a trivial plain-FP8 GEMM. Isolates SF bugs from structural bugs.
    (void)amax; return 127;
#endif
    if (!(amax > 0.0f)) return 127;                  // 2^0 = 1 for zero/NaN input
    // need 2^e >= amax / 448  ->  e = ceil(log2(amax/448))
    int e;
    float m = frexpf(amax / E4M3_MAX, &e);           // amax/448 = m * 2^e, m in [0.5,1)
    // m in [0.5,1) => amax/448 in [2^(e-1), 2^e) => smallest pow2 >= it is 2^e
    (void)m;
    int byte = e + 127;
    if (byte < 0)   byte = 0;
    if (byte > 254) byte = 254;
    return (uint8_t)byte;
}

__host__ __device__ __forceinline__ float e8m0_to_scale(uint8_t b) {
    return ldexpf(1.0f, (int)b - 127);
}

__device__ __forceinline__ uint8_t f32_to_e4m3(float v) {
    return (uint8_t)__nv_cvt_float_to_fp8(v, __NV_SATFINITE, __NV_E4M3);
}

__device__ __forceinline__ float e4m3_to_f32(uint8_t b) {
    __half_raw hr = __nv_cvt_fp8_to_halfraw((__nv_fp8_storage_t)b, __NV_E4M3);
    return __half2float(*reinterpret_cast<__half*>(&hr));
}

// ===========================================================================
// MXFP8 quantization kernel
//
// Input  : [rows][D] fp32                       (rows = B*S*H for BNSD Q or K)
// Output : [rows][D]      e4m3 data
//          [rows][D/32]   ue8m0 scale factors   (plain row-major)
//
// Block scale runs along D (the QK^T contraction dim). One warp per row;
// lane L owns column blk*32 + L inside each of the D/32 blocks.
// ===========================================================================
__global__ void quantize_mxfp8_kernel(const float* __restrict__ in,
                                      uint8_t* __restrict__ out_data,
                                      uint8_t* __restrict__ out_sf,
                                      long rows, int D) {
    int warps_per_block = blockDim.x >> 5;
    long row = (long)blockIdx.x * warps_per_block + (threadIdx.x >> 5);
    int  lane = threadIdx.x & 31;
    if (row >= rows) return;

    const float* in_row  = in       + row * D;
    uint8_t*     dat_row = out_data  + row * D;
    uint8_t*     sf_row  = out_sf    + row * (D / SF_VEC);

    for (int blk = 0; blk < D / SF_VEC; ++blk) {
        float v = in_row[blk * SF_VEC + lane];
        float a = fabsf(v);
        #pragma unroll
        for (int o = 16; o > 0; o >>= 1)
            a = fmaxf(a, __shfl_xor_sync(0xffffffffu, a, o));   // warp-wide block amax

        uint8_t  sf_byte = mx_choose_e8m0(a);
        float    scale   = e8m0_to_scale(sf_byte);
        dat_row[blk * SF_VEC + lane] = f32_to_e4m3(v / scale);
        if (lane == 0) sf_row[blk] = sf_byte;
    }
}

// Dequantize (for the self-test): reconstruct fp32 from e4m3 data + ue8m0 SF.
__global__ void dequantize_mxfp8_kernel(const uint8_t* __restrict__ data,
                                        const uint8_t* __restrict__ sf,
                                        float* __restrict__ out,
                                        long rows, int D) {
    long idx = (long)blockIdx.x * blockDim.x + threadIdx.x;
    if (idx >= rows * (long)D) return;
    long row = idx / D;
    int  col = idx % D;
    float scale = e8m0_to_scale(sf[row * (D / SF_VEC) + col / SF_VEC]);
    out[idx] = e4m3_to_f32(data[idx]) * scale;
}

// ---------------------------------------------------------------------------
// Host helper: quantize a device fp32 tensor to MXFP8 (allocates outputs).
// ---------------------------------------------------------------------------
struct Mxfp8Tensor {
    uint8_t* data = nullptr;   // [rows][D] e4m3
    uint8_t* sf   = nullptr;   // [rows][D/32] ue8m0
    long     rows = 0;
    int      D    = 0;
    void free_() { if (data) cudaFree(data); if (sf) cudaFree(sf); data = sf = nullptr; }
};

static Mxfp8Tensor quantize_mxfp8(const float* d_in, long rows, int D) {
    Mxfp8Tensor t; t.rows = rows; t.D = D;
    CUDA_CHECK(cudaMalloc(&t.data, rows * (long)D));
    CUDA_CHECK(cudaMalloc(&t.sf,   rows * (long)(D / SF_VEC)));
    int threads = 256;                              // 8 warps/block
    long blocks = (rows + (threads / 32) - 1) / (threads / 32);
    quantize_mxfp8_kernel<<<blocks, threads>>>(d_in, t.data, t.sf, rows, D);
    CUDA_CHECK(cudaGetLastError());
    return t;
}

// ===========================================================================
// Self-test: quantize -> dequantize -> compare. Verifies the MXFP8 codec.
// ===========================================================================
static int quant_selftest() {
    printf("=== Operator 6 / M2 Route C — MXFP8 quantization self-test ===\n\n");

    struct Shape { int B, S, H, D; const char* name; };
    Shape shapes[] = {
        {1, 2048,  16, 128, "B1_S2048_H16_D128"},
        {1, 8192,  40, 128, "B1_S8192_H40_D128"},
        {1, 4096,  32, 128, "B1_S4096_H32_D128_GQAq"},
    };

    int fails = 0;
    for (auto& s : shapes) {
        long rows = (long)s.B * s.S * s.H;
        long n    = rows * s.D;

        std::vector<float> h_in(n);
        std::mt19937 rng(1234);
        std::normal_distribution<float> dist(0.0f, 1.0f);
        for (long i = 0; i < n; ++i) h_in[i] = dist(rng);

        float* d_in;
        CUDA_CHECK(cudaMalloc(&d_in, n * sizeof(float)));
        CUDA_CHECK(cudaMemcpy(d_in, h_in.data(), n * sizeof(float), cudaMemcpyHostToDevice));

        Mxfp8Tensor q = quantize_mxfp8(d_in, rows, s.D);

        float* d_deq;
        CUDA_CHECK(cudaMalloc(&d_deq, n * sizeof(float)));
        int th = 256;
        dequantize_mxfp8_kernel<<<(n + th - 1) / th, th>>>(q.data, q.sf, d_deq, rows, s.D);
        CUDA_CHECK(cudaDeviceSynchronize());

        std::vector<float> h_deq(n);
        CUDA_CHECK(cudaMemcpy(h_deq.data(), d_deq, n * sizeof(float), cudaMemcpyDeviceToHost));

        double max_rel = 0, mean_abs = 0;
        for (long i = 0; i < n; ++i) {
            double d = fabs((double)h_deq[i] - h_in[i]);
            mean_abs += d;
            double denom = fmax(fabs((double)h_in[i]), 1e-3);
            max_rel = fmax(max_rel, d / denom);
        }
        mean_abs /= n;

        // e4m3 has 3 mantissa bits => ~1/16 relative step; block scaling adds a
        // little. Expect mean abs error well under 0.05 for unit-gaussian data.
        bool ok = (mean_abs < 0.05);
        printf("  %-26s rows=%-9ld mean_abs_err=%.5f max_rel_err=%.4f  [%s]\n",
               s.name, rows, mean_abs, max_rel, ok ? "OK" : "FAIL");
        if (!ok) ++fails;

        cudaFree(d_in); cudaFree(d_deq); q.free_();
    }
    printf("\n%s\n", fails == 0 ? "QUANT SELF-TEST PASS" : "QUANT SELF-TEST FAIL");
    return fails == 0 ? 0 : 1;
}

// ===========================================================================
#ifdef MXFP8_FULL
// ---------------------------------------------------------------------------
// Full MXFP8 FMHA driver.
//   - Q,K quantized to MXFP8 (e4m3 data + ue8m0 SF, plain), V to plain e4m3.
//   - plain SF repacked into Sm1xxBlockScaledConfig<32> swizzled layout.
//   - kernel = example-77 forward kernel + the Route-C block-scaled mainloop
//     (files 2-3); QK N-tile is 64 so TileShape = (256, 64, 128).
//   - verify: dequantize MXFP8 inputs back to fp32 and run example 77's
//     fp32 reference, compare O.
// ---------------------------------------------------------------------------
#include "cute/tensor.hpp"
#include "cutlass/cutlass.h"
#include "cutlass/kernel_hardware_info.h"
#include "cutlass/detail/sm100_blockscaled_layout.hpp"

#include "reference/fmha_fwd_reference.hpp"
#include "device/fmha.hpp"
#include "collective/fmha_fusion.hpp"
#ifdef MXFP8_N128
// CtaN=128 single-softmax-warp variant (M3). See docs/OP6_M3_CTAN128_PLAN.md.
#include "sm100_fmha_fwd_mainloop_mxfp8_n128_pvmx.hpp"
#include "sm100_fmha_fwd_epilogue_mxfp8_n128.hpp"
#include "fmha_mxfp8_n128_schedule.hpp"
#else
#include "sm100_fmha_fwd_mainloop_mxfp8.hpp"
#include "collective/sm100_fmha_fwd_epilogue_tma_warpspecialized.hpp"
#endif
#include "kernel/fmha_options.hpp"
#include "kernel/fmha_tile_scheduler.hpp"
// [PVMX 2a.0] custom kernel header: passes TensorStorage to softmax() so the
// softmax warps can write P -> smem_p for the SS PV MMA.
#include "sm100_fmha_fwd_kernel_mxfp8_pvmx.hpp"

using namespace cute;

// Repack a plain row-major [rows][D/32] ue8m0 SF buffer into the CUTLASS
// block-scaled swizzled layout described by `layout` (over (S, D, ((Hr,Hk),B))).
template<class LayoutSF>
__global__ void repack_sf_kernel(const uint8_t* __restrict__ plain_sf,
                                 uint8_t* __restrict__ sw_sf,
                                 LayoutSF layout, int S, int H, int B, int D) {
    long nblk = (long)S * H * B * (D / SF_VEC);
    long idx  = (long)blockIdx.x * blockDim.x + threadIdx.x;
    if (idx >= nblk) return;
    int  blk = idx % (D / SF_VEC);
    long row = idx / (D / SF_VEC);
    int  s = row % S;
    int  h = (row / S) % H;
    int  b = row / ((long)S * H);
    uint8_t v = plain_sf[row * (D / SF_VEC) + blk];
    // SF layout coord: (M=s, K=d, L=(0, head + H*batch)). The SF tensor's L is
    // a flat (_1, H*B) axis; the leading sub-coord is the (_1):(_0) dummy.
    auto crd = cute::make_coord(s, blk * SF_VEC, cute::make_coord(0, h + b * H));
    sw_sf[layout(crd)] = v;
}

// e4m3 -> fp32 (for dequantizing V before the fp32 reference).
__global__ void e4m3_to_f32_kernel(const uint8_t* in, float* out, long n) {
    long i = (long)blockIdx.x * blockDim.x + threadIdx.x;
    if (i < n) out[i] = e4m3_to_f32(in[i]);
}

// [PVMX 2a.1b] V dequant applying V-SF (per-32 along S, per-d row).
// data: BHSD e4m3 ; sf_plain: [(b,h,d), kb] e8m0 ; out: BHSD f32.
__global__ void dequantize_v_mxfp8_kernel(const uint8_t* __restrict__ data,
                                          const uint8_t* __restrict__ sf_plain,
                                          float* __restrict__ out,
                                          int B, int H, int S, int D) {
    long total = (long)B * H * S * D;
    long idx = (long)blockIdx.x * blockDim.x + threadIdx.x;
    if (idx >= total) return;
    int d = (int)(idx % D);
    long rem = idx / D;
    int s = (int)(rem % S);
    long bh = rem / S;
    int h = (int)(bh % H);
    int b = (int)(bh / H);
    int kblocks = S / SF_VEC;
    int kb = s / SF_VEC;
    long sf_off = ((((long)b * H + h) * D) + d) * kblocks + kb;
    float scale = e8m0_to_scale(sf_plain[sf_off]);
    out[idx] = e4m3_to_f32(data[idx]) * scale;
}

// [PVMX 2a.1b] Quantize V to MXFP8 with per-32 SF along seqlen_kv. V layout BHSD
// (rows = B*H*S, D); per (b,h,d) we block 32 consecutive S values, derive e8m0 SF,
// scale V to e4m3. SF output is plain row-major [b][h][d][kblock].
__global__ void quantize_v_mxfp8_kernel(const float* __restrict__ in,
                                        uint8_t* __restrict__ out_data,
                                        uint8_t* __restrict__ out_sf_plain,
                                        int B, int H, int S, int D) {
    int kblocks = S / SF_VEC;
    long idx = (long)blockIdx.x * blockDim.x + threadIdx.x;
    long total = (long)B * H * kblocks * D;
    if (idx >= total) return;
    int d = idx % D;
    long rem = idx / D;
    int kb = rem % kblocks;
    long bh = rem / kblocks;
    int h = bh % H;
    int b = bh / H;

    long row_base = ((long)b * H + h) * S + (long)kb * SF_VEC;
    float amax = 0.0f;
    for (int s = 0; s < SF_VEC; ++s) {
        float v = in[(row_base + s) * D + d];
        amax = fmaxf(amax, fabsf(v));
    }
    uint8_t sf = mx_choose_e8m0(amax);
    float   scale = e8m0_to_scale(sf);
    for (int s = 0; s < SF_VEC; ++s) {
        float v = in[(row_base + s) * D + d];
        out_data[(row_base + s) * D + d] = f32_to_e4m3(v / scale);
    }
    long sf_off = ((((long)b * H + h) * D) + d) * kblocks + kb;
    out_sf_plain[sf_off] = sf;
}

// [PVMX 2a.1b] Repack plain V-SF [b][h][d][kblock] -> layout_SFV (PV SFB).
template<class LayoutSF>
__global__ void repack_v_sf_kernel(const uint8_t* __restrict__ plain_sf,
                                   uint8_t* __restrict__ sw_sf,
                                   LayoutSF layout, int S, int H, int B, int D) {
    int kblocks = S / SF_VEC;
    long total = (long)B * H * D * kblocks;
    long idx = (long)blockIdx.x * blockDim.x + threadIdx.x;
    if (idx >= total) return;
    int kb = idx % kblocks;
    long rem = idx / kblocks;
    int d = rem % D;
    long bh = rem / D;
    int h = bh % H;
    int b = bh / H;
    // For PV SFB layout: M-coord = N = d ; K-coord = kb * SF_VEC ; L = (0, h + b*H).
    auto crd = cute::make_coord(d, kb * SF_VEC, cute::make_coord(0, h + b * H));
    sw_sf[layout(crd)] = plain_sf[idx];
}

#ifdef MXFP8_PROBE
// Controlled probe: write SFB so KV row kv (all d-blocks) = 2^((kv%11)-2).
// With Q=K=e4m3(1) and SFA=2^0, the kernel QK score S[0,kv] = 2^(5+(kv%11))
// iff the MMA used SFB[kv]; decoding S reveals the SFB row actually used.
template<class LayoutSF>
__global__ void probe_sfb_kernel(uint8_t* sw, LayoutSF layout,
                                 int S, int H, int B, int D) {
    long nblk = (long)S * H * B * (D / SF_VEC);
    long idx  = (long)blockIdx.x * blockDim.x + threadIdx.x;
    if (idx >= nblk) return;
    int  blk = idx % (D / SF_VEC);
    long row = idx / (D / SF_VEC);
    int  s = row % S, h = (row / S) % H, b = row / ((long)S * H);
#ifdef MXFP8_PROBE_DBLK
    uint8_t v = (uint8_t)(127 + 3 * blk);          // SF varies per d-block
#else
    uint8_t v = (uint8_t)(100 + (s % 128));        // SF unique per row (mod 128)
#endif
    sw[layout(cute::make_coord(s, blk * SF_VEC,
                               cute::make_coord(0, h + b * H)))] = v;
}
// d-block probe: Q data e4m3(2^db) for d in d-block db.
__global__ void probe_q_dblk_kernel(uint8_t* out, long n, int D) {
    long i = (long)blockIdx.x * blockDim.x + threadIdx.x;
    if (i >= n) return;
    int db = (int)((i % D) / SF_VEC);
    out[i] = f32_to_e4m3((float)(1 << db));
}

// Joint (kv-row, d-block) probe. SFB[s,db] exponent = 4*db + ((s>>(2*db))&3),
// i.e. each d-block db carries a distinct 4-bit window of S[0,n], and within
// that window the value encodes 2 bits of the row index s. With Q=K=e4m3(1)
// and SFA=2^0 the kernel score is
//   S[0,n] = 32 * sum_db 2^(4*db + ((n_sourced>>(2*db))&3))
// so decoding the 4 nibbles of S/32 recovers, per d-block, which row the MMA
// actually sourced SFB from. Identity => every nibble's row bits == n.
template<class LayoutSF>
__global__ void probe_sfb_joint_kernel(uint8_t* sw, LayoutSF layout,
                                       int S, int H, int B, int D) {
    long nblk = (long)S * H * B * (D / SF_VEC);
    long idx  = (long)blockIdx.x * blockDim.x + threadIdx.x;
    if (idx >= nblk) return;
    int  blk = idx % (D / SF_VEC);
    long row = idx / (D / SF_VEC);
    int  s = row % S, h = (row / S) % H, b = row / ((long)S * H);
    uint8_t v = (uint8_t)(127 + 4 * blk + ((s >> (2 * blk)) & 3));
    sw[layout(cute::make_coord(s, blk * SF_VEC,
                               cute::make_coord(0, h + b * H)))] = v;
}
#endif

// fp32 -> plain e4m3 (V path: no block scaling).
__global__ void f32_to_e4m3_kernel(const float* in, uint8_t* out, long n) {
    long i = (long)blockIdx.x * blockDim.x + threadIdx.x;
    if (i < n) out[i] = f32_to_e4m3(in[i]);
}

#ifdef MXFP8_PROBE
// Controlled probe: fill an e4m3 buffer with the byte for 1.0.
__global__ void fill_e4m3_one_kernel(uint8_t* out, long n) {
    long i = (long)blockIdx.x * blockDim.x + threadIdx.x;
    if (i < n) out[i] = f32_to_e4m3(1.0f);
}
#endif

static int run_fmha_mxfp8(int argc, char** argv) {
    using Element             = cutlass::float_e4m3_t;
    using ElementAccumulator  = float;
    using ElementOut          = cutlass::half_t;

    // ---- problem shape (Route C verify config) ------------------------------
    int B = 1, H = 4, S = 512, D = 128;
    // do_verify : run the FP32 reference + compare. The reference kernel stores a
    //   full score row in dynamic smem (S*4 bytes); at large S this exceeds the
    //   B200 227KB/block limit and throws. Pass --verify=0 for real SOW shapes.
    // quick     : skip the loop/graph benchmarks and use a short solo run — for
    //   huge shapes where each kernel is ~1s+ and 50-iter loops take minutes.
    int do_verify = 1, quick = 0, noinit = 0;
    const char* dump_prefix = nullptr;   // --dump=<prefix>: write fp32 Q/K/V + O for torch SDPA golden
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto val = [&](const char* k){ return atoi(a.substr(strlen(k)).c_str()); };
        if      (a.rfind("--b=",0)==0) B = val("--b=");
        else if (a.rfind("--h=",0)==0) H = val("--h=");
        else if (a.rfind("--s=",0)==0) S = val("--s=");
        else if (a.rfind("--d=",0)==0) D = val("--d=");
        else if (a.rfind("--verify=",0)==0) do_verify = val("--verify=");
        else if (a.rfind("--quick=",0)==0)  quick     = val("--quick=");
        else if (a.rfind("--noinit=",0)==0) noinit   = val("--noinit=");
        else if (a.rfind("--dump=",0)==0)    dump_prefix = argv[i]+7;
    }
#ifdef MXFP8_D64
    D = 64;   // bisection: single SF block per row
#endif
    printf("=== Operator 6 / M2 Route C — MXFP8 FlashAttention ===\n");
#if defined(MXFP8_N128)
    printf("  B=%d H=%d S=%d D=%d  (MHA, NoMask, TileShape 128x128x128 / single-stage)\n\n", B, H, S, D);
#else
    printf("  B=%d H=%d S=%d D=%d  (MHA, NoMask, TileShape 256x64x128)\n\n", B, H, S, D);
#endif

    const int h_r = 1, h_k = H;
    // ProblemShape : Q K D ((H_R,H_K), B)
    using ProblemShape = cute::tuple<int,int,int, cute::tuple<cute::tuple<int,int>,int>>;
    ProblemShape problem_shape = cute::make_tuple(
        S, S, D, cute::make_tuple(cute::make_tuple(h_r, h_k), B));

    // BHSD-contiguous strides.
    // NOTE: stride elements are int64_t, not int. The batch stride is H*S*D;
    // for the SOW real shape (H=40,D=128) that overflows int32 at S>419430
    // (e.g. S=510300 -> 2.61e9 > 2^31-1). A wrapped-negative stride becomes a
    // huge uint64 in the TMA descriptor and trips the `stride < 2^40` assert in
    // copy_traits_sm90_tma.hpp. 64-bit strides keep the real SOW shapes legal.
    using StrideQ   = cute::tuple<int64_t,_1, cute::tuple<cute::tuple<int64_t,int64_t>,int64_t>>;
    using StrideK   = cute::tuple<int64_t,_1, cute::tuple<cute::tuple<_0,int64_t>,int64_t>>;
    using StrideV   = StrideK;
    using StrideO   = StrideQ;
    using StrideLSE = cute::tuple<_1, cute::tuple<cute::tuple<int,int>,int>>;

    const int64_t SD = (int64_t)S * D, HSD = (int64_t)H * S * D;
    StrideQ stride_Q = make_stride((int64_t)D, _1{}, make_stride(make_stride(SD, SD), HSD));
    StrideK stride_K = make_stride((int64_t)D, _1{}, make_stride(make_stride(_0{}, SD), HSD));
    StrideV stride_V = stride_K;
    StrideO stride_O = stride_Q;
    StrideLSE stride_LSE = make_stride(_1{}, make_stride(make_stride(S, S), S*H));

    // ---- collective / kernel types -----------------------------------------
#if defined(MXFP8_N128)
    using TileShape = Shape<_128, _128, _128>;         // [2SM M2] per-CTA tile stays M=128; MMA tile M=256 decoupled in mainloop
#elif defined(MXFP8_D64)
    using TileShape = Shape<_256, _64, _32>;           // bisection D=32
#else
    using TileShape = Shape<_256, _64, _128>;          // QK N-tile = 64 (Route C)
#endif
    using Mainloop = cutlass::fmha::collective::Sm100FmhaFwdMainloopTmaWarpspecializedMxfp8<
        Element, ElementAccumulator, ElementAccumulator,
        TileShape, StrideQ, StrideK, StrideV,
        // [precision fix] ResidualMask masks kv >= seqlen_kv in the partial last KV
        // tile (S % 128 != 0). NoMask left the tail unmasked -> padded positions
        // leaked into the softmax denominator, biasing O low. ResidualMask routes
        // the last tile through the masked softmax_step path (apply_mask -> -inf).
        cutlass::fmha::collective::ResidualMask>;
    using Epilogue = cutlass::fmha::collective::Sm100FmhaFwdEpilogueTmaWarpspecialized<
        ElementOut, ElementAccumulator, typename Mainloop::TileShapePV, StrideO, StrideLSE>;
    using Kernel = cutlass::fmha::kernel::Sm100FmhaFwdKernelTmaWarpspecialized<
        ProblemShape, Mainloop, Epilogue,
#if defined(MXFP8_N128) && defined(MXFP8_PERSISTENT)
        // opt-in: persistent scheduler (SM-count CTAs, each grabs M-tiles from a
        // global counter). BUG: on multi-wave launches it corrupts the partial
        // tail when S%128!=0 — the 2nd-wave tiles produce inf/garbage (seen at
        // H=40 S=628: heads 29-39 = exactly tile-idx >=148). Kept only for A/B
        // perf comparison; DO NOT use for real SOW shapes (S=170100/510300 are
        // both S%128!=0). It is also ~7% SLOWER than individual at large S.
        cutlass::fmha::kernel::PersistentTileScheduler,
        cutlass::fmha::kernel::Sm100FmhaCtxKernelWarpspecializedScheduleSingleStage
#elif defined(MXFP8_N128)
        // default: individual one-CTA-per-tile scheduler. Correct for every S
        // (incl. S%128!=0) and faster than persistent at large S — NCU
        // gpu__time_duration: 712 vs 666 TFLOPS at S=170100, 698 vs 657 at
        // S=32768. (The 256-CTA/148-SM "1.73-wave tail" persistent was meant to
        // fix is negligible once S is large: 1329 M-tiles/head amortise it.)
        cutlass::fmha::kernel::IndividualTileScheduler,
        cutlass::fmha::kernel::Sm100FmhaCtxKernelWarpspecializedScheduleSingleStage
#else
        cutlass::fmha::kernel::IndividualTileScheduler
#endif
        >;
    using Operation = cutlass::fmha::device::FMHA<Kernel>;

    // ---- SF layouts ---------------------------------------------------------
    // tile_atom_to_shape_SF* gives the SF tensor a FLAT L = (_1, H*B): a single
    // head*batch axis (the leading sub-mode is a static (_1):(_0) dummy). The
    // kernel's blk_coord linearises (head,batch) into that flat axis colex as
    // l = head + H*batch, so repack / probes must write each (h,b) at the SAME
    // flat L index. (The original bug: repack passed make_coord(h,b) which put
    // h on the stride-0 dummy => every head collapsed onto one SF slot.)
    using SFConfig = cutlass::detail::Sm1xxBlockScaledConfig<SF_VEC>;
    (void)h_r;
    auto problem_qk = cute::make_tuple(S, S, D, cute::make_tuple(H, B));
    auto layout_SFA = SFConfig::tile_atom_to_shape_SFA(problem_qk);
    auto layout_SFB = SFConfig::tile_atom_to_shape_SFB(problem_qk);

    // ---- host data ----------------------------------------------------------
    long rows_q = (long)B * H * S, rows_k = rows_q;
    long n_q = rows_q * D, n_k = rows_k * D, n_v = n_k;
    std::vector<float> hQ(n_q), hK(n_k), hV(n_v);
    std::mt19937 rng(20260521);
    std::normal_distribution<float> dist(0.0f, 1.0f);
#ifdef MXFP8_BIGDATA
    // Bisection: large-magnitude inputs so quantized e4m3 data spans e4m3's
    // full range (~+-448), mimicking real MXFP8 data with UNIT_SF.
    const float in_scale = 128.0f;
#else
    const float in_scale = 1.0f;
#endif
    // Parallel host RNG: at SOW shape (B*H*S*D ~ 2.6B floats) a single-threaded
    // std::normal_distribution loop takes minutes. Each thread uses an
    // independent mt19937 seeded deterministically from the base seed so output
    // remains reproducible.
    auto fill = [&](std::vector<float>& v, uint32_t seed_off) {
        const long N = (long)v.size();
        const int T = std::max(1, (int)std::thread::hardware_concurrency());
        std::vector<std::thread> ts; ts.reserve(T);
        for (int t = 0; t < T; ++t) {
            ts.emplace_back([&, t]{
                long lo = (N * t) / T, hi = (N * (t + 1)) / T;
                std::mt19937 r(20260521u ^ (seed_off * 0x9E3779B1u) ^ (uint32_t)t);
                std::normal_distribution<float> d(0.0f, 1.0f);
                for (long i = lo; i < hi; ++i) v[i] = d(r) * in_scale;
            });
        }
        for (auto& th : ts) th.join();
    };
    if (!noinit) {
        fill(hQ, 1);
        fill(hK, 2);
        fill(hV, 3);
    } else {
        // perf-only: skip host RNG; leave zero-initialized (std::vector default).
        // Output will be meaningless but kernel cost is data-independent.
        printf("[fmha_mxfp8] --noinit=1: skipping host RNG (perf-only run)\n");
    }
    (void)rng; (void)dist;

    float *dQf, *dKf, *dVf;
    CUDA_CHECK(cudaMalloc(&dQf, n_q*sizeof(float)));
    CUDA_CHECK(cudaMalloc(&dKf, n_k*sizeof(float)));
    CUDA_CHECK(cudaMalloc(&dVf, n_v*sizeof(float)));
    CUDA_CHECK(cudaMemcpy(dQf, hQ.data(), n_q*sizeof(float), cudaMemcpyHostToDevice));
    CUDA_CHECK(cudaMemcpy(dKf, hK.data(), n_k*sizeof(float), cudaMemcpyHostToDevice));
    CUDA_CHECK(cudaMemcpy(dVf, hV.data(), n_v*sizeof(float), cudaMemcpyHostToDevice));

    // ---- quantize Q,K,V -> MXFP8 ---------------------------
    Mxfp8Tensor qQ = quantize_mxfp8(dQf, rows_q, D);
    Mxfp8Tensor qK = quantize_mxfp8(dKf, rows_k, D);

    // [PVMX 2a.1b] V: per-32 along seqlen_kv (PV contraction). Writes scaled e4m3 +
    // plain V-SF [b][h][d][kblock]. Repack to layout_SFV below.
    uint8_t* dV;       CUDA_CHECK(cudaMalloc(&dV, n_v));
    int kblocks_v = S / SF_VEC;
    long v_sf_plain_n = (long)B * H * D * kblocks_v;
    uint8_t* dV_sf_plain;  CUDA_CHECK(cudaMalloc(&dV_sf_plain, v_sf_plain_n));
    {
        int th = 256;
        long total = (long)B * H * kblocks_v * D;
        quantize_v_mxfp8_kernel<<<(int)((total+th-1)/th), th>>>(dVf, dV, dV_sf_plain, B, H, S, D);
        CUDA_CHECK(cudaGetLastError());
    }

    // swizzled SF buffers
    int sfa_cosize = cute::cosize(layout_SFA);
    int sfb_cosize = cute::cosize(layout_SFB);
    uint8_t *dSFA, *dSFB;
    CUDA_CHECK(cudaMalloc(&dSFA, sfa_cosize));
    CUDA_CHECK(cudaMalloc(&dSFB, sfb_cosize));
    {
        long nblk = rows_q * (D/SF_VEC);
        int th = 256;
        repack_sf_kernel<<<(int)((nblk+th-1)/th), th>>>(qQ.sf, dSFA, layout_SFA, S, H, B, D);
        repack_sf_kernel<<<(int)((nblk+th-1)/th), th>>>(qK.sf, dSFB, layout_SFB, S, H, B, D);
        CUDA_CHECK(cudaGetLastError());
    }

    // [PVMX 2a.1] PV-side scale factors. PV problem = (M=seqlen_q, N=D, K=seqlen_kv).
    //   SFB_pv = V scale factors (real; const 1.0 = e8m0 127 for 2a.1, real per-32 later).
    //   SFA_pv = P scale factors — P is on-chip, so this gmem buffer is a valid-shape
    //            DUMMY only needed so params_pv's SF-TMA descriptor construction doesn't
    //            assert; the kernel sources P-SF on-chip (UTCCP), not from this TMA.
    auto problem_pv = cute::make_tuple(S, D, S, cute::make_tuple(H, B));
    // P-SF gmem is NEVER read (UTCCP feeds P-SF on-chip). Build a TILE-SIZED dummy
    // layout for the TMA descriptor — the full-problem layout overflows TMA's 2^40
    // stride limit at SOW seqlen. Same TYPE as the real layout, just tiny extents.
    auto problem_pv_tile = cute::make_tuple(128, 128, 128, cute::make_tuple(1, 1));
    auto layout_SFP = SFConfig::tile_atom_to_shape_SFA(problem_pv_tile);  // P-SF (dummy, tile-sized)
    auto layout_SFV = SFConfig::tile_atom_to_shape_SFB(problem_pv);       // V-SF (real, full)
    // [PVMX 2a.1] P-SF gmem is never read (P-SF is built on-chip via UTCCP); only the
    // TMA descriptor for params_pv needs a valid (non-null) buffer. Allocate a tiny
    // page — full cosize would overflow at SOW seqlen (S^2*L bytes).
    const size_t sfp_dummy_bytes = 4096;
    size_t sfv_cosize = cute::cosize(layout_SFV);
    uint8_t *dSFP, *dSFV;
    CUDA_CHECK(cudaMalloc(&dSFP, sfp_dummy_bytes));
    CUDA_CHECK(cudaMalloc(&dSFV, sfv_cosize));
    CUDA_CHECK(cudaMemset(dSFP, 127, sfp_dummy_bytes));   // 2^0 = 1.0 (dummy)
    // [PVMX 2a.1b] V-SF: repack plain dV_sf_plain -> dSFV via layout_SFV.
    {
        int th = 256;
        long nblk = (long)B * H * D * kblocks_v;
        repack_v_sf_kernel<<<(int)((nblk+th-1)/th), th>>>(dV_sf_plain, dSFV, layout_SFV, S, H, B, D);
        CUDA_CHECK(cudaGetLastError());
    }

#ifdef MXFP8_DBG
    // Direct verify: does repack land qK.sf[row][blk] / qQ.sf[row][blk] at the
    // exact byte the kernel's TMA will read via layout_SF{A,B}? This is the one
    // link the joint probe bypasses (it writes dSF* via layout directly).
    {
        CUDA_CHECK(cudaDeviceSynchronize());
        int NB = D / SF_VEC;
        auto chk = [&](const char* nm, uint8_t* dsw, int sw_cosize,
                       uint8_t* dplain, auto layout) {
            printf("[dbg] %s layout = ", nm); cute::print(layout); printf("\n");
            printf("[dbg] %s cosize=%d  repack-domain=%ld\n",
                   nm, sw_cosize, (long)S*H*B*NB);
            // collision histogram over the FULL repack domain (s,blk,h,b)
            std::vector<int> hits((size_t)sw_cosize, 0);
            int max_hits = 0; long colls = 0;
            int ce_s=-1,ce_blk=-1,ce_h=-1,ce_b=-1, ce_s2=-1,ce_blk2=-1,ce_h2=-1,ce_b2=-1;
            std::vector<int> firstcoord((size_t)sw_cosize, -1);
            for (int b = 0; b < B; ++b)
             for (int h = 0; h < H; ++h)
              for (int s = 0; s < S; ++s)
               for (int blk = 0; blk < NB; ++blk) {
                 auto crd = cute::make_coord(s, blk*SF_VEC,
                              cute::make_coord(0, h + b*H));
                 long off = layout(crd);
                 if (off < 0 || off >= sw_cosize) { ++colls; continue; }
                 int packed = ((b*H+h)*S+s)*NB+blk;
                 if (hits[off]++ == 1) {
                   ++colls;
                   if (ce_s < 0) {
                     int p = firstcoord[off];
                     ce_blk=p%NB; p/=NB; ce_s=p%S; p/=S; ce_h=p%H; ce_b=p/H;
                     ce_blk2=blk; ce_s2=s; ce_h2=h; ce_b2=b;
                   }
                 }
                 if (firstcoord[off] < 0) firstcoord[off] = packed;
                 max_hits = std::max(max_hits, hits[off]);
               }
            printf("[dbg] %s collisions: %ld  max_hits/offset=%d\n", nm, colls, max_hits);
            if (ce_s >= 0)
              printf("[dbg] %s  e.g. (s=%d blk=%d h=%d b=%d) and (s=%d blk=%d h=%d b=%d) -> same off\n",
                     nm, ce_s,ce_blk,ce_h,ce_b, ce_s2,ce_blk2,ce_h2,ce_b2);
            std::vector<uint8_t> hsw(sw_cosize), hpl((size_t)rows_k*NB);
            CUDA_CHECK(cudaMemcpy(hsw.data(), dsw, sw_cosize, cudaMemcpyDeviceToHost));
            CUDA_CHECK(cudaMemcpy(hpl.data(), dplain, (size_t)rows_k*NB, cudaMemcpyDeviceToHost));
            int mism = 0;
            for (int row = 0; row < 128 && row < S; ++row)
              for (int db = 0; db < NB; ++db) {
                auto crd = cute::make_coord(row, db*SF_VEC,
                             cute::make_coord(0, 0));
                int off = layout(crd);
                uint8_t got = hsw[off], exp = hpl[(long)row*NB+db];
                if (got != exp) ++mism;
              }
            printf("[dbg] %s repack verify: %d mismatches (of %d)\n", nm, mism, 128*NB);
        };
        chk("SFB", dSFB, sfb_cosize, qK.sf, layout_SFB);
        chk("SFA", dSFA, sfa_cosize, qQ.sf, layout_SFA);
    }
#endif

#ifdef MXFP8_PROBE
    // Controlled probe: Q=K=e4m3(1). One of SFA/SFB carries the 2^((r%11)-2)
    // pattern, the other is uniform 2^0. Decoding the QK scores reveals which
    // SF row the MMA used per output row.
    {
        int th = 256;
        fill_e4m3_one_kernel<<<(int)((n_k+th-1)/th), th>>>(qK.data, n_k);
        long nblk = rows_k * (D/SF_VEC);
#ifdef MXFP8_PROBE_DBLK
        // Q data varies per d-block (2^db); SFB varies per d-block (127+3db).
        probe_q_dblk_kernel<<<(int)((n_q+th-1)/th), th>>>(qQ.data, n_q, D);
        CUDA_CHECK(cudaMemset(dSFA, 127, sfa_cosize));
        probe_sfb_kernel<<<(int)((nblk+th-1)/th), th>>>(dSFB, layout_SFB, S, H, B, D);
        printf("[probe] d-block pairing: Q[d]=e4m3(2^db), K=e4m3(1), SFA=2^0, SFB[*,db]=2^(3db)\n");
        printf("[probe] correct S[0,0] = 32*sum_j 2^(4j) = %d\n", 32*(1+16+256+4096));
#elif defined MXFP8_PROBE_SFA
        fill_e4m3_one_kernel<<<(int)((n_q+th-1)/th), th>>>(qQ.data, n_q);
        probe_sfb_kernel<<<(int)((nblk+th-1)/th), th>>>(dSFA, layout_SFA, S, H, B, D);
        CUDA_CHECK(cudaMemset(dSFB, 127, sfb_cosize));
        printf("[probe] Q=K=e4m3(1), SFB=2^0, SFA[q]=2^((q%%11)-2)\n");
#elif defined MXFP8_PROBE_JOINT
        fill_e4m3_one_kernel<<<(int)((n_q+th-1)/th), th>>>(qQ.data, n_q);
        CUDA_CHECK(cudaMemset(dSFA, 127, sfa_cosize));
        probe_sfb_joint_kernel<<<(int)((nblk+th-1)/th), th>>>(dSFB, layout_SFB, S, H, B, D);
        printf("[probe] JOINT: Q=K=e4m3(1), SFA=2^0, SFB[s,db]=2^(4db+((s>>2db)&3))\n");
#else
        fill_e4m3_one_kernel<<<(int)((n_q+th-1)/th), th>>>(qQ.data, n_q);
  #ifdef MXFP8_PROBE_BOTH
        probe_sfb_kernel<<<(int)((nblk+th-1)/th), th>>>(dSFA, layout_SFA, S, H, B, D);
        probe_sfb_kernel<<<(int)((nblk+th-1)/th), th>>>(dSFB, layout_SFB, S, H, B, D);
        printf("[probe] Q=K=e4m3(1), SFA[q]=SFB[kv]=2^(byte-127), byte=100+row%%64\n");
  #else
        CUDA_CHECK(cudaMemset(dSFA, 127, sfa_cosize));
        probe_sfb_kernel<<<(int)((nblk+th-1)/th), th>>>(dSFB, layout_SFB, S, H, B, D);
        printf("[probe] Q=K=e4m3(1), SFA=2^0, SFB[kv]=2^(byte-127), byte=100+kv%%64\n");
  #endif
#endif
        CUDA_CHECK(cudaGetLastError());
    }
#endif

    // outputs
    ElementOut* dO;          CUDA_CHECK(cudaMalloc(&dO, n_q*sizeof(ElementOut)));
    ElementAccumulator* dLSE; CUDA_CHECK(cudaMalloc(&dLSE, rows_q*sizeof(ElementAccumulator)));

    // ---- build arguments ----------------------------------------------------
    cutlass::KernelHardwareInfo hw_info;
    hw_info.device_id = 0;
    hw_info.sm_count = cutlass::KernelHardwareInfo::query_device_multiprocessor_count(0);

    typename Operation::Arguments arguments{
        problem_shape,
        {   // mainloop
            {   // load
                reinterpret_cast<const Element*>(qQ.data), stride_Q,
                reinterpret_cast<const Element*>(qK.data), stride_K,
                reinterpret_cast<const Element*>(dV),      stride_V,
                reinterpret_cast<const cutlass::float_ue8m0_t*>(dSFA), layout_SFA,
                reinterpret_cast<const cutlass::float_ue8m0_t*>(dSFB), layout_SFB,
                reinterpret_cast<const cutlass::float_ue8m0_t*>(dSFP), layout_SFP,  // [PVMX 2a.1] P-SF (dummy, on-chip real)
                reinterpret_cast<const cutlass::float_ue8m0_t*>(dSFV), layout_SFV   // [PVMX 2a.1] V-SF (real)
            },
            0.0f,            // scale_softmax -> 1/sqrt(D)
            1.0f, 1.0f, 1.0f,// scale_q/k/v (SF already dequantizes Q,K)
            1.0f             // inv_scale_o
        },
        {   // epilogue
            dO, stride_O, dLSE, stride_LSE
        },
        hw_info
    };

#if defined(MXFP8_2SM_BEACON)
    unsigned* h_bcn = nullptr; unsigned* d_bcn = nullptr;
    cudaHostAlloc((void**)&h_bcn, 64*sizeof(unsigned), cudaHostAllocMapped);
    for (int i=0;i<64;i++) h_bcn[i]=0;
    cudaHostGetDevicePointer((void**)&d_bcn, h_bcn, 0);
    cudaMemcpyToSymbol(cutlass::fmha::collective::g_bcn, &d_bcn, sizeof(d_bcn));
#endif
    Operation op;
    cutlass::Status status = op.can_implement(arguments);
    if (status != cutlass::Status::kSuccess) {
        printf("[fmha_mxfp8] can_implement failed: %d\n", int(status));
        return 1;
    }
    size_t ws = Operation::get_workspace_size(arguments);
    uint8_t* workspace = nullptr;
    if (ws) CUDA_CHECK(cudaMalloc(&workspace, ws));
    status = op.initialize(arguments, workspace);
    if (status != cutlass::Status::kSuccess) {
        printf("[fmha_mxfp8] initialize failed: %d\n", int(status));
        return 1;
    }
    status = op.run();
#if defined(MXFP8_2SM_BEACON)
    for (int t=0; t<250; ++t) {
      printf("[beacon t=%d] CTA0=%u CTA1=%u\n", t, h_bcn[0], h_bcn[16]);
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    printf("[beacon FINAL] MMA: CTA0=%u CTA1=%u | LOAD: %u/%u | SOFTMAX: %u/%u | CORR: %u/%u\n", h_bcn[0],h_bcn[16], h_bcn[1],h_bcn[17], h_bcn[5],h_bcn[21], h_bcn[7],h_bcn[23]);
    printf("  MMA: 1=enter 2=pastQwait 3=pastK0wait 4=pastUTCCP 5=pastQKmma 6=pastcommit\n  LOAD: 1=preQacquire 2=Qacquired 3=QTMAissued\n");
    printf("  MASKS: CTA0 a=%u b=%u sfb=%u | CTA1 a=%u b=%u sfb=%u\n", h_bcn[2],h_bcn[3],h_bcn[4], h_bcn[18],h_bcn[19],h_bcn[20]);
    printf("  SMID(+1): CTA0=%u CTA1=%u  (even=odd-value, i.e. value 1,3,..=physical SM 0,2,..=even)\n", h_bcn[8], h_bcn[24]);
    fflush(stdout);
    return 0;
#endif
    CUDA_CHECK(cudaDeviceSynchronize());
    if (status != cutlass::Status::kSuccess) {
        printf("[fmha_mxfp8] run failed: %d (%s)\n", int(status),
               cudaGetErrorString(cudaGetLastError()));
        return 1;
    }
    printf("[fmha_mxfp8] kernel launched & completed.\n");

#ifdef MXFP8_EXITTS
    // [T2 exit-drain probe] per-warp exit-barrier arrival deltas for cluster 0.
    // Names the slowest finisher: every other warp's exit cluster_wait samples
    // (kernel:854, ~10% of pcsamp) are time spent waiting for it.
    {
        unsigned long long ts[64];
        CUDA_CHECK(cudaMemcpyFromSymbol(ts, cutlass::fmha::kernel::g_exit_ts, sizeof(ts)));
        unsigned long long tmin = ~0ull;
        for (int c = 0; c < 2; ++c)
            for (int w = 0; w < 16; ++w)
                if (ts[c*32+w] && ts[c*32+w] < tmin) tmin = ts[c*32+w];
        const char* role[16] = {"Sm0.0","Sm0.1","Sm0.2","Sm0.3","Sm1.0","Sm1.1","Sm1.2","Sm1.3",
                                "Corr0","Corr1","Corr2","Corr3","MMA","Load","Epi","Empty"};
        printf("[exitts] warp arrival at exit barrier, us after first arriver (cluster 0):\n");
        for (int c = 0; c < 2; ++c) {
            printf("[exitts] CTA%d:", c);
            for (int w = 0; w < 16; ++w) {
                double d = ts[c*32+w] ? (double)(ts[c*32+w] - tmin) / 1e3 : -1.0;
                printf(" %s=%.1f", role[w], d);
            }
            printf("\n");
        }
        for (int c = 0; c < 2; ++c)
            if (ts[62+c]) printf("[exitts] CTA%d post-wait: +%.1f us\n", c, (double)(ts[62+c]-tmin)/1e3);
    }
#endif

#ifdef MXFP8_DBG
    {
        std::vector<uint8_t> dsfk(2048), dsfq(2048);
        CUDA_CHECK(cudaMemcpyFromSymbol(dsfk.data(), g_dbg_sfk, 2048));
        CUDA_CHECK(cudaMemcpyFromSymbol(dsfq.data(), g_dbg_sfq, 2048));
        std::vector<uint8_t> plainK(rows_k*(D/SF_VEC)), plainQ(rows_q*(D/SF_VEC));
        CUDA_CHECK(cudaMemcpy(plainK.data(), qK.sf, plainK.size(), cudaMemcpyDeviceToHost));
        CUDA_CHECK(cudaMemcpy(plainQ.data(), qQ.sf, plainQ.size(), cudaMemcpyDeviceToHost));
        // slot 0 of smem_sfk = K tile 0 SFB chunk = SF for KV rows 0..127, h0 b0
        // = plainK[0 .. 128*4). Compare as sorted multisets.
        auto hist = [](const uint8_t* p, int n){
            std::vector<int> h(256,0); for(int i=0;i<n;++i) h[p[i]]++; return h; };
        auto dump = [&](const char* nm, const uint8_t* smem, const uint8_t* plain, int n){
            auto hs = hist(smem,n), hp = hist(plain,n);
            int diff = 0; for(int v=0;v<256;++v) diff += abs(hs[v]-hp[v]);
            printf("[dbg] %s: smem[0:8]=", nm);
            for(int i=0;i<8;++i) printf("%d ", smem[i]);
            printf(" plain[0:8]=");
            for(int i=0;i<8;++i) printf("%d ", plain[i]);
            printf(" multiset-diff=%d\n", diff);
            int vmin=255,vmax=0; for(int i=0;i<n;++i){vmin=std::min(vmin,(int)smem[i]);vmax=std::max(vmax,(int)smem[i]);}
            printf("[dbg] %s: smem range [%d,%d]\n", nm, vmin, vmax);
        };
        dump("SFK", dsfk.data(), plainK.data(), 128*(D/SF_VEC));
        dump("SFQ", dsfq.data(), plainQ.data(), 128*(D/SF_VEC));
        // [续19j] smem_sfv (V-SF) vs repacked dSFV. The smem holds StageCountKV stages;
        // multiset compare the whole 2048 region to dSFV (covers >= the loaded tiles).
        {
            std::vector<uint8_t> dsfv(2048);
            CUDA_CHECK(cudaMemcpyFromSymbol(dsfv.data(), g_dbg_sfv, 2048));
            size_t cmpn = std::min((size_t)2048, sfv_cosize);
            std::vector<uint8_t> rep(cmpn);
            CUDA_CHECK(cudaMemcpy(rep.data(), dSFV, cmpn, cudaMemcpyDeviceToHost));
            std::vector<int> hs(256,0), hp(256,0);
            for (size_t i=0;i<cmpn;++i){ hs[dsfv[i]]++; hp[rep[i]]++; }
            int diff=0; for(int v=0;v<256;++v) diff += abs(hs[v]-hp[v]);
            int nz=0; for(size_t i=0;i<2048;++i) if(dsfv[i]) ++nz;
            printf("[dbg] SFV: smem nonzero=%d/2048  multiset-diff(vs dSFV[0:%zu])=%d\n", nz, cmpn, diff);
            for (int st=0; st<4; ++st){ int c=0; for(int i=st*512;i<st*512+512;++i) if(dsfv[i])++c; printf("[dbg]   SFV stage %d (KV-slot): nonzero=%d/512  [0:6]=", st, c); for(int i=st*512;i<st*512+6;++i) printf("%d ", dsfv[i]); printf("\n"); }
            printf("[dbg]   dSFV[0:6]="); for(int i=0;i<6 && (size_t)i<cmpn;++i) printf("%d ", rep[i]);
            printf(" dSFV[512:518]="); for(int i=512;i<518 && (size_t)i<cmpn;++i) printf("%d ", rep[i]); printf("\n");
        }
        // [续19d] ELEMENTWISE compare leader's SFB smem (tile 0) to the REPACKED
        // dSFB the TMA actually loads. Reports which byte-offsets differ -> maps to
        // n-region. If upper-half (n=64-127) differs -> cluster-aware partition is
        // N-SPLITTING the SFB (leader only got n=0-63; needs full-N broadcast).
        {
            int nsf = 512;   // tile 0 = SF for kv 0..127 x 4 kblocks (128*4)
            std::vector<uint8_t> rep(nsf);
            CUDA_CHECK(cudaMemcpy(rep.data(), dSFB, nsf, cudaMemcpyDeviceToHost));
            int nd = 0, first_lo = -1, first_hi = -1, lo_diff = 0, hi_diff = 0;
            for (int i = 0; i < nsf; ++i) {
                if (dsfk[i] != rep[i]) { ++nd; if (i < 256) { ++lo_diff; if (first_lo<0) first_lo=i; } else { ++hi_diff; if (first_hi<0) first_hi=i; } }
            }
            printf("[dbg] SFB smem-vs-repacked(dSFB) elementwise: total_diff=%d  lo[0:256]=%d (1st@%d)  hi[256:512]=%d (1st@%d)\n",
                   nd, lo_diff, first_lo, hi_diff, first_hi);
            printf("[dbg]   smem[252:260]="); for(int i=252;i<260;++i) printf("%d ", dsfk[i]);
            printf("\n[dbg]   dSFB[252:260]="); for(int i=252;i<260;++i) printf("%d ", rep[i]); printf("\n");
        }
    }
#endif

    // ---- benchmark ----------------------------------------------------------
    // FMHA flops: 4 * B * H * S * S * D  (QK + PV, MHA, no mask)
    const double flops = 4.0 * B * H * (double)S * S * D;
    if (!quick) {
        // (a) plain back-to-back launch loop — includes per-launch host overhead.
        const int warmup = 10, iters = 50;
        for (int i = 0; i < warmup; ++i) op.run();
        CUDA_CHECK(cudaDeviceSynchronize());
        cudaEvent_t e0, e1;
        cudaEventCreate(&e0); cudaEventCreate(&e1);
        cudaEventRecord(e0);
        for (int i = 0; i < iters; ++i) op.run();
        cudaEventRecord(e1);
        cudaEventSynchronize(e1);
        float ms = 0; cudaEventElapsedTime(&ms, e0, e1);
        ms /= iters;
        printf("[fmha_mxfp8] loop : %.4f ms/iter   %.1f TFLOPS  (incl. launch overhead)\n",
               ms, flops * 1e-12 / (ms * 1e-3));
        cudaEventDestroy(e0); cudaEventDestroy(e1);
    }
    {
        // (c) single-launch timing — one kernel per measurement, take the min.
        const int solo_warmup = quick ? 3 : 10, solo_iters = quick ? 8 : 50;
        for (int i = 0; i < solo_warmup; ++i) op.run();
        CUDA_CHECK(cudaDeviceSynchronize());
        cudaEvent_t e0, e1;
        cudaEventCreate(&e0); cudaEventCreate(&e1);
        double best_ms = 1e30;
        for (int i = 0; i < solo_iters; ++i) {
            cudaEventRecord(e0);
            op.run();
            cudaEventRecord(e1);
            cudaEventSynchronize(e1);
            float ms = 0; cudaEventElapsedTime(&ms, e0, e1);
            if (ms < best_ms) best_ms = ms;
        }
        printf("[fmha_mxfp8] solo : best %.4f ms/iter   %.1f TFLOPS  (single launch)\n",
               best_ms, flops * 1e-12 / (best_ms * 1e-3));
        cudaEventDestroy(e0); cudaEventDestroy(e1);
    }
    if (!quick) {
        // (b) CUDA-Graph timing — real kernel time, launch overhead amortised.
        cudaStream_t stream;
        CUDA_CHECK(cudaStreamCreate(&stream));
        const int graph_iters = 20, measure = 10;
        for (int i = 0; i < 10; ++i) op.run(stream);
        CUDA_CHECK(cudaStreamSynchronize(stream));

        cudaGraph_t graph; cudaGraphExec_t graphExec;
        CUDA_CHECK(cudaStreamBeginCapture(stream, cudaStreamCaptureModeGlobal));
        for (int i = 0; i < graph_iters; ++i) op.run(stream);
        CUDA_CHECK(cudaStreamEndCapture(stream, &graph));
        CUDA_CHECK(cudaGraphInstantiate(&graphExec, graph, nullptr, nullptr, 0));
        CUDA_CHECK(cudaGraphLaunch(graphExec, stream));   // graph warmup
        CUDA_CHECK(cudaStreamSynchronize(stream));

        cudaEvent_t e0, e1;
        cudaEventCreate(&e0); cudaEventCreate(&e1);
        double best_ms = 1e30, sum_ms = 0.0;
        for (int m = 0; m < measure; ++m) {
            cudaEventRecord(e0, stream);
            CUDA_CHECK(cudaGraphLaunch(graphExec, stream));
            cudaEventRecord(e1, stream);
            cudaEventSynchronize(e1);
            float ms = 0; cudaEventElapsedTime(&ms, e0, e1);
            ms /= graph_iters;
            if (ms < best_ms) best_ms = ms;
            sum_ms += ms;
        }
        double avg_ms = sum_ms / measure;
        printf("[fmha_mxfp8] graph: avg %.4f ms  best %.4f ms/iter   %.1f TFLOPS (best %.1f)\n",
               avg_ms, best_ms, flops * 1e-12 / (avg_ms * 1e-3),
               flops * 1e-12 / (best_ms * 1e-3));
        cudaEventDestroy(e0); cudaEventDestroy(e1);
        cudaGraphExecDestroy(graphExec); cudaGraphDestroy(graph);
        cudaStreamDestroy(stream);
    }

    // ---- optional I/O dump for external (torch SDPA) golden ----------------
    // Writes the dequantized fp32 Q/K/V (the exact values the kernel consumed)
    // and the GPU output O. Skips the memory-heavy fp32 reference, so it works
    // on the huge SOW shapes (S=170100/510300) where do_verify is impossible.
    if (dump_prefix) {
        float *dQd,*dKd,*dVd;
        CUDA_CHECK(cudaMalloc(&dQd,n_q*sizeof(float)));
        CUDA_CHECK(cudaMalloc(&dKd,n_k*sizeof(float)));
        CUDA_CHECK(cudaMalloc(&dVd,n_v*sizeof(float)));
        int th=256;
        dequantize_mxfp8_kernel<<<(int)((n_q+th-1)/th),th>>>(qQ.data,qQ.sf,dQd,rows_q,D);
        dequantize_mxfp8_kernel<<<(int)((n_k+th-1)/th),th>>>(qK.data,qK.sf,dKd,rows_k,D);
        // [precision-verify fix] apply V's per-block SF (was e4m3_to_f32 = data only, ~128x off)
        dequantize_v_mxfp8_kernel<<<(int)((n_v+th-1)/th),th>>>(dV,dV_sf_plain,dVd,B,H,S,D);
        CUDA_CHECK(cudaDeviceSynchronize());
        std::vector<float> hQ(n_q),hK(n_k),hV(n_v),hOf(n_q);
        std::vector<ElementOut> hOd(n_q);
        CUDA_CHECK(cudaMemcpy(hQ.data(),dQd,n_q*4,cudaMemcpyDeviceToHost));
        CUDA_CHECK(cudaMemcpy(hK.data(),dKd,n_k*4,cudaMemcpyDeviceToHost));
        CUDA_CHECK(cudaMemcpy(hV.data(),dVd,n_v*4,cudaMemcpyDeviceToHost));
        CUDA_CHECK(cudaMemcpy(hOd.data(),dO,(size_t)n_q*sizeof(ElementOut),cudaMemcpyDeviceToHost));
        for(long i=0;i<n_q;++i) hOf[i]=float(hOd[i]);
        std::string p(dump_prefix);
        auto wr=[&](const char*suf,const void*d,size_t b){FILE*f=fopen((p+suf).c_str(),"wb");fwrite(d,1,b,f);fclose(f);};
        wr(".q",hQ.data(),(size_t)n_q*4); wr(".k",hK.data(),(size_t)n_k*4);
        wr(".v",hV.data(),(size_t)n_v*4); wr(".o",hOf.data(),(size_t)n_q*4);
        {FILE*f=fopen((p+".meta").c_str(),"w");fprintf(f,"%d %d %d %d\n",B,H,S,D);fclose(f);}
        printf("[dump] wrote %s.{q,k,v,o}+.meta (B=%d H=%d S=%d D=%d)\n",dump_prefix,B,H,S,D);
        cudaFree(dQd);cudaFree(dKd);cudaFree(dVd);
    }

    // ---- verify vs FP32 reference ------------------------------------------
    // Dequantize the *exact* kernel inputs back to fp32 and run example 77's
    // fp32 reference; tolerance absorbs e4m3 quantization of P in the PV GEMM.
    bool verified = false;
    if (do_verify) {
        float *dQ_deq, *dK_deq, *dV_deq;
        CUDA_CHECK(cudaMalloc(&dQ_deq, n_q*sizeof(float)));
        CUDA_CHECK(cudaMalloc(&dK_deq, n_k*sizeof(float)));
        CUDA_CHECK(cudaMalloc(&dV_deq, n_v*sizeof(float)));
        int th = 256;
        dequantize_mxfp8_kernel<<<(int)((n_q+th-1)/th), th>>>(qQ.data, qQ.sf, dQ_deq, rows_q, D);
        dequantize_mxfp8_kernel<<<(int)((n_k+th-1)/th), th>>>(qK.data, qK.sf, dK_deq, rows_k, D);
        dequantize_v_mxfp8_kernel<<<(int)((n_v+th-1)/th), th>>>(dV, dV_sf_plain, dV_deq, B, H, S, D);
        CUDA_CHECK(cudaGetLastError());

        float* dO_ref; CUDA_CHECK(cudaMalloc(&dO_ref, n_q*sizeof(float)));

        auto HB = get<3>(problem_shape);
        auto problem_ref = cute::make_tuple(S, S, D, D, HB);
        Tensor mQ_ref = make_tensor(make_gmem_ptr(dQ_deq), select<0,2,3>(problem_shape), stride_Q);
        Tensor mK_ref = make_tensor(make_gmem_ptr(dK_deq), select<1,2,3>(problem_shape), stride_K);
        Tensor mV_ref = make_tensor(make_gmem_ptr(dV_deq), select<1,2,3>(problem_shape), stride_V);
        Tensor mO_ref = make_tensor(make_gmem_ptr(dO_ref), select<0,2,3>(problem_shape), stride_O);
        Tensor mLSE_ref = make_tensor(make_gmem_ptr(static_cast<float*>(nullptr)),
                                      select<0,3>(problem_shape), stride_LSE);

        fmha_reference(problem_ref, mQ_ref, mK_ref, mV_ref, mO_ref, mLSE_ref,
                       cutlass::fmha::collective::NoMask{});
        CUDA_CHECK(cudaDeviceSynchronize());

#ifdef MXFP8_DBG
        // Compare the kernel's raw QK scores (Q-row 0, KV 0..63) to a reference
        // computed from the same dequantized inputs.
        {
            float dbgS[256]; int got = 0;
            int nkvS = (S < 256 ? (int)S : 256);
            CUDA_CHECK(cudaMemcpyFromSymbol(dbgS, g_dbg_S, sizeof(dbgS)));
            CUDA_CHECK(cudaMemcpyFromSymbol(&got, g_dbg_S_got, sizeof(int)));
            std::vector<float> q0(D), kref(nkvS*D);
            CUDA_CHECK(cudaMemcpy(q0.data(), dQ_deq, D*sizeof(float), cudaMemcpyDeviceToHost));
            CUDA_CHECK(cudaMemcpy(kref.data(), dK_deq, nkvS*D*sizeof(float), cudaMemcpyDeviceToHost));
            printf("[dbg] QK scores got=%d  kv : kernel  ref  diff   (FULL 0..%d incl g1 half)\n", got, nkvS-1);
            double maxd = 0, maxd_g1 = 0;
            for (int kv = 0; kv < nkvS; ++kv) {
                double acc = 0;
                for (int d = 0; d < D; ++d) acc += (double)q0[d]*kref[kv*D+d];
                double diff = fabs(acc - dbgS[kv]);
                maxd = fmax(maxd, diff);
                if (kv >= 64) maxd_g1 = fmax(maxd_g1, diff);
                if (diff > 0.05 || kv < 4 || (kv>=64 && kv<68)) printf("[dbg]  kv=%2d : %9.3f  %9.3f  %8.4f %s\n",
                                    kv, dbgS[kv], acc, diff, diff>0.05?"<<BAD":"");
            }
            printf("[dbg] QK max|diff| over %d = %.4f   (g1 half kv>=64 max|diff| = %.4f)\n", nkvS, maxd, maxd_g1);
            // [续19k] PEER QK: g_dbg_Sp[kv] = peer S for Q row 128. ref = q128·K[kv].
            if (S >= 256) {
                float dbgSp[256]; int sprow=0; CUDA_CHECK(cudaMemcpyFromSymbol(dbgSp, g_dbg_Sp, sizeof(dbgSp)));
                CUDA_CHECK(cudaMemcpyFromSymbol(&sprow, g_dbg_Sp_row, sizeof(int)));
                printf("[dbg] PEER coordinate-row seen = %d (peer local row0 = Q row 128)\n", sprow);
                { int qnz=0,qsz=0; CUDA_CHECK(cudaMemcpyFromSymbol(&qnz,g_dbg_qnz,sizeof(int))); CUDA_CHECK(cudaMemcpyFromSymbol(&qsz,g_dbg_qsz,sizeof(int)));
                  printf("[dbg] PEER smem_q nonzero=%d/%d  -> if ~half nonzero: peer Q LOADED (bug is MMA not computing peer); if 0: peer Q-load bug\n", qnz, qsz); }
                { int sv[8]; CUDA_CHECK(cudaMemcpyFromSymbol(sv,g_dbg_sbuf_val,sizeof(sv)));
                  printf("[dbg] tile0 sbuf: LEADER(blk0)=buf%d  PEER(blk1)=buf%d  %s\n", sv[0], sv[1], sv[0]==sv[1]?"(match)":"<<DESYNC! peer reads empty buffer -> S=0"); }
                { int nzq=0,szq=0,nzk=0; CUDA_CHECK(cudaMemcpyFromSymbol(&nzq,g_dbg_sfq_peer_nz,sizeof(int))); CUDA_CHECK(cudaMemcpyFromSymbol(&szq,g_dbg_sfq_peer_sz,sizeof(int))); CUDA_CHECK(cudaMemcpyFromSymbol(&nzk,g_dbg_sfk_peer_nz,sizeof(int)));
                  printf("[dbg] PEER smem_sfq(Q-scale) nz=%d/%d  smem_sfk(K-scale) nz=%d  %s\n", nzq, szq, nzk, nzq==0?"<<SFA(Q-scale)=0 -> peer S=Q*K*2^-127~0 !!":"(SFA loaded)"); }
                { unsigned long long qs[2]={0,0}; CUDA_CHECK(cudaMemcpyFromSymbol(qs,g_dbg_qsum,sizeof(qs)));
                  printf("[dbg] smem_q sum LEADER=%llu PEER=%llu  %s\n", qs[0], qs[1], qs[0]==qs[1]?"<<EQUAL -> peer Q is m0-127 DUPLICATE (A/M-split broken)":"(differ -> peer Q = m128-255 OK; bug is SFA-only)"); }
                { unsigned int tb[2]={0xffffffff,0xffffffff}; CUDA_CHECK(cudaMemcpyFromSymbol(tb,g_dbg_tmem_base,sizeof(tb)));
                  printf("[dbg] tmem_base_ptr LEADER=%u PEER=%u  %s\n", tb[0], tb[1], (tb[0]==tb[1])?"(match -> base-relative refactor won't help)":"<<DIFFER! peer reads base+S0 but MMA wrote leader-abs S0 -> peer S=0 ROOT CAUSE"); }
                std::vector<float> q128(D);
                CUDA_CHECK(cudaMemcpy(q128.data(), dQ_deq + 128*D, D*sizeof(float), cudaMemcpyDeviceToHost));
                double maxdp=0; int nbp=0;
                for (int kv=0; kv<nkvS; ++kv){ double a=0; for(int d=0;d<D;++d) a+=(double)q128[d]*kref[kv*D+d]; double diff=fabs(a-dbgSp[kv]); if(diff>maxdp)maxdp=diff; if(diff>0.05){++nbp; if(nbp<=6)printf("[dbg]  PEER kv=%3d : k=%.3f ref=%.3f\n",kv,(double)dbgSp[kv],a);} }
                printf("[dbg] PEER QK (Q row128) max|diff| over %d = %.4f (bad>0.05: %d)\n", nkvS, maxdp, nbp);
            }

            // Non-circular check: recompute QK from raw e4m3 bytes + SF, in the
            // MMA's per-d-block-then-scale order, with a host e4m3 decoder.
            {
                std::vector<uint8_t> qd(D), kd(64*D), qsf(D/SF_VEC), ksf(64*(D/SF_VEC));
                CUDA_CHECK(cudaMemcpy(qd.data(),  qQ.data, D, cudaMemcpyDeviceToHost));
                CUDA_CHECK(cudaMemcpy(kd.data(),  qK.data, 64*D, cudaMemcpyDeviceToHost));
                CUDA_CHECK(cudaMemcpy(qsf.data(), qQ.sf, D/SF_VEC, cudaMemcpyDeviceToHost));
                CUDA_CHECK(cudaMemcpy(ksf.data(), qK.sf, 64*(D/SF_VEC), cudaMemcpyDeviceToHost));
                auto h_e4m3 = [](uint8_t b)->double {
                    int s=(b>>7)&1, e=(b>>3)&0xF, m=b&7;
                    double v = (e==0) ? (m/8.0)*ldexp(1.0,-6)
                                      : (1.0+m/8.0)*ldexp(1.0,e-7);
                    return s ? -v : v;
                };
                int NB = D/SF_VEC; double maxk = 0;
                printf("[dbg] kernel-order QK (raw e4m3+SF):  kv : kernel  recompute  diff\n");
                for (int kv = 0; kv < 64; ++kv) {
                    double s = 0;
                    for (int db = 0; db < NB; ++db) {
                        double dot = 0;
                        for (int d = db*SF_VEC; d < db*SF_VEC+SF_VEC; ++d)
                            dot += h_e4m3(qd[d]) * h_e4m3(kd[kv*D+d]);
                        double sA = ldexp(1.0, (int)qsf[db]-127);
                        double sB = ldexp(1.0, (int)ksf[kv*NB+db]-127);
                        s += sA*sB*dot;
                    }
                    double diff = fabs(s - dbgS[kv]);
                    maxk = fmax(maxk, diff);
                    if (kv < 12) printf("[dbg]  kv=%2d : %9.3f  %9.3f  %8.4f\n",
                                        kv, dbgS[kv], s, diff);
                }
                printf("[dbg] kernel-order QK max|diff| over 64 = %.4f\n", maxk);
            }

            // Probe: for each kv, search which SFB-row r the MMA effectively
            // used.  S_kernel[0,kv] under "MMA used SFB row r" =
            //   sum_db (SFB[r,db]/SFB[kv,db]) * partialdot_deq(0,kv,db)
            std::vector<uint8_t> sfbK(128*(D/SF_VEC));
            CUDA_CHECK(cudaMemcpy(sfbK.data(), qK.sf, sfbK.size(), cudaMemcpyDeviceToHost));
            int NB = D/SF_VEC;
            printf("[dbg] SFB-row probe (kv -> best-fit r; search r 0..%d):\n", nkvS-1);
            int probe_kv[] = {0,1,2,3, 64,65,66,67,68,69,74,75};
            for (int pi = 0; pi < (int)(sizeof(probe_kv)/sizeof(int)); ++pi) {
                int kv = probe_kv[pi]; if (kv >= nkvS) continue;
                double pd[8] = {0};
                for (int db = 0; db < NB; ++db)
                    for (int d = db*SF_VEC; d < db*SF_VEC+SF_VEC; ++d)
                        pd[db] += (double)q0[d]*kref[kv*D+d];
                int best_r = -1; double best_err = 1e30;
                for (int r = 0; r < nkvS; ++r) {
                    double hyp = 0;
                    for (int db = 0; db < NB; ++db) {
                        double ratio = ldexp(1.0, (int)sfbK[r*NB+db] - (int)sfbK[kv*NB+db]);
                        hyp += ratio * pd[db];
                    }
                    double err = fabs(hyp - dbgS[kv]);
                    if (err < best_err) { best_err = err; best_r = r; }
                }
                printf("[dbg]  kv=%2d -> r=%2d  (err %.4f)\n", kv, best_r, best_err);
            }
#ifdef MXFP8_PROBE
            float dbgSrow[64];
            CUDA_CHECK(cudaMemcpyFromSymbol(dbgSrow, g_dbg_Srow, sizeof(dbgSrow)));
  #ifdef MXFP8_PROBE_DBLK
            // S[0,0] = 32 * sum_j 2^(3*sj + dj).  Correct (sj=dj=j) => 139808.
            // Decompose S/32 into 4 powers of two -> the exponents 3sj+dj.
            {
                long v = (long)llround(dbgS[0] / 32.0);
                printf("[probe] d-block: S[0,0]=%.1f  S/32=%ld  (correct 4369 => exps 0,4,8,12)\n",
                       dbgS[0], v);
                printf("[probe] d-block exps present:");
                for (int e = 0; e < 20; ++e) if (v & (1L<<e)) printf(" %d", e);
                printf("\n[probe] %s\n", (llround(dbgS[0])==139808) ? "D-BLOCK PAIRING OK"
                                                                    : "<-- D-BLOCK MISPAIR");
            }
  #elif defined MXFP8_PROBE_JOINT
            // S[0,n]/32 = sum_db 2^(4*db + w), w = 2 bits of the sourced row.
            // Decode each d-block's 4-bit nibble -> recover the row the MMA used.
            printf("[probe] JOINT decode: n -> per-d-block sourced row"
                   "  [identity => nsrc==n]\n");
            {
                int njmis = 0;
                for (int n = 0; n < 64; ++n) {
                    double sc = dbgS[n];
                    long v = (long)llround(sc / 32.0);
                    int wd[4], nsrc = 0; bool clean = true;
                    for (int db = 0; db < 4; ++db) {
                        int nib = (int)((v >> (4*db)) & 0xF);
                        int w = -1;
                        for (int c = 0; c < 4; ++c) if (nib == (1<<c)) w = c;
                        if (w < 0) clean = false;
                        wd[db] = w;
                        if (w >= 0) nsrc |= (w << (2*db));
                    }
                    bool ok = clean && (nsrc == n);
                    if (!ok) ++njmis;
                    if (n < 32)
                        printf("[probe]  n=%2d : S=%.8g  v=0x%05lx  w=[%d %d %d %d]"
                               "  nsrc=%d  %s\n",
                               n, sc, v, wd[0], wd[1], wd[2], wd[3], nsrc,
                               ok ? "OK" : (clean ? "<-- WRONG ROW" : "<-- UNCLEAN"));
                }
                printf("[probe] JOINT total mis (of 64) = %d\n", njmis);
            }
  #else
    // SF byte = 100 + (row%128).  Single SF axis: S = 2^((row%128) - 20).
    #ifdef MXFP8_PROBE_SFA
            printf("[probe] q -> decoded SFA row used  [identity => decoded==q]\n");
            const float* probe_arr = dbgSrow; const char* probe_lbl = "q";
    #else
            printf("[probe] kv -> decoded SFB row used  [identity => decoded==kv]\n");
            const float* probe_arr = dbgS;    const char* probe_lbl = "kv";
    #endif
            int nmis = 0;
            for (int r = 0; r < 64; ++r) {
                double s = probe_arr[r];
                int dec = (s > 0) ? (int)lround(log2(s) + 20.0) : -99;
                bool ok = (dec == r);
                if (!ok) ++nmis;
                if (r < 32)
                    printf("[probe]  %s=%2d : S=%.5g  decoded(row used)=%d  %s\n",
                           probe_lbl, r, s, dec, ok ? "OK" : "<-- MISINDEX");
            }
            printf("[probe] total misindexed rows (of 64) = %d\n", nmis);
  #endif
#endif
        }
#endif

        std::vector<ElementOut> hO(n_q);
        std::vector<float>      hO_ref(n_q);
        CUDA_CHECK(cudaMemcpy(hO.data(), dO, n_q*sizeof(ElementOut), cudaMemcpyDeviceToHost));
        CUDA_CHECK(cudaMemcpy(hO_ref.data(), dO_ref, n_q*sizeof(float), cudaMemcpyDeviceToHost));

        double max_abs = 0, mean_abs = 0;
        long   argmax = 0;
        std::vector<double> head_max(H, 0.0);
        long n_bad = 0;
        // [续19k] per-CTA-half breakdown (leader rows s%256<128 vs peer s%256>=128)
        long bad_lead=0, bad_peer=0; double mx_lead=0, mx_peer=0;
        for (long i = 0; i < n_q; ++i) {
            double d = fabs((double)float(hO[i]) - (double)hO_ref[i]);
            if (d > max_abs) { max_abs = d; argmax = i; }
            mean_abs += d;
            if (d > 1e-1) ++n_bad;
            long row = i / D; int hh = (int)((row / S) % H);
            head_max[hh] = fmax(head_max[hh], d);
            int srow = (int)(row % S);
            if ((srow % 256) < 128) { if(d>0.1)++bad_lead; mx_lead=fmax(mx_lead,d); }
            else                    { if(d>0.1)++bad_peer; mx_peer=fmax(mx_peer,d); }
        }
        printf("[fmha_mxfp8] per-half: LEADER(s%%256<128) bad=%ld max=%.4f | PEER(>=128) bad=%ld max=%.4f\n", bad_lead, mx_lead, bad_peer, mx_peer);
        mean_abs /= n_q;
        {   // localize the worst element
            long row = argmax / D; int dd = (int)(argmax % D);
            int s = (int)(row % S), hh = (int)((row / S) % H), bb = (int)(row / ((long)S*H));
            printf("[fmha_mxfp8] worst @ b=%d h=%d s=%d d=%d : kernel=%.4f ref=%.4f\n",
                   bb, hh, s, dd, float(hO[argmax]), hO_ref[argmax]);
            printf("[fmha_mxfp8] bad(>0.1)=%ld / %ld (%.3f%%)  per-head max:",
                   n_bad, n_q, 100.0*n_bad/n_q);
            for (int hh2 = 0; hh2 < H; ++hh2) printf(" h%d=%.3f", hh2, head_max[hh2]);
            printf("\n");
        }
        // e4m3 P-quantization in PV => ~1e-1 max diff (matches example 77 fp8).
        const double kMaxThresh = 1e-1, kMeanThresh = 5e-2;
        verified = (max_abs < kMaxThresh) && (mean_abs < kMeanThresh);
        printf("[fmha_mxfp8] verify vs FP32 ref:  max_abs=%.5f  mean_abs=%.5f  [%s]\n",
               max_abs, mean_abs, verified ? "OK" : "FAIL");

#ifdef MXFP8_DBG
        // [续19c] O for (b0,h0,q-row0): kernel-captured (g_dbg_O, pre-store in
        // correction_epilogue) vs final stored hO vs FP32 ref hO_ref. Row 0 maps
        // to dO_ref index = d (i = row*D+d, row=0). Localizes the PV/O error by d.
        {
            float dbgO[128]; int gotO = 0;
            CUDA_CHECK(cudaMemcpyFromSymbol(dbgO, g_dbg_O, sizeof(dbgO)));
            CUDA_CHECK(cudaMemcpyFromSymbol(&gotO, g_dbg_O_got, sizeof(int)));
            printf("[dbg] O row0 got=%d   d : g_dbg_O  hO  ref  |krnl-ref|\n", gotO);
            double mx = 0; int nbad = 0;
            for (int d = 0; d < D && d < 128; ++d) {
                double kr = (double)dbgO[d], st = (double)float(hO[d]), rf = (double)hO_ref[d];
                double diff = fabs(st - rf); if (diff > mx) mx = diff;
                if (diff > 0.05) { ++nbad;
                    if (nbad <= 40) printf("[dbg]  d=%3d : %9.4f  %9.4f  %9.4f  %8.4f <<BAD\n", d, kr, st, rf, diff); }
            }
            printf("[dbg] O row0 max|hO-ref| over %d = %.4f  (bad>0.05: %d)\n", D, mx, nbad);
            // [续19c] test d/d+64 aliasing in the PV output (O[d]==O[d+64]?).
            int alias = 0; printf("[dbg] O[d]==O[d+64] alias d:");
            for (int d = 0; d + 64 < D && d < 64; ++d)
                if (fabs((double)dbgO[d] - (double)dbgO[d+64]) < 1e-4) { ++alias; if (alias <= 32) printf(" %d", d); }
            printf("\n[dbg] alias count = %d / 64\n", alias);

            // [续19d] SPLIT softmax-vs-PV: recompute O[row0,d] from the kernel's OWN
            // raw exp weights P (g_dbg_P) + row_sum, times the dequantized V. If this
            // matches the true ref O -> the kernel's softmax P is CORRECT and the bug
            // is in the PV MMA / V / SFV. If it matches g_dbg_O (kernel O) instead ->
            // PV faithfully uses P and the error is in P/softmax.
            {
                float dbgP[256]; int gotP = 0; float rowsum = 0;
                CUDA_CHECK(cudaMemcpyFromSymbol(dbgP, g_dbg_P, sizeof(dbgP)));
                CUDA_CHECK(cudaMemcpyFromSymbol(&gotP, g_dbg_P_got, sizeof(int)));
                CUDA_CHECK(cudaMemcpyFromSymbol(&rowsum, g_dbg_rowsum, sizeof(float)));
                int nkv = (S < 128 ? (int)S : 128);
                std::vector<float> hV(nkv*D);
                CUDA_CHECK(cudaMemcpy(hV.data(), dV_deq, nkv*D*sizeof(float), cudaMemcpyDeviceToHost));
                printf("[dbg] P got=%d rowsum=%.4f   first 8 P: ", gotP, rowsum);
                for (int kv=0; kv<8; ++kv) printf("%.4f ", dbgP[kv]);
                printf("\n[dbg] O_from_kernelP vs ref vs kernelO:  d : fromP  ref  kernelO\n");
                double mxP=0, mxPK=0;
                for (int d=0; d<D && d<128; ++d) {
                    double acc=0;
                    for (int kv=0; kv<nkv; ++kv) acc += (double)dbgP[kv]*(double)hV[kv*D+d];
                    acc /= (rowsum==0?1.0:rowsum);
                    double dr = fabs(acc-(double)hO_ref[d]);   if (dr>mxP)  mxP=dr;
                    double dk = fabs(acc-(double)dbgO[d]);     if (dk>mxPK) mxPK=dk;
                    if (d<16) printf("[dbg]  d=%3d : %9.4f  %9.4f  %9.4f\n", d, acc, (double)hO_ref[d], (double)dbgO[d]);
                }
                printf("[dbg] O_from_kernelP : max|fromP-ref|=%.4f  max|fromP-kernelO|=%.4f\n", mxP, mxPK);
                printf("[dbg]   -> if fromP~ref: softmax OK, bug in PV.  if fromP~kernelO: bug in softmax/P.\n");

                // [续19e] V-INDEPENDENT per-tile softmax check: kernel raw-exp P uses
                // the ONLINE running max per tile (tile t's P = exp((S-max_after_t)/√D)),
                // so refP uses the per-128-tile running max. Tile 0 (kv0-127) uses
                // max over kv0-127; tile 1 (kv128-255) uses max over kv0-255 (global).
                // Verifies softmax P for BOTH tiles against the (exact) S. If tile-1 P
                // is wrong -> softmax bug; if right -> bug is in block-scaled PV SF.
                {
                    int nkvP = (S < 256 ? (int)S : 256);
                    std::vector<float> q0v(D), kall(nkvP*D);
                    CUDA_CHECK(cudaMemcpy(q0v.data(), dQ_deq, D*sizeof(float), cudaMemcpyDeviceToHost));
                    CUDA_CHECK(cudaMemcpy(kall.data(), dK_deq, nkvP*D*sizeof(float), cudaMemcpyDeviceToHost));
                    std::vector<double> rS(nkvP);
                    for (int kv=0; kv<nkvP; ++kv){ double a=0; for(int d=0;d<D;++d) a+=(double)q0v[d]*kall[kv*D+d]; rS[kv]=a; }
                    double sm = 1.0/sqrt((double)D);
                    // running max per 128-tile (online softmax)
                    auto run_max = [&](int kv)->double{ int up = ((kv/128)+1)*128; if(up>nkvP)up=nkvP; double m=-1e30; for(int j=0;j<up;++j) if(rS[j]>m)m=rS[j]; return m; };
                    double maxdP0=0,maxdP1=0; int nbad=0; printf("[dbg] P vs refP (per-tile online max):  kv : kernelP  refP  diff\n");
                    for (int kv=0; kv<nkvP; ++kv){
                        double rp = exp((rS[kv]-run_max(kv))*sm);
                        double diff = fabs((double)dbgP[kv]-rp);
                        if (kv<128){ if(diff>maxdP0)maxdP0=diff; } else { if(diff>maxdP1)maxdP1=diff; }
                        if (diff>0.02){ ++nbad; if(nbad<=12) printf("[dbg]  kv=%3d : %9.4f  %9.4f  %8.4f <<\n", kv, (double)dbgP[kv], rp, diff); }
                    }
                    printf("[dbg] P vs refP: tile0(kv<128) max|diff|=%.4f  tile1(kv>=128) max|diff|=%.4f  (bad>0.02: %d/%d)\n", maxdP0, maxdP1, nbad, nkvP);

                    // [续19f] correction cross-tile rescale check (row 0). For corr_tile t,
                    // old=max over kv[0..t*128), new=max over kv[0..(t+1)*128). scale should
                    // be exp2(slog2*(old-new)) = exp((old-new)/sqrt(D)). Verify kernel's scale.
                    {
                        float cs[8], co[8], cn[8]; int cnn=0;
                        CUDA_CHECK(cudaMemcpyFromSymbol(cs, g_dbg_corr_scale, sizeof(cs)));
                        CUDA_CHECK(cudaMemcpyFromSymbol(co, g_dbg_corr_old, sizeof(co)));
                        CUDA_CHECK(cudaMemcpyFromSymbol(cn, g_dbg_corr_new, sizeof(cn)));
                        CUDA_CHECK(cudaMemcpyFromSymbol(&cnn, g_dbg_corr_n, sizeof(int)));
                        printf("[dbg] correction rescale (n=%d):  t : kScale  refScale  kOld/kNew  refOld/refNew\n", cnn);
                        for (int t=1; t<cnn+1 && t*128 < nkvP; ++t) {
                            double mold=-1e30, mnew=-1e30;
                            for (int j=0;j<t*128 && j<nkvP;++j) mold=fmax(mold,rS[j]);
                            for (int j=0;j<(t+1)*128 && j<nkvP;++j) mnew=fmax(mnew,rS[j]);
                            double refscale = exp((mold-mnew)*sm);
                            printf("[dbg]  t=%d : %9.5f  %9.5f   %.3f/%.3f  %.3f/%.3f\n",
                                   t, (double)cs[t], refscale, (double)co[t], (double)cn[t], mold, mnew);
                        }
                    }

                    // [续19g] SFP exponent per global kv-block (row 0): kernel vs expected
                    // from raw P (exact). expected_exp = ceil(log2(amax_block/448)).
                    {
                        int sfpe[8]; CUDA_CHECK(cudaMemcpyFromSymbol(sfpe, g_dbg_sfp_exp, sizeof(sfpe)));
                        int nblk = (nkvP+31)/32; if (nblk>8) nblk=8;
                        printf("[dbg] SFP exp per kv-block (row0):  blk : kExp  refExp  amax\n");
                        int bad=0;
                        for (int g=0; g<nblk; ++g) {
                            double amax=0; for(int j=g*32;j<g*32+32 && j<nkvP;++j) amax=fmax(amax,(double)dbgP[j]);
                            int refe = (amax==0.0)? -127 : (int)ceil(log2(amax/448.0));
                            if (refe<-127)refe=-127; if(refe>127)refe=127;
                            if (sfpe[g]!=refe) ++bad;
                            printf("[dbg]  blk=%d : %4d  %4d   %.4f %s\n", g, sfpe[g], refe, amax, sfpe[g]!=refe?"<<BAD":"");
                        }
                        printf("[dbg] SFP exp mismatches: %d / %d blocks\n", bad, nblk);
                    }

                    // [续19g] dequantized smem_p (e4m3×2^exp) vs raw P — the P data the PV
                    // reads directly. Tests the e4m3 quant + smem_p buffer per tile.
                    {
                        float pdq[256]; CUDA_CHECK(cudaMemcpyFromSymbol(pdq, g_dbg_Pdq, sizeof(pdq)));
                        double m0=0,m1=0; int nb=0;
                        for (int kv=0; kv<nkvP; ++kv){
                            double diff=fabs((double)pdq[kv]-(double)dbgP[kv]);
                            if(kv<128){if(diff>m0)m0=diff;}else{if(diff>m1)m1=diff;}
                            if(diff>0.03){++nb; if(nb<=8) printf("[dbg]  Pdq kv=%3d : dq=%.4f raw=%.4f diff=%.4f <<\n",kv,(double)pdq[kv],(double)dbgP[kv],diff);}
                        }
                        printf("[dbg] smem_p dequant vs rawP: tile0 max|diff|=%.4f tile1 max|diff|=%.4f (bad>0.03: %d) [e4m3 quant err ~0.02 expected]\n", m0, m1, nb);
                    }

                    // [续19h] O after PV(0) (un-normalized tile-0 accumulator, row0) vs
                    // ref = Σ_kv0-127 rawP0[kv]·V_ref[kv,d]. If this MATCHES, PV(0) is
                    // correct and the multi-tile corruption is in PV(1)-accumulate /
                    // correction. (rawP0 = exact; uses dequant V via dV_deq.)
                    if (S >= 256) {
                        float oa[128]; CUDA_CHECK(cudaMemcpyFromSymbol(oa, g_dbg_OafterPV0, sizeof(oa)));
                        // running max for tile0 = max over kv0-127 (rawP0 in dbgP[0..127])
                        double mx=0; int nb2=0;
                        printf("[dbg] O-after-PV0 vs tile0-ref (un-normalized):  d : kO  ref  diff\n");
                        for (int d=0; d<D && d<128; ++d) {
                            double acc=0; for (int kv=0; kv<128; ++kv) acc += (double)dbgP[kv]*(double)hV[kv*D+d];
                            double diff=fabs((double)oa[d]-acc); if(diff>mx)mx=diff;
                            if (diff>0.05){ ++nb2; if(nb2<=8) printf("[dbg]  d=%3d : %9.4f  %9.4f  %8.4f <<\n", d, (double)oa[d], acc, diff); }
                        }
                        printf("[dbg] O-after-PV0 max|diff|=%.4f (bad>0.05: %d) -> if ~0: PV(0) OK, bug in PV(1)/correction\n", mx, nb2);
                    }

                    // [续19h] O before normalize (full PV(0)+PV(1)) vs ref = Σ_kv0-255 rawP·V
                    // (rescale=1.0 here). If MATCHES -> PV accumulate OK, bug=rowsum-normalize.
                    // If NOT -> PV(1) accumulate corrupts O.
                    if (S >= 256) {
                        float ob[128]; CUDA_CHECK(cudaMemcpyFromSymbol(ob, g_dbg_Obn, sizeof(ob)));
                        std::vector<float> hV2(nkvP*D);
                        CUDA_CHECK(cudaMemcpy(hV2.data(), dV_deq, nkvP*D*sizeof(float), cudaMemcpyDeviceToHost));
                        double mx=0; int nb3=0;
                        printf("[dbg] O-before-norm vs full-ref (un-norm):  d : kO  ref  diff\n");
                        for (int d=0; d<D && d<128; ++d) {
                            double acc=0; for (int kv=0; kv<nkvP; ++kv) acc += (double)dbgP[kv]*(double)hV2[kv*D+d];
                            double diff=fabs((double)ob[d]-acc); if(diff>mx)mx=diff;
                            if (diff>0.1){ ++nb3; if(nb3<=8) printf("[dbg]  d=%3d : %9.4f  %9.4f  %8.4f <<\n", d, (double)ob[d], acc, diff); }
                        }
                        printf("[dbg] O-before-norm max|diff|=%.4f (bad>0.1: %d) -> if ~0: PV accum OK (bug=normalize); else PV(1) corrupts\n", mx, nb3);
                    }

                    // [续19h] correction RMW round-trip: O re-read after copy_out vs copy_in
                    // (g_dbg_OafterPV0). With scale=1.0 MUST match. If not -> copy_out scrambles O.
                    if (S >= 256) {
                        float oa[128], orr[128];
                        CUDA_CHECK(cudaMemcpyFromSymbol(oa, g_dbg_OafterPV0, sizeof(oa)));
                        CUDA_CHECK(cudaMemcpyFromSymbol(orr, g_dbg_Orr, sizeof(orr)));
                        double mx=0; int nb=0;
                        for (int d=0; d<D && d<128; ++d){ double diff=fabs((double)oa[d]-(double)orr[d]); if(diff>mx)mx=diff; if(diff>1e-4){++nb; if(nb<=8)printf("[dbg]  RMW d=%3d : in=%.4f out=%.4f\n",d,(double)oa[d],(double)orr[d]);}}
                        printf("[dbg] correction RMW round-trip max|diff|=%.4f (mismatch: %d) -> if >0: copy_out scrambles O\n", mx, nb);
                    }

                    // [续19i] which V KV-stage + SF buffer each PV used. PV(0) and PV(1)
                    // must use DIFFERENT vidx (V0 vs V1) and buf (0 vs 1). If PV(1) reuses
                    // PV(0)'s vidx -> it accumulates P1@V0 (wrong) = the 4.12 corruption.
                    {
                        int vidx[8], buf[8], first[8], npv=0;
                        CUDA_CHECK(cudaMemcpyFromSymbol(vidx, g_dbg_pv_vidx, sizeof(vidx)));
                        CUDA_CHECK(cudaMemcpyFromSymbol(buf, g_dbg_pv_buf, sizeof(buf)));
                        CUDA_CHECK(cudaMemcpyFromSymbol(first, g_dbg_pv_first, sizeof(first)));
                        CUDA_CHECK(cudaMemcpyFromSymbol(&npv, g_dbg_npv, sizeof(int)));
                        printf("[dbg] PV calls (n=%d):  pv : vidx  buf  first\n", npv);
                        for (int p=0; p<npv && p<8; ++p) printf("[dbg]  pv=%d : vidx=%d buf=%d first=%d\n", p, vidx[p], buf[p], first[p]);
                    }
                }
            }
        }
#endif

        cudaFree(dQ_deq); cudaFree(dK_deq); cudaFree(dV_deq); cudaFree(dO_ref);
    }

    qQ.free_(); qK.free_();
    cudaFree(dQf); cudaFree(dKf); cudaFree(dVf); cudaFree(dV);
    cudaFree(dSFA); cudaFree(dSFB); cudaFree(dO); cudaFree(dLSE);
    if (workspace) cudaFree(workspace);

    if (!do_verify) {
        printf("\nMXFP8 FMHA PERF-ONLY (--verify=0; FP32 ref skipped — its score row "
               "needs S*4B smem > B200 227KB at large S)\n");
        return 0;
    }
    printf("\n%s\n", verified ? "MXFP8 FMHA PASS" : "MXFP8 FMHA FAIL");
    return verified ? 0 : 1;
}
#endif

// ===========================================================================
int main(int argc, char** argv) {
    cudaDeviceProp prop;
    CUDA_CHECK(cudaGetDeviceProperties(&prop, 0));
    printf("GPU: %s (SM %d.%d)\n", prop.name, prop.major, prop.minor);

#ifdef MXFP8_FULL
    return run_fmha_mxfp8(argc, argv);
#else
    (void)argc; (void)argv;
    return quant_selftest();
#endif
}
