# MXFP8 与 FlashAttention-4 MUFU 分流对比分析

日期：2026-08-04  
设备：NVIDIA B200（148 SM，SM100）  
CUDA：13.3  
GPU 核心频率：1965 MHz（计时阶段锁频）

## 1. 结论

MXFP8 分流没有明显提速，不是因为实现里存在运行时分支、取模、溢出寄存器或额外访存，而是因为它所在的 softmax 临界段已经被 `MXFP8_E2RSF` 填得很满。把一部分 `MUFU.EX2` 替换为多条 packed FMA/ALU 指令后，虽然 MUFU/XU 压力和部分等待下降，但同一个 softmax warp 的普通数学发射槽变得拥塞，最终只是把瓶颈从 MUFU 等待搬到了 math-pipe/issue-slot 压力。

官方 FA4 的有效场景则满足两个条件：

1. 原始关键路径确实受 MUFU 吞吐和结果等待约束；
2. 普通 FMA/ALU 管线仍有足够空槽，且 exp2 临界段没有同时融合 row-sum 等额外工作。

本机 A/B 数据直接支持这一判断：官方 BF16 分流在减少约 18.6% XU 指令后，`wait` 降低 34.2%，而 math-pipe throttle 只增加 3.6%，中位性能提升约 4.5%；MXFP8 在近似相同的激进卸载比例下，math-pipe throttle 增加约 89.5%，正常计时反而下降约 0.4%。最终保留的 6.25% MXFP8 分流把回退压到噪声范围，但也只得到约 +0.1%，不能视为有效提升。

因此，当前问题的本质不是“分流比例还不够好”，而是“分流被加进了一个已经通过 E2RSF 软件流水化的循环”。若不先改变 row-sum、P 转换和 P 发布的排布，继续微调比例很难得到 FA4 那种收益。

## 2. 对比对象

### 2.1 MXFP8 实现

- 入口：`/home/ubuntu/workspace/oyhj/temp/test/test_merge/Benchmark/FA/b200_blackwell_fmha_mxfp8/b200_blackwell_fmha.cu`
- 主循环：`collective/sm100_fmha_fwd_mainloop_mxfp8_n128_pvmx.hpp`
- 独立分流实现：`collective/fa4_mufu_split_sm100.hpp`
- 基线目标：`op6static2sm_b200_blackwell_fmha_mxfp8`
- 分流目标：`op6static2sm_fa4mufu_b200_blackwell_fmha_mxfp8`
- 最终分流策略：只在 pair `[8, 24)` 内每 8 个 pair 选择 1 个，共 2/32 pair，即 6.25%。

MXFP8 的一个 softmax group 每次处理 64 个元素。循环中依次进行：packed scale/subtract、exp2、滞后 8 元素的 row-sum 累加，随后还要完成 E4M3 P 转换与写出。`MXFP8_E2RSF` 的设计目的本身就是用已就绪的旧 exp2 结果填补新 MUFU 指令的等待空洞。

### 2.2 官方 FA4 实现

- 主实现：`/home/ubuntu/workspace/oyhj/FAG/flash-attention/flash_attn/cute/flash_fwd_sm100.py`
- softmax 分流：`flash_attn/cute/softmax.py::apply_exp2_convert`
- Cody-Waite + 三次多项式：`flash_attn/cute/utils.py::ex2_emulation_2`

官方实现把一行划分为 32 元素 fragment，在编译期决定每个 float2 使用硬件 `MUFU.EX2` 还是 packed 软件模拟。对 head-dim 128、非因果、2-CTA 的默认配置，`freq=10, start_fragment=1`：首 fragment 和末 fragment 保持硬件 exp2，只在中间 fragment 分流，约有 20% 的整行元素进入模拟路径。

更关键的是官方执行顺序：

1. scale/subtract row max；
2. exp2 + 转换；
3. 分段写 P 到 TMEM，并尽早通知 MMA 消费 P；
4. P 已发布后再调用 `update_row_sum`。

也就是说，官方没有把 row-sum 加法塞进每一对 exp2 的核心循环。其 correction warpgroup 还把 O rescale 从 softmax 关键路径中解耦。

## 3. 计时结果

### 3.1 官方 FA4 BF16：可复现提升

输入：`B=8, H=16, SQ=SK=8192, D=128, causal=false`，20 次 warmup，50 次计时，交替顺序为 baseline → split → split → baseline。

| 版本 | 两次中位 TFLOPS/s | 合并中位代表值 | 相对变化 |
|---|---:|---:|---:|
| 全硬件 exp2，freq=0 | 1374.6, 1373.3 | 1373.95 | 基线 |
| 官方分流，freq=10 | 1438.6, 1432.4 | 1435.50 | **+4.48%** |

对应中位时间由约 3.2015 ms 降至 3.0635 ms，下降约 4.31%。这说明在该官方流水线上，分流确实缩短了端到端关键路径。

### 3.2 MXFP8 最终 6.25% 分流：基本持平

输入：`B=1, H=H_K=40, SQ=SK=170100, D=128, mask=no`，每个样本 5 次 warmup、20 次迭代，共 4 个独立样本。

| 版本 | 4 个样本 TFLOPS/s | 中位数 | 均值 | 相对变化 |
|---|---|---:|---:|---:|
| 基线 | 1534.96, 1528.91, 1528.00, 1526.90 | 1528.46 | 1529.69 | 基线 |
| 6.25% 分流 | 1534.39, 1530.25, 1529.76, 1528.43 | 1530.01 | 1530.71 | 中位 **+0.10%**，均值 **+0.07%** |

这个差值低于运行波动，结论应记为“持平”，而不是“已有稳定收益”。更激进的 12.5% 和 18.75% 版本分别约为 -0.22% 和 -0.39%。

### 3.3 官方 FA4 FP8 补充对照

还对当前官方仓库的 E4M3 FP8 路径做了单形状 A/B：`B=1, H=16, S=16384, D=128`。非因果 `freq=0/10` 和因果 `freq=0/8` 在本次驱动下均接近持平，未复现代码注释中某些形状的较大收益。

这项负对照很重要：即使在官方代码中，分流也不是无条件加速。官方配置本身明确按 1/2-CTA、causal、head-dim、架构和数据类型分别调参，并注明某些 FP8 非因果配置会回退。论文也只建议经验调优 10%–25%，而不是固定比例通用。

## 4. NCU 流水线证据

以下官方指标来自 `B=8,H=16,S=8192,D=128` 的 BF16 内核；MXFP8 指标来自 `B=1,H=40,S=16384,D=128`。绝对指令数因工作量不同不能横向比较，应该比较各自 A/B 的百分比变化。`*_per_issue_active.pct` 表示每个 active issue 周期的平均停顿 warp 数，可能大于 100%。

### 4.1 官方 FA4：等待显著下降，math pipe 几乎没有恶化

| 指标 | 全硬件 | 分流 | 变化 |
|---|---:|---:|---:|
| XU 指令 | 270,534,976 | 220,203,328 | **-18.6%** |
| FMA 指令 | 288,542,834 | 491,966,578 | +70.5% |
| ALU 指令 | 338,896,241 | 389,227,738 | +14.9% |
| wait stall | 226.46% | 149.09% | **-34.2%** |
| long scoreboard | 325.58% | 231.66% | **-28.8%** |
| short scoreboard | 38.68% | 27.39% | **-29.2%** |
| math-pipe throttle | 9.34% | 9.68% | **+3.6%** |
| MIO throttle | 10.44% | 3.28% | -68.6% |

虽然 FMA 指令增加很多，但 math-pipe throttle 基本不变，说明这些 FMA 大多进入了原本空闲的发射机会。减少 MUFU 后，真正的依赖等待同步下降，所以端到端时间变短。

### 4.2 MXFP8 最终 6.25%：等待下降较少，发射压力快速上升

| 指标 | 基线 | 6.25% 分流 | 变化 |
|---|---:|---:|---:|
| XU 指令 | 342,315,140 | 321,343,604 | **-6.1%** |
| FMA 指令 | 498,066,656 | 566,224,096 | +13.7% |
| ALU 指令 | 477,987,212 | 519,930,237 | +8.8% |
| wait stall | 204.80% | 185.58% | -9.4% |
| long scoreboard | 431.19% | 371.16% | -13.9% |
| short scoreboard | 46.83% | 46.36% | -1.0% |
| math-pipe throttle | 9.52% | 13.09% | **+37.5%** |
| not-selected stall | 36.37% | 46.52% | **+27.9%** |

NCU replay 中总 cycle 从 1.091B 降到 1.067B，但独立端到端计时没有稳定收益。多 pass replay 会改变 producer/consumer 的重叠和缓存状态，因此这里把 cycle 只作为诊断信号，不作为性能结论；性能结论以锁频独立计时为准。

### 4.3 近似相同 MUFU 卸载比例的直接对比

MXFP8 的 18.75% 激进版本曾把 XU 指令减少 18.4%，与官方 BF16 的 18.6% 基本相同，因此可以排除“MXFP8 只是卸载比例太小”这一解释。

| 约 18% XU 卸载 | 官方 FA4 | MXFP8 激进版 |
|---|---:|---:|
| XU 指令变化 | -18.6% | -18.4% |
| wait stall 变化 | -34.2% | -23.6% |
| math-pipe throttle 变化 | **+3.6%** | **+89.5%** |
| 正常计时 | **约 +4.5%** | **约 -0.4%** |

同样减少约 18% XU，结果完全不同。决定收益的不是替换了多少 `MUFU.EX2`，而是替换后的 FMA/ALU 能否利用空闲 issue slots，以及新增依赖链是否进入 softmax 的关键路径。

## 5. 为什么两个实现表现不同

### 5.1 MXFP8 已经用 E2RSF 消费了 MUFU 等待空洞

`MXFP8_E2RSF` 在 exp2 循环里读取滞后 8 个元素的已就绪结果，并用 packed add 累加 row-sum。原始硬件 MUFU 有较长延迟，正好给这组独立加法留下调度空间。换成软件 exp2 后，每个被选中的 float2 需要 clamp、range reduction、3 级 Horner FMA、指数位拼接等多条指令；这些指令和 E2RSF 都竞争普通数学/发射资源。

所以分流前的“FMA 利用率不高”不等于“该 warp 的 FMA 指令可以免费增加”。硬件整体 FMA pipe 可能空闲，但单 warp 的串行依赖、统一 issue 前端和相邻 packed add 已经限制了可用调度窗口。NCU 中 math throttle 的 +37.5% / +89.5% 就是直接证据。

### 5.2 官方 exp2 临界段更干净

官方 softmax 先完成 exp2 和低精度转换，按 fragment 将 P 写入 TMEM并提前释放给 MMA，之后再执行 row-sum。这样多项式 FMA 可以和其余硬件 MUFU 交错，而不会在每一个 pair 后面再插入 row-sum 累加。

MXFP8 则在一个 fully-unrolled 循环里同时承担：

- scale/subtract；
- 硬件或软件 exp2；
- E2RSF 滞后 row-sum；
- 后续 E4M3 P 转换和存储所需的结果就绪。

这让软件多项式更容易落入关键路径。分流减少了 MUFU 等待，但没有让 P 更早发布，节省的等待被新增发射工作抵消。

### 5.3 fragment 拓扑不同

官方针对 128 元素整行，以 32 元素 fragment 为调优单位，并保护首尾 fragment 使用硬件 exp2。首 fragment 影响流水线启动，末 fragment 影响 drain 和 P 最终发布；把模拟集中在中间 fragment，更容易和前后硬件工作重叠。

当前 MXFP8 每个 group 只有 64 元素，最终策略只在中间窗口选择 pair 8 和 pair 16。虽然也是编译期静态选择，但它并没有复刻官方 128 元素整行的 producer-release 节奏。相同的 pair 比例不代表相同的关键路径位置。

### 5.4 MXFP8 的 P 路径比 BF16 更重

官方 BF16 测试中，P 的目标精度和 row-sum 路径较简单。当前 MXFP8 还包含 E4M3 P 转换、静态 block-scale 语义、双 group/2SM 协作和对应的存储协议。虽然 `MXFP8_PSTATIC` 会编译掉动态 amax/scale 的大量代码，但剩余转换、row-sum 和同步仍然与软件 exp2 共享 softmax warp 的时间预算。

因此，FA4 论文的“把 MUFU 工作分到 FMA”只能在资源不冲突时成立；对 MXFP8 来说，FMA/ALU 并不是完全空闲的第二条高速公路。

### 5.5 分流实现本身是干净的

对最终实现做过以下审计：

- 分流策略是编译期常量，fully-unrolled call site 被常量折叠；
- 分流前后 branch 指令总数相同：3060 → 3060；
- 没有 `IDIV/REM`：0 → 0；
- 主内核寄存器保持 128；
- local memory 保持 0；
- 关闭所有模拟时，生成 SASS 的 SHA-256 与基线完全一致；
- SASS 中 `MUFU.EX2` 从 991 降到 935，`FFMA2` 从 448 增到 532，符合预期资源迁移。

因此不能把无收益归因于低级实现开销。它是一个真实的流水线/资源平衡问题。

## 6. 建议的下一步优化方向

### 6.1 先做“干净 exp2 burst”，再重新引入分流

优先实验不是继续扫 6.25% 附近的比例，而是做下面的结构性 A/B：

1. 暂时关闭 `MXFP8_E2RSF`；
2. exp2 + P convert/store 保持为一个干净 burst；
3. 在 P 已经写出并通知 MMA 后，再做 row-sum；
4. 在该版本上比较 hardware-only 与 10%–25% 分流；
5. 若出现收益，再考虑把 row-sum 移给 correction warp 或用另一个阶段覆盖，而不是重新塞回 exp2 pair 循环。

这一步是在复刻 FA4 真正有用的“调度上下文”，而不只是复刻其多项式公式。

### 6.2 按 fragment 和发布时刻调优

建议以 32 元素 fragment 为单位建立编译期策略：

- 首 fragment：全部 MUFU；
- 中间 fragment：尝试 10%、12.5%、18.75%、25%；
- 末 fragment：全部 MUFU；
- 分别对 g0/g1、causal/noncausal 调参；
- 评价指标除 TFLOPS/s 外，必须同时观察 P 首段/末段 release 时刻、math-pipe throttle 和 `wait`。

若某方案满足“XU 明显下降，但 math-pipe throttle 增幅超过 wait 降幅”，应立即淘汰，不必继续扩大分流比例。

### 6.3 保留 E2RSF 与保留 MUFU 可能就是当前最优组合

现有 E2RSF 已经以更低指令成本利用 MUFU 延迟窗口。若把 row-sum 移出关键段会损失更多，那么当前硬件 MUFU + E2RSF 很可能比 MUFU/FMA 分流更适合 MXFP8。优化目标应是端到端流水线，而不是强制让某项论文技巧在所有数据类型上都出现正收益。

## 7. 可复现命令摘要

MXFP8 独立计时：

```bash
./op6static2sm_b200_blackwell_fmha_mxfp8 \
  --b=1 --h=40 --h_k=40 --q=170100 --k=170100 --d=128 --mask=no \
  --warmup_iterations=5 --iterations=20

./op6static2sm_fa4mufu_b200_blackwell_fmha_mxfp8 \
  --b=1 --h=40 --h_k=40 --q=170100 --k=170100 --d=128 --mask=no \
  --warmup_iterations=5 --iterations=20
```

MXFP8 NCU 诊断形状：

```bash
ncu --kernel-name device_kernel --launch-count 1 \
  --metrics smsp__average_warps_issue_stalled_wait_per_issue_active,\
smsp__average_warps_issue_stalled_math_pipe_throttle_per_issue_active,\
smsp__average_warps_issue_stalled_short_scoreboard_per_issue_active,\
smsp__average_warps_issue_stalled_long_scoreboard_per_issue_active,\
sm__inst_executed_pipe_xu_realtime,sm__inst_executed_pipe_fma,\
sm__inst_executed_pipe_alu,sm__cycles_elapsed.sum \
  ./op6static2sm_b200_blackwell_fmha_mxfp8 \
  --b=1 --h=40 --h_k=40 --q=16384 --k=16384 --d=128 --mask=no \
  --warmup_iterations=1 --iterations=1
```

官方 BF16 A/B 使用当前仓库中的 `bench_fa4_emu.py`，将 `_TUNING_CONFIG` 的 `ex2_emu_freq` 分别编译为 0 和 10；每次切换后清理 CuTe DSL 编译缓存并启动新进程。

## 8. 论文与代码依据

FA4 论文给出的关键约束与本次数据一致：B200 MUFU 吞吐为 16 ops/clock/SM；软件 exp2 使用 Cody-Waite range reduction、指数位拼接和 Horner 多项式；全量模拟会增加寄存器压力、寄存器带宽和延迟，因此只对 10%–25% 元素做经验调优的部分模拟。论文同时强调新 pipeline 必须最大化 MMA、softmax 和内存操作的重叠。

本次实验说明：多项式只是分流的计算手段，真正决定收益的是它被放进什么样的 producer/consumer pipeline。当前 MXFP8 已经通过 E2RSF 对 MUFU 延迟做过一次软件流水化，所以直接叠加 FA4 式多项式并不会自动获得论文中的吞吐收益。
