# B200 MXFP8 FMHA 1300 TFLOPS 优化版本对比分析

## 1. 对比范围与结论

对比对象：

- 调优前：`oyhj/staticQuant/Benchmark/FA/b200_blackwell_fmha_mxfp8`
- 调优后：`oyhj/staticQuant_mxfp8/Benchmark/FA/b200_blackwell_fmha_mxfp8`

目录级 diff 涉及 8 个文件，核心性能改动集中在：

- `b200_blackwell_fmha.cu`
- `collective/sm100_fmha_fwd_mainloop_mxfp8_n128_pvmx.hpp`
- `collective/sm100_fmha_load_tma_mxfp8_n128.hpp`
- `kernel/sm100_fmha_fwd_kernel_mxfp8_pvmx.hpp`
- `kernel/fmha_tile_scheduler.hpp`

**修正后的核心结论：约 1300 TFLOPS 不能主要归因于 2-CTA。优化后版本即使关闭 `FMHA_2CTA`，仍然启用 softmax 解耦、exp2 分流、SFP/KV 流水重构、静态 SFP 算术融合和条件 O rescale。新版本 1-CTA 也具有高性能，说明这些 CTA 数量无关的关键路径优化才是性能跃升的主体；2-CTA 是建立在高效 1-CTA 核心上的额外执行方案。**

主文件中的 CUDA event 计时框架基本未变，因此两个版本的 event 结果具有直接对比意义。不过，仅凭静态 diff 无法精确分解各优化点贡献，最终仍应通过逐项消融实验确认。

## 2. 优化总览

| 优化点 | 主要作用 | 预期影响 |
|---|---|---|
| softmax 双组 loose split-N | 移除两个 256-thread 全组同步点 | 1/2-CTA 通用，降低关键路径同步 |
| 25% exp2 使用 FMA 多项式 | 将部分 exp 从 MUFU 分流到 FMA/ALU pipe | 1/2-CTA 通用，缓解 MUFU 瓶颈 |
| SFP 搭载到 KV TMA slot | 删除独立 SFP pipeline 的关键路径依赖 | 1/2-CTA 通用，隐藏搬运和等待 |
| KV pipeline 4 stage 增至 6 stage | 补偿 K slot 延迟释放造成的预取深度损失 | 1/2-CTA 通用，保持 load/compute overlap |
| 条件跳过 correction rescale | `scale == 1` 时跳过整块 TMEM 读写 | 1/2-CTA 通用，降低 O 关键路径 |
| softmax 寄存器 208 降至 192 | 降低寄存器压力 | 改善调度/occupancy，收益取决于 ptxas 结果 |
| 2-CTA / 2-SM cooperative UMMA | 两个 CTA 合作计算一个 M=256 tile | 仅 2-CTA 生效的额外结构性优化 |

### 2.1 为什么新版本 1-CTA 仍然很快

`FMHA_2CTA` 关闭后，只会回退到 1-SM schedule、`ClusterShape<1,1,1>` 和 M=128。上述 softmax、exp2、SFP/KV pipeline、rescale 与寄存器优化都不在该宏保护范围内，仍然完整生效。

因此，新版本 1-CTA 并不是“旧版本减去 2-CTA”，而是已经包含绝大多数关键路径优化的新 kernel。

## 3. 最关键优化：2-CTA / 2-SM Cooperative UMMA

### 3.1 从 1-SM MMA 切换到 2-SM MMA

调优前固定使用：

```cpp
KernelTmaWarpSpecialized1SmMxf8f6f4Sm100
```

调优后在 `FMHA_2CTA` 下使用：

```cpp
using ClusterShape = Shape<_2, _1, _1>;
using KernelScheduleFmha =
    cutlass::gemm::KernelTmaWarpSpecialized2SmMxf8f6f4Sm100;
```

这使两个 CTA 组成 cluster，共同执行一个 `cta_group::2` UMMA。主程序中 N128 路径的 cooperative tile 从 M=128 改为 M=256，每个 CTA 仍负责其中 128 行。

### 3.2 2-CTA 数据分工

load collective 增加了 cluster rank、TMA partition 和 multicast mask：

- Q、SFA、SFP 按 M 维拆分，每个 CTA 加载自己的 128 行。
- K、V 按 2-SM MMA 所需方式拆分。
- SFK、SFV 通过 pair-wide TMA multicast，使两个 CTA 都拿到完整 B-side scale tile。
- MMA、softmax、LSE 和 epilogue 均按 CTA rank 映射到正确的 M-half。

因此，2-CTA 并非简单地把 grid 扩大一倍，而是把一个 M=256 tile 的数据搬运、TMEM、UMMA 和输出完整地协作化。

### 3.3 2-CTA 配套修正

优化版本还补齐了 2-SM 路径必须具备的同步和资源管理：

- 使用 `Allocator2Sm` 进行 pair-wide TMEM 分配。
- 仅 pair leader CTA 发射 `tcgen05 mma/cp/commit`。
- TMA transaction bytes 乘以 CTA pair 因子。
- pipeline consumer arrival count 覆盖两个 CTA。
- 增加 cluster-wide pipeline initialization barrier。
- 增加 pair-wide TMEM deallocation handshake。
- epilogue tile 改为 per-CTA M=128，并修正输出 tile 坐标。

这些主要是 2-CTA 路径的正确性保障，但也是 cooperative UMMA 能稳定运行并获得性能的前提。

### 3.4 Scheduler 调整

`IndividualTileScheduler` 现在令：

```text
grid.x = cooperative_M_tile_count * cluster_m
m_tile = blockIdx.x / cluster_m
```

同一 cluster 内两个 CTA 得到相同 cooperative tile 坐标。由于原 persistent scheduler 不理解 CTA pair，2-CTA 路径主动回退到 individual scheduler，避免两个 CTA 解码成不同 tile。

## 4. Softmax 关键路径优化

### 4.1 移除全组同步，允许两个 softmax group 漂移

调优前两个 softmax group 通过以下 256-thread barrier 强同步：

- `B_SREADY`：group 0 等待 QK 后通知 group 1。
- `B_PDONE`：两个 group 写完 P 后统一释放。

调优后两个 group 都直接作为 S pipeline consumer：

- 各自等待 QK commit。
- 各自写完自己的 P half 后释放。
- S pipeline 的 empty barrier 统计两个 group 的 arrival。

这减少了非必要 rendezvous，使两个 group 可以在非 reduction 阶段独立推进，从而降低等待并让 exp2 发射更连续。

### 4.2 reduction barrier 从 256-thread 缩小到 64-thread

max/sum 合并实际上只需要同一 row band 的两个 warp 配对。调优后将单个 256-thread barrier 改为四个 64-thread warp-pair barrier，四个 row band 可以独立推进。

同时 exchange shared memory 增加 tile parity 双缓冲，避免两个漂移 group 在相邻 tile 间覆盖彼此数据。

### 4.3 MUFU 与 FMA pipe 混合执行 exp2

代码注释记录了 profiling 结论：softmax 的 `ex2.approx` 一度约占 kernel wall time 的 51%，B200 上属于 MUFU element-throughput bound。

优化后新增 `fast_exp2f_poly()`，每四对 exp 中选择一对走 degree-4 FMA 多项式，其余仍走 PTX `ex2.approx`：

```cpp
if ((j & 7) == 6) {
  fast_exp2f_poly(...);
} else {
  fast_exp2f(...);
}
```

这样把约 25% exp 工作分流到 FMA/ALU pipe，与 MUFU 并行。注释中还记录了 50%、37.5%、FP16x2 等方案因 issue pressure 或硬件吞吐限制而回退，最终保留 25% 分流，说明这是实测后的平衡点。

## 5. SFP 与内存流水优化

### 5.1 SFP 不再走独立 pipeline

调优前每个 PV tile 使用独立 SFP TMA pipeline。调优后 SFP 搭载到 K/V TMA transaction：

- 每个 K slot 携带真实 SFP。
- V slot 重复携带相同 SFP，用于保持各 stage transaction bytes 一致。
- SFP 使用 3-stage SMEM 环形缓冲，索引为 `k % 3`。
- softmax 和 UTCCP 都通过 KV pipeline 的依赖关系获得 SFP 可见性。

收益在于删除独立 SFP pipeline 的 acquire/wait/release 关键路径，并把少量 SFP 数据搬运隐藏进已有 KV TMA transaction。

### 5.2 KV pipeline 从 4 stage 增至 6 stage

因为 K slot 同时承载 SFP，K 的 release 延迟一个 iteration，确保 softmax 和 UTCCP 已完成读取。为补偿这项延迟对预取距离的损失，MXFP8 KV pipeline 从 4 stage 增至 6 stage。

这是典型的“用更多 SMEM stage 换取更深 load/compute overlap”。2-CTA 后每 CTA 的 K/V tile 变小，也为增加 stage 留出了 shared memory 空间。

### 5.3 静态 SFP 缩放折叠进 exp2

SFP 是 ue8m0，即 2 的整数次幂。优化版本把 `P / sf` 从 exp 后的逐元素除法，折叠进 exp2 输入 bias：

```text
P / sf = exp2(scale * (S - rowmax) - (e - 127))
```

这样避免了逐元素除法；量化前也不再需要额外乘 `inv_scale`。为了保持真正的 softmax normalizer，row sum 按两个 32-column scale group 分别累加，再乘回各自 SFP。

## 6. O Pipeline 优化：跳过恒等 Rescale

在线 softmax 中，只有 row max 更新时才需要重缩放历史 O accumulator。调优前无条件执行整个 128x128 FP32 TMEM round trip。

调优后使用：

```cpp
if (__any_sync(0xffffffffu, scale != 1.0f)) {
  correction_rescale(...);
}
```

当一个 warp 的 32 行都没有更新 row max 时，直接跳过 rescale。该优化数值上精确，并缩短 depth-1 O pipeline 的关键路径；序列较长、row max 逐渐稳定后，命中机会会增加。

## 7. 其他改动的性质

以下改动重要，但主要属于正确性、可验证性或大 shape 支持，不能直接视为 1300 TFLOPS 的性能来源：

- SFP reference 的真实静态量化语义和 row-major repack 修正。
- SFP allocation 使用 `size_t` 分模式计算，避免超大 shape 下 int32 overflow。
- 2-CTA 下 LSE、output row、epilogue 坐标修正。
- SFP pipeline mask 初始化和 cluster barrier，避免 deadlock。
- CMake 增加 `FMHA_2CTA_FLAG`，允许保留 1-CTA baseline。
- target 改名本身不影响性能。

## 8. 对 1300 TFLOPS 来源的修正判断

结合新版本 1-CTA 也具有高性能的实测信息，应把收益拆成两层。

**旧版本到新版本的主体收益，优先考虑 1/2-CTA 通用优化：**

1. softmax loose split-N 与 64-thread pair reduction barrier。
2. 25% FMA polynomial exp2 分流。
3. SFP-on-K-slot 与 6-stage KV pipeline。
4. 静态 SFP 缩放折叠进 exp2 bias。
5. 条件跳过 O rescale。
6. softmax 寄存器从 208 调至 192。

**新版本 1-CTA 到新版本 2-CTA 的额外收益，才来自：**

- 2-SM cooperative UMMA。
- M=256 cooperative tile。
- 2-CTA TMA partition/multicast 与配套调度。

所以必须分开分析“旧 1-CTA → 新 1-CTA”和“新 1-CTA → 新 2-CTA”，不能把前一段提升归到 2-CTA。

## 9. 建议的消融与 Profiling 方法

为了定量确认 1300 TFLOPS 的来源，建议固定相同输入、编译参数、GPU clocks 和 event 迭代次数，依次测试：

1. 旧版本 1-CTA baseline。
2. 新版本完整 1-CTA。
3. 新版本 1-CTA 逐项关闭 loose split-N、polynomial exp2、conditional rescale。
4. 新版本 1-CTA 对比独立 SFP pipeline 与 SFP-on-K-slot。
5. 新版本完整 1-CTA 对比完整 2-CTA，单独测 cooperative UMMA 增益。

建议关注 NCU 指标：

- Tensor Core / UMMA active cycles
- SM active、eligible warps、issue active
- MUFU/XU utilization 与 FMA utilization
- barrier stall、MIO throttle、long scoreboard
- TMA bytes、L2 hit rate、DRAM throughput
- registers/thread、shared memory/CTA、active clusters

同时必须检查：

- 两版本 FLOP 计算公式完全一致。
- `--verify` 均通过且使用相同 SFP/输入分布。
- 2-CTA 与 1-CTA 使用相同 shape、mask、head 数和 batch。
- event 区间仅覆盖 kernel iterations，不包含初始化、reference 和 host/device repack。

## 10. 最终总结

优化版本的设计方向可以概括为：

> 新版本首先通过 softmax 解耦、MUFU/FMA 分流、SFP/KV 流水重构和条件 rescale，大幅优化 1-CTA 与 2-CTA 共用的关键路径；然后可选地使用 Blackwell 2-SM cooperative UMMA。

优化后 1-CTA 性能也很高是合理且重要的证据：**新版高性能的主体来自通用 kernel 内部优化，而不是仅来自 CTA 数量变化。** 2-CTA 应视为建立在高效 1-CTA 核心之上的额外执行方案。约 1300 TFLOPS 的具体组成，需要用“旧 1-CTA、新 1-CTA、新 2-CTA”三组严格同条件数据定量拆分。
