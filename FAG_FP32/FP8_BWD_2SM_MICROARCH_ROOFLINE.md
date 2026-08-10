# B200 FP8 FlashAttention Backward 2SM：微架构瓶颈、Cycle Roofline 与消融结论

> 分析对象：B200，2-CTA/2SM cluster，full attention backward，`B=1, H=16, D=DVO=128`，`Q=K={4096,8192,16384}`。  
> 精度约束：输入、输出以及所有既有中间数据类型均保持不变；`maxdiff < 0.1`。  
> FP8 源码：`/home/ubuntu/workspace/oyhj/temp/test/test_merge/Benchmark/FAG_FP32/kernel/sm100_fmha_bwd_kernel_tma_2sm_warpspecialized.hpp`。  
> FP16 对照：`/home/ubuntu/workspace/oyhj/FAG/flash-attention/flash_attn/cute/flash_bwd_sm100.py`。  
> 基线编译项：`BWD_2SM + FAG_EX2_EMU_STRIDE=4 + FAG_REGALLOC_FA4_CURRENT`。

## 1. 结论先行

本轮没有找到同时覆盖三个 shape、保持全部精度约束并达到 10% 的有效优化。当前锁定的短窗口最高基线为 **1235.92 / 1561.82 / 1657.19 TFLOPS**。多组独立消融表明，常规的 TMEM 指令宽度、MUFU/软件 exp2 比例、dQ 分批、dS 直写、寄存器微调和局部预取都不能提供 10% 增益；能消除同步的版本则不能保持结果稳定或精度正确。

当前实现触及的具体限制不是单独的 TMEM 端口、HBM、Tensor Core 或 CUDA Core 峰值，而是：

> **由寄存器和共享内存共同锁死的一 CTA/SM 驻留上限，使 512-thread CTA 只能维持 16 个 active warps（理论 occupancy 25%）；低驻留无法隐藏 S/P TMEM 复用所必需的 256-thread barrier，以及 L1TEX/TMEM 生产者—消费者的 long-scoreboard 延迟。FP8 减少了 MMA 和字节工作量，却没有提高 occupancy，因而 FP32 P 重构链与同步延迟从 FP16 中被暴露出来。**

这一定义同时解释了以下看似矛盾的现象：

- FP8 Tensor Core、TMEM、DRAM 都没有跑满，但性能仍难以上升；
- P 链是重要的暴露串行链，却不是 raw-resource roofline 中唯一的最大项；
- FP16 的计算和 L1/TEX 活动率更高，而 FP8 的 barrier stall 比例反而更高；
- TMEM→REG 的实际 S 读取仅约 42.91 cycles，不是旧模型中的 512 cycles；
- 仅调度 P、扩大 `tcgen05.ld`、继续改变 MUFU 分流比例，均不能获得 10%。

严格地说，不能证明任何全新算法结构都不可能再快 10%。本文结论的边界是：**在当前 128×128 tile、2CTA ownership、S/P 和 dP/dS TMEM 复用关系及现有精度类型不变的依赖图中，常规指令调度和局部参数调整已经到达结构性上限。** 若要跨过 10%，需要重做 tile/ownership/流水线，使同步可被合法删除，或同时把寄存器与 SMEM 降到双 CTA 驻留阈值；这不是普通调参。

## 2. 最新基线、精度与 10% 目标

GPU application-clock 请求值锁定在 1965 MHz。下表是与用户历史数据一致的短窗口最高基线，性能值为三次独立运行的中位数；10% 目标也以该最高基线计算。

| Shape (`Q=K`) | 最新 FP8 基线 | 时间 | 10% 目标 | 需要新增性能 |
|---:|---:|---:|---:|---:|
| 4096 | **1235.92 TFLOPS** | 0.2780 ms | 1359.512 TFLOPS | +123.592 |
| 8192 | **1561.82 TFLOPS** | 0.8800 ms | 1718.002 TFLOPS | +156.182 |
| 16384 | **1657.19 TFLOPS** | 3.3174 ms | 1822.909 TFLOPS | +165.719 |

原先记录为 1235.88 / 1561.83 / 1636.77 TFLOPS。前两项与此前短窗口重测一致，16384 的短窗口最高中位数提高至 1657.19 TFLOPS，因此后文以该锁定最高值为准。

交付前在加入候选消融宏后重新构建了同一 baseline target。目标 kernel 的 6691 行 SASS 与上述最高基线二进制**逐行完全一致（diff=0）**；ptxas 仍为 128 registers、4 barriers、8-B stack frame、4-B spill store 和 4-B spill load。使用显式短窗口 `iterations={100,50,20}` 对三个 shape 重测，三次中位数为 1230.51 / 1557.04 / 1656.22 TFLOPS，分别只比最高基线低 0.44% / 0.31% / 0.06%，属于运行状态波动而不是代码回归。

需要特别说明：`nvidia-smi -lgc 1965,1965` 是请求时钟，不保证功耗受限时仍维持 1965 MHz。1000-iteration 的 16384 持续负载触发 1000 W power limit，实测 P-clock 降到约 1680–1687 MHz，吞吐也随之下降。本文所有 baseline/候选性能比较均采用相同的短窗口口径；不能把持续功耗受限结果与短窗口峰值混合。SASS、精度和 occupancy 结论与该性能窗口无关；物理 cycle 则使用各自微基准/NCU 的同一运行上下文单独报告，不与短窗口 TFLOPS 直接换算。

严格精度和稳定性结果如下。输入 fingerprint 完整性检查均通过。

| Shape | maxdiff dQ | maxdiff dK | maxdiff dV | 稳定性 |
|---:|---:|---:|---:|---:|
| 4096 | 0.0878906 | 0.0937500 | 0.0078125 | 3 次 hash 一致 |
| 8192 | 0.0859375 | 0.0859375 | 0.00585938 | 2 次 hash 一致 |
| 16384 | 0.0683594 | 0.0761719 | 0.00585938 | 2 次 hash 一致 |

## 3. 必须区分的两种 cycle

旧分析中最大的混淆，是把用于性能折算的“等效 cycle”和 NCU/微基准测得的物理时钟 cycle 混在了一起。本文严格分开：

1. **物理 cycle**：来自 `clock64` 微基准或 NCU 的 SM active cycles，用来回答某条真实依赖链需要多少硬件周期。
2. **归一化等效 cycle**：以 FP16 的 2688-cycle roofline 为参考，用性能比反推的无量纲性能坐标；用于跨实现折算，但不是实际经历的 GPU 时钟数。

对于固定 shape，backward FLOP 口径相同，因此：

```text
C_eq,FP8 = C_ref,FP16 × Perf_FP16 / Perf_FP8
Perf_FP8 = Perf_FP16 × C_ref,FP16 / C_eq,FP8
```

这里 `C_ref,FP16=2688` 来自 FA4 2CTA backward 的最大资源项。该式适合复用参考 DOCX 的分析方法，但 `C_eq` 不能与后文 NCU 的 3318.63 physical cycles/tile 直接相减。

## 4. FA4 2CTA backward 的 FP16→FP8 raw roofline

FlashAttention-4 Table 3 对 B200 2CTA backward 给出的 FP16/BF16 资源 cycle 为：

| 资源分支 | FP16/BF16 cycles |
|---|---:|
| 5 个 MMA | 2560 |
| MMA 的 SMEM operands | 1536 |
| dS SMEM write | 256 |
| dS DSMEM exchange | 384 |
| FP32 dQ write + read | 512 |
| **SMEM 合计** | **2688** |
| exponential unit | 1024 |

所以 FP16 raw roofline 为：

```text
C_FP16,raw = max(2560, 2688, 1024) = 2688 cycles
```

在不改变当前 FP8 路径中间类型的前提下做资源工作量转换：

- FP8 Tensor Core 吞吐约为 FP16 的 2 倍：`2560 → 1280`；
- FP8 MMA operands：`1536 → 768`；
- FP8 dS write：`256 → 128`；
- FP8 dS DSMEM：`384 → 192`；
- dQ accumulator 仍为 FP32：`512 → 512`，不能减半；
- 完整 P 重构链由独立微基准得到 1125.06 cycles。

因此理想重叠下的 FP8 raw-resource roofline 为：

```text
C_SMEM,FP8 = 768 + 128 + 192 + 512 = 1600 cycles
C_FP8,raw  = max(C_TC=1280, C_SMEM=1600, C_P=1125) = 1600 cycles
```

这个结果修正了“FP8 后 P-softmax 必然成为最大 raw 资源项”的说法：**P 是很大的暴露串行链，但理论最大资源工作量仍是约 1600-cycle 的 SMEM 分支。** 真正使实测远离 1600 的，是只有一 CTA/SM 时无法覆盖的同步与 scoreboard 空洞。

## 5. 完整 P 重构链：不是只计算 exp，也不包含 dS

Backward 的 P 阶段不重新执行 forward softmax 的 row-max 和 row-sum；它读取已经保存的 LSE，实际执行的是：

```text
S(FP32, TMEM)
  → TMEM→register
  → FP32 (S × softmax_scale × log2(e) − LSE)
  → 完整 exp2 语义
  → FP32→E4M3（当前既有 P 类型）
  → 256-thread S/P alias barrier
  → register→TMEM
  → P publication
```

dS 在 P 发布之后独立处理，本文没有把 dS 算术强行串加到 P 链上，符合 P/dS 分段分析要求。

### 5.1 当前真实 SASS 工作量

当前 `fag_stage_regalloc` 的 P 段从首条 S `LDTM`（PC `0x6ee0`）到最后一条 P `STTM`（PC `0x7d40`），包含 231 条静态 warp 指令。主要构成为：

| SASS 指令类别 | 静态条数/warp | 作用 |
|---|---:|---|
| `MUFU.EX2` | 48 | 75% 元素的硬件 exp2 |
| `FFMA2` | 32 | 两路 FP32 argument FMA |
| `FFMA2.FTZ` | 24 | 25% 元素的软件 exp2 多项式 |
| `F2FP...E4M3...PACK` | 32 | FP32→E4M3 |
| `FMNMX.FTZ` | 16 | 软件 exp2 range clamp |
| `FADD2.FTZ` | 16 | 软件 exp2 range reduction |
| `FADD2.FTZ.RM` | 8 | 软件 exp2 rounding step |
| `LDS.128` | 16 | LSE/辅助共享内存读取 |
| `LDTM.x16` | 4 | S 的 TMEM→register |
| `STTM.x4` | 4 | P 的 register→TMEM |
| `BAR.SYNC.DEFER_BLOCKING` | 1 | 256 compute threads 的 S/P alias 同步 |

这也修正了旧笔记中“当前 B200 SASS 没有 `FFMA2`”的错误：当前二进制明确生成了 `FFMA2` 和 `FFMA2.FTZ`。

### 5.2 理论下界

当前一个 CTA tile 有 16384 个 P 元素，其中 75% 走硬件 MUFU：

```text
N_MUFU = 16384 × 75% = 12288 results
C_MUFU,min = 12288 / (16 results/cycle/SM) = 768 cycles
```

若乐观地按 4 个 warp scheduler、每 scheduler 每 cycle 发射一条 warp 指令估计，静态 issue 下界为：

```text
C_issue,min ≈ 231 instructions/warp × 8 warps / 4 schedulers = 462 cycles
```

这只是乐观下界，不包含依赖、端口冲突和 barrier。MUFU 的 768-cycle 下界已经大于该 issue 下界；同时软件 exp2 会占用 packed FP32 FMA 路径，因此必须在两条执行管线间平衡，而不能简单把所有 exp2 推给某一侧。

### 5.3 独立完整 P-chain 微基准

微基准执行真实的 4×`LDTM.x16`、FP32 scale/LSE、与主算子完全相同的软件 exp2、硬件 MUFU、FP32→E4M3、256-thread barrier、4×`STTM.x4`，并实际消费寄存器结果；ptxas 无 spill。一个 128×128 P tile、8 compute warps 的中位数为：

| P-chain 版本 | physical cycles/tile | 相对当前 3:1 |
|---|---:|---:|
| 仅 transport：LDTM + barrier + STTM | 165.91 | — |
| 100% 硬件 MUFU | 1188.08 | +63.02 |
| 100% 软件 exp2 | 1686.53 | +561.47 |
| 1:1 硬件/软件 | 1261.10 | +136.04 |
| **当前 3:1 硬件/软件** | **1125.06** | **最佳** |
| 7:1 硬件/软件 | 1140.16 | +15.10 |
| 15:1 硬件/软件 | 1216.06 | +91.00 |

结论是：当前 25% 软件、75% MUFU 的分流已经处在实测局部最优点。它比全硬件 MUFU 还快约 63 cycles，说明 CUDA Core/FMA 与 MUFU 的分流确实有效；继续改变比例不能提供 10%。

## 6. TMEM→REG：为什么不是 128 B/cycle，也不是 512 cycles

PTX 对 `tcgen05.ld.sync.aligned.32x32b.x16.b32` 的定义是：一个 warp 访问 32 个 Tensor Memory lanes，每 lane 访问 `(32 × num)` bits。代入 `num=16`：

```text
payload/instruction = 32 lanes × 32 bits × 16 = 16384 bits = 2048 B
```

这是**单条指令的逻辑 payload**，不是“每 cycle 处理 2048 B”，也不是端口吞吐。NVIDIA PTX ISA 没有为该指令公开一个可直接代入的 128 B/cycle 数字。因此旧模型中的：

```text
64 KiB / 128 B/cycle = 512 cycles
```

没有指令手册依据。

独立微基准使用真实 `tcgen05.ld.sync.aligned.32x32b.x16.b32`，把读取数据放入真实寄存器并通过后续指令消费，再用 wait/fence 保证操作不能被删除。结果为：

| 并发 warps | 有效 B/cycle | 64 KiB-equivalent cycles |
|---:|---:|---:|
| 1 | 191.43 | 342.35 |
| 2 | 382.83 | 171.19 |
| 4 | 765.66 | 85.59 |
| **8（主算子 P 路径）** | **1527.21** | **42.91** |
| 16 | 1607.12 | 40.78 |
| 32 | 1626.41 | 40.29 |

单 warp、单 2 KiB atom 的依赖延迟为 8.79 cycles；它是 latency，不是可持续带宽。主算子有 8 个 compute warps，因此正确的 S tile TMEM→REG throughput 估计是 **42.91 cycles/64 KiB**。

这个结论还得到完整算子消融支持：把四条 `x16` 改为两条 `x32` 后，三个 shape 为 1233.55 / 1561.92 / 1669.14 TFLOPS，没有稳定的整体提升。若 S load 真是 512-cycle 最大瓶颈，指令数减半不可能几乎无效。

## 7. NCU 的物理 cycle 与 10% 缺口

以 `Q=K=8192` 为代表点，FP8 kernel 的 launch/occupancy 为：

- grid：1024 CTAs；block：512 threads；cluster：2 CTAs；148 SM；
- registers/thread：128；
- dynamic SMEM：141.57 KB/CTA，driver SMEM：1.02 KB/CTA；
- block limit：registers = 1，shared memory = 1；
- theoretical active warps：16；theoretical occupancy：25%；
- achieved active warps：15.99；achieved occupancy：24.98%。

NCU 的 `SM active cycles = 1,469,523.72`。每 CTA 有 `8192/128=64` 个 inner-loop Q tiles，因此每个 per-SM CTA inner tile 的物理 cycle 为：

```text
C_physical = 1,469,523.72 / [(1024 / 148) × 64]
           = 3318.63 cycles/tile
```

固定 FLOP 和频率下，提高 10% 要求：

```text
C_target = 3318.63 / 1.10 = 3016.93 cycles/tile
required saving = 301.69 cycles/tile
```

完整 P 链的 1125.06 cycles 相当于该物理 tile 的 33.90%，但 P 与其他阶段存在部分重叠，不能把两者简单相加。若其他部分完全不变，单靠 P 链获得 10% 需要再消掉约 `301.69/1125.06=26.8%`；分流扫描、barrier 正确性和预取消融均表明当前局部实现没有这样的空间。

关键 NCU 活动率与 stall 如下：

| FP8，N=8192 | 数值 |
|---|---:|
| DRAM throughput | 2.70% |
| L1/TEX throughput | 53.01% |
| SM throughput | 41.41% |
| tensor-memory active | 40.63% |
| tensor pipe active | 37.75% |
| issue active | 38.25% |
| long scoreboard stall | 31.67% |
| barrier stall | 23.99% |
| wait stall | 11.86% |
| MIO throttle | 3.91% |
| math-pipe throttle | 2.32% |

stall 百分比表示采样到的 warp scheduler 状态，彼此不可直接相加为执行时间。作为“等效 stall mass”仅用于量级判断：barrier 约对应 796 cycles/tile，long scoreboard 约对应 1051 cycles/tile，均足以覆盖 302-cycle 的 10% 缺口。这与“低 occupancy 无法隐藏依赖”一致。

另外，`tensor pipe active × physical cycles ≈ 0.3775×3318.63=1253 cycles`，接近理论 FP8 MMA 的 1280 cycles；`tensor-memory active × physical cycles ≈ 1348 cycles`，反映 TMEM 指令与 MMA 处在相近的重叠窗口；`L1/TEX × physical cycles ≈ 1759 cycles`，接近 1600-cycle SMEM roof 加实际调度开销。这是理论 roofline 与 NCU 的独立交叉验证。

## 8. 与 FP16 实现的代码和 NCU 对比

FP16 对照源码和 FP8 源码具有相同的关键结构：

- 128×128 tile、512 threads、8 compute warps；
- S 以 FP32 accumulator 形式从 TMEM 读取；
- P 使用 FP32 packed FMA 形成 `S×scale×log2(e)-LSE`，再执行完整 exp2；
- S 与 P 复用同一 TMEM 区域；
- P 写回前都必须等待 compute threads 完成 S load；
- P 发布后再进入独立 dS 阶段。

FP16 当前源码在 P 段明确写有：“若无该 barrier，一个 warp 可能在另一个 warp 尚未读完 S 时写 P”。FP8 的 S/P ownership 也不是逐线程完全同构，因此同一 hazard 仍存在。

性能对比必须区分用户原始 FP16 记录和当前源码重测：

| Shape | 用户提供 FP16 | 当前 FP16 源码重测 | 当前 FP8 | FP8/用户 FP16 | FP8/当前 FP16 |
|---:|---:|---:|---:|---:|---:|
| 4096 | 1015.00 | 1076.43 | 1235.92 | 1.2177× | 1.1482× |
| 8192 | 1166.00 | 1342.51 | 1561.82 | 1.3395× | 1.1634× |
| 16384 | 1269.00 | 1402.94 | 1657.19 | 1.3059× | 1.1812× |

当前 FP16 代码比用户原记录更新且更快，所以两组 FP16 数据不能混合校准。二者都说明 FP8 不可能仅凭 Tensor Core 2× 吞吐得到 2× 端到端加速。

在相同 `N=8192` 下，NCU 对比如下：

| 指标 | FP16 | FP8 | 解释 |
|---|---:|---:|---|
| registers/thread | 128 | 128 | FP8 未释放 CTA 驻留所需的寄存器级别 |
| theoretical occupancy | 25% | 25% | 两者都只有 1 CTA/SM |
| achieved occupancy | 23.17% | 24.98% | FP8 仍不能得到第二 CTA |
| dynamic SMEM/CTA | 231.42 KB | 141.57 KB | FP8 降低很多，但仍超过双驻留阈值 |
| compute/SM throughput | 78.89% | 41.41% | FP16 有更长的可执行工作来填管线 |
| L1/TEX throughput | 71.50% | 53.01% | FP8 减少字节后端口未饱和 |
| issue active | 34.33% | 38.25% | 都存在大量不可发射周期 |
| long scoreboard stall | 37.10% | 31.67% | 两者均受生产者—消费者依赖影响 |
| barrier stall | 14.71% | 23.99% | FP8 缩短 MMA 后固定同步被显著暴露 |
| math-pipe throttle | 1.93% | 2.32% | CUDA Core 峰值不是独立瓶颈 |

FP16 的 SMEM 和 MMA 工作更多，因此可用计算覆盖固定 P/barrier 延迟；FP8 把这些可覆盖工作缩短，却没有换来更高 occupancy。这正是瓶颈从“FP16 raw SMEM/MMA 资源工作量”转为“FP8 低驻留下暴露的 P + barrier + scoreboard 依赖”的原因。

## 9. 为什么第二个 CTA 无法驻留

### 9.1 寄存器是独立的一票否决

B200（compute capability 10.0）每 SM 有 64K 个 32-bit registers。当前 kernel 为 512 threads、128 registers/thread：

```text
512 threads × 128 registers = 65536 registers = 64K registers/CTA
```

因此单个 CTA 已占满按 launch resource 计算的寄存器预算。要驻留两个同样大小的 CTA，必须满足：

```text
registers/thread ≤ 65536 / (512 × 2) = 64
```

但仅 P 阶段的 128×128 FP32 S fragment，由 256 compute threads 分担，就已经是：

```text
16384 FP32 values / 256 threads = 64 FP32 values/compute thread
```

这 64 个寄存器还没有计入 LSE、地址、pipeline state、软件 exp2 临时量、P 量化结果及 dP/dS 状态。因此在当前 tile 和完整 P 语义下，把整个 kernel 压到 ≤64 registers/thread 不具可行余量。安全地把 dP load 提前到 P 后用于覆盖 latency，会扩大 live range，并实测产生 64-B spill store、72-B spill load、48-B stack frame，性能从约 1563 降到 1406 TFLOPS。

### 9.2 SMEM 是第二个独立的一票否决

B200 每 SM 最大 shared memory 约 228 KiB。当前 FP8 CTA 的 NCU dynamic SMEM 为 141.57 KB，另有约 1.02 KB driver SMEM。两个 CTA 的需求显著超过 228 KiB。即使先解决寄存器，SMEM 仍会把 block limit 锁在 1。

要双驻留，单 CTA 连同 driver overhead 必须降到约 114 KiB 以下，相对当前需要至少约 18–20% 的结构性缩减。当前 S/P、dP/dS、dQ 和 cluster exchange buffer 的 lifetime/alias 已高度复用；局部布局调参没有这样的空间。

所以结论不是“也许寄存器不够”，而是 NCU 已直接报告：**register block limit = 1，shared-memory block limit = 1；两者分别都足以禁止第二 CTA。**

## 10. 消融实验总表

所有标为“有效”的候选均保持输入、输出和中间类型；不稳定或超出 `maxdiff<0.1` 的版本直接判无效。

| 消融/候选 | 4096 | 8192 | 16384 | 结论 |
|---|---:|---:|---:|---|
| **baseline：3:1 MUFU/软件 + current regalloc** | **1235.92** | **1561.82** | **1657.19** | 最终基线 |
| 手工 dS bit pack | 1230.68 | 1563.03 | 1672.28 | 相关 SASS 被编译器折叠；无可重复整体收益 |
| TMEM load `4×x16 → 2×x32` | 1233.55 | 1561.92 | 1669.14 | 基本中性，排除 S-load 指令数瓶颈 |
| exp2 two-phase | 1205.20 | 1532.80 | 1631.17 | 约慢 2–3% |
| exp2 hardware-first | 1205.90 | 1532.48 | 1628.00 | 约慢 2–3% |
| direct dS path | 1116.14 | 1382.50 | 1484.45 | 约慢 10–11% |
| compute regalloc P144（三次中位数） | 1243.92 | 1569.53 | 1657.02 | +0.65%/+0.49%/中性，不是三 shape 通用增益 |
| dQ store 64/128/batched（N=8192） | — | 1187.61/1455.43/1372.97 | — | 均显著变慢 |
| split dK（N=8192） | — | 1506.56 | — | 变慢 |
| P store x8/x16（N=8192） | — | 1557.83/1558.77 | — | 中性或略慢 |
| P144 + x32 load（N=8192） | — | 1547.68 | — | 组合后更慢 |
| reduction regalloc 144（N=8192） | — | 1551.16 | — | 更慢 |
| dP 在 P 前预取 | — | deadlock | — | 形成 pipeline dependency cycle |
| dP 在 P 后安全预取 | — | 1405.89 | — | spill 激增，约慢 10% |
| 删除 P barrier/self-owned 假设 | — | — | — | dQ=0.126953，dK=0.111328，dV=0.068359；不稳定 |
| 每 warpgroup 独立 P barrier | 1200.13 | — | — | 单次 diff 可过但 5 次 hash 全变化，判无效 |

完整算子的 exp2 分流扫描也与 P-chain 微基准一致：

| 软件 exp2 比例 | 4096 | 8192 | 16384 |
|---:|---:|---:|---:|
| 0% | 1150.11 | 1449.21 | 1538.61 |
| 100% | 1037.86 | 1295.70 | 1365.60 |
| 50% | 1159.65 | 1470.36 | 1547.62 |
| **25%** | **1228.30** | **1551.04** | **1630.21** |
| 12.5% | 1190.20 | 1499.19 | 1588.56 |
| 25% + current regalloc（旧记录） | **1235.88** | **1561.83** | **1636.77** |

## 11. 对每个候选“硬件瓶颈”的自我反驳

| 假设 | 支持它的直觉 | 反证 | 判定 |
|---|---|---|---|
| HBM bandwidth 是瓶颈 | 长序列数据量大 | DRAM throughput 仅 2.70% | 排除 |
| TMEM→REG 端口是最大瓶颈 | S 为 64 KiB FP32 | 8-warp 实测仅 42.91 cycles；x32 load 无增益；TMEM active 40.63% | 排除为单独瓶颈 |
| Tensor Core 不够快 | backward 有 5 个 MMA | tensor pipe active 37.75%；FP8 理论 MMA 1280 cycles，低于 SMEM 1600 | 排除为单独瓶颈 |
| CUDA Core 与 Tensor Core 差距导致 FMA 饱和 | 软件 exp2 使用 packed FMA | math-pipe throttle 仅 2.32%；3:1 分流反而优于全硬件和更多软件 | 排除“CUDA Core 峰值饱和”，但 FMA/MUFU 平衡是 P 局部约束 |
| SMEM 带宽是唯一瓶颈 | raw roofline 最大项为 1600 cycles | L1/TEX 仅 53.01%，并未达到持续端口峰值；大量时间处于 barrier/scoreboard stall | SMEM 工作量重要，但不是“端口跑满”式唯一瓶颈 |
| P-exp 是唯一瓶颈 | FP8 后 exp 不减半 | 完整 P 链 1125 < SMEM 1600；只算 exp 会漏掉 FMA/转换/barrier/store | P 是暴露关键链，但不是唯一 raw roof |
| P barrier 可以删掉 | S 已经发出异步 load | S/P 同址且 thread ownership 不同；删除或缩小 barrier 导致精度失败/跨次 hash 不稳定 | barrier 是正确性依赖，不能删 |
| 增加预取可以隐藏 long scoreboard | P 期间存在算术窗口 | P 前预取形成 pipeline cycle；P 后预取使 live range 溢出并慢约 10% | 当前寄存器预算不允许 |
| 提高 occupancy 可隐藏所有延迟 | 25% occupancy 明显偏低 | 寄存器和 SMEM 各自都把 CTA/SM 限制为 1；必须同时解决两项 | 正是绑定限制，但需要结构重写 |

自我反驳后的最小充分结论是：

> **Raw 资源层面，FP8 的最大工作量是约 1600-cycle SMEM 分支；调度层面，真正绑定实测性能的是一 CTA/SM 的驻留上限，导致 P 的 1125-cycle 完整链、必需的 S/P alias barrier 和 L1TEX/TMEM long-scoreboard 延迟无法被其他 warps/CTA 隐藏。**

## 12. 校准 Roofline：从用户 FP16 数据折算到当前 FP8

沿用参考文档的固定 FLOP、性能反比等效 cycle 方法，并使用用户提供的 FP16 参考 1015 / 1166 / 1269 TFLOPS：

```text
C_eq,FP8(shape) = 2688 × Perf_FP16(shape) / Perf_FP8(shape)
η_pipeline(shape) = C_raw,FP8 / C_eq,FP8 = 1600 / C_eq,FP8
Perf_pred = Perf_FP16 × 2688 / (1600 / η_pipeline)
```

| Shape | 用户 FP16 | FP8 raw roof | 校准 pipeline 效率 | FP8 等效 cycle | 模型 FP8 | 当前实测 FP8 |
|---:|---:|---:|---:|---:|---:|---:|
| 4096 | 1015 | 1600 | 72.48% | 2207.52 | 1235.92 | 1235.92 |
| 8192 | 1166 | 1600 | 79.73% | 2006.77 | 1561.82 | 1561.82 |
| 16384 | 1269 | 1600 | 77.73% | 2058.35 | 1657.19 | 1657.19 |

这里的效率不是任意添加的“P=2100 cycles”拟合项，而是对 raw resource roof 与实际 pipeline/同步损失的明确分层：

- 4096 只有约 3.46 CTA waves/SM，prologue/epilogue 和尾部占比最高，所以效率最低；
- 8192 达到约 6.92 waves/SM，流水线摊销最好；
- 16384 的固定开销继续被摊薄，但更长的依赖/缓存路径使效率没有单调上升；
- NCU 在 8192 上直接观测到 31.67% long-scoreboard、23.99% barrier、25% occupancy，给出了该效率损失的硬件来源。

如果改用当前重测的 FP16 代码 1076.43 / 1342.51 / 1402.94，得到的 FP8 等效 cycle 是 2341.13 / 2310.55 / 2275.60。它们与上表不同，是因为 FP16 参照实现本身已变快；不能拿这组 cycle 再去配用户旧 FP16 性能。

因此，旧文档中约 2.0k–2.2k 的数字可以保留为**归一化等效 cycle**，但不能再解释为“完整 P 链的物理 cycle”。真实独立 P 链是 1125.06 physical cycles，真实 NCU inner tile 是 3318.63 physical cycles。

## 13. 为什么当前设计达不到再快 10%

在 `N=8192` 上，10% 需要净省约 302 physical cycles/tile。逐项检查：

1. S 的 TMEM→REG 只有 42.91 cycles，即使不现实地完全删除，也不足 302 cycles。
2. 完整 transport（LDTM + barrier + STTM）为 165.91 cycles，即使全部免费，也仍不足 302 cycles。
3. 当前 3:1 P 分流已是局部最优；全硬件、多软件、two-phase 和 hardware-first 都更慢。
4. 256-thread barrier 由 S/P 同址且 ownership 不同决定；删除或拆分后精度/稳定性失败。
5. dP 预取若放在 P 前会形成等待环；放在 P 后会因额外 FP32 live range 发生 spill，损失约 10%。
6. 第二 CTA 无法提供 latency hiding：128 regs/thread 和 141.57 KB SMEM/CTA 各自都禁止双驻留。
7. HBM、Tensor pipe、math pipe 和 TMEM pipe 都没有峰值饱和，因此不存在一个可通过“加宽单端口”直接拿回 302 cycles 的简单资源项。

所以当前性能是**结构性 latency-hiding roof**，不是某条单指令的吞吐 roof。要获得 10%，至少要满足以下某一种结构变化：

- 重构 S/P 的 TMEM ownership，使每个写 P 的线程只覆盖它自己已完成读取的 S，从而合法删除全 256-thread barrier；
- 为 S 与 P 提供不冲突的 TMEM 区域，并重新安排 dQ/dP/dS alias，避免增加总 TMEM/SMEM lifetime；
- 改变 tile/decomposition，使寄存器降至 ≤64/thread 且 SMEM 降至约 ≤114 KiB/CTA，从而得到 2 CTA/SM；
- 构造跨 tile 的更深 producer-consumer pipeline，但必须在不增加寄存器和 SMEM live set 的情况下完成。

这些方向理论上仍可保持所有数据类型不变，但都需要重新设计 ownership 或 tile，而不是在当前实现上继续调一个模板参数。若不进行此级别重写，10% 目标没有可验证的 cycle 来源。

## 14. 复现信息

### 14.1 基线

```bash
cd /home/ubuntu/workspace/oyhj/temp/test/test_merge/Benchmark/FAG_FP32
cmake --build build-profile --target fag_stage_regalloc -j

./build-profile/fag_stage_regalloc \
  --b=1 --h=16 --h_k=16 --q=8192 --k=8192 --d=128 --d_vo=128 \
  --iterations=50 --skip-reference
```

短窗口回归对 4096/8192/16384 分别使用 100/50/20 iterations，并对每个点运行三次取中位数。精度与稳定性应独立运行，避免把昂贵 reference 混入性能窗口：

```bash
./build-profile/fag_stage_regalloc \
  --b=1 --h=16 --h_k=16 --q=8192 --k=8192 --d=128 --d_vo=128 \
  --iterations=3 --verify --strict-max-diff=0.1 \
  --strict-mean-diff=0.02 --stability-runs=2
```

把 `q/k` 分别替换为 4096、8192、16384；4096 的稳定性回归使用 3 runs。基线目标在 `CMakeLists.txt` 中定义为：

```text
fag_stage_regalloc: BWD_2SM FAG_EX2_EMU_STRIDE=4 FAG_REGALLOC_FA4_CURRENT
```

### 14.2 微基准

```bash
./build/tmem_reg_bandwidth_microbench
./build-profile/b200_softmax_p_microbench
```

源码：

- `tools/tmem_reg_bandwidth_microbench.cu`
- `tools/b200_softmax_p_microbench.cu`

### 14.3 SASS

```bash
cuobjdump --dump-sass build-profile/fag_stage_regalloc > fag_stage_regalloc.kernel.sass
cuobjdump --dump-sass build-profile/b200_softmax_p_microbench > b200_softmax_p_microbench.sass
```

## 15. 资料来源

- FlashAttention-4，§3.2 / Table 3，2CTA backward cycle 分析：<https://arxiv.org/abs/2603.05451>
- NVIDIA PTX ISA，`tcgen05.ld` 的 shape、repeat、寄存器 fragment 和异步语义：<https://docs.nvidia.com/cuda/parallel-thread-execution/#tcgen05-instructions-tcgen05-ld>
- NVIDIA CUDA Programming Guide，compute capability 10.x 的 registers/shared-memory/warp 规格：<https://docs.nvidia.com/cuda/archive/13.1.1/cuda-programming-guide/05-appendices/compute-capabilities.html>
- NVIDIA Blackwell Tuning Guide：<https://docs.nvidia.com/cuda/archive/13.0.2/blackwell-tuning-guide/index.html>
- NVIDIA CUDA Best Practices Guide，occupancy、register pressure 与 latency hiding：<https://docs.nvidia.com/cuda/cuda-c-best-practices-guide/index.html>

---

最终判定：**当前 2SM + 3:1 MUFU/软件分流版本已经到达现有 tile/ownership/精度依赖图的可验证上限。最具体的硬件限制是寄存器与 SMEM 联合造成的一 CTA/SM 驻留上限；它暴露了不可删除的 S/P alias barrier 和 long-scoreboard 延迟。TMEM S-load、HBM、Tensor Core 峰值或 CUDA Core 峰值均不是能够单独解释并解除当前上限的瓶颈。**
