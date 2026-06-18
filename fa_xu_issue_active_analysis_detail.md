# FA FP16 / MXFP8 XU 与 Issue Active 性能现象分析

## 1. 分析对象

本文分析以下 Nsight Compute report 中 FA forward 主 kernel 的 XU pipe、issue active、eligible warp 和等待来源：

| 版本 | Report | 对应代码 |
|---|---|---|
| FP16 FA | `flash-attention/agent_space/flash_fwd_sm100_b1_h40_q170100_d128.ncu-rep` | `flash-attention/flash_attn/cute/flash_fwd_sm100.py` |
| MXFP8 staticQuantmxfp | `staticQuant/Benchmark/FA/build/staticQuantmxfp.ncu-rep` | `staticQuant/Benchmark/FA/b200_blackwell_fmha_mxfp8/b200_blackwell_fmha.cu` |
| MXFP8 staticReport | `staticQuant/Benchmark/FA/build/staticReport.ncu-rep` | `staticQuant/Benchmark/FA/b200_blackwell_fmha_mxfp8/b200_blackwell_fmha.cu` |
| MXFP8 1182 | `staticQuant/Benchmark/FA/build/1182report.ncu-rep` | `staticQuant/Benchmark/FA/b200_blackwell_fmha_mxfp8/b200_blackwell_fmha.cu` |
| MXFP8 stage6 | `6stage/Benchmark/FA/build/stage6report.ncu-rep` | `6stage/Benchmark/FA/b200_blackwell_fmha_mxfp8/b200_blackwell_fmha.cu` |
| MXFP8 1314 | `staticQuant_mxfp8/1300/Benchmark/FA/build/1314report.ncu-rep` | `staticQuant_mxfp8/1300/Benchmark/FA/b200_blackwell_fmha_mxfp8/b200_blackwell_fmha.cu` |
| MXFP8 scaleno1 | `staticQuant_mxfp8/Benchmark/FA/build2cta/scaleno1report.ncu-rep` | `staticQuant_mxfp8/Benchmark/FA/b200_blackwell_fmha_mxfp8/b200_blackwell_fmha.cu` |

除初始化/reference kernel 外，本文只讨论 FA forward 主 kernel。

## 2. 核心结论

这些版本中的低 XU 利用率，不能简单理解为 softmax 不需要 XU，也不能理解为 XU/MUFU 压力小。更合理的解释是：

**低 XU 的直接原因是 issue active 和 eligible warp 偏低，scheduler 经常没有足够 ready warp 可以发射 XU-heavy 指令。更底层的原因是 SFP/shared-memory/TMEM 数据依赖、barrier、scoreboard、spill 等等待打断了 softmax/P 生成/PV 流水。**

反过来，XU 更高也不一定代表性能更差。在这些 MXFP8 版本中，scaleno1 的 XU 更高但 kernel 更快，说明优化减少了等待，让 softmax/P 生成路径更连续地 issue。

## 3. 主指标对比

| 版本 | Duration | XU avg | XU max | Issue active avg | Issue active max | Eligible warp avg | Eligible warp max |
|---|---:|---:|---:|---:|---:|---:|---:|
| FP16 | 498.38 ms | 70.88% | 70.88% | 49.10% | 56.01% | 0.587 | 0.726 |
| staticQuantmxfp | 2198.8 ms | 39.86% | 39.95% | 38.43% | 38.51% | 0.524 | 0.525 |
| staticReport | 941.68 ms | 47.16% | 47.79% | 41.03% | 41.58% | 0.579 | 0.588 |
| 1182 | 795.69 ms | 55.15% | 56.04% | 40.14% | 40.81% | 0.603 | 0.616 |
| stage6 | 788.66 ms | 55.71% | 56.46% | 39.86% | 40.30% | 0.566 | 0.578 |
| 1314 | 715.42 ms | 61.26% | 62.21% | 40.85% | 43.00% | 0.613 | 0.659 |
| scaleno1 | 681.27 ms | 64.38% | 65.55% | 47.02% | 49.76% | 0.720 | 0.773 |

从表中可以看到：

- staticQuantmxfp 的 XU 只有约 40%，同时 issue active 和 eligible warp 也最低。
- staticReport 的 XU 约 47.8%，比 staticQuantmxfp 好，但仍明显低于 1182/stage6，说明 SFP/shared 等待仍然重。
- 1182/stage6 的 XU 回到约 56%，但 issue active 仍只有约 40%，说明流水仍被等待限制。
- scaleno1 的 issue active 和 eligible warp 明显提高，因此 XU 利用率也提高，kernel 时间反而下降。
- FP16 的 Tensor/TMEM 很高，同时 softmax 非 Tensor 指令也连续执行，所以 XU 最高。

## 4. 等待来源对比

| 版本 | Shared bank conflicts | Local spilling requests | Long scoreboard | Short scoreboard | Barrier | MIO throttle | Wait |
|---|---:|---:|---:|---:|---:|---:|---:|
| staticQuantmxfp | 108.62B | 283.02M | 4.85 | 0.89 | 0.79 | 0.37 | 1.15 |
| staticReport | 33.82B | 265.43M | 4.12 | 0.44 | 0.49 | 0.91 | 1.27 |
| 1182 | 2.32B | 106.21M | 3.79 | 0.42 | 0.64 | 0.88 | 1.49 |
| stage6 | 7.10B | 601.13M | 3.76 | 0.47 | 0.42 | 0.98 | 1.92 |
| 1314 | 178.80M | 124.01M | 3.58 | 0.29 | 0.34 | 0.98 | 1.56 |
| scaleno1 | 920.53M | 124.01M | 3.49 | 0.19 | 0.003 | 0.61 | 1.36 |

这些 stall 指标说明，低 issue active 的直接来源主要是：

1. **Long scoreboard 高**：warp 在等待长延迟数据依赖，通常与 global/L1TEX/TMEM/shared 访问和前序数据未 ready 有关。
2. **Shared bank conflict 高**：shared 访问被拆成更多 wavefront，softmax/SFP/P buffer 相关访问被拉长。
3. **Barrier 高**：rowmax/row_sum 跨 warp-group 或跨 half 的同步打断流水。
4. **Local spilling 高**：寄存器压力导致 local memory load/store，进一步制造 scoreboard 等待。
5. **MIO throttle / wait 高**：shared/TMEM/UTCCP 等数据通路或同步队列压力增加，发射端被迫等待。

## 5. staticQuantmxfp 为什么 XU 很低

staticQuantmxfp 的 XU max 只有约 39.95%，但这不是因为 softmax 中没有 XU-heavy 指令。它仍然需要 softmax 的 `fmax`、`exp2`、P 量化、row_sum、correction rescale 等操作。

真正的问题是这些指令发射不连续。staticQuantmxfp 的 shared bank conflicts 达到 108.62B，远高于 1182 的 2.32B 和 stage6 的 7.10B；eligible warp avg 只有 0.524，issue active avg 只有 38.43%。这说明 scheduler 大量周期找不到 ready warp。

因此 staticQuantmxfp 的低 XU 应解释为：

**SFP/shared-memory 路径把 softmax/PV 流水卡住，warp 经常等待 SFP 或 shared/TMEM 相关数据，导致 `exp2/fmax/convert` 等 XU 指令不能持续发射。**

## 6. staticReport: XU 介于 staticQuantmxfp 和 1182 之间

`staticReport.ncu-rep` 的主 kernel 时间约 941.68 ms，XU max 约 47.79%，issue active avg 约 41.03%，eligible warp avg 约 0.579。它比 staticQuantmxfp 的 2198.8 ms / 39.95% XU 明显好，但仍慢于 1182 的 795.69 ms / 56.04% XU。

这个版本的关键特征是：

- shared bank conflicts 仍有 33.82B，远高于 1182 的 2.32B；
- local spilling requests 为 265.43M，也高于 1182 的 106.21M；
- long scoreboard 为 4.12，高于 1182 的 3.79；
- XU 只有 47.79%，说明 softmax/P 生成的 `exp2/fmax/convert` 仍然没有被连续喂饱。

所以 staticReport 应该被看作一个中间状态：

**它已经比 staticQuantmxfp 减少了部分等待，因此 issue active 和 XU 回升；但 SFP/shared-memory 路径仍然很重，shared conflict 和 spill 仍然把大量 warp 卡在等待状态，导致 XU 不能达到 1182/stage6 的 56% 水平。**

从代码路径看，staticReport 仍落在 `staticQuant/Benchmark/FA/b200_blackwell_fmha_mxfp8` 这一套早期 mainloop 上，主要风险点仍是：

- SFP smem allocation 和 SFP TMEM copy；
- PV 前 SFP staging；
- softmax 在线 P scale/SFP 生成；
- `fence_view_async_tmem_store` 后接 PV；
- P 量化和 SFP 写入 shared。

这解释了为什么 staticReport 的 issue active 虽略高于 staticQuantmxfp，但 duration 仍接近 1 秒，且 XU 只有约 48%。

## 7. 1182 为什么 XU 约 56%

1182 主 kernel 的 XU max 为 56.04%，比 staticQuantmxfp 高很多，但 issue active 仍只有约 40%。这说明它比 staticQuantmxfp 更能推进 softmax/P 生成，但仍没有充分跑满。

1182 的代码中存在 per-tile rowmax/row_sum 相关 shared exchange 和 NamedBarrier，例如：

- rowmax 跨 half / warp group 的 `B_REDUCE`
- P 写完后的 `B_PDONE`
- final row_sum merge
- P 量化和 SFP/TMEM 相关依赖

因此 1182 的 XU 较 staticQuantmxfp 更高，是因为等待有所减少，softmax 的 XU 指令可以更连续地发射；但 barrier、scoreboard、shared conflict 和 spill 仍然明显，所以 issue active 仍低。

## 8. stage6 为什么并没有明显更低的 XU

stage6 的 XU max 是 56.46%，和 1182 的 56.04% 基本相同，甚至略高。因此如果使用 `sm__inst_executed_pipe_xu.max.pct_of_peak_sustained_elapsed` 作为 XU 峰值，不能说 stage6 比 1182 更低。

但 stage6 的 issue active 和 eligible warp 更低：

- issue active avg: 39.86%
- eligible warp avg: 0.566
- local spilling: 601.13M
- shared bank conflicts: 7.10B

stage6 代码中 SFP 路径更重。load 侧把 SFP 加进 K/V transaction：

```cpp
TransactionBytesLoadK = K_data + SFK + SFP;
TransactionBytesLoadV = V_data + SFK + SFP;
```

K/V TMA 路径中也会 copy `tma_load_sfp`。在 PV 前，还会执行：

```cpp
load_sfp((k - 1) % 3, pv_buf);
do_pv(...);
```

这意味着每个 PV tile 前都要把 P-SFP 从 smem staging 到 TMEM。softmax 里还直接从 `smem_sfp` 读 SFP byte，并将 SFP exponent 融入 softmax bias：

```cpp
uint8_t sfp_e0 = sfp_smem[sfp_off];
uint8_t sfp_e1 = sfp_smem[sfp_off + 1];
bias_g0 = -row_max_scale - float(int(sfp_e0) - 127);
```

所以 stage6 的问题更像：

**SFP 搬运和 SFP 依赖贴近 softmax/PV 临界路径，造成 shared/TMEM/UTCCP 和 spill 压力，使 warp ready 度下降。**

## 9. 1314 为什么 XU 升高

1314 的 XU max 提高到 62.21%，duration 也从 1182 的 795.69 ms 降到 715.42 ms。它的 shared bank conflicts 从 2.32B 降到 178.80M，说明 shared 路径明显改善。

1314 所在代码路径进入了更完整的 2CTA/2SM op6 静态路径，包含更多 overlap 和调度优化。它减少了部分 shared/barrier 压力，让 softmax/P 生成、scale factor、PV 相关工作更连续地推进。

因此 1314 的 XU 变高不是坏事，而是说明：

**原先等待中的周期被更多 softmax/P 生成相关指令填上了，XU-heavy 指令流更连续。**

## 10. scaleno1 为什么 XU 更高但性能更好

scaleno1 的 XU max 达到 65.55%，高于 1314，但 duration 进一步下降到 681.27 ms。它的 issue active avg 从 1314 的 40.85% 提高到 47.02%，eligible warp avg 从 0.613 提高到 0.720。

这说明 scaleno1 的优化有效减少了等待，使 scheduler 更容易找到 ready warp。源码层面的关键方向包括：

- 固定 P scale，减少动态 per-block amax、ceil-log2、动态 SFP 写入等开销。
- 减少 full-width scale-factor rendezvous 和串行 scale 更新。
- E2RSF / lazy chain / combo 调度将 row_sum、exp2、P conversion 更紧地组织到可发射窗口中。
- 减少 barrier 压力，scaleno1 的 barrier stall 约 0.003，远低于 1314 的 0.34 和 1182 的 0.64。

所以 scaleno1 的现象应解释为：

**XU 更高是因为等待减少后，softmax/P 生成路径被更充分地执行；它不是 XU 瓶颈恶化，而是指令发射效率提高。**

## 11. FP16 为什么 XU 最高

FP16 的 Tensor/TMEM 利用率很高：

- Tensor pipe: 约 87.0%
- TMEM/mem tensor: 约 91.3%
- XU max: 约 70.88%

FP16 softmax 路径包含 `row_max` reduce、scale/subtract rowmax、`exp2`、row_sum、acc scale 和 O correction rescale。CUTE softmax 中的 `cute.math.exp2(..., fastmath=True)`、`fmax/fadd/fma/rcp/log2` 等都会贡献非 Tensor 指令压力。

FP16 没有 MXFP8 的 P scale/SFP/P quantization 复杂路径，因此它的 softmax 非 Tensor 指令可以和 Tensor/TMEM 主流水更稳定地交错执行。XU 高表示 softmax 相关的非 Tensor 计算也被持续推进。

## 12. MUFU 与 XU 的关系

可以这样表述：

`exp2f` / `ex2.approx.ftz.f32` 这类特殊函数通常会映射到 SFU/MUFU 相关执行资源，在 Nsight Compute 的 pipe 归类中常表现为 XU 或特殊函数相关压力。因此 softmax 中大量 `exp2` 是 XU 利用率高的重要来源之一。

但不能把全部 XU 都等同于 MUFU。XU 还可能包括非 Tensor 的执行、转换、控制、部分特殊指令等。当前 report 没有明确导出完整 MUFU 指令计数，所以只能说 `exp2` 是 XU 高的重要贡献者，不能说 XU 全部来自 MUFU。

## 13. 代码级原因映射

这一节把上面的性能现象映射到具体代码路径。

### 13.1 FP16: XU 高来自连续 softmax 数学路径

FP16 版本的 softmax 主流程在 `flash_fwd_sm100.py` 的 `softmax_step` 中：

```python
row_max, acc_scale = softmax.update_row_max(...)
softmax.scale_subtract_rowmax(...)
softmax.apply_exp2_convert(...)
softmax.update_row_sum(...)
```

对应代码位置：

- `flash-attention/flash_attn/cute/flash_fwd_sm100.py:2302`
- `flash-attention/flash_attn/cute/flash_fwd_sm100.py:2317`
- `flash-attention/flash_attn/cute/flash_fwd_sm100.py:2328`
- `flash-attention/flash_attn/cute/flash_fwd_sm100.py:2356`

`SoftmaxSm100` 中直接使用 `cute.math.exp2(..., fastmath=True)`：

- `flash-attention/flash_attn/cute/softmax.py:312`
- `flash-attention/flash_attn/cute/softmax.py:370`
- `flash-attention/flash_attn/cute/softmax.py:379`

这条路径主要是 rowmax、scale、exp2、row_sum、correction rescale，没有 MXFP8 的 SFP 搬运和 P scale 更新，所以 XU 指令更连续。FP16 的 XU 高不是因为等待多，而是 softmax 数学操作本身持续执行。

### 13.2 staticQuantmxfp / staticReport: SFP 搬运贴近 PV 临界路径

staticQuantmxfp 和 staticReport 都来自 `staticQuant/Benchmark/FA/b200_blackwell_fmha_mxfp8` 这一套早期 mainloop。它们都有明显的 per-tile SFP staging。代码中为 SFP 配置 smem 和 TMEM copy：

- `staticQuant/Benchmark/FA/b200_blackwell_fmha_mxfp8/collective/sm100_fmha_fwd_mainloop_tma_warpspecialized.hpp:114`
- `staticQuant/Benchmark/FA/b200_blackwell_fmha_mxfp8/collective/sm100_fmha_fwd_mainloop_tma_warpspecialized.hpp:152`
- `staticQuant/Benchmark/FA/b200_blackwell_fmha_mxfp8/collective/sm100_fmha_fwd_mainloop_tma_warpspecialized.hpp:444`
- `staticQuant/Benchmark/FA/b200_blackwell_fmha_mxfp8/collective/sm100_fmha_fwd_mainloop_tma_warpspecialized.hpp:448`

PV 前会把 SFP 从 smem copy 到 TMEM，然后立刻做 PV：

```cpp
copy(tiled_copy_s2t_SFP0, ...);
fence_view_async_tmem_store();
gemm_zero_acc(mma_pv, ..., tCtSFP0, tCtSFV0);
```

对应位置：

- `staticQuant/.../sm100_fmha_fwd_mainloop_tma_warpspecialized.hpp:679`
- `staticQuant/.../sm100_fmha_fwd_mainloop_tma_warpspecialized.hpp:682`
- `staticQuant/.../sm100_fmha_fwd_mainloop_tma_warpspecialized.hpp:688`

循环中也反复出现：

- `staticQuant/.../sm100_fmha_fwd_mainloop_tma_warpspecialized.hpp:738`
- `staticQuant/.../sm100_fmha_fwd_mainloop_tma_warpspecialized.hpp:739`
- `staticQuant/.../sm100_fmha_fwd_mainloop_tma_warpspecialized.hpp:744`
- `staticQuant/.../sm100_fmha_fwd_mainloop_tma_warpspecialized.hpp:747`
- `staticQuant/.../sm100_fmha_fwd_mainloop_tma_warpspecialized.hpp:802`
- `staticQuant/.../sm100_fmha_fwd_mainloop_tma_warpspecialized.hpp:807`

这解释了 staticQuantmxfp 和 staticReport 的高 shared conflict 和低 issue active：SFP 不是一次性常量，而是作为 PV block-scaled MMA 的 scale operand 被频繁搬运，并贴近 PV 的关键路径。warp 很容易在 SFP smem/TMEM copy、fence、PV consumer dependency 上等待。

### 13.3 staticQuantmxfp / staticReport: 动态 P scale 生成增加 softmax 侧 shared 写入

staticQuantmxfp/staticReport 的 softmax 中不仅计算 P，还在线生成 P 的 scale factor。代码里先读取 score，做 rowmax：

- `staticQuant/.../sm100_fmha_fwd_mainloop_tma_warpspecialized.hpp:948`
- `staticQuant/.../sm100_fmha_fwd_mainloop_tma_warpspecialized.hpp:964`
- `staticQuant/.../sm100_fmha_fwd_mainloop_tma_warpspecialized.hpp:969`

然后计算 P 和 per-32 group max：

- `staticQuant/.../sm100_fmha_fwd_mainloop_tma_warpspecialized.hpp:1028`
- `staticQuant/.../sm100_fmha_fwd_mainloop_tma_warpspecialized.hpp:1039`
- `staticQuant/.../sm100_fmha_fwd_mainloop_tma_warpspecialized.hpp:1046`

再把 SFP 写回 shared：

- `staticQuant/.../sm100_fmha_fwd_mainloop_tma_warpspecialized.hpp:1007`
- `staticQuant/.../sm100_fmha_fwd_mainloop_tma_warpspecialized.hpp:1095`
- `staticQuant/.../sm100_fmha_fwd_mainloop_tma_warpspecialized.hpp:1100`

这条路径会产生大量 shared 写入和后续读取。它解释了为什么 staticQuantmxfp 和 staticReport 的 shared bank conflicts 都远高于 1182：softmax 不只是 `exp2`，还承担了在线 P scale 生成、SFP 写入、P 量化和同步。

### 13.4 1182: 固定 SFP 后减少动态 scale，但仍有 softmax/P 生成等待

1182 的 mainloop 使用固定 SFP 方案，避免了 staticQuantmxfp 那种在线 per-block P scale 生成。对应代码中有固定 scale：

- `staticQuant/Benchmark/FA/b200_blackwell_fmha_mxfp8/collective/sm100_fmha_fwd_mainloop_mxfp8_n128_pvmx.hpp:50`
- `staticQuant/.../sm100_fmha_fwd_mainloop_mxfp8_n128_pvmx.hpp:560`
- `staticQuant/.../sm100_fmha_fwd_mainloop_mxfp8_n128_pvmx.hpp:594`
- `staticQuant/.../sm100_fmha_fwd_mainloop_mxfp8_n128_pvmx.hpp:691`

因此 1182 的 XU 从 staticQuantmxfp 的约 40% / staticReport 的约 48% 回到约 56%，shared conflict 也大幅下降。

但是 1182 仍然有 per-tile softmax barrier 和 P done barrier：

- rowmax reduce / `B_REDUCE`: `staticQuant/.../sm100_fmha_fwd_mainloop_mxfp8_n128_pvmx.hpp:975`
- P 写完后的 `B_PDONE`: `staticQuant/.../sm100_fmha_fwd_mainloop_mxfp8_n128_pvmx.hpp:1105`
- final row_sum merge: `staticQuant/.../sm100_fmha_fwd_mainloop_mxfp8_n128_pvmx.hpp:1154`

softmax 中仍有 `fmax`、`fast_exp2f`、P convert、row_sum：

- `staticQuant/.../sm100_fmha_fwd_mainloop_mxfp8_n128_pvmx.hpp:944`
- `staticQuant/.../sm100_fmha_fwd_mainloop_mxfp8_n128_pvmx.hpp:1062`
- `staticQuant/.../sm100_fmha_fwd_mainloop_mxfp8_n128_pvmx.hpp:1082`
- `staticQuant/.../sm100_fmha_fwd_mainloop_mxfp8_n128_pvmx.hpp:1114`

所以 1182 的低 issue active 主要来自：固定 SFP 降低了动态 scale 开销，但 softmax/P 生成仍被 barrier、TMEM load/store、P conversion 和 row_sum reduce 打断。

### 13.5 stage6: 每个 K/V tile 带 SFP，并在 PV 前 load_sfp

stage6 代码更直接地解释了 SFP 等待问题。它把 SFP 加进 K/V transaction：

```cpp
TransactionBytesLoadK = K_data + SFK + SFP;
TransactionBytesLoadV = V_data + SFK + SFP;
```

对应位置：

- `6stage/Benchmark/FA/b200_blackwell_fmha_mxfp8/collective/sm100_fmha_fwd_mainloop_mxfp8_n128_pvmx.hpp:302`
- `6stage/.../sm100_fmha_fwd_mainloop_mxfp8_n128_pvmx.hpp:304`
- `6stage/.../sm100_fmha_fwd_mainloop_mxfp8_n128_pvmx.hpp:305`

load 侧每个 K/V tile 都 copy `tma_load_sfp`：

- `6stage/Benchmark/FA/b200_blackwell_fmha_mxfp8/collective/sm100_fmha_load_tma_mxfp8_n128.hpp:376`
- `6stage/.../sm100_fmha_load_tma_mxfp8_n128.hpp:397`
- `6stage/.../sm100_fmha_load_tma_mxfp8_n128.hpp:412`
- `6stage/.../sm100_fmha_load_tma_mxfp8_n128.hpp:426`

PV 前又从 smem 把 SFP copy 到 TMEM：

- `6stage/.../sm100_fmha_fwd_mainloop_mxfp8_n128_pvmx.hpp:508`
- `6stage/.../sm100_fmha_fwd_mainloop_mxfp8_n128_pvmx.hpp:652`
- `6stage/.../sm100_fmha_fwd_mainloop_mxfp8_n128_pvmx.hpp:687`

softmax 也直接从 `smem_sfp` 读 SFP exponent，并放进 softmax bias：

- `6stage/.../sm100_fmha_fwd_mainloop_mxfp8_n128_pvmx.hpp:915`
- `6stage/.../sm100_fmha_fwd_mainloop_mxfp8_n128_pvmx.hpp:919`
- `6stage/.../sm100_fmha_fwd_mainloop_mxfp8_n128_pvmx.hpp:925`

因此 stage6 的 issue active 低，可以具体归因到：SFP TMA 增大 K/V load 事务，SFP smem->TMEM UTCCP 贴近 PV 临界路径，softmax 对 SFP smem load 有直接依赖。warp 等 SFP ready 时，XU 的 `fast_exp2f` 和 convert 就发不连续。

### 13.6 stage6: split-N barrier 让 softmax 两组 warp 锁步

stage6 的 softmax 是 split-N cooperative softmax。代码注释说明 stage 0/1 各处理 64 列，然后通过 shared exchange 和 NamedBarrier 合并：

- `6stage/.../sm100_fmha_fwd_mainloop_mxfp8_n128_pvmx.hpp:724`
- `6stage/.../sm100_fmha_fwd_mainloop_mxfp8_n128_pvmx.hpp:738`
- `6stage/.../sm100_fmha_fwd_mainloop_mxfp8_n128_pvmx.hpp:880`
- `6stage/.../sm100_fmha_fwd_mainloop_mxfp8_n128_pvmx.hpp:881`
- `6stage/.../sm100_fmha_fwd_mainloop_mxfp8_n128_pvmx.hpp:1029`
- `6stage/.../sm100_fmha_fwd_mainloop_mxfp8_n128_pvmx.hpp:1083`
- `6stage/.../sm100_fmha_fwd_mainloop_mxfp8_n128_pvmx.hpp:1084`

这些 barrier 会把两个 softmax group 锁步。如果其中一组因为 SFP/shared/TMEM 慢，另一组也要等。profile 中 stage6 的 eligible warp 比 1182 更低，和这个结构一致。

### 13.7 1314 / scaleno1: 用 2CTA/2SM、lazy chain、E2RSF 和 PSTATIC 减少等待

后续 `staticQuant_mxfp8` 版本的代码不再只是简单搬 SFP，而是围绕减少等待做了多处重排。

关键优化点包括：

- 2CTA/2SM load pipeline 去掉一些不必要 fence，避免 cluster mbarrier 等待：
  - `staticQuant_mxfp8/Benchmark/FA/b200_blackwell_fmha_mxfp8/collective/sm100_fmha_load_tma_mxfp8_n128.hpp:540`
  - `staticQuant_mxfp8/.../sm100_fmha_load_tma_mxfp8_n128.hpp:565`
  - `staticQuant_mxfp8/.../sm100_fmha_load_tma_mxfp8_n128.hpp:590`
  - `staticQuant_mxfp8/.../sm100_fmha_load_tma_mxfp8_n128.hpp:620`

- lazy chain 用共享的 rowmax 链，减少 full-width 每步同步压力：
  - `staticQuant_mxfp8/.../sm100_fmha_fwd_mainloop_mxfp8_n128_pvmx.hpp:2970`
  - `staticQuant_mxfp8/.../sm100_fmha_fwd_mainloop_mxfp8_n128_pvmx.hpp:2977`
  - `staticQuant_mxfp8/.../sm100_fmha_fwd_mainloop_mxfp8_n128_pvmx.hpp:2984`
  - `staticQuant_mxfp8/.../sm100_fmha_fwd_mainloop_mxfp8_n128_pvmx.hpp:2986`

- E2RSF 把 row_sum 工作融合进 exp2/P conversion 的节奏里：
  - `staticQuant_mxfp8/.../sm100_fmha_fwd_mainloop_mxfp8_n128_pvmx.hpp:3031`
  - `staticQuant_mxfp8/.../sm100_fmha_fwd_mainloop_mxfp8_n128_pvmx.hpp:3042`
  - `staticQuant_mxfp8/.../sm100_fmha_fwd_mainloop_mxfp8_n128_pvmx.hpp:3046`
  - `staticQuant_mxfp8/.../sm100_fmha_fwd_mainloop_mxfp8_n128_pvmx.hpp:3058`

- PSTATIC 固定 P scale，避免动态 SFP 生成/写入：
  - `staticQuant_mxfp8/.../sm100_fmha_fwd_mainloop_mxfp8_n128_pvmx.hpp:3075`
  - `staticQuant_mxfp8/.../sm100_fmha_fwd_mainloop_mxfp8_n128_pvmx.hpp:3078`
  - `staticQuant_mxfp8/.../sm100_fmha_fwd_mainloop_mxfp8_n128_pvmx.hpp:3296`

- O wait 和 correction chain 被移出热点路径或延后：
  - `staticQuant_mxfp8/.../sm100_fmha_fwd_mainloop_mxfp8_n128_pvmx.hpp:3168`
  - `staticQuant_mxfp8/.../sm100_fmha_fwd_mainloop_mxfp8_n128_pvmx.hpp:3176`
  - `staticQuant_mxfp8/.../sm100_fmha_fwd_mainloop_mxfp8_n128_pvmx.hpp:4003`

这些代码改动对应 profile 中的现象是：barrier stall 明显下降，eligible warp 增加，issue active 增加，XU 利用率升高但 kernel 时间下降。

### 13.8 汇总: 哪些代码最容易导致低 issue active

按影响类型归纳：

| 代码模式 | 典型位置 | 性能后果 |
|---|---|---|
| 每 tile 搬 SFP / SFP 进入 K/V transaction | stage6 `TransactionBytesLoadSFP`、`tma_load_sfp` | load pipeline 更重，long scoreboard 增加 |
| PV 前 smem->TMEM `load_sfp` | stage6 `load_sfp(...); do_pv(...)`，staticQuantmxfp SFP copy 后 PV | PV 临界路径等待 SFP，eligible warp 降低 |
| softmax 在线生成 P scale/SFP | staticQuantmxfp `row_max_32`、`sSFP_tile(...) = ...` | shared 写入多，bank conflict 高 |
| split-N rowmax/row_sum shared exchange | stage6 / 1182 `smem_sm_exch` + `B_REDUCE` | barrier stall，warp group 锁步 |
| P 写完等待 `B_PDONE` | stage6 / 1182 `B_PDONE` | P producer/consumer 同步打断流水 |
| 大量 P conversion + exp2 | `NumericArrayConverter` + `fast_exp2f` | XU/ALU 工作多；如果数据 ready 则 XU 高，如果等待多则 XU 低 |
| register pressure / spill | profile 中 local spilling 高，stage6 601M | local memory 访问导致 scoreboard 等待 |

## 14. 最终判断

这些版本的性能现象可以总结为：

1. **低 XU 往往不是好现象**：staticQuantmxfp 的 XU 很低，是因为 warp 大量等待 shared/SFP/TMEM/scoreboard，softmax 指令发不出来。
2. **低 issue active 的直接原因是 eligible warp 不足**：scheduler 找不到足够 ready warp，所以 XU、Tensor、ALU 都难以吃满。
3. **SFP 路径是 MXFP8 早期版本的重要限制**：动态 SFP load/store、smem staging、UTCCP、P scale 依赖会拉长 softmax/PV 临界路径。
4. **shared bank conflict 和 spill 会进一步放大等待**：staticQuantmxfp 的 108.62B shared conflicts 和 stage6 的 601M spill 都会显著降低 ready warp。
5. **后续版本 XU 更高但更快，说明优化方向是正确的**：1314/scaleno1 通过减少等待、固定/简化 scale、改善 overlap，让 softmax/P 生成更连续发射。
6. **判断瓶颈时不能只看 XU 高低**：需要同时看 issue active、eligible warp、scoreboard、barrier、shared conflict、spill 和 kernel duration。

一句话结论：

**在这组 FA MXFP8 kernel 中，低 XU 的根因更多是 warp 等待和流水断裂；优化后 XU 升高，反而说明 softmax/P 生成路径从等待中释放出来，指令发射更充分。**
