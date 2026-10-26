# Corner-Turn 消融实验：FP8 FAG vs 官方 BF16（2-SM，最优版，MUFU 分流）

聚焦 `ABLATION_FP8_VS_BF16.md` 里的 **corner** 单项，在 **两侧都用最优 2-SM + MUFU 分流** 配置重做一遍，并额外给出 **三个 kernel 合计**（主 backward + 2 个辅助 kernel）的时间和 TFLOPS。

- **消融对象**：仅 dS 的 **跨 CTA DSMEM 交换**（exchange-only）。跳过「peer 半 pack 到交换 buffer + post-loop dump + DSMEM bulk copy」，**保留** own 半 SMEM 写和喂 dK 的 TMEM 写，以及**所有** pipeline/mbarrier 握手（避免死锁）。
- **测量口径**：nsys `cuda_gpu_kern_sum` 的 **Med（中位数，ns）**。GPU 锁频 1965 MHz。`FLOP = 10·b·h·L²·d`。
- **正确性**：消融版故意破坏数值（dQ 的 peer 半是垃圾），仅用于测成本；base 路径零影响。

## 实验对象与配置

| 版本 | 路径 | 精度 | 2-SM 最优 + MUFU 分流配置 |
|---|---|---|---|
| **FP8** | `temp/test/test_merge/Benchmark/FAG_FP32/` (CUTLASS) | FP8 e4m3 | `fag_stage_regalloc` = `BWD_2SM FAG_EX2_EMU_STRIDE=4 FAG_REGALLOC_FA4_CURRENT`（EX2_STRIDE=4 即 MUFU 分流）|
| **BF16** | `FAG/flash-attention/flash_attn/cute/flash_bwd_sm100.py` (FA4 CuTe-DSL) | BF16 | `flash_attn_func` 默认 2-CTA（MUFU 硬件 exp2）|

消融触发：FP8 = 编译宏 `FAG_ABLATE_CORNER`（target `fag_abl_corner`）；BF16 = 环境变量 `FAG_ABLATE_CORNER=1`。

## 原始中位数（ns）

### FP8：主 kernel `Sm100FmhaBwdKernelTma2SmWarpSpecialized` + `FmhaKernelBwdSumOdO` + `FmhaKernelBwdConvert`

| shape | | base | | | ablate | | |
|---|---|---|---|---|---|---|
| | 主 | sumOdO | convert | 主 | sumOdO | convert |
| (1,16,4096,128) | 252511 | 10976 | 10528 | 216095 | 10976 | 10400 |
| (1,16,8192,128) | 823932 | 21216 | 20768 | 694396 | 21184 | 20704 |
| (1,16,16384,128) | 3152304 | 39552 | 43008 | 2620915 | 39616 | 43200 |

### BF16：主 kernel `FlashAttentionBackwardSm100` + `…Preprocess` + `…Postprocess`

| shape | | base | | | ablate | | |
|---|---|---|---|---|---|---|
| | 主 | preprocess | postprocess | 主 | preprocess | postprocess |
| (1,16,4096,128) | 279262 | 11456 | 11904 | 258271 | 11520 | 11776 |
| (1,16,8192,128) | 944604 | 20543 | 24351 | 886364 | 20671 | 24768 |
| (1,16,16384,128) | 3716910 | 40095 | 41151 | 3464366 | 40128 | 41471 |

> 辅助 kernel 时间在 ablate 下几乎不变（符合预期：corner 只影响主 kernel）。

## 结果 A：主 kernel TFLOPS

| shape | FP8 base | FP8 去corner | FP8 Δ | BF16 base | BF16 去corner | BF16 Δ |
|---|---|---|---|---|---|---|
| (1,16,4096,128) | 1361 | 1590 | **+16.9%** | 1230 | 1330 | +8.1% |
| (1,16,8192,128) | 1668 | 1979 | **+18.7%** | 1455 | 1551 | +6.6% |
| (1,16,16384,128) | 1744 | 2098 | **+20.3%** | 1479 | 1587 | +7.3% |

## 结果 B：三个 kernel 合计 TFLOPS（主 + 2 辅助）

合计时间 = 三者中位数之和（preprocess→main→postprocess 顺序执行，逐次中位数求和即单次 backward 的代表时间）。

| shape | FP8 base | FP8 去corner | FP8 Δ | BF16 base | BF16 去corner | BF16 Δ |
|---|---|---|---|---|---|---|
| (1,16,4096,128) | 1254 | 1447 | **+15.4%** | 1135 | 1220 | +7.5% |
| (1,16,8192,128) | 1587 | 1867 | **+17.6%** | 1389 | 1475 | +6.2% |
| (1,16,16384,128) | 1700 | 2033 | **+19.6%** | 1447 | 1550 | +7.1% |

## 结果 C：去 corner 后时间下降（ablate vs base）

| 版本 | shape | 主 kernel 时间↓ | 主 TFLOPS↑ | 三 kernel 合计时间↓ | 合计 TFLOPS↑ |
|---|---|---|---|---|---|
| FP8 | 4096 | −14.4% | +16.9% | −13.3% | +15.4% |
| FP8 | 8192 | −15.7% | +18.7% | −15.0% | +17.6% |
| FP8 | 16384 | −16.9% | +20.3% | −16.4% | +19.6% |
| BF16 | 4096 | −7.5% | +8.1% | −7.0% | +7.5% |
| BF16 | 8192 | −6.2% | +6.6% | −5.8% | +6.2% |
| BF16 | 16384 | −6.8% | +7.3% | −6.6% | +7.1% |

## 关键结论

### 1. FP8 的 corner 瓶颈远大于 BF16（核心观察成立）

- **FP8**：去 corner 主 kernel 时间降 **14–17%**（TFLOPS **+17–20%**），且随 shape 增大占比上升。
- **BF16**：去 corner 主 kernel 时间降 **6–8%**（TFLOPS **+7–8%**），三个 shape 基本持平。
- 两个口径（主 kernel / 三 kernel 合计）结论一致；辅助 kernel 不受 corner 影响，因此合计的相对增益略小（被不变的辅助 kernel 稀释）。

这与你的观察一致：**fp16(BF16) 时 corner 影响较小**，FP8 时 corner 是最大单项瓶颈。

### 2. 为什么 FP8 比 BF16 重——dS 字宽

| | dS 元素宽 | peer 半 pack 方式 | 单条开销 |
|---|---|---|---|
| FP8 | 1 字节 | 标量 `pack4`：4 次移位+或拼成 `uint32`，再组 `uint4`（kernel 内 2263–2272 行）| 12 条标量移位/或，compute warpgroup 串行 |
| BF16 | 2 字节 | `autovec_copy` 直拷（flash_bwd_sm100.py 3321/3343）| 向量化拷贝，无标量拼装 |

FP8 的 1 字节 dS 必须用标量移位/或拼成 4 字节，在 compute warpgroup 上串行执行，远贵于 BF16 的向量化拷贝。这正是 `fag_candidate_pack`（`FAG_DS_RECAST_PACK`，`recast<uint32_t>` 直读 bit pattern）针对的优化方向。

### 3. 与 `ABLATION_FP8_VS_BF16.md` 的差异

- **FP8**：base 与旧报告吻合（1350/1664/1739 vs 本轮 1361/1668/1744）；本轮去 corner 增益略高（旧 +14–16% → 本 +17–20%），因为本轮 exchange-only 把 **peer 半的 pack4 计算**也一并跳过（旧版可能只跳了搬运）。
- **BF16**：base 吻合（旧 1268/1473/1500 vs 本轮 1230/1455/1479）；本轮去 corner 增益（+7–8%）比旧报告（+0.5–2%）大，同样因为本轮 exchange-only 包含了「exchange 寄存器填充 + post-loop dump + bulk copy」三件，而旧版 BF16 ablation 范围更窄。**两侧用同一 exchange-only 定义，FP8-vs-BF16 的对比仍然有效且方向一致。**

## 复现

GPU 锁频与构建：
```bash
nvidia-smi -lgc 1965,1965
cd /home/ubuntu/workspace/oyhj/temp/test/test_merge/Benchmark/FAG_FP32
cmake --build build --target fag_stage_regalloc fag_abl_corner -j
```
FP8（nsys 抓主 backward kernel 中位数，二进制不带 `--verify` 以纯计时）：
```bash
nsys profile --force-overwrite=true -o /tmp/fp8_base_16384 \
  ./build/fag_stage_regalloc --b=1 --h=16 --q=16384 --k=16384 --d=128 --d_vo=128 --iterations=20 --mask=no
nsys profile --force-overwrite=true -o /tmp/fp8_ablate_16384 \
  ./build/fag_abl_corner   --b=1 --h=16 --q=16384 --k=16384 --d=128 --d_vo=128 --iterations=20 --mask=no
nsys stats -r cuda_gpu_kern_sum /tmp/fp8_base_16384.nsys-rep   # 取 Sm100FmhaBwdKernelTma2SmWarpSpecialized / SumOdO / Convert 的 Med 列
```
BF16（在 FAG/flash-attention 目录运行，确保 import 解析到本份源码；`FAG_ABLATE_CORNER=1` 触发消融）：
```bash
cd /home/ubuntu/workspace/oyhj/FAG/flash-attention
nsys profile --force-overwrite=true -o /tmp/bf16_base_16384   /usr/bin/python3 bench_ablate_bwd.py 16384
FAG_ABLATE_CORNER=1 nsys profile --force-overwrite=true -o /tmp/bf16_ablate_16384 /usr/bin/python3 bench_ablate_bwd.py 16384
nsys stats -r cuda_gpu_kern_sum /tmp/bf16_base_16384.nsys-rep  # 取 BackwardSm100 / Preprocess / Postprocess 的 Med 列
```

## 改动文件

| 文件 | 改动 |
|---|---|
| `FAG_FP32/kernel/sm100_fmha_bwd_kernel_tma_2sm_warpspecialized.hpp` | 加 `#ifdef FAG_ABLATE_CORNER` 守卫：pack 循环跳过 peer 半；exchange 块把 `arrive_and_expect_tx(8192)+bulk_copy` 换成无 tx 的 `ds_full.arrive(peer)`（防死锁）。base 路径零影响。|
| `FAG/flash-attention/flash_attn/cute/flash_bwd_sm100.py` | 加 `import os` + `self.fag_ablate_corner`（读 `FAG_ABLATE_CORNER`）；`compute_loop` 内 gate 三处交换点（exchange 寄存器填充 / post-loop dump / DSMEM bulk copy 换 0-tx arrive）。base 路径零影响。|
| `/tmp/corner_ablation_raw.csv` | 主 kernel 中位数原始数据 |
| `/tmp/analyze_corner.py` | 三 kernel 中位数提取 + TFLOPS 计算 |

## 验证记录

- 两侧 ablate 在最小 shape 均无死锁（barrier 握手全部保留；FP8 用无 tx `arrive`，BF16 用 0-tx `mbarrier_arrive_and_expect_tx`）。
- BF16 确认：import 解析到 FAG 源码（`flash_attn.cute.flash_bwd_sm100.__file__` = FAG 路径）；2-CTA 激活（ablate 与 base 中位数不同 ⇒ `use_2cta_instrs` 分支生效、确实重新编译）。
- 辅助 kernel 时间在 ablate 下基本不变，符合「corner 只影响主 kernel」。
