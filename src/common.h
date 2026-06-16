#pragma once
#include <cuda_runtime.h>
#include <cstdio>
#include <cmath>
#include <vector>

#define CUDA_CHECK(call) do {                    \
    cudaError_t err = call;                       \
    if (err != cudaSuccess) {                     \
        fprintf(stderr, "CUDA error %s:%d: %s\n", \
                __FILE__, __LINE__, cudaGetErrorString(err)); \
        exit(1);                                  \
    }                                             \
} while(0)

struct GpuTimer {
    cudaEvent_t start, stop;
    GpuTimer()  { CUDA_CHECK(cudaEventCreate(&start)); CUDA_CHECK(cudaEventCreate(&stop)); }
    ~GpuTimer() { cudaEventDestroy(start); cudaEventDestroy(stop); }
    void tic()  { CUDA_CHECK(cudaEventRecord(start)); }
    void toc()  { CUDA_CHECK(cudaEventRecord(stop)); CUDA_CHECK(cudaEventSynchronize(stop)); }
    float ms()  { float t; CUDA_CHECK(cudaEventElapsedTime(&t, start, stop)); return t; }
};

// B200 theoretical specs
#define B200_SM_COUNT         160
#define B200_PEAK_TFLOPS_FP16 4500.0f
#define B200_PEAK_TFLOPS_FP8  9000.0f
#define B200_MEM_BW_GBS       8000.0f

// GEMM shapes from SOW document
struct GemmConfig {
    int M, N, K;
    const char* name;
};

static const GemmConfig SOW_GEMM_SHAPES[] = {
    // {M, N, K, name}
    {170100, 13824, 5120, "shape1_M170100_N13824_K5120"},
    {510300, 13824, 5120, "shape2_M510300_N13824_K5120"},
    {170100, 5120,  5120, "shape3_M170100_N5120_K5120"},
    {510300, 5120,  5120, "shape4_M510300_N5120_K5120"},
    {8192,   768,   4096, "shape5_M8192_N768_K4096"},
    {16384,  768,   4096, "shape6_M16384_N768_K4096"},
    {32768,  768,   4096, "shape7_M32768_N768_K4096"},
    {8192,   4096,  2048, "shape8_M8192_N4096_K2048"},
    {16384,  4096,  2048, "shape9_M16384_N4096_K2048"},
    {32768,  4096,  128,  "shape10_M32768_N4096_K128"},
    {16384,  768,   128,  "shape11_M16384_N768_K128"},
    {32768,  768,   128,  "shape12_M32768_N768_K128"},
    {8192,   4096,  128,  "shape13_M8192_N4096_K128"},
};

static const int SOW_GEMM_COUNT = sizeof(SOW_GEMM_SHAPES) / sizeof(SOW_GEMM_SHAPES[0]);

// Sparsify shapes (MxK matrices)
struct SparsifyConfig {
    int M, K;
    const char* name;
};

static const SparsifyConfig SOW_SPARSIFY_SHAPES[] = {
    {170100, 5120, "sp1_M170100_K5120"},
    {510300, 5120, "sp2_M510300_K5120"},
    {8192,   4096, "sp3_M8192_K4096"},
    {16384,  4096, "sp4_M16384_K4096"},
    {32768,  4096, "sp5_M32768_K4096"},
    {8192,   2048, "sp6_M8192_K2048"},
    {16384,  2048, "sp7_M16384_K2048"},
    {32768,  2048, "sp8_M32768_K2048"},
};

static const int SOW_SPARSIFY_COUNT = sizeof(SOW_SPARSIFY_SHAPES) / sizeof(SOW_SPARSIFY_SHAPES[0]);

inline double gemm_flops(int M, int N, int K) {
    return 2.0 * static_cast<double>(M) * N * K;
}

inline double gemm_tflops(int M, int N, int K, float ms) {
    return gemm_flops(M, N, K) / (ms * 1e-3) * 1e-12;
}

inline double sparsify_bytes(int M, int K) {
    // input fp32 + output fp32 + indices int32 = 4+4+4 = 12 bytes per element
    return static_cast<double>(M) * K * 12.0;
}

inline double sparsify_bw_gbs(int M, int K, float ms) {
    return sparsify_bytes(M, K) / (ms * 1e-3) * 1e-9;
}
