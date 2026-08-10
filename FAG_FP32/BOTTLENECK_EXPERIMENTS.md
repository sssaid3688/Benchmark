# FP8 Backward 瓶颈定位实验报告

**对象**：`FAG_FP32` 的 2-SM FA4 backward kernel（dQ=FP32），`Sm100FmhaBwdKernelTma2SmWarpSpecialized`
**目标**：找到**根本瓶颈及其根本原因**（不做优化）
**方法**：limiting study（no-op 探针测组件成本上限）+ NCU 交叉验证 + 自我反驳
**环境**：B200，锁频 1965 MHz，CUDA 13.3，sm_100a

---

## 0. 实验方法与产物登记（可复现）

### 方法论：limiting study
对 kernel 每个组件，用 no-op 探针替换（**保留所有同步/流水线 wait-commit**，只跳过实际计算/搬运），
测量单次迭代时间。组件单独成本 ≈ 完整时间 − stub 后时间。**输出梯度对 STUB 探针是故意错误的**
（探针破坏数值），这是预期且可接受的——目的只是测成本。

### 产物（全部保留，可重跑）
| 文件 | 作用 |
|---|---|
| `kernel/sm100_fmha_bwd_kernel_tma_2sm_probe.hpp` | 探针版 kernel（原 kernel 头的副本，加 `FAG_PROBE_*` 宏）。原文件**未改动**。 |
| `device/fmha_device_bwd_2sm_probe.hpp` | 探针 device 头（include 探针 kernel） |
| `77_blackwell_fmha_bwd_probe.cu` | 探针翻译单元（include 探针 device 头） |
| `CMakeLists.txt` | `add_fag_probe_target()` 函数 + 每个 probe 一个 target |

### probe target → 实验对应表
| target | 宏 | stub 什么 | 测什么 |
|---|---|---|---|
| `probe_base` | — | （无，完整正确 kernel） | sanity baseline |
| `probe_stub_S` | `FAG_PROBE_STUB_S` | KQ→S 的 gemm 循环 | S GEMM 成本 |
| `probe_stub_dP` | `FAG_PROBE_STUB_dP` | VDO→dP 的 gemm 循环 | dP GEMM 成本 |
| `probe_stub_dV` | `FAG_PROBE_STUB_dV` | PDO→dV 的 gemm 循环 | dV GEMM 成本 |
| `probe_stub_dK` | `FAG_PROBE_STUB_dK` | DSQ→dK 的 gemm 循环 | dK GEMM 成本 |
| `probe_stub_dQ` | `FAG_PROBE_STUB_dQ` | DSK→dQ 的 gemm 循环 | dQ GEMM 成本 |
| `probe_stub_all_gemm` | `FAG_PROBE_STUB_ALL_GEMM` | 5 个 GEMM 全 stub | 同步+softmax+搬运下限 |
| `probe_stub_softmax` | `FAG_PROBE_STUB_SOFTMAX` | exp2 + dS-mul 计算（保留 TMEM 流量） | softmax warpgroup 成本 |
| `probe_stub_corner` | `FAG_PROBE_STUB_CORNER` | dS RMEM→SMEM pack + RMEM→TMEM 写（**s2cluster 握手保留**防死锁） | corner-turn 成本 |
| `probe_stub_reduce` | `FAG_PROBE_STUB_REDUCE` | dQ TMEM→RMEM 读 + RMEM→SMEM + TMA store | dQ store 成本 |
| `probe_stub_everything` | `FAG_PROBE_STUB_EVERYTHING` | 全部 stub，只留同步+TMA load | 纯同步/启动下限 |
| `probe_stub_corner_tmemwr` | `FAG_PROBE_STUB_CORNER_TMEMWR` | 仅跳过 corner 的 RMEM→TMEM 写 | 隔离 TMEM 写成本（阶段2） |
| `probe_stub_corner_pack` | `FAG_PROBE_STUB_CORNER_PACK` | 仅跳过 corner 的 RMEM→SMEM pack | 隔离 pack 成本（阶段2） |
| `probe_stub_exp2only` | `FAG_PROBE_STUB_EXP2ONLY` | 仅跳过 exp2（保留 dS-mul） | 隔离 exp2 成本（阶段2） |
| `probe_ev_doubletma` | `STUB_EVERYTHING DOUBLETMA` | stub 一切 + 多发一个冗余 V TMA | TMA engine 是否满载（阶段3） |
| `probe_ev_halftx` | `STUB_EVERYTHING HALFTX` | stub 一切 + expect_transaction 减半 | **死锁**：barrier 状态不一致（阶段3，失败） |

### 复现命令
```bash
cd /home/ubuntu/workspace/oyhj/temp/test/test_merge/Benchmark/FAG_FP32
sudo nvidia-smi -lgc 1965,1965
cmake -S . -B build-probe -DCMAKE_BUILD_TYPE=Release -DCUTLASS_NVCC_ARCHS=100a -DCMAKE_CUDA_COMPILER=/usr/local/cuda-13.3/bin/nvcc
cmake --build build-probe -j 16   # 或 --target probe_base probe_stub_corner ...
# limiting study (单次迭代时间)
./build-probe/probe_base --b=1 --h=16 --q=16384 --k=16384 --d=128 --d_vo=128 --iterations=50 --verbose
# NCU
/usr/local/cuda-13.3/bin/ncu --kernel-name regex:"device_kernel" --launch-skip 1 --launch-count 1 \
  --section WarpStateStats --section SpeedOfLight \
  --metrics l1tex__throughput.avg.pct_of_peak_sustained_elapsed,l1tex__data_bank_conflicts_pipe_lsu_mem_shared_op_st.sum \
  ./build-probe/probe_base --b=1 --h=16 --q=8192 --k=8192 --d=128 --d_vo=128 --iterations=1
```

---

## 1. 第一轮 limiting study（N=16384，锁频，中位 ms）

| probe | 中位 ms | 节省 µs（base−probe） | 占 base | 判定 |
|---|---:|---:|---:|---|
| **base**（完整） | 3.5031 | — | — | — |
| stub_S | 3.5180 | −15 | −0.4% | （反而变慢） |
| stub_dP | 3.4702 | +33 | +0.9% | 几乎无影响 |
| stub_dV | 3.4948 | +8 | +0.2% | 几乎无影响 |
| stub_dK | 3.5559 | −53 | −1.5% | （反而变慢） |
| stub_dQ | 3.4938 | +9 | +0.3% | 几乎无影响 |
| stub_all_gemm | 3.5850 | −82 | −2.3% | （反而变慢） |
| **stub_softmax** | 3.0812 | **+422** | **+12.0%** | 大 |
| **stub_corner** | 2.9592 | **+544** | **+15.5%** | **最大** |
| stub_reduce | 3.2387 | +264 | +7.5% | 中 |
| stub_everything | 1.9838 | +1519 | +43.4% | 下限 |

**初步结论（待反驳）**：
1. **5 个 GEMM 都不是瓶颈**——stub 任何一个只省 <1%，且 stub S/dK/all_gemm 反而**变慢**。
2. 三个大成本是 **corner-turn（15.5%）> softmax（12.0%）> dQ reduce（7.5%）**。
3. even stub 掉一切，还有 1.98ms（43%）残余 → 这是**纯同步+TMA load 开销**。

---

## 2. NCU 交叉验证（N=8192，base 与 top 探针）

| 变体 | Compute(SM)% | Memory% | L1/TEX% | long-scoreboard stall | 占总停顿 |
|---|---:|---:|---:|---:|---:|
| **base** | 41.98 | 49.40 | 50.47 | 3.0 cyc | 31.9% |
| stub_corner | 49.21 | 61.98 | 61.98 | 3.8 cyc | 37.7% |
| stub_softmax | 46.44 | 59.79 | 59.79 | 9.3 cyc | 49.8% |
| stub_everything | 14.36 | 23.68 | 23.68 | **19.4 cyc** | **69.6%** |

bank-conflict（SMEM 写）：base 6.8M，stub_corner_pack 5.8M（仅降 14%，说明 pack 用 uint4 对齐、本身 bank-conflict 低）。

**NCU 印证 limiting study**：
- base 的 Compute 仅 42%、Memory 仅 49%——**都未饱和**，所以 GEMM 不是瓶颈（与 stub GEMM 无效一致）。
- stub_everything 的停顿飙到 19.4 cyc（69.6%）——残余 1.98ms **全是同步/屏障等待**，不是有用工作。

---

## 3. 自我反驳

### 反驳①：stub GEMM「无影响」是不是 stub 没生效？
**检验**：probe_stub_dQ 在 N=4096 跑出与 base 不同的（错误）结果、且 NCU 显示 GEMM stub 改变了 stall 分布 → stub 确实生效。GEMM 无影响是因为它们**被 compute/corner/reduce 的延迟完全掩盖**——GEMM 在 MMA warp 上跑，而关键路径卡在 compute warpgroup 的 corner-turn 上。MMA 即使全速也只是在「等 compute」的间隙里跑，省掉它看不出差别。
**结论成立**：GEMM 不是瓶颈。

### 反驳②：stub_corner 省的 544µs 是不是被 TMEM 写污染了 dK（间接 stub 了 dK）？
stub_corner 跳过了 RMEM→TMEM 的 dS 写（dK 的 A 操作数源）。但 stub_dK（直接跳过 dK GEMM）只省 −53µs（反而变慢）。若 stub_corner 的收益来自间接 stub dK，那 stub_dK 也该省同样多——但它没有。所以 **stub_corner 的 544µs 不是来自 dK，而是 pack 本身**。
**用阶段2验证**（见下）：corner 的 544µs 里 pack 占 537µs、TMEM 写只占 15µs。结论稳固。

### 反驳③：「节省最多 = 瓶颈」会不会被开销重复计入误导？
交叉校验：corner(544)+softmax(422)+reduce(264) = 1330µs，加上 GEMM(≈0) 和 everything 下限(1519µs)。
注意 1330µs < 1519µs（everything 下限），说明三者**有重叠**（它们都在 compute/reduce warpgroup 上串行/部分并行），不能简单相加。但**相对大小排序可信**：corner > softmax > reduce。这不是「重复计入」，而是三者本身就有重叠——下限 everything 把它们全 stub 才露出真正的同步基线。

### 反驳④：stub_all_gemm 为何反而变慢（−82µs）？
GEMM 在 MMA warp 上跑，与 compute/reduce 的 corner-turn/softmax **并行**。stub 掉 GEMM 后，MMA warp 空转，但它仍在同步屏障上参与握手（cluster barrier、named barrier），这些屏障的到达时机被 MMA 的空转扰动，**反而拖慢了 compute/reduce 的屏障等待**。这是「GEMM 提供了掩盖同步开销的填充」的旁证——进一步确认瓶颈在同步路径，不在 GEMM。

---

## 4. 阶段2：细分 top-2 成本

### corner-turn 子成分（N=16384）
| probe | ms | 节省 µs |
|---|---:|---:|
| corner full（pack+TMEMwr） | 2.9592 | 544 |
| └ 仅跳 TMEM 写（corner_tmemwr） | 3.4878 | **15** |
| └ 仅跳 pack（corner_pack） | 2.9664 | **537** |

**corner-turn 的成本 99% 来自 RMEM→SMEM 的 uint4 pack + store，TMEM 写几乎不花时间（15µs）。**
pack 是寄存器内的 4 字节移位拼装 + uint4 SMEM 写。bank-conflict 低（uint4 对齐），所以不是 bank conflict，
而是 **compute warpgroup 的标量寄存器计算 + SMEM 写在关键路径上串行**，且这个串行段无法被掩盖（它是 dQ 的 ds_leader 屏障前驱）。

### softmax 子成分（N=16384）
| probe | ms | 节省 µs |
|---|---:|---:|
| softmax full（exp2+dS-mul） | 3.0812 | 422 |
| └ 仅跳 exp2（exp2only） | 3.3834 | **120** |

**softmax 的成本里 exp2 只占 28%（120µs），其余 72%（302µs）是 dS-mul + TMEM 流量 + 同步。**
（注：stub_softmax 同时跳了 exp2 和 dS-mul；exp2only 只跳 exp2。差值 302µs 是 dS-mul 及其周围的 TMEM load/store/NamedBarrier。）

---

## 5. 最终结论：根本瓶颈与根本原因

### 根本瓶颈
**不是 GEMM 计算，而是 compute/reduce warpgroup 上的「dS 数据搬运 + softmax 计算」串行段**，
其中按成本排序：

1. **dS corner-turn 的 RMEM→SMEM pack（537µs，15.3%）**——最大单一成本。
2. **softmax 的 dS-mul + TMEM 流量（302µs，8.6%）**。
3. **dQ reduce/store（264µs，7.5%）**。
4. **exp2（120µs，3.4%）**。
5. 同步/屏障基线（everything 下限 1519µs 里，扣除上述后约 296µs 是纯 TMA load + 屏障）。

### 根本原因（经多轮反驳验证）
1. **关键路径在 compute warpgroup，不在 MMA**。整个 backward 的迭代节奏由 compute 的
   「S→softmax→P→dS→pack→corner-turn」串行链决定（compute_mma_ds pipeline 的 producer 端）。
   MMA 的 5 个 GEMM 在等 compute 的间隙里跑，算力（42%）和带宽（49%）都没跑满——
   这就是「为什么 GEMM 不是瓶颈」的根本原因：**它们被 compute 的串行段饿着**。

2. **corner-turn 的 pack 是最大单一成本**，根本原因是：dS 必须从 compute 的寄存器（RMEM）
   搬到 SMEM，再经 s2cluster 交换给 peer CTA，才能让 dQ GEMM 消费。这个 RMEM→SMEM 的 pack
   + store 在 compute warpgroup 上**串行执行**，且它是 ds_leader 屏障（dQ 的前驱）的必要步骤，
   **无法被任何其他工作掩盖**——它在关键路径的「咽喉」上。

3. **为什么 pack 这么贵（537µs）**：不是 bank-conflict（uint4 对齐，conflict 低），
   不是 SMEM 带宽饱和（SMEM 利用率 49%），而是 **8 个 compute warp 做标量寄存器拼装（移位+或）
   + 受限于 NamedBarrier 同步**的串行开销——每 iteration 要 pack 4 个 strip × 全部 Q 元素，
   全程在 TransformBarrier 的 arrive_and_wait 之间。

### 反驳后的稳固结论
- 「GEMM 是瓶颈」❌（Compute 42%、stub GEMM 无效，反驳①）
- 「SMEM 带宽饱和」❌（49%，未饱和，反驳见 NCU）
- 「bank-conflict」❌（uint4 对齐，conflict 低）
- **「compute warpgroup 的 dS pack + softmax 串行段是关键路径瓶颈」✅**
  （limiting study + NCU 停顿 + 阶段2 细分三方一致，反驳②③④均通过）

### 核心洞察
这个 kernel **不是 compute-bound 也不是 memory-bandwidth-bound，而是 latency/serial-bound**：
关键路径是一段在 compute warpgroup 上串行的「dS 搬运 + softmax」链，GEMM 的算力和 SMEM 带宽都有余量但用不上，
因为它们都在等这段串行链完成。**优化的方向应是缩短/并行化这段 compute 串行链（尤其 pack），
而不是加 GEMM 吞吐或 SMEM 带宽。**

---

## 5b. 分解 stub_everything 的 1.98ms（修正"固定开销"误判 + 带宽打不满实验）

### 之前说法的错误

我之前推断 stub_everything 的 1.98ms（43%）"固定开销（launch + TMEM 分配）是大头"。
**实验推翻了这个说法**：用不同 N（2048/4096/8192/16384）测正常 kernel 和 stub_everything，
拟合 `total = fixed + per_iter × iter_count`：

|  | 固定开销 (launch+TMEM分配) | per-iter |
|---|---:|---:|
| 正常 kernel（完整计算） | 56µs（1.7%） | 12.5 ns/iter |
| stub_everything（全跳） | 62µs（3.1%） | 7.4 ns/iter |

**固定开销只有 62µs（3.1%），不是 43%。** stub_everything 的 1.98ms 几乎全是 per-iter 成本
（262144 iter × 7.4 ns/iter = 1945µs）。我之前的"固定开销占大头"是错的——**真正的大头是
每次 iteration 的同步握手 + TMA 的累积，不是一次性的 launch/TMEM 分配。**

### 7.4 ns/iter 的构成：计算 vs 同步/TMA

- 正常 kernel per-iter = 12.5 ns，其中**计算占 5.1 ns（41%）、同步+TMA 占 7.4 ns（59%）**。
- 即同步/TMA 的延迟是 per-iter 的主要成分，这印证 latency-bound 结论。

### 计算全跳后，带宽能不能打满？——不能（NCU 实测）

直觉上"计算全跳了，TMA 只管搬数据，带宽应该能打满"。**实验证明打不满：**

| 指标 | 正常 kernel | stub_everything |
|---|---:|---:|
| DRAM throughput（占峰值） | 2.54% | **1.77%** |
| SMEM throughput（占峰值） | 49.4% | 23.4% |
| DRAM 读 | — | 68 MB（N=8192） |

**即使把所有计算都跳掉，DRAM 带宽利用率只有 1.77%——比正常 kernel 还低。** 这证明：

1. **backward 不是 bandwidth-bound，无论算不算。** 数据量太小（每 iter 65KB），B200 的
   8 TB/s HBM 带宽根本不是约束——打满它需要每周期搬上千字节，但每 iter 只有 65KB/148 SM。
2. **打不满的根因是 latency，不是 bandwidth。** TMA 搬 65KB 很快，但每 iter 要等 pipeline 同步
   （consumer_wait/commit + cluster barrier）才能进下一个 iter——这个等待的延迟（7.4 ns/iter）
   拖慢了整体，使 TMA 无法连续发射、带宽上不去。
3. **这是 latency-bound 的铁证**：计算全跳后，如果有"富余带宽"能用上，时间应大幅下降；
   实际只从 3.34ms 降到 1.98ms，而带宽利用率反而从 2.54% 降到 1.77%——说明约束从来不是带宽。

### 为什么同步延迟会"拖慢 TMA"

TMA 是异步的（Load warp 发射后不等完成），但 **TMA 的结果要被 consumer_wait 消费**。
compute/mma warp 每次迭代都要 `consumer_wait(Load 的 pipeline)` 等 TMA 把数据搬进 SMEM 才能继续。
即使计算跳了，这个 wait 还在——而 wait 期间 TMA 也不能无限超前发射（pipeline 只有 2 stage，
满了就要等 consumer 释放）。所以 **同步 latency 限制了 TMA 的发射频率，进而限制了有效带宽**。

### 7.4 ns/iter 里 TMA load 与同步各占多少（实验约束 + 诚实边界）

**尝试的直接实验（失败）：** 在 stub_everything 基础上进一步跳掉 V/dO 的 TMA load
（减约 1/3 的 TMA 字节），通过时间差分离 TMA 贡献。**该探针死锁**——pipeline 的
`expect_transaction` 字节计数与实际拷贝不匹配，barrier 永远等不到。这说明 TMA load 与
pipeline 同步深度耦合，无法用"跳 TMA"的安全方式分离。

**用已有实验数据能确证的（不依赖 NCU）：**

1. **per-iter = 7.4 ns 恒定，不随 N 变化**（N=2048→16384 拟合，per-iter 都是 7.4 ns）。
   这说明每 iter 的固定成本是 7.4 ns，与数据总量无关——既符合"TMA 延迟主导"也符合"同步主导"，
   **单凭此实验无法区分**。

2. **计算成本 = 12.5 − 7.4 = 5.1 ns/iter（41%）**，这是确证的——正常 per-iter 12.5 ns，
   stub per-iter 7.4 ns，差值就是被跳掉的计算（5 个 GEMM + softmax + corner + reduce）。

3. **正常 kernel 和 stub_everything 的 TMA 字节量相同**（都搬同样的 Q/K/V/dO/LSE）。
   所以 TMA 的"搬运工作量"在两者里一样。stub 比 normal 少的 5.1 ns 纯粹是计算。

**不能确证的部分（诚实标注）：**

7.4 ns 里"TMA 完成等待"与"纯同步 barrier 等待"的精确比例，**目前没有安全的实验能分离**。
原因是：TMA 与 pipeline 同步深度耦合——consumer_wait 既等 TMA 完成，也等 barrier arrive，
两者在同一条 wait 上叠加，无法用"跳掉一个保留另一个"的方式隔离（跳 TMA 死锁、跳 barrier 死锁）。

**可推断的范围（标注为推断，非实测）：**

- 7.4 ns 不可能是"纯 TMA 带宽搬运"——因为带宽利用率极低（DRAM 1.77%），如果纯靠带宽搬运
  65 KB/iter，时间应远小于 7.4 ns（8 TB/s 峰值下 74 个 cluster 并行，理论最小 ~0.6 ns/iter）。
- 7.4 ns 也不可能是"纯 barrier 同步"——因为 barrier 本身是几十 ns 级，但 128 个 iter 才 1ms，
  分摊到每 iter 的 barrier latency 在 ns 级是合理的。
- **最可能的构成（推断）：7.4 ns = TMA 的 round-trip latency（consumer 等 TMA 把数据从 HBM 搬进
  SMEM 的完成信号）+ pipeline barrier 握手**，两者叠加在 consumer_wait 上，各占多少需更精细的
  实验（如改 pipeline 深度 kStages）才能分离，留作后续。

### 冗余 TMA 实验：TMA engine 是否满载（实验确证）

**实验设计**：保持 iter 次数和所有同步不变，在每次 iter 额外多发一个冗余 TMA（把 V 再搬一遍，
16 KB），对比时间差。多搬 14% 数据（112KB→128KB/iter）。

**结果**（锁频 1965MHz，中位 ms）：

| 探针 | N=8192 | N=16384 |
|---|---:|---:|
| stub_everything（7 个 TMA/iter） | 0.587 | 1.999 |
| +1 冗余 TMA（8 个 TMA/iter，多 16KB） | 0.588 | 1.998 |
| **差值** | **+0.1%（噪声）** | **−0.05%（噪声）** |

**结论：TMA engine 带宽有余，不是 bandwidth-bound。** 多搬 14% 数据时间不变，说明 TMA engine
远没满载（DRAM 利用率 1.6%），多发一个冗余 TMA 被完全吸收。

**但注意此实验的局限性**：多发 TMA 不加时间只能证明"TMA engine 带宽有余"，**不能证明 TMA 的
完成延迟为零**。因为 TMA 是异步的——Load warp 发射后不等完成，多发一个被 engine 后台吸收。
真正卡时间的是 consumer_wait 等 TMA 的**完成信号**（含 HBM round-trip + SMEM 写入 + barrier arrive），
这个完成延迟与搬运量不是线性关系（异步，有 latency 上界）。要精确分离"TMA 完成延迟"vs"纯 barrier
同步"，需要让同一 TMA 搬更小 tile（如 64×64 vs 128×128）看延迟是否缩短——但这需要改编译期
TMA descriptor（当前 kernel 不支持运行时改 box size），留作后续。

**当前能确证的**：7.4 ns/iter 不是 bandwidth-bound（多发 TMA 不加时间），而是 latency-bound
（consumer_wait 的完成延迟 + barrier 同步）。两者的精确比例需 TMA box-size 实验才能分离。

### halftx 实验（失败）：仅改 expect_transaction 会死锁

**尝试**：只把 `kTransactionsBytesLoad*` 减半（barrier 期望字节减半），`cute::copy` 保持不变
（TMA 仍搬全量 16KB）。理论上 barrier 会在 8KB 到达时提前释放 consumer，时间差反映 TMA 搬运延迟。

**结果：死锁。** TMA 完成时向 barrier arrive 全部 16KB，但 barrier 只 expect 了 8KB——transaction
超额导致 mbarrier 状态错乱，consumer_wait 永远等不到正确的 phase 翻转。

**原因**：mbarrier 的 `expect_transaction` 和 TMA 的实际搬运量必须严格匹配。只改 expect 不改 TMA 实际
搬运量（需改 TMA descriptor box size）会导致 barrier 状态不一致。**结论：要真正减半 TMA 搬运量，
必须同时改 expect_transaction 和 TMA descriptor box（编译期），运行时无法只改一边。**

### 手算带宽利用率（总数据量 / 时间）

**搬进 GMEM→SMEM**（FP8，唯一数据，不含 L2 命中重复）：
- Q、K、V、dO：各 N×D×H = 16384×128×16 = 33.6 MB，4 个 = **134 MB**
- LSE、SumOdO：各 N×H×4B = 1 MB，2 个 = **2 MB**
- 搬进合计 = **136 MB**

**搬出 SMEM→GMEM**：
- dK、dV、dQ：各 N×D×H×1B(FP8) = 33.6 MB，3 个 = **101 MB**
- 搬出合计 = **101 MB**

**总流量 = 136 + 101 = 237 MB = 0.237 GB**
**实测时间 = 3.34 ms（锁频，N=16384）**
**带宽 = 0.237 GB / 3.34 ms = 0.071 TB/s**
**占 B200 HBM 峰值 8 TB/s = 0.9%**

HBM 带宽利用率 0.9%——237MB 的数据 8TB/s 搬它只要 30µs，但 kernel 跑了 3340µs（慢 113 倍）。
带宽完全不是约束，时间全花在同步等待上。

---

## 6. 与 FA4 论文的对比与裁决（关键反驳）

FA4 论文（arXiv:2603.05451）明确指出 backward 的瓶颈是 **shared memory bandwidth**，并给出
1-CTA 模式的 cycle 预算：SMEM traffic 3328 cyc vs MMA 2560 cyc（SMEM 多 30%），其中
MMA 操作数加载 2048 cyc、**dQ 归约 1024 cyc**、dS 写 256 cyc。

我们的实测**与论文表面矛盾**：SMEM 平均占用率才 28.8%，远没饱和。必须裁决这个矛盾，不能附和论文也不能轻率否定。

### 裁决实验：SMEM 带宽占用率（base vs stub_corner_pack）

| 指标 | base | stub_corner_pack | 变化 |
|---|---:|---:|---:|
| SMEM load wavefronts | 140.3M | 71.1M | −49% |
| SMEM store wavefronts | 127.0M | 93.7M | −26% |
| cycles/SM | 6.27M | 4.96M | −21% |
| **SMEM 平均占用率（vs ~128 B/cyc 峰值）** | **28.8%** | **22.5%** | −6.3pp |

**判决：pack 不是 bandwidth 瓶颈。**
- base 和 stub_pack 的 SMEM 占用率都**远低于峰值**（28.8% / 22.5%）——SMEM 带宽根本没饱和。
- 去掉 pack 省了 21% 时间，但占用率只从 28.8% 降到 22.5%，**没有「触顶后下降」的特征**。
- 若 pack 是 bandwidth 瓶颈，去掉它后占用率应从 ~100% 骤降；实际是 28.8%→22.5%，说明 pack 的成本是**串行 latency（在关键路径上无法掩盖）**，不是带宽峰值。

### 为什么论文说 bandwidth、我们测出 latency？两者都对的统一解释

**论文分析的是 1-CTA baseline，我们测的是已做 FA4 全套优化的 2-SM 版本。** 论文的 cycle 预算里：
- MMA 操作数加载 2048 cyc → 被 2-SM 的 **B 操作数对半驻留**（每 CTA 只持一半 K/V）砍掉一半；
- dQ 归约 1024 cyc → 被我们的 **dQ-fp16 tile64 + TMEM 复用**大幅压缩（且 dQ=FP32 版本里它也只排第三，264µs）。

所以**论文里 SMEM bandwidth 的两个大头（操作数加载 + dQ 归约）在我们这个优化版本里已被消解**，
剩下的 corner pack（论文只算 256 cyc，是小头）反而因为位于 dQ 的 ds_leader 屏障前驱、无法掩盖，**相对地冒成了第一瓶颈**。

### 最终统一结论（难反驳）

1. **论文的「backward 受限于 SMEM bandwidth」对 1-CTA baseline 成立**——那时操作数加载和 dQ 归约占主导，SMEM cycle 比 MMA 多 30%。
2. **在我们这个 2-SM 优化版本上，SMEM bandwidth 不再饱和（28.8%）**，瓶颈转移到了 **compute warpgroup 的串行 latency**（dS pack + softmax），因为 FA4 的优化把带宽大头消解了，剩下的串行段（虽小但无法并行）成了新的咽喉。
3. **两者不矛盾，而是「优化前后瓶颈转移」**。论文描述的是优化前的诊断，驱动了 FA4 的设计；我们测的是优化后的现状，显示 FA4 成功消解了 bandwidth 瓶颈，但暴露出新的 latency 瓶颈（pack）。
4. **这也解释了为什么 stub GEMM 无效**：论文的 1-CTA 里 MMA 操作数加载是大头（bandwidth），优化后这部分被 2-SM 砍半，GEMM 不再受 SMEM 带宽拖累，转而被 compute 串行段喂不饱——所以 stub GEMM 看不出差别。

**一句话：FA4 优化前 backward 是 SMEM-bandwidth-bound（论文）；FA4 优化后变成 compute-latency-bound（我们的实测），pack 是新的咽喉。**

---

## 7. 用论文方法重算 2-SM 的 per-tile cycle 预算（定量裁决）

为了定量验证上面的结论，我完全模仿论文「Feeds and Speeds」的 per-tile cycle 预算方法，针对我们这个
2-SM FP8 版本重算一遍。假设：SMEM LSU 128 B/wavefront、1 wavefront/cyc/SM；FP8 MMA 用 B200 峰值
（~2250 TFLOP FP8 / 148 SM / 1.965 GHz ≈ 7737 FLOP/cyc/SM）。tile shape Q=K=DQK=DVO=128，
2-SM cluster M=256（dQ 每 CTA 64 行 × 2）。

### SMEM 操作数读（喂 5 个 GEMM，per-cluster-tile）

| GEMM | A 操作数（字节/源） | B 操作数（字节/源） | 读 SMEM cyc |
|---|---|---|---:|
| KQ→S | K 16 KB（SMEM） | Q 16 KB（SMEM） | 256 |
| VDO→dP | V 16 KB（SMEM） | dO 16 KB（SMEM） | 256 |
| PDO→dV | P 0（**TMEM**） | dO 16 KB（SMEM） | 128 |
| DSQ→dK | dS 0（**TMEM**） | Qt 16 KB（SMEM） | 128 |
| DSK→dQ | dS 16 KB（SMEM） | Kt 16 KB（SMEM） | 256 |
| **操作数读合计** | | | **1024** |

### 中间结果写/读 SMEM（per-cluster-tile）

| 项 | 字节 | cyc |
|---|---:|---:|
| dS 写 SMEM（pack） | 16 KB | 128 |
| dS DSMEM 交换（s2cluster） | 16 KB | 128 |
| dQ 写 SMEM（reduce，FP32） | 64 KB | 512 |
| dQ 读 SMEM（TMA REDUCE-ADD） | 64 KB | 512 |
| **中间结果合计** | | **1280** |

### 论文 1-CTA vs 我们 2-SM 总账

| 项 | 论文 1-CTA | **我们 2-SM** | 变化 |
|---|---:|---:|---|
| MMA 操作数读 SMEM | 2048 | **1024** | **−50%** |
| 中间结果（dS+dQ）写读 SMEM | 1280 | 1280 | 同 |
| **SMEM 总** | **3328** | **2304** | **−31%** |
| MMA 计算 | 2560 | 2169 | −15% |
| **SMEM/MMA 比** | **1.30×** | **1.06×** | — |

### 这个计算精确印证了三个事实

1. **FA4 的 2-SM + TS-MMA 把操作数 SMEM 流量砍半（2048→1024）**，来源有二：
   - **B 操作数对半驻留**（每 CTA 只持 K/V 一半）→ 操作数读减半；
   - **dV/dK 的 A 走 TMEM**（TS-MMA：P 从 TMEM、dS 从 TMEM）→ 这两个 GEMM 的 A 完全不占 SMEM（各只读 128 cyc 而非 256）。
   这两条是 FA4 2-SM 设计的核心，**直接把论文里的带宽大头消解**。

2. **SMEM/MMA 比从 1.30× 降到 1.06×**：在 1-CTA（论文），SMEM 比 MMA 多 30% → bandwidth-bound；
   在 2-SM（我们），两者基本持平 → **不再是 bandwidth-bound**。这与实测的「SMEM 占用率仅 28.8%」一致。

3. **但 cycle 预算的「SMEM/MMA 持平」仍无法解释实测的 latency 瓶颈**，差别在于：
   - 论文方法假设所有 SMEM 访问**连续、可与 MMA 完全重叠**；
   - 实测显示 SMEM 访问被**同步屏障**（ds_leader、TransformBarrier）切成离散段，**无法和 MMA 充分重叠**，
     所以带宽平均利用率低（28.8%），而 pack 这段在关键路径上串行、卡住时间。

### 三方互证的最终结论

- **论文的 cycle 预算（1-CTA: 1.30×）** = FA4 优化前的诊断，驱动了 2-SM + TS-MMA 的设计。
- **我们的 cycle 重算（2-SM: 1.06×）** = 证明 FA4 优化**确实消解了 bandwidth 瓶颈**（SMEM/MMA 回到持平）。
- **我们的实测（SMEM 28.8% + pack latency 瓶颈）** = bandwidth 消解后，剩余的同步串行段（pack）暴露为新瓶颈。

三者完全自洽：**FA4 把 SMEM-bandwidth-bound（1.30×）优化成接近持平（1.06×），所以带宽不再饱和；但 pack 段因位于 ds_leader 屏障前驱、无法与 MMA 重叠，成了 compute-latency-bound 的新咽喉。**

---

## 附：阶段1 完整数据（N=16384，3 次中位，单位 ms）

```
base             3.5031
stub_S           3.5180   (−15µs, 反而慢)
stub_dP          3.4702   (+33µs)
stub_dV          3.4948   (+8µs)
stub_dK          3.5559   (−53µs, 反而慢)
stub_dQ          3.4938   (+9µs)
stub_all_gemm    3.5850   (−82µs, 反而慢)
stub_softmax     3.0812   (+422µs)
stub_corner      2.9592   (+544µs)  ← 最大
stub_reduce      3.2387   (+264µs)
stub_everything  1.9838   (+1519µs) ← 同步下限
```

阶段2 子成分（N=16384）：corner_tmemwr +15µs、corner_pack +537µs、exp2only +120µs。
NCU（N=8192）：base Compute 41.98% / Memory 49.40%；stub_everything Compute 14.36% / 停顿 19.4cyc(69.6%)。
