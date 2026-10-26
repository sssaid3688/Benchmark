# dQ Drain 瓶颈分析（FP32 精度硬约束版）

**约束声明**：dQ 数据通路的精度语义**不可修改**——TMEM 累加、SMEM 暂存、gmem workspace
累加、Convert 全链保持 FP32，partial 求和必须精确。本文在此约束下重做完整分析；
改变精度的方案（如 BF16 staging，实测 +3.8% 但将 partial 舍入到 8 位尾数）**一律排除**，
仅留档于附录以说明其被否决的理由。

- 基线（约束下的最优）：`fag_cand_colloop` = BWD_2SM + EX2_STRIDE=4 + REGALLOC +
  FENCE_CTA + FAG_DS_RUN_STORES + FAG_DS_COLUMN_SPLIT
- 成绩（锁频 1965 MHz，20 次，b=1 h=16 d=128，`--mask=no`）：
  - 3-kernel：**1377 / 1790 / 1964 TFLOPS**（4096/8192/16384）
  - 主 kernel Med：**1562 / 1930 / 2067 TFLOPS**（220.0 / 712.3 / 2660.1 µs）
  - vs FP16 FA4 主 kernel：1.26× / 1.37× / 1.46×；vs 重构前 base +13.5~16.6%
  - 精度：与未重构 fence_cta 版在 8192 **逐位相同**（哈希 `0x69fb2194…` 已验证），
    全部容差门（verify/1SM/causal/完整性）通过。

---

## 1. 瓶颈定位（V3 消融矩阵，三 shape）

`fag3_reduce`（删 drain 数据路径、保留全部握手）：主 kernel Δt = **−9.32 / −9.16 / −9.03%**
（4096/8192/16384，nsys Med）——重构后最大可行动单项，三 shape 稳定。

## 2. 三方分解：−9% 到底在哪一段

| 实验（均为 timing-only，握手全保留） | 保留 | 删除 | Δt@16384 | 结论 |
|---|---|---|---:|---|
| `fag3_reduce` | 握手 | LDTM+STS+TMA | −9.03% | 总量 |
| `fag3_reduce_sts` | 握手+LDTM+fence+release | STS+TMA | −9~−11% | **LDTM/release ≈ 0** |
| `fag3_reduce_tma` | 握手+LDTM+STS | 仅 TMA | −0.46% | **TMA ≈ 0.5%** |

⇒ **−9% ≈ 全部来自 SMEM staging 的 STS.128 指令**（每迭代每线程 2 段 × 4×STS.128，
128 线程共 1024 条/迭代），其余环节（TMEM 读、栅栏结构、TMA 原子加）近乎免费。

## 3. 指令级证据（NCU，主内核 @16384）

- Warp stall 采样（171,167 样本）：`MUFU.EX2` 9.5%、`FENCE.VIEW.ASYNC` 5.9%、
  `BAR.SYNC` 5.5%、`LDTM` 仅 0.6%、`UTMAREDG`（reduce TMA）仅 0.03%
- 最热单条 `UCGABAR_WAIT`（9.4%）经 SASS 定位在 **内核 EXIT 尾部**（cluster_arrive/wait
  +EXIT）——先完成 warp 等待落后者的采样伪影，非瓶颈
- SMEM 计数器差分（base vs reduce_sts）：
  - ST wavefront **119M → 44M**（staging 贡献 75M，其中含冲突放大 ~12%）
  - ST bank conflict 18.3M → 10.4M；LD conflict 12.6M → ≈0（TMA 读，延迟不敏感）
  - **L1TEX 管线占用 65.1% → 41.4%**：staging 把共享内存管线推向高位，拖慢所有 warp
    的 SMEM 操作（含 compute warps 的 dS STS 与 LSE LDS——差分采样中 compute 侧
    STTM 停顿 −33%、BSSY −69% 佐证跨 warp 干扰）
- 定量核算同时**否定**纯带宽/发射槽解释（字节与指令量相对 SMEM 容量都很小）：
  成本机制 = 突发窗口内的 L1TEX 管线占用叠加

## 4. 假设-反驳链（全部为精度中立实验）

| # | 假设 | 反驳实验 | 结果 | 裁决 |
|---|---|---|---|---|
| H1 | 栅栏串行（8×命名栅栏/迭代） | `fag3_dq_batched`（8→2 栅栏，4 TMA 背靠背） | **−7.1% 更慢**：逐段 TMA(i)/staging(i+1) 流水被整迭代排空气泡取代（与 roofline 文档 L338 旧基线数据一致：dq64/128/batched 均显著变慢） | ✗ |
| H2 | 2-stage 轮换深度不足 | `fag3_dq_deep4`（4-stage 串行，f32） | −0.5~+0.3% 中性：TMA 完成等待只是移位 | ✗ |
| H3 | LDTM 延迟 + release 链 | `reduce_sts ≈ reduce`（§2） | LDTM/release ≈ 0 | ✗ |
| H4 | TMA reduce-add 原子流量 | `reduce_tma`（§2） | −0.46%（1GB 级流量被异步引擎完全遮蔽） | ✗ |
| H5 | dQ TMEM 可双缓冲以解除相邻 dQ MMA 串行 | TMEM 分配算术（kernel L104-116） | 512 列 = DK 128 + DV 128 + DP/DS 128 + S/P 128，dQ(64 列) 已与 S 区上半 **triple-alias**；无任何可让出的列 | ✗ 结构性不可能 |
| H6 | 字节带宽饱和 | NCU 计数（§3） | 带宽远未饱和；但 L1TEX 管线 65% 占用实证 | ✗（机制改判为管线占用） |
| H7 | UCGABAR_WAIT 9.4% 是隐藏同步瓶颈 | SASS 地址定位 | 位于 EXIT 尾部，采样伪影 | ✗ |
| — | **staging STS 本身（L1TEX 管线占用）** | §2 三方分解 + §3 差分 | 8.5% 确证 | ✓ |

## 5. FP32 约束下的候选空间穷尽论证

要降低 8.5%，只有三类途径，逐一类穷尽：

**A. 减少暂存字节数** —— ✗ 被 FP32 精确累加约束封死
- TMA `cp.reduce.async.bulk` 的浮点类型仅 f32/f64/f16x2/bf16x2；保持精确累加只有 **f32**
- f16/bf16 将 partial 舍入到 ≤11 位尾数（本仓库已实测其效应：最终 FP8 输出 4.4% 元素
  翻转一个刻度、全体平均偏移 9.5e-5——虽过容差门，但**违反本约束**，否决）
- 无压缩表示（Kahan 对拆 = 同字节数）

**B. 减少指令数 / 换搬运通路** —— ✗ 无通路
- `STS.128` 已是最宽 SMEM 写；`stmatrix` 仅支持 16 位类型（fp32 不适用）
- TMEM→gmem/TMA 无直通（`tcgen05.cp` 仅 SMEM→TMEM 方向）
- 跨 warp 转运（借空闲的 relay/empty warp）需 SMEM 中转：STS→LDS→STS 净增指令
- reduce warp 数不可增：TMEM 子分区固定 4 个，4 reduce warp 已一一对应
  （warp14/15 映射到子分区 2/3 也无法新增第 5 个子分区）

**C. 改结构（消除/摊薄 drain）** —— ✗ 全部实验否定或容量封死
- 栅栏结构（H1）、stage 深度（H2）、TMA 时序（H4）：实验否定
- TMEM 双缓冲（H5）：容量 100% 占满，结构性不可能；缩小 tile（如 Q 128→64）可腾出
  TMEM 但直接减半每迭代 MMA 规模，属另一个（更差的）设计点
- 合并 2 个 KV-block-pair 到一个 cluster 以减半 drain 次数：需 2× dK/dV TMEM 累加器，
  容量同样不可行
- 唯一未穷尽的微杠杆：**staging STS 的 bank-conflict 修复**（实测 ~12% wavefront 放大，
  7.9M 冲突）。理论上当前 SW128 swizzle 布局对 32-lane 行分散写应已无冲突，实测放大
  可能来自 wavefront 计口径/部分相位；修复上限 ≈ 8.5%×12% ≈ **1%**，且需与 TMA
  descriptor 的 swizzle 模式联动，风险收益比差，未采纳（留作后续可选实验）。

## 6. 结论（约束下的最终定版）

1. **保持 FP32 精度的最优版本不变：`fag_cand_colloop`（= FAG_0816 默认 `fag_bwd_fp8_2sm`）**
   —— 3-kernel 1377/1790/1964，主 kernel 1562/1930/2067 TFLOPS；dQ 通路与未重构版本
   **逐位一致**，无任何精度语义变化。
2. dQ drain 的 −9% 在 FP32 约束下为**结构性税**：来源是 KV-split 工作划分（每
   (KV块, Q块) 对必须产生一次 FP32 partial 并 REDUCE_ADD 进 workspace），成本集中在
   staging STS 对 L1TEX 管线的占用；FP16 FA4 同构路径同亏 −9.45%（`FAG_ABLATE_DQ_REDUCE`
   钩子实测），两侧共有。
3. 剩余不可消除项量化：drain 8.5%（本约束下 0 可回收，微杠杆 ≤1% 未证）+ MUFU.EX2
   ~9.5% 样本（s4 配比经 {2,4,8,全硬件} 重扫确认最优，全硬件 −5~9%）+ softmax −2.6%
   （数学必需）+ floor −34~38%（TMA 装载 + 屏障骨架，两侧内核一致的结构常数）。
4. shape 专属分支：三 shape 在全部实验中行为一致（各项 Δ 差异 ≤1%），**无需分 shape
   配置**。
5. 若未来放宽精度约束：BF16 staging 实测 +3.6~4.3%（3-kernel 1427/1863/2048，主 kernel
   1633/2019/2161），精度门全过（见附录 B）——作为政策选项留档，默认不启用。

---

## 附录 A：实验靶全表（3-kernel TFLOPS @16384，base=1964）

| 靶 | 改动 | TFLOPS | Δ | 精度中立 |
|---|---|---:|---:|---|
| `fag_cand_colloop` | 最优基线 | 1964 | — | — |
| `fag3_dq_batched` | 8→2 栅栏，批量 TMA | 1825 | −7.1% | ✓ |
| `fag3_dq64` | 64 列 store 粒度 | 1335 | −32% | ✓ |
| `fag3_dq128` | 128 列 store 粒度 | 1546 | −21% | ✓ |
| `fag3_dq_deep4` | 4-stage 串行 | 1970 | +0.3%（中性） | ✓ |
| `fag3_reduce_tma` | 只删 TMA（timing-only） | 2012 | +2.4%* | ✓ |
| `fag3_reduce_sts` | 删 STS+TMA（timing-only） | 2179 | +11%* | ✓ |
| `fag3_ex2_s2` / `fag3_ex2_s8` | EX2 分流配比 | 1920 / 1887 | −2.2% / −3.9% | ✓ |
| `fag3_opt_noex2` | 全硬件 exp2 | 1825 | −7.1% | ✓ |

*timing-only 消融（正确性故意破坏），Δ 用于成本归因，不可作为优化采纳。

## 附录 B：被否决的 BF16 方案留档（违反精度约束）

`FAG_DQ_BF16_STAGING`：partial 以 BF16 暂存/累加（SASS 纯度 100%：恰 `ST.E.128 32→16`
+ 64 条 `F2FP.BF16.PACK_AB`）。实测：3-kernel 1427/1863/2048（+3.6/4.1/4.3%），主
kernel 1633/2019/2161；精度：vs 参考 max 0.082/mean 0.0119（与 f32 版噪声级相同）、
1SM/causal PASSED；最终 FP8 dQ 4.4% 元素翻转一个刻度、平均偏移 9.5e-5、dK/dV 逐位
不变。**因改变 partial 累加精度语义，按本约束否决**；靶位与代码保留（`fag3_dq_bf16*`）。

## 附录 C：复现

```bash
nvidia-smi -lgc 1965,1965
cd temp/test/test_merge/Benchmark/FAG_FP32
cmake --build build --target fag_cand_colloop fag3_reduce fag3_reduce_sts \
  fag3_reduce_tma fag3_dq_batched fag3_dq64 fag3_dq128 fag3_dq_deep4 \
  fag3_ex2_s2 fag3_ex2_s8 fag3_opt_noex2 -j
# 主 kernel Med
nsys profile -o /tmp/r ./build/fag3_dq_deep4 --b=1 --h=16 --q=16384 --k=16384 \
  --d=128 --d_vo=128 --iterations=20 --mask=no
nsys stats -r cuda_gpu_kern_sum --format csv /tmp/r.nsys-rep
# 指令级采样（lineinfo 构建）
cmake -S . -B build-li -DFAG_ENABLE_LINEINFO=ON && cmake --build build-li --target fag_cand_colloop -j
ncu --kernel-name device_kernel --launch-skip 1 --launch-count 1 \
  --section SourceCounters --export /tmp/main ./build-li/fag_cand_colloop --b=1 --h=16 \
  --q=16384 --k=16384 --d=128 --d_vo=128 --iterations=1 --mask=no
ncu --import /tmp/main.ncu-rep --page source --csv   # 每条 SASS 的 stall 样本
# 精度门（逐位：colloop 与未重构 fence_cta 同哈希）
./build/fag_cand_colloop --b=1 --h=16 --q=8192 --k=8192 --d=128 --d_vo=128 \
  --iterations=1 --verify --stability-runs=3 --compare-gradients=/tmp/grad_1sm.bin
```

---

## 补充（V3.2）：区域级停顿归属 + 半拆分结构实验（H8）+ 新组件探针

### S1. 停顿样本按代码区域归属（NCU 采样，171,167 样本 @16384）

| 区域 | 样本 | 占比 | 主导停顿 | 热指令 |
|---|---:|---:|---|---|
| COMPUTE warps（softmax/corner/dKdV 尾部） | 88,956 | 52.0% | long_sb / wait | MUFU.EX2 16309, FADD2 10574 |
| REDUCE warps（dQ drain） | 38,386 | 22.4% | **barrier 14487** | BSSY 10983, BAR 5773, FENCE 3039 |
| RELAY + EXIT 尾部 | 28,990 | 16.9% | barrier（UCGABAR 16078=出口伪影） | — |
| LOAD warp | 8,376 | 4.9% | membar 3515（TMA 前代理栅栏，PTX 必需） | FENCE 3606 |
| MMA warp | 4,004 | 2.3% | long_sb | UTCQMMA 仅 194 样本 |

reduce 区 22.4% 的停车（栅栏+重构汇合为主，STS 本身停车极少）引出假设 H8。

### S2. H8（新增反驳）：reduce 停车时间可经"半拆分"收割 → 否决

**假设**：drain 的 4 轮 staging 中 half0（tile 0,1）与 half1（tile 2,3）数据完全不相交，
却被全 128 线程栅栏串行化且每轮有 64/64 分支发散——并行化两半应能砍掉栅栏关键路径。

**实验 `FAG_DQ_HALF_SPLIT`**（每半独立 64 线程命名栅栏[硬件 id 5/6]、各自 lane-0 发 TMA、
半内零发散、4-stage 轮换；注意 `PipelineTmaStore` 是 per-leader TMA bulk-group 计数器而非
per-stage mbarrier，双发射者需各计各组）：

| shape | base | half-split | Δ |
|---|---:|---:|---:|
| 4096 / 8192 / 16384 | 1377 / 1790 / 1964 | 1330 / 1742 / 1884 | **−3.4 / −2.7 / −4.1%** |

正确性全过（verify/1SM PASSED，数值与 base 同级）——**结构正确但仍净亏**。机理：两半
TMA 同时发射使每迭代瞬时 4 路 reduce-add RMW 并发（对照串行发射的天然节拍），TMA/L2 RMW
路径拥塞；栅栏/重构汇合的节省不足以补偿。**结论：reduce warps 的 22.4% 停车是保护性
空闲，不可收割**——它们等待的正是必要的 TMA/store 完成节拍。

### S3. 新组件探针（均为 timing-only）

| 探针 | @4096/8192/16384 | 判读 |
|---|---|---|
| `fag3_lse_load`（删 LSE+sumOdO 的 LDS 读） | 1225/1529/1702 = **−11/−15/−13%** | **负成本装载类**第三成员（sload −12.5%、dpload −17.9% 同类）：删除反而更慢——LDS 延迟槽为调度器提供切换机会；此类装载不是可回收瓶颈 |
| `fag3_red_fence`（删 reduce 循环 4×代理栅栏） | +0.4/−0.3/+1.1% | ≈0~1%，正确性必需，不可行动 |
| `fag3_comp_bar`（删 compute 2×TransformBarrier） | +0.7/+0.8/+0.2% | ≈0.5%，正确性必需，不可行动 |

### S4. 测量事故记录（方法学）

调试半拆分死锁期间，一个被 timeout 掩护的后台筛查任务持续产生死锁内核进程（僵尸占用
SM），污染了一轮测量（含 base 一度测得 757）。发现后 kill 全部残留、base 复测 1966.8
恢复后才重测全部数字——本文所有 V3.2 数据均为清场后测量。死锁根因：命名栅栏计数误写
`64*2=128`（SASS `BAR.SYNC R,0x80` 可见）而每半仅 64 线程到达；修复为 `2*32`。

### S5. V3.2 结论

1. **未发现新的 >1% 可行动瓶颈**：同步残量（reduce fence ~1%、compute bar ~0.5%、
   load-warp membar ~2% 停车）全部为正确性必需且不在关键路径；负成本装载类不可回收。
2. **H8 反驳完成**：FP32 约束下 dQ drain 的结构性税结论不变（§5 穷尽论证 + 半拆分
   反证）。区域归属表（S1）为最终画像。
3. FP32 约束下的最优版本维持 `fag_cand_colloop`（1377/1790/1964；主 kernel
   1562/1930/2067）。

新靶：`fag3_dq_half{,_p1,_dbg}`、`fag3_lse_load`、`fag3_red_fence`、`fag3_comp_bar`。

---

## S6. 全消融上限 vs 4500 TFLOPS：为什么删光计算也到不了

**问题**：把所有消融可发现的瓶颈全部解决，为何仍远达不到 4500 TFLOPS（B200 FP8 dense
峰值）？删光所有计算后 TFLOPS 也只有 ~3352？

### 定量（主 kernel @16384，FLOPs = 10·B·H·Q·K·D = 5.498 PFLOP）

| 场景 | 时间 | TFLOPS | vs 4500 |
|---|---:|---:|---:|
| 4500 TF（FP8 峰值）所需预算 | 1222 µs | 4500 | 100% |
| 纯 MMA 背靠背（无间隔理论值） | ~1410 µs | ~3900 | 87% |
| **floor（删光全部可消融计算，仅剩骨架）** | **1640 µs** | **3352** | **74%** |
| 当前最优 colloop | 2649 µs | 2075 | 46% |

**空骨架 1640 µs > 4500 TF 预算 1222 µs——即使计算无限快，本结构也到不了 4500。**

### 骨架为什么这么贵（floor 的 NCU 取证）

floor 内核（零计算）stall 画像：**long_scoreboard 19.7 周期/指令（占停顿 ~70%）+ barrier
3.8 + wait 1.5**；`issue_active` 仅 **14.2%**——发射槽 86% 空闲，机器被"等"饿死：
1. **每迭代串行依赖链**：warp-specialized 的 load→mma→compute→mma→reduce 角色交接，
   每迭代 10+ 个 mbarrier/命名栅栏往返（各 ~100-300ns），且链条长度由算法依赖钉死
   （S 必须先于 softmax，P 先于 dV，dS 先于 dK/dQ……）；
2. **浅流水下的 TMA 装载延迟**：Q/dO 2-stage、LSE/OdO 更浅，~1µs 级装载延迟只能部分
   预取隐藏——floor 的 long_scoreboard 主要就是等数据到达与 mbarrier 相位；
3. **1 CTA/SM 占用**：141.57KB SMEM + 64K 寄存器恰好钉死每 SM 一个 CTA（理论占用 25%、
   16 warp）——流水线停顿时 SM 没有第二个 CTA 可切换，延迟 100% 裸露；
4. **不是带宽**：全部 TMA 装载流量 ~540MB @8TB/s ≈ 67µs，仅占 floor 的 4%。

### MMA 依赖链的第二堵墙

4500 = 峰值要求 MMA 零间隔背靠背。实际每 tile 是 **5 个串行依赖的 GEMM**（S→[softmax]→
P/dP→dS→dK/dV/dQ），中间夹 TMEM/SMEM 往返：tensor 管线实测利用率 ~52%；即便假设 MMA
间隔为零，128 迭代 × 11.05µs（335.8 MFLOP/迭代 ÷ 30.4 TFLOPS/SM）≈ 1410µs = 3900 TF
的硬上限，仍 < 4500。

### 结论

1. **是——固有开销本身就是大头**：骨架占当前运行时间 62%（1640/2649），它由同步延迟链
   + TMA 延迟 + 单 CTA 占用构成，属延迟型成本，删计算不削减它；
2. 消融的可回收空间（2075→3352）受骨架钳制；3352→3900 受 MMA 依赖链钳制；4500 需要
   零间隔峰值发射——对 attention backward 的依赖结构不可达；
3. 现实天花板 = 骨架与 MMA 链的最大重叠 ≈ 2100~2600 TF（当前 2075 已在其间）。要突破
   必须动结构（roofline 文档 §13 同结论）：2 CTA/SM（需 ≤114KB SMEM，即缩 tile）、更深
   跨 tile 流水、减少每迭代同步点数。

### S6.1 实测补充：骨架+纯 MMA（`fag3_floor_mma`，保留 TMA/流水线/全部 5 个 MMA，删光计算侧工作）

| 配置 | 时间 | TFLOPS | 增量 |
|---|---:|---:|---|
| floor（纯骨架，无 MMA） | 1640 µs | 3352 | — |
| **floor_mma（骨架+5 MMA）** | **2072 µs** | **2653** | **+432 µs = MMA 暴露成本** |
| base（全部） | 2649 µs | 2075 | +577 µs = 计算暴露成本 |

**1640 + 432 + 577 = 2649 µs，与 base 精确可加。** 三个关键修正/确认：

1. **MMA 的暴露成本仅 432 µs**（理论背靠背 1410 µs 的 31%）——即便零计算、无竞争，
   MMA 也有 69% 藏在骨架的延迟等待后面（tensor 管线在 mbarrier/TMA 延迟的阴影里跑）；
2. **在完整 base 中删除全部 MMA（`fag3_mma_all`）只回收 −1.1%（~30 µs）**——有计算
   工作在场时 MMA 几乎 100% 被遮挡。**修正 §6 原先"MMA 依赖链是第二堵墙（3900 TF）"
   的理论推断：实测 MMA 链不是约束**，真实排序 = 骨架(62%) ≫ 计算(22%) ≫ MMA(1%)；
3. 时间线本质上是**串行叠加**：骨架延迟链先走 → 计算的未遮挡部分接上 → MMA 几乎全藏。
   warp-specialization 的重叠收益远低于名义——因为单 CTA/SM 下没有任何东西可切换。

结论不变且更强：**4500 不可达的结构根源是骨架延迟链（1640 µs > 1222 µs 预算）**；
且"删光计算"的上限应按 2653 TF（还需跑 MMA）而非 3352 TF 计——真实 kernel 必须
执行其 GEMM。现实区间 = [当前 2075, 骨架+MMA 2653]，进一步压缩只能靠缩短骨架
（更深流水/减同步点/2 CTA-SM）。

---

## S7. 精度缩放的 Amdahl 效应：为什么低精度算子的固定开销占比大

**观察**：FP8（低精度）的固定开销（骨架）占比大；FP16/高精度因搬运与计算量大，同样
的固定开销被摊薄。

**结构解释**：内核时间 T = S（骨架：同步链/TMA 延迟/流水线交接，**与精度无关**——它是
延迟型成本，字节数减半不缩短 mbarrier 往返）+ W（MMA+计算路径暴露，随精度近似减半）。
而峰值算力 ∝ 1/字节数 → 时间预算 = FLOPs/峰值 ∝ 字节数。精度减半 = 预算减半、S 不变。

### 定量（@16384 主 kernel）

| | FP8（本内核） | FP16（FA4） |
|---|---:|---:|
| 峰值（dense） | 4500 TF | 2250 TF |
| 预算 = FLOPs/峰值 | 1222 µs | 2443 µs |
| 骨架 S（各自 floor 实测） | 1640 µs | 2480 µs |
| **S / 预算** | **1.34×（超支 34%）** | **1.02×（恰好打平）** |
| 实测 / 峰值效率 | **46%** | **63%** |

- FP8 的骨架超出其峰值预算 34% → 效率结构性钳制在 ~46-50%；
- FP16 骨架绝对值更大（2480 vs 1640，不同实现），但其预算也翻倍 → 仅打平 → 63%；
- **FP8/FP16 实测加速 = 3882/2649 = 1.47×，远小于理论 2×**——恒等式
  speedup = (S+W₁₆)/(S+W₈)，S 不变时 S 占比越大加速越趋向 1。这正是业界"FP8 在
  attention 类内核上很难拿到 2×"的结构原因。

### shape 摊销趋势（FP8 主 kernel）

| Q=K | 效率（实测/4500） |
|---|---:|
| 4096 | 35% |
| 8192 | 43% |
| 16384 | 46% |

工作量每翻倍效率 +3~8%（每内核/每波次固定成本摊薄）——同向印证固定开销主导。
（floor@4096/8192/16384 = 143/486/1640 µs，含波次量化效应：grid=(K/128)×16头，大
shape 多波串行，逐迭代均值不直接可比。）

### 推论

1. 低精度提升的正确姿势是**压骨架或摊骨架**，不是再抠计算：骨架减半（1640→820）=
   T≈1830µs ≈ **3000 TF（67%）**，比消灭全部计算暴露（577µs）的上限更高；
2. 摊骨架 = 更大 tile（迭代数减半）——被 TMEM 512 列钉死；或 2 CTA/SM——被 SMEM/寄存器
   钉死（roofline §13 的结构性选项，即此处的定量依据）；
3. 高精度算子（FP16+）在同一结构上天然获得更高峰值效率，其优化重点才是搬运/计算本身
   ——两类算子的瓶颈画像不同，优化策略应分化。
