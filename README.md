# FP16 FA vs MXFP8 FA 性能分析

分析基于现有 Nsight Compute report 与对应源码静态检查，未重新采集 profile，也未修改 kernel 源码。

## 1. 对比对象

| 名称 | Report | 主 kernel | 备注 |
| --- | --- | --- | --- |
| FP16 FA | `/home/ubuntu/workspace/oyhj/flash-attention/agent_space/flash_fwd_sm100_b1_h40_q170100_d128.ncu-rep` | `flash_fwd_sm100 FlashAttentionForwardSm100` | Python/CUTE DSL 路径 |
| MXFP8 1314 | `/home/ubuntu/workspace/oyhj/staticQuant_mxfp8/1300/Benchmark/FA/build/1314report.ncu-rep` | `Sm100FmhaFwd...Mxfp8...` | 2CTA/2SM MXFP8 路径，`MXFP8_PSTATIC_EXP=7` |
| MXFP8 scaleno1 | `/home/ubuntu/workspace/oyhj/staticQuant_mxfp8/Benchmark/FA/build2cta/scaleno1report.ncu-rep` | `Sm100FmhaFwd...Mxfp8...` | 2CTA/2SM MXFP8 路径，固定 P scale `2^-9` |

MXFP8 report 的主 kernel 带有 CLC/Work ID 相关 warning，部分 Nsight 指标为 `nan`。下表只使用 raw CSV 中非 `nan` 的同名指标。

## 2. 主指标

| 指标 | FP16 FA | MXFP8 1314 | MXFP8 scaleno1 |
| --- | ---: | ---: | ---: |
| Kernel duration | 498.379 ms | 715.418 ms | 681.267 ms |
| XU pipe | 71.365% | 61.328% | 64.372% |
| ALU pipe | 30.925% | 19.902% | 27.538% |
| Tensor pipe | 86.998% | 30.167% | 31.664% |
| TMEM / mem tensor pipe | 91.293% | 36.540% | 37.746% |
| Issue active | 49.099% | 40.845% | 47.025% |
| Eligible warps / scheduler | 0.587 | 0.613 | 0.720 |
| Active warps / SM | 15.000 | 14.489 | 14.489 |
| Executed instructions | 186.345 B | 235.678 B | 276.444 B |
| Memory throughput | 21.778% | 16.157% | 16.975% |
| DRAM throughput | 2.739% | 0.106% | 0.112% |
| L2 hit rate | 86.933% | 99.503% | 99.483% |
| Registers / thread | 128 | 128 | 128 |
| Shared memory / block | 233.472 KB | 180.656 KB | 180.656 KB |
| Waves / SM | 1.00 | 359.46 | 359.46 |

一句话结论：FP16 这份更接近 Tensor/TMEM 高利用的 FA kernel；MXFP8 两份并没有把 Tensor Core 打满，主要时间更像耗在 softmax、P 生成/量化、scale-factor、UTCCP、同步和控制这些非 Tensor 路径上。scaleno1 虽然 XU 更高，但 duration 更低，说明它提高了可发射工作比例，而不是单纯“XU 更糟”。

## 3. FP16 FA 性能判断

FP16 主 kernel 时间约 `498.38 ms`，Tensor pipe `~87.0%`，TMEM/mem tensor `~91.3%`。Nsight details 也提示 TMEM 是按 elapsed cycles 看最高的 pipe。这个形态说明主循环中的 QK/PV Tensor/TMEM 路径已经非常忙。

但 XU 也有 `~71.4%`，不能忽略。对应源码里，softmax 路径包含：

- `update_row_max`：做 row max，非首 tile 时计算 `acc_scale = exp2((old_max - new_max) * scale_log2)`。
- `scale_subtract_rowmax`：对 score 做 `score * scale_log2 - row_max * scale_log2`，使用 packed FMA。
- `apply_exp2_convert`：对 score 执行 `cute.math.exp2(..., fastmath=True)` 并转换成 P。
- `update_row_sum`：累加 softmax 分母。
- `flash_fwd_sm100.py` 中 softmax 阶段还会把 `acc_scale` 写入共享存储，用于 O correction/rescale，并在 P 写回后释放给 PV。

相关源码锚点：

- `/home/ubuntu/workspace/oyhj/flash-attention/flash_attn/cute/softmax.py:302`
- `/home/ubuntu/workspace/oyhj/flash-attention/flash_attn/cute/softmax.py:330`
- `/home/ubuntu/workspace/oyhj/flash-attention/flash_attn/cute/softmax.py:348`
- `/home/ubuntu/workspace/oyhj/flash-attention/flash_attn/cute/flash_fwd_sm100.py:2300`

所以 FP16 的瓶颈不是内存带宽。DRAM 只有 `~2.74%`，memory throughput `~21.78%`。它更像是 Tensor/TMEM 很忙，同时 softmax 的 exp2/fmax/fadd/scale/correction 给 XU/ALU 造成了持续压力。

## 4. MXFP8 1314 为什么慢

MXFP8 1314 主 kernel 时间约 `715.42 ms`，明显慢于 FP16。但关键点不是 Tensor Core 饱和：Tensor pipe 只有 `~30.2%`，TMEM/mem tensor 只有 `~36.5%`，远低于 FP16。

这说明 MXFP8 版本的主要限制更偏向非 Tensor 路径：

- QK/PV 使用 block-scaled MXFP8，需要 SFQ/SFK/SFV/SFP scale-factor 路径。
- P 不是普通 fp16/fp32 中间值，而是 softmax 后要生成/量化成 E4M3，并配合 P 的 scale factor 进入 PV。
- 源码中有 SFP double buffering、per-tile SFP/SFV UTCCP、softmax_step 内的 `exp2f`、`fmax`、row_sum、row_max、chain/correction 同步。
- 1314 路径编译为 `MXFP8_PSTATIC_EXP=7`，不是 scaleno1 的固定 `2^-9` 路径。

因此 1314 的慢主要不是“MXFP8 Tensor Core 不够快”，而是“为了让 P/V 以 MXFP8 block-scaled 形式跑 PV，softmax/P 生成/scale-factor 侧的标量和控制工作太多，Tensor pipe 等不到足够连续的工作”。

## 5. scaleno1 为什么 XU 更高但更快

scaleno1 主 kernel 时间约 `681.27 ms`，比 1314 快约：

```text
(715.418 - 681.267) / 715.418 = 4.77%
```

它的 XU 从 `61.33%` 升到 `64.37%`，ALU 从 `19.90%` 升到 `27.54%`，issue active 从 `40.85%` 升到 `47.02%`，eligible warps 从 `0.613` 升到 `0.720`。这组变化很关键：XU/ALU 升高并不代表性能变差，而是更多 warp 能发射非 Tensor 指令，等待和空洞减少。

源码 diff 显示 scaleno1 的核心变化是固定 P 的 static scale：

- `MXFP8_PSTATIC_EXP=-9`。
- 固定 scale 为 `2^floor(log2(1/448)) = 2^-9`。
- softmax 写 `E4M3(P * 512)`。
- PV 用 UE8M0 byte `118` 反量化。
- 注释明确说在线 per-32-block amax、e8m0 ceil-log2、per-tile SFP smem store、per-tile SFP UTCCP 被编译掉或移出热路径，SFP TMEM 在 `first_pv` 只填一次常量。

相关源码锚点：

- `/home/ubuntu/workspace/oyhj/staticQuant_mxfp8/Benchmark/FA/b200_blackwell_fmha_mxfp8/collective/sm100_fmha_fwd_mainloop_mxfp8_n128_pvmx.hpp:44`
- `/home/ubuntu/workspace/oyhj/staticQuant_mxfp8/Benchmark/FA/b200_blackwell_fmha_mxfp8/collective/sm100_fmha_fwd_mainloop_mxfp8_n128_pvmx.hpp:2243`
- `/home/ubuntu/workspace/oyhj/staticQuant_mxfp8/Benchmark/FA/b200_blackwell_fmha_mxfp8/collective/sm100_fmha_fwd_mainloop_mxfp8_n128_pvmx.hpp:2444`
- `/home/ubuntu/workspace/oyhj/staticQuant_mxfp8/Benchmark/FA/b200_blackwell_fmha_mxfp8/collective/sm100_fmha_fwd_mainloop_mxfp8_n128_pvmx.hpp:2700`

所以 scaleno1 的优化不是降低 softmax 数学本身，而是降低 P scale-factor 的动态开销和串行依赖。剩下的 softmax/P quantization 仍然密集使用 `exp2f/fmax/fma/row_sum`，所以 XU 很高；但由于动态 amax/ceil-log2/SFP 更新路径更轻，整体排队更少，发射率更高，kernel 时间更短。

## 6. XU、MUFU 与 softmax 瓶颈的关系

可以说 MUFU/特殊函数是 XU 高的重要来源之一，但不能把 XU 全部等同于 MUFU。

原因：

- softmax 的 `exp2f` / `cute.math.exp2(..., fastmath=True)` 通常会走特殊函数/SFU/MUFU 相关执行资源，在 Nsight 的 pipe 视角里会体现为 XU 或特殊函数压力。
- 但是 XU 还会包含很多非 Tensor 的执行、转换、控制、地址和同步相关指令。MXFP8 路径尤其有 P E4M3 convert、scale-factor 常量/动态处理、UTCCP 调度、barrier/chain/correction 控制等。
- 当前 report 没有导出明确的 MUFU 指令计数列，所以不能严谨地说“XU 高全部就是 MUFU 高”。

更准确的表述是：

```text
XU 高说明 softmax/P 生成/scale-factor 这类非 Tensor 路径很重；
其中 exp2f/log2/rcp 等特殊函数很可能贡献了相当一部分 XU 压力；
但 XU 高不是 MUFU 的同义词，也不是单独证明 softmax 是唯一瓶颈。
```

结合源码和指标，softmax 确实是 MXFP8 当前的核心瓶颈之一，尤其是 P 生成、P quantization、row_sum/row_max/correction 这组和 PV 前后强相关的路径。但 1314/scaleno1 的差异还包含 scale-factor 和调度/同步开销，不应只归咎于 exp2。

## 7. 后续建议

如果继续优化 MXFP8，优先看这几类方向：

1. 继续减少 P scale-factor 动态路径：scaleno1 已证明固定 P scale 能带来约 `4.8%` 收益。
2. 降低 softmax/P 生成依赖链：重点看 `exp2f` 后的 convert、row_sum fuse、amax fuse、correction rescale 是否仍有串行等待。
3. 提高 Tensor pipe 连续性：MXFP8 Tensor/TMEM 只有 `~30-38%`，说明当前不是 Tensor Core 算力吃满，优化目标应是让 PV/QK 少等 softmax/P/SF。
4. 若要严格证明 MUFU 占比，需要重新采集包含 SASS source counters / MUFU 指令统计的 Nsight section，而不是只依赖当前 raw report。

