# FP8 vs BF16 Backward 消融实验报告

## 实验方法

对 flash-attention backward 的两个最优 2-CTA 实现做 4 组消融（去除 exp2 / softmax+dS 算术 / DSMEM corner-turn 交换 / dQ reduce TMA store），量化每个组件的性能占比。

**控制变量**：每个消融用条件编译宏控制（`FAG_ABLATE_*` / 环境变量），base 路径零影响；保留所有 pipeline 握手防止死锁，仅跳过目标算术/数据搬运。

**测量口径**：nsys 抓主 backward kernel 中位时间（排除 preprocess/postprocess 辅助 kernel），FLOP = `10·b·h·L²·d`（non-causal）。三个 shape `(1,16,{4096,8192,16384},128)`。

## 实验对象

| 版本 | 路径 | 精度 | 配置 |
|---|---|---|---|
| **FP8** | `temp/test/test_merge/Benchmark/FAG_FP32/` (CUTLASS) | FP8 e4m3 | FAG_EX2_EMU_STRIDE=4 + FAG_REGALLOC_FA4_CURRENT（最优版 `fag_stage_regalloc`）|
| **BF16** | `FAG/flash-attention/flash_attn/cute/flash_bwd_sm100.py` (FA4 CuTe-DSL) | BF16 | 2-CTA 默认（MUFU 硬件 exp2）|

## 原始数据

### FP8 主 kernel 中位时间 (ns)

| shape | base | exp2 | softmax | corner | reduce |
|---|---|---|---|---|---|
| (1,16,4096,128) | 254430 | 256158 | 240318 | 222942 | 252542 |
| (1,16,8192,128) | 825787 | 835546 | 782907 | 713755 | 825179 |
| (1,16,16384,128) | 3161900 | 3162892 | 2960237 | 2714159 | 3192268 |

### BF16 主 kernel 中位时间 (ns)

| shape | base | exp2 | softmax | corner | reduce |
|---|---|---|---|---|---|
| (1,16,4096,128) | 270942 | 277470 | 261438 | 265630 | 260126 |
| (1,16,8192,128) | 932954 | 946682 | 909242 | 924154 | 874587 |
| (1,16,16384,128) | 3665417 | 3710728 | 3565705 | 3645928 | 3473929 |

## 结果：主 kernel TFLOPS

### FP8

| shape | base | 去exp2 | 去softmax+dS | 去corner | 去reduce |
|---|---|---|---|---|---|
| (1,16,4096,128) | 1350 | 1341 | 1430 | **1541** | 1361 |
| (1,16,8192,128) | 1664 | 1645 | 1755 | **1926** | 1666 |
| (1,16,16384,128) | 1739 | 1738 | 1857 | **2026** | 1722 |

### BF16

| shape | base | 去exp2 | 去softmax+dS | 去corner | 去reduce |
|---|---|---|---|---|---|
| (1,16,4096,128) | 1268 | 1238 | 1314 | 1294 | **1321** |
| (1,16,8192,128) | 1473 | 1452 | 1512 | 1487 | **1571** |
| (1,16,16384,128) | 1500 | 1482 | 1542 | 1508 | **1583** |

## 结果：vs base 性能变化

### FP8

| shape | 去exp2 | 去softmax+dS | 去corner | 去reduce |
|---|---|---|---|---|
| (1,16,4096,128) | -0.7% | +5.9% | **+14.1%** | +0.7% |
| (1,16,8192,128) | -1.2% | +5.5% | **+15.7%** | +0.1% |
| (1,16,16384,128) | -0.0% | +6.8% | **+16.5%** | -1.0% |

### BF16

| shape | 去exp2 | 去softmax+dS | 去corner | 去reduce |
|---|---|---|---|---|
| (1,16,4096,128) | -2.4% | +3.6% | +2.0% | **+4.2%** |
| (1,16,8192,128) | -1.5% | +2.6% | +1.0% | **+6.7%** |
| (1,16,16384,128) | -1.2% | +2.8% | +0.5% | **+5.5%** |

## 省时绝对值（L=16384）

### FP8

| 消融 | 省时 | 占比 |
|---|---|---|
| **去 corner（DSMEM 交换）** | **+448 µs** | **+14.2%（最大）** |
| 去 softmax+dS | +202 µs | +6.4% |
| 去 exp2 | -1 µs | -0.0% |
| 去 reduce | -30 µs | -1.0% |

### BF16

| 消融 | 省时 | 占比 |
|---|---|---|
| **去 reduce（dQ TMA store）** | **+191 µs** | **+5.2%（最大）** |
| 去 softmax+dS | +100 µs | +2.7% |
| 去 corner | +19 µs | +0.5% |
| 去 exp2 | -45 µs | -1.2%（变慢）|

## BF16 vs FP8 消融对比（L=16384）

| 消融 | BF16 | FP8 | 差异原因 |
|---|---|---|---|
| 去 exp2 | -1.2%（变慢）| -0.0% | BF16：exp2 被 MMA(2560cy) 藏住，删它破坏指令调度反而变慢；FP8：最优版已开 MUFU 分流(ex2_s4)，exp2 已被优化 |
| 去 softmax+dS | +2.8% | +6.8% | FP8 占比更大：FP8 的 MMA(1280cy)<softmax段，softmax 更接近瓶颈 |
| **去 corner（DSMEM）** | **+0.5%** | **+16.5%** | **差异最大：FP8 的 dS 是 1B，corner-turn 要 4 字节标量 pack(uint4)，BF16 的 2B pack 简单得多** |
| 去 reduce | +5.2%（最大）| -1.0% | 反转：BF16 reduce 是最大开销，FP8 的 corner 太大把 reduce 占比稀释 |

## 关键结论

### 1. FP8 和 BF16 的最大瓶颈完全不同

- **FP8 最大瓶颈**：DSMEM corner-turn 的 RMEM→SMEM uint4 pack（+14.2%）。FP8 的 dS 是 1 字节/元素，pack 成 uint32 要做 4 个字节的标量移位+或运算（`pack4` lambda），在 compute warpgroup 上串行执行。这与 `BOTTLENECK_EXPERIMENTS.md` 的 probe 结论一致（pack 占 537µs/3.5ms ≈ 15%）。
- **BF16 最大瓶颈**：dQ reduce 的 TMA store（+5.2%）。dQ 从 TMEM→RMEM→SMEM staging + TMA atomic-add 到 GMEM 的数据搬运链。

### 2. exp2 在两个精度下都不是瓶颈（但原因不同）

- **BF16**：exp2 走 MUFU 单元，和 FP32 FMA 并行，被 MMA(2560cy) 完全重叠掉。删它反而变慢（-1.2%），因为破坏了 MUFU/FMA 的指令调度平衡。
- **FP8**：最优版已开 MUFU 分流（ex2_s4），25% 的 exp2 走软件 FMA 仿真，MUFU 瓶颈已被削掉。再去掉 exp2 无额外收益。

### 3. corner-turn 是 FP8 特有的精度相关开销

BF16 的 dS 是 2 字节，可以直接用 `autovec_copy` 或简单的 pack；FP8 的 dS 是 1 字节，必须用 `pack4` lambda 做 4 字节标量拼装（shift+or），这个标量 pack 在 compute warpgroup 上是串行的，占 14.2%。

**优化方向**：`FAG_DS_RECAST_PACK`（testMerge 的 `fag_candidate_pack`）把标量 pack 改成 `recast<uint32_t>` 直读 bit pattern，避免 12 条标量移位/或指令。这正是针对 FP8 corner-turn 瓶颈的优化。

## 复现方法

### FP8（CUTLASS, testMerge）
```bash
cd /home/ubuntu/workspace/oyhj/temp/test/test_merge/Benchmark/FAG_FP32/build
# base 最优版
nsys profile --stats=true ./fag_stage_regalloc --b=1 --h=16 --q=16384 --k=16384 --d=128 --iterations=10 --mask=no
# 消融版（CMake target: fag_abl_exp2 / fag_abl_softmax / fag_abl_corner / fag_abl_reduce）
nsys profile --stats=true ./fag_abl_corner --b=1 --h=16 --q=16384 --k=16384 --d=128 --iterations=10 --mask=no
```

### BF16（FA4 CuTe-DSL）
```bash
cd /home/ubuntu/workspace/oyhj/FAG/flash-attention
# 消融通过环境变量控制
FAG_ABLATE_EXP2=1 python3 bench_ablate_bwd.py 16384
```

## 文件位置

- FP8 消融 kernel（已还原）：`temp/test/test_merge/Benchmark/FAG_FP32/kernel/sm100_fmha_bwd_kernel_tma_2sm_warpspecialized.hpp`
- FP8 消融 CMake target：`temp/test/test_merge/Benchmark/FAG_FP32/CMakeLists.txt`（`fag_abl_*`）
- BF16 消融 benchmark 脚本：`FAG/flash-attention/bench_ablate_bwd.py`
- FP8 原始数据：`/tmp/fp8_ablation_times.csv`
- BF16 原始数据：`/tmp/ablation_times.csv`
