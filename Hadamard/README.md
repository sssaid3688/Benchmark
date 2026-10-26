# Hadamard: doublewarp Hadamard+NVFP4 融合量化 benchmark

B200 GPU 上的 NVFP4 列量化（含 Hadamard 变换）融合 kernel 的性能测试代码。
对应源码为 `src/doublewarp_fusion.cu`（kernel 函数 `row_col_rht_gemm_device`，
md5 `e6ff5d9c1896c34dbbbafbe02bdea0ca`）。

> 本工程与同级的 `FA` / `FAG` / `FAG_FP32` 并列。那些是独立的 CMake 工程；
> 而 Hadamard 的融合 kernel 深度依赖 TransformerEngine（TE）的 installed header
> 与 `libtransformer_engine.so`，**不能脱离 TE 独立编译**，因此采用「部署到容器内
> TE 构建环境做增量编译」的方式工作（详见下文「工作方式」）。

## 目录结构

```
Hadamard/
├── README.md                         # 本文件
├── run.sh                            # 一键运行入口（CUDA event / nsys 两种模式）
├── src/
│   └── doublewarp_fusion.cu          # doublewarp kernel 源码
├── scripts/
│   ├── deploy_and_build.sh           # 部署源码到容器 + 增量编译
│   └── benchmark.py                  # benchmark 脚本（CUDA event 计时 / nsys 模式）
└── 3rdparty/                         # 第三方依赖（仅供阅读/排查）
    ├── README.md                     # 依赖说明
    └── te_headers/                   # TransformerEngine 私有头文件
```

CUTLASS 不在本目录内，复用仓库顶层的同级 submodule `../cutlass`（v4.5+），
详见 [`3rdparty/README.md`](3rdparty/README.md)。

## 工作方式：容器内 TE 增量编译

`doublewarp_fusion.cu` 编译时需要：

1. **CUTLASS 头**（`<cutlass/...>`、`<cute/...>`）—— 来自 `../cutlass/include`
2. **TransformerEngine 私有头**（`common/common.h`、`common/util/*`、
   `customized_pipeline.cuh`、`<transformer_engine/hadamard_transform.h>` 等）
   —— 来自 TE 的 `transformer_engine/` 目录（`3rdparty/te_headers/` 是其只读副本）
3. **链接 `libtransformer_engine.so`** —— 需要完整的 TE 构建产物

这意味着必须在「已配置好 TE 构建环境」的地方编译。本工程的做法是：
把 `src/doublewarp_fusion.cu` **覆盖**到容器内 TE 的 fusion.cu 路径，
再触发 ninja 增量编译（只重编这一个 `.o` + relink `.so`）。

## 前提条件

1. **Docker 容器运行中**（默认名 `te-base`，可用环境变量 `CONTAINER` 覆盖）

2. 容器内有 TransformerEngine 的 build 环境：
   ```
   <TE_ROOT>/                         # 默认 /workspace/hadamard_opti/TransformerEngine
   ├── transformer_engine/            # TE python 包 + libtransformer_engine.so
   ├── build/cmake/                   # ninja build 目录（含 build.ninja）
   └── 3rdparty/cutlass/include/      # CUTLASS 头文件
   ```
   （`TE_ROOT` 可用环境变量覆盖）

3. 容器内有 CUDA 13.x + Python 3.12 + PyTorch（镜像
   `nvcr.io/nvidia/pytorch:26.01-py3` 即满足）

4. 宿主机为 NVIDIA B200 / Blackwell（`sm_100a`）

## 快速开始

### 1. 部署 + 编译（首次或切换源码版本时执行）

```bash
cd <path-to>/Hadamard
bash scripts/deploy_and_build.sh
```

这会：
- 把 `src/doublewarp_fusion.cu` 部署到容器内 TE 的 fusion.cu 路径
- 增量编译（删 `.o` → ninja 重编 → copy `.so`）
- md5 验证

如需指定容器或 TE 路径：
```bash
CONTAINER=my-te  TE_ROOT=/path/to/TE  bash scripts/deploy_and_build.sh
```

### 2. 运行 benchmark

```bash
cd <path-to>/Hadamard

# 默认：跑所有 7 个 shape × fm0/fm1（CUDA event 计时）
bash run.sh

# 只跑 fast_math=on
bash run.sh --fm 1

# 只跑某个 shape（如 shape5, idx=1）
bash run.sh --shape-idx 1

# nsys 采集模式（生成 .nsys-rep 到 ./nsys_reports/）
bash run.sh --nsys --shape-idx 1 --fm 1
```

### 3. 直接在容器内运行

也可把 `benchmark.py` 拷进容器直接跑：

```bash
docker cp scripts/benchmark.py te-base:/tmp/benchmark.py
docker exec -e PYTHONPATH=<TE_ROOT>:$PYTHONPATH \
    te-base python3 /tmp/benchmark.py --fm 0
```

## Shape 定义

| idx | name | M | K | grid | waves | 说明 |
|---|---|---|---|---|---|---|
| 0 | shape8 | 8192 | 2048 | 128 | 0.86 | 最小，SM 未塞满 |
| 1 | shape5 | 8192 | 4096 | 256 | 1.73 | 小 shape |
| 2 | shape9 | 16384 | 2048 | 256 | 1.73 | 小 shape |
| 3 | shape6 | 16384 | 4096 | 512 | 3.46 | 中 shape |
| 4 | shape7 | 32768 | 4096 | 1024 | 6.92 | 中 shape |
| 5 | shape1 | 170112 | 5120 | 6680 | 45.1 | 大 shape（padded from 170100）|
| 6 | shape2 | 510336 | 5120 | 19960 | 134.9 | 最大 shape（padded from 510300）|

## fast_math 控制

通过环境变量 `NVTE_USE_FAST_MATH` 控制（对应源码模板参数 `kUseFastMath`）：
- `0`（默认）：使用精确除法（`1.0 / x`），额外 FP32↔BF16 转换
- `1`：使用 `reciprocal_approximate_ftz`，跳过 FP32↔BF16 往返

源码中两条路径（见 `doublewarp_fusion.cu`）：
```cpp
if constexpr (kUseFastMath) {
    acc_scales = cutlass::reciprocal_approximate_ftz<...>{}(qpvscale_scaled);
} else {
    acc_scales = cutlass::divides<...>{}(1.0, qpvscale_scaled);
}
```

```bash
# 方式1：通过 --fm 参数
bash run.sh --fm 1

# 方式2：通过环境变量
NVTE_USE_FAST_MATH=1 bash run.sh
```

## 输出说明

### 普通 benchmark 模式

```
idx  shape     M        K    min_us    mean_us  median_us    max_us
  0  shape8   8192     2048     9.632      9.908      9.920    10.240
  1  shape5   8192     4096    15.200     15.647     15.647    16.288
...
```

**注意**：CUDA event 计时会包含 kernel launch 间隙（~3-5us），对于短 kernel（<20us）会偏高。

- **min_us**：最快批次时间（排除了 OS 抖动，但含 launch 间隙）
- **median_us**：中位数（最稳定的指标）
- **max_us**：最慢批次时间（含 OS 抖动）

**精确测量单个 kernel 时间（不含 launch 间隙），请用 nsys 模式：**
```bash
bash run.sh --nsys --shape-idx 1 --fm 1
```

### nsys 模式

```bash
bash run.sh --nsys --shape-idx 1 --fm 1
```

生成 nsys 报告到 `./nsys_reports/`，并直接打印 fusion kernel
（`row_col_rht_gemm_device`）的 min/median/mean/max（us）。

## DVFS 降频注意

**大 shape（shape1/shape2）会触发 DVFS 降频（18-21%）**，导致实测时间偏慢：

| shape | 满载频率 | 降频幅度 |
|---|---|---|
| shape8-shape6 | 1965 MHz | 0%（不降频）|
| shape7 | 1830 MHz | 6.5% |
| shape1 | 1590 MHz | 18.8% |
| shape2 | 1537 MHz | 21.2% |

大 shape 的实际满频性能 ≈ 测量值 × (满载频率 / 1965)。小 shape 不受影响。

## 关键性能数据（参考，nsys 测量）

以下数据由 nsys profile 采集（不含 CUDA launch 间隙），CUDA event 模式会偏高 3-5us：

| shape | fm0 median | fm1 median | fm 提升 |
|---|---|---|---|
| shape8 | 9.92 us | 7.30 us | +26.4% |
| shape5 | 15.65 us | 12.00 us | +23.3% |
| shape6 | 30.53 us | 27.01 us | +11.5% |
| shape7 | 54.08 us | 51.46 us | +4.8% |
| shape1 | 341.5 us | 345.3 us | -1.1%（降频抵消）|
| shape2 | 1043.6 us | 1049.4 us | -0.6%（降频抵消）|

**带宽利用率（fm1，B200 HBM3e 理论 8.0 TB/s）：**

| shape | 搬运量/GB | 带宽/TB/s | 带宽利用率 |
|---|---|---|---|
| 8192×2048 | 0.042 | 5.76 | 73.7% |
| 8192×4096 | 0.085 | 7.08 | 89.6% |
| 16384×2048 | 0.085 | 7.10 | 89.8% |
| 16384×4096 | 0.171 | 6.33 | 79.6% |
| 32768×4096 | 0.343 | 6.67 | 83.6% |
| 170100×5120 | 2.231 | 6.46 | 80.8% |
| 510300×5120 | 6.695 | 6.38 | 79.8% |

## 验证部署状态

```bash
# 确认源码 md5（应为 e6ff5d9c1896c34dbbbafbe02bdea0ca）
docker exec te-base md5sum \
    <TE_ROOT>/transformer_engine/common/hadamard_transform/row_cast_col_hadamard_transform_cast_fusion.cu

# 确认 .so 编译时间（应为最近）
docker exec te-base ls -la \
    <TE_ROOT>/transformer_engine/libtransformer_engine.so
```
