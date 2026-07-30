# FAG: Blackwell FP8 FlashAttention Backward

`FAG` 是从 CUTLASS `examples/77_blackwell_fmha` 中剥离出的独立 FP8 FlashAttention backward 工程。它保留本次优化后的 2-SM（双 CTA cooperative UMMA）实现，同时提供 1-SM 验证 target。

工程目录位于：

```text
/home/ubuntu/workspace/oyhj/staticQuant_mxfp8/Benchmark/
├── FAG/        # 本工程：源码、CMakeLists.txt、README、build/
└── cutlass/    # 第三方 CUTLASS 依赖；不再使用 FAG/cutlass/examples 路径
```

`FAG` 本身不复制 CUTLASS 的完整源码树，而是像同级 `FA` 工程一样通过 CMake 的 `add_subdirectory(../cutlass ...)` 使用该依赖。这样项目源码与 CUTLASS examples 目录完全分离，同时避免复制大型依赖。

## 包含的实现

- `fag_bwd_fp8_2sm`：优化后的 FP8 backward 2-SM 主 target。
- `fag_bwd_fp8_1sm`：FP8 backward 1-SM 对照 target。
- `77_blackwell_fmha_bwd.cu`：测试、计时、严格参考比较和梯度快照逻辑。
- `collective/`、`common/`、`device/`、`kernel/`、`reference/`：运行 backward 所需的本地实现头文件。

## 编译要求

- NVIDIA B200 / Blackwell，支持 `sm_100a`。
- CUDA Toolkit 13.x。
- CMake >= 3.20。
- 同级目录中的 CUTLASS：`../cutlass`。

## 构建

在 `FAG` 根目录执行：

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DCUTLASS_NVCC_ARCHS=100a \
  -DCMAKE_CUDA_COMPILER=/usr/local/cuda-13.3/bin/nvcc
cmake --build build --target fag_bwd_fp8_2sm -j 16
```

基准构建默认不嵌入 `lineinfo`，以匹配性能版本。需要 Nsight 源码定位时，单独建立 profiling build：

```bash
cmake -S . -B build-profile -DCMAKE_BUILD_TYPE=Release -DCUTLASS_NVCC_ARCHS=100a \
  -DCMAKE_CUDA_COMPILER=/usr/local/cuda-13.3/bin/nvcc -DFAG_ENABLE_LINEINFO=ON
cmake --build build-profile --target fag_bwd_fp8_2sm -j 16
```

如果 CUDA 位于不同路径，将 `-DCMAKE_CUDA_COMPILER` 改为对应的 `nvcc` 绝对路径。工程也会自动尝试 `/usr/local/cuda-13.3/bin/nvcc` 和 `/usr/local/cuda/bin/nvcc`。

如需同时构建 1-SM 对照：

```bash
cmake --build build --target fag_bwd_fp8_1sm -j 16
```

关键编译设置：

- `FP8`：启用 FP8 E4M3 测试路径。
- `BWD_2SM`：仅 2-SM target 启用，选择双 CTA cooperative UMMA 实现。
- `--use_fast_math`：与原优化版本保持一致。
- `-DFAG_ENABLE_LINEINFO=ON`：可选地保留 Nsight Compute/Systems 的源码定位信息；默认性能构建关闭它。
- `-Xptxas=-v`：构建时打印寄存器、spill 和 shared-memory 使用量。
- `CUTLASS_NVCC_ARCHS=100a`：生成 B200 需要的 Blackwell TensorCore 指令代码。

## 性能测试

进入 build 目录：

```bash
cd build
nvidia-smi --query-compute-apps=pid,process_name,used_memory --format=csv,noheader
```

确认没有其他 compute process 后运行：

```bash
./fag_bwd_fp8_2sm --b=1 --h=16 --q=4096  --k=4096  --d=128 --d_vo=128 --iterations=100 --verbose
./fag_bwd_fp8_2sm --b=1 --h=16 --q=8192  --k=8192  --d=128 --d_vo=128 --iterations=100 --verbose
./fag_bwd_fp8_2sm --b=1 --h=16 --q=16384 --k=16384 --d=128 --d_vo=128 --iterations=100 --verbose
```

优化后的参考结果如下；实际 event 时间会随 GPU 时钟和共享运行环境有小幅波动。

| N | 2-SM FP8 |
|---:|---:|
| 4096 | 1153.93 TFLOPS，297.762 us |
| 8192 | 1453.63 TFLOPS，945.486 us |
| 16384 | 1540.59 TFLOPS，3568.47 us |

## 严格正确性验证

`--verify` 会运行高精度参考。额外的 strict 参数要求 `max diff <= 0.1`、`mean diff <= 0.02`；`--stability-runs=3` 要求三次运行产生相同的梯度哈希。

先由 1-SM 生成同输入的对照快照，再比较 2-SM：

```bash
./fag_bwd_fp8_1sm --b=1 --h=16 --q=4096 --k=4096 --d=128 --d_vo=128 --iterations=1 \
  --dump-gradients=grad_1sm_4096.bin

./fag_bwd_fp8_2sm --b=1 --h=16 --q=4096 --k=4096 --d=128 --d_vo=128 --iterations=1 --verify \
  --strict-max-diff=0.1 --strict-mean-diff=0.02 --stability-runs=3 \
  --compare-gradients=grad_1sm_4096.bin
```

将命令中的 `4096` 同时替换为 `8192` 或 `16384` 即可验证另两组 shape。

可选的访存安全检查：

```bash
/usr/local/cuda-13.3/bin/compute-sanitizer --tool memcheck --error-exitcode=99 \
  ./fag_bwd_fp8_2sm --b=1 --h=16 --q=4096 --k=4096 --d=128 --d_vo=128 --iterations=1
```

## 本次 2-SM 优化摘要

1. dS corner-turn 使用显式 `uint4` 的 128-bit shared-memory 写入，匹配 FP8 数据宽度。
2. dQ reduction 使用直接向量化行映射，移除逐元素坐标散射。
3. 融合 `sum(O*dO)` 与 dQ accumulator 清零，移除单独的 `cudaMemsetAsync`。
4. convert kernel 的序列批量由 8 提升到 32，并展开循环。

在 N=4096 上，主 kernel 的 NCU 指令数从约 175.943M 降至 76.467M，shared-memory bank conflict 从约 42.053M 降至 1.826M；最终 2-SM 相对给定 1-SM baseline 的性能提升为 10.68% / 13.48% / 15.14%。
