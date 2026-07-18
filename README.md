# doublewarp Hadamard+NVFP4 融合量化 benchmark

B200 GPU 上的 NVFP4 列量化（含 Hadamard 变换）融合 kernel 的性能测试代码。

## 目录结构

```
hadamard_final/
├── README.md                    # 本文件
├── run.sh                       # 一键运行入口
├── src/
│   └── doublewarp_fusion.cu     # doublewarp kernel 源码（md5 e6ff5d9c）
└── scripts/
    ├── deploy_and_build.sh      # 部署源码到容器 + 增量编译
    └── benchmark.py             # benchmark 脚本（CUDA event 计时 / nsys 模式）
```

## 前提条件

1. **Docker 容器 `te-base` 运行中**

2. 容器内有 TransformerEngine 的 build 环境：
   ```
   /workspace/hadamard_opti/TransformerEngine/
   ├── transformer_engine/                    # TE python 包 + libtransformer_engine.so
   ├── build/cmake/                           # ninja build 目录
   └── 3rdparty/cutlass/include/              # CUTLASS 头文件
   ```

3. 容器内有 CUDA 13.1 + Python 3.12 + PyTorch 2.10

## 快速开始

### 1. 部署 + 编译（首次或切换版本时执行）

```bash
cd /home/ubuntu/workspace/oyhj/hadamard_final
bash scripts/deploy_and_build.sh
```

这会：
- 把 `src/doublewarp_fusion.cu` 部署到容器内 TE 的 fusion.cu 路径
- 增量编译（删 .o → ninja 重编 → copy .so）
- md5 验证

### 2. 运行 benchmark

```bash
cd /home/ubuntu/workspace/oyhj/hadamard_final

# 默认：跑所有 7 个 shape × fm0/fm1
bash run.sh

# 只跑 fast_math=on
bash run.sh --fm 1

# 只跑某个 shape（如 shape5, idx=1）
bash run.sh --shape-idx 1

# nsys 采集模式（生成 .nsys-rep）
bash run.sh --nsys --shape-idx 1 --fm 1
```

### 3. 直接在容器内运行

也可以把 benchmark.py 拷进容器直接跑：

```bash
docker cp scripts/benchmark.py te-base:/tmp/benchmark.py
docker exec -e PYTHONPATH=/workspace/hadamard_opti/TransformerEngine:$PYTHONPATH \
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
| 5 | shape1 | 170112 | 5120 | 6680 | 45.1 | 大 shape |
| 6 | shape2 | 510336 | 5120 | 19960 | 134.9 | 最大 shape |

## fast_math 控制

通过环境变量 `NVTE_USE_FAST_MATH` 控制：
- `0`（默认）：使用精确除法（`1.0 / x`），额外 FP32↔BF16 转换
- `1`：使用 `reciprocal_approximate_ftz`，跳过 FP32↔BF16 往返

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
# 然后在容器内：
nsys stats /tmp/xxx.nsys-rep  # 查看 kernel 时间
```

### nsys 模式

```bash
bash run.sh --nsys --shape-idx 1 --fm 1
```

生成 nsys 报告到容器内 `/tmp/`，可用 `nsys stats` 查看 kernel 时间分布。

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
| shape8 | 9.92 us | 7.36 us | +25.8% |
| shape5 | 15.65 us | 12.10 us | +22.7% |
| shape6 | 30.53 us | 27.07 us | +11.3% |
| shape7 | 54.08 us | 51.49 us | +4.8% |
| shape1 | 341.5 us | 344.6 us | -0.9%（降频抵消）|
| shape2 | 1043.6 us | 1047.6 us | -0.4%（降频抵消）|

## 验证部署状态

```bash
# 确认源码 md5（应为 e6ff5d9c）
docker exec te-base md5sum \
    /workspace/hadamard_opti/TransformerEngine/transformer_engine/common/hadamard_transform/row_cast_col_hadamard_transform_cast_fusion.cu

# 确认 .so 编译时间（应为最近）
docker exec te-base ls -la \
    /workspace/hadamard_opti/TransformerEngine/transformer_engine/libtransformer_engine.so
```
