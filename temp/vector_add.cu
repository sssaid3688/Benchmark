#include <stdio.h>
#include <cuda_runtime.h>

// CUDA 核函数：每个线程负责一个元素的加法
__global__ void vectorAdd(const float *A, const float *B, float *C, int N) {
    int idx = blockIdx.x * blockDim.x + threadIdx.x;
    if (idx < N) {
        C[idx] = A[idx] + B[idx];
    }
}

int main() {
    int N = 1 << 20; // 向量长度：1M 个元素
    size_t size = N * sizeof(float);

    // 1. 在主机上分配内存
    float *h_A = (float*)malloc(size);
    float *h_B = (float*)malloc(size);
    float *h_C = (float*)malloc(size);

    // 2. 初始化输入数据
    for (int i = 0; i < N; ++i) {
        h_A[i] = static_cast<float>(i);
        h_B[i] = static_cast<float>(2 * i);
    }

    // 3. 在设备上分配内存
    float *d_A, *d_B, *d_C;
    cudaMalloc((void**)&d_A, size);
    cudaMalloc((void**)&d_B, size);
    cudaMalloc((void**)&d_C, size);

    // 4. 将数据从主机拷贝到设备
    cudaMemcpy(d_A, h_A, size, cudaMemcpyHostToDevice);
    cudaMemcpy(d_B, h_B, size, cudaMemcpyHostToDevice);

    // 5. 配置内核启动参数并调用核函数
    int threadsPerBlock = 256;
    int blocksPerGrid = (N + threadsPerBlock - 1) / threadsPerBlock;
    vectorAdd<<<blocksPerGrid, threadsPerBlock>>>(d_A, d_B, d_C, N);

    // 6. 等待 GPU 完成，并将结果从设备拷回主机
    cudaMemcpy(h_C, d_C, size, cudaMemcpyDeviceToHost);

    // 7. 验证结果（检查几个元素）
    bool correct = true;
    for (int i = 0; i < N; ++i) {
        if (fabs(h_C[i] - (h_A[i] + h_B[i])) > 1e-5) {
            printf("Error at %d: %f + %f = %f (expected %f)\n",
                   i, h_A[i], h_B[i], h_C[i], h_A[i] + h_B[i]);
            correct = false;
            break;
        }
    }
    if (correct)
        printf("Vector addition passed!\n");

    // 8. 释放内存
    cudaFree(d_A);
    cudaFree(d_B);
    cudaFree(d_C);
    free(h_A);
    free(h_B);
    free(h_C);

    return 0;
}