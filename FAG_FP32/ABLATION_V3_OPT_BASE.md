# V3 消融矩阵：重构后最优基线（colloop）上的瓶颈重排 + FP16 对照（2026-08-16）

基线 = §11 采纳的最优配置 `fag_cand_colloop`（`FAG3_BASE` = BWD_2SM + EX2=4 + REGALLOC +
FENCE_CTA + **FAG_DS_RUN_STORES** + **FAG_DS_COLUMN_SPLIT**，主 kernel 2649 µs @16384 =
2075 TFLOPS）。目的：corner-turn 重构消掉 MMA/SMEM(交换/寻址) 成本后，**重排暴露瓶颈**，
并与 FP16（FA4 官方 2-CTA，python CuTe DSL）同协议对照。

- 测量口径：主 kernel nsys Med（20 次，锁频 1965 MHz，`--mask=no`）；FP8 快筛用 3-kernel
  TFLOPS 打印（排名与主内核一致）。
- 消融语义：timing-only，每靶只删一个组件，全部流水线握手保留（沿用 V1/V2 方法论）。
- FP16 侧：`flash_bwd_sm100.py` 既有 `FAG_ABLATE_*` 环境变量钩子（MMA_S/DP/DV/DK/DQ、
  CORNER、EXP2、SOFTMAX、FLOOR），`bench_bwd_perf_fp16.py` + nsys，**双样本取中位**
  （单样本会因 python 侧冷启动离群——首轮 base@16384 = 4.189 ms，复测 3.882 ms ≈ 文档
  1410 TFLOPS 口径）。
- 新增 run-path 消融 stub：`FAG_ABLATE_{CORNER,XCHG_STORE,OWN_STORE}` 在
  `FAG_DS_RUN_STORES` 分支的等价物（原 stub 只在 strip 路径编译，新基线上会是 no-op）。
  靶塔：`fag3_*` × 23（CMakeLists `FAG3_CFGS`）。

## 1. FP8 主 kernel 消融（nsys Med，三 shape，双击复核）

Base（fag_cand_colloop）Med：4096 = 219423 ns、8192 = 705212 ns、16384 = 2649327 ns。

| 消融 | Δt@4096 | Δt@8192 | Δt@16384 | 判读 |
|---|---:|---:|---:|---|
| **dQ reduce** | **−9.32%** | **−9.16%** | **−9.03%** | **三 shape 稳居 #1**：dQ 卸载（TMEM load → SMEM → TMA REDUCE_ADD） |
| reduce_tma（只删 TMA 发射，保留 drain） | −1.33% | −2.24% | −0.46% | 原子流量 ≈0.5~2%；**−9% 几乎全在 drain 串行链**（见 §3.2） |
| pack（整条 corner-turn：4×STS + 交换发射） | −4.44% | −3.21% | −3.04% | 重构后残链（V2 时 −9.6%），小 shape 略高 |
| softmax 微分（FADD2/FMUL2） | −3.65% | −2.85% | −2.62% | 数学必需 |
| corner（peer 半 stores + bulk copy + 握手） | −0.89% | −1.16% | −0.56% | **塌缩**（V2 时 −6.6%） |
| bulk（copy+tx） | +0.68% | −0.10% | +1.22% | ≈0（调度扰动类） |
| floor（全数据路径删空） | −38.11% | −34.13% | −38.10% | 固定骨架：TMA 装载 + 屏障 + epilogue |

快筛补充（3-kernel TFLOPS，@16384 base 2004）：scale_lse +0.75%、xchgstore +0.55%、
ownstore +0.52%、fence +0.06%、s/dv/dp/exp2/cwait ≈0、mma_all −1.1%、dsstore −0.9%。
快筛排名与 nsys Med 在三 shape 上一致（reduce 三 shape 均 −8.9~−9.5% 居首）。

**已知调度扰动类**（删除反而变慢，V2 已标注 ⚠ 同类，非真实成本）：sload −12.5%、
dpload −17.9%、pstore −10.7%（@16384 3-kernel）、dk −3.0%、dq −1.6%、quantv3 −2.2%。

## 2. V2 → V3 组件迁移（主 kernel Δt@16384）

| 组件 | V2（fence_cta 基线） | V3（colloop 基线） | 归因 |
|---|---:|---:|---|
| pack / 双 STS | −9.6% | −3.0% | **RUN_STORES 重构吃掉 ~2/3**（循环内 swizzle 寻址清零 + 更宽 TMEM 指令） |
| own STS | −6.0% | ≈+0.5% | 同上 |
| xchg STS | −5.9% | ≈+0.55% | 同上 |
| corner 交换总链 | −6.6% | −0.56% | 交换家族合计 ≤1.2% |
| copy+tx | −2.8% | ≈0 | 字节搬运本已便宜 |
| fence cta 残余 | +0.3% | +0.06% | ≈0 |
| **dQ reduce** | −7.0%（#2） | **−9.0%（#1）** | 基线加速后占比放大 |
| softmax | −6.0% | −2.6% | COLUMN_SPLIT 调度改善其暴露 |
| exp2 / scale_lse | −2.9 / −2.7 | ≈0 | 同上 |
| MMA 全部 | −0.9% | −1.1% | 依然完全隐藏 |
| floor | −38.7% | −38.1% | 不变 |

## 3. FP16 对照（FA4 2-CTA，双样本中位，三 shape）

Base Med：4096 = 275774 ns（1245 TF）、8192 = 977656 ns（1406 TF）、16384 = 3882376 ns（1416 TF）。

| 消融 | Δt@4096 | Δt@8192 | Δt@16384 |
|---|---:|---:|---:|
| **dq_reduce（跨 SM dQ 归约数据路径）** | **−7.7%** | **−9.3%** | **−9.45%** |
| mma_s | −5.2% | −8.2% | −8.3% |
| mma_dp | −1.1% | −7.1% | −9.0% |
| mma_dq | −3.5% | −7.4% | −5.7% |
| mma_dv | −1.9% | −6.8% | −7.5% |
| mma_dk | −3.3% | −6.9% | −3.6% |
| corner（DSMEM 交换） | −6.7% | −6.5% | −7.4% |
| softmax | −2.6% | −1.8% | −2.0% |
| exp2 | +1.6% | +1.0% | +4.1%（钩子语义=删硬件 exp2，仪器失真，判 ≈0） |
| floor | −30.6% | −34.6% | −36.1% |

Shape 趋势：MMA 各项成本随 shape 增大而放大（4096 时 mma_dp 仅 −1.1%，16384 达 −9.0%
——小 shape 下 MMA 时间被固定骨架遮蔽）；**dq_reduce 与 corner 在三个 shape 都稳定为大项**，
dq_reduce 在 8192/16384 均为最大/并列最大。

### 3.1 勘误与补充：FP16 同样做跨 SM dQ 归约（新增 `FAG_ABLATE_DQ_REDUCE` 钩子）

初版结论"FP16 无对应暴露"**错误**：`flash_bwd_sm100.py` 与 FP8 FAG 同构——同样是 n_block(KV)
外层 / m_block(Q) 内层，dQ 部分和经 reduce warps `TMEM→RMEM→SMEM→cp.async.bulk.reduce_add.f32`
累加进 FP32 gmem workspace（`dQacc_reduce`，带确定性模式的 gmem 信号量），最终由独立的
postprocess kernel（FP32→FP16，@8192 仅 24.7 µs）收尾。tile-pair 数量与 REDUCE_ADD 字节量
与 FP8 完全相同（(K/128)(Q/128)×64KB）。

为此新增钩子 `FAG_ABLATE_DQ_REDUCE`（删 TMEM load + SMEM 暂存 + reduce-add 发射，保留
pipeline_dQ 握手/reduce barrier/TMA 排空/信号量逻辑），双样本 @8192/16384：

| | base | ablate | Δt |
|---|---:|---:|---:|
| @8192 | 977049 | 886758 | **−9.24%** |
| @16384 | 3882376 | 3516697 | **−9.45%** |

**dQ 归约在 FP16 侧同样是并列最大瓶颈（−9.4%，与 mma_dp 持平）**。

### 3.2 FP8 侧 reduce 分解：drain vs TMA 原子流量（新增 `fag3_reduce_tma`）

保留 TMEM load + SMEM 暂存、只删 TMA REDUCE_ADD 发射（`FAG_ABLATE_REDUCE_TMA`），三 shape：

| 主 kernel Med Δt | 4096 | 8192 | 16384 |
|---|---:|---:|---:|
| reduce_tma（只删 TMA） | −1.33% | −2.24% | −0.46% |
| reduce（全删） | −9.32% | −9.16% | −9.03% |

**三 shape 一致：−9% 几乎全部来自 drain（TMEM→RMEM 加载 + SMEM 暂存 + 栅栏串行链），
TMA reduce-add 的原子流量本身仅 ≈0.5~2%**（异步引擎完全被 drain 遮蔽；带宽假说被否定
——1GB REDUCE_ADD 流量不是瓶颈，reduce warps 的串行三段链才是）。

### 3.3 MUFU 分流价值复测（`fag3_opt_noex2`，反向消融）

在最优基线上**去掉** ex2 的 MUFU/软件 FMA 分流（不定义 `FAG_EX2_EMU_STRIDE`，48 次
exp2/线程/迭代全走硬件 MUFU），其余配置不变。×3 发射中位（波动 ≤0.2%）：

| shape | 3-kernel：最优 → 去分流 | 损失 | 主 kernel：最优 → 去分流 | 损失 |
|---|---|---:|---|---:|
| 4096 | 1377 → 1297 | **−5.8%** | 1562 → 1457（235.8 µs） | **−6.7%** |
| 8192 | 1790 → 1698 | **−5.1%** | 1930 → 1822（754.5 µs） | **−5.6%** |
| 16384 | 1964 → 1825 | **−7.1%** | 2067 → 1881（2922.3 µs） | **−9.0%** |

**解读**：分流在新基线上的价值（+5.4~+7.7%）显著大于旧基线（V2 时 exp2 消融仅 −2.9%）。
机理：corner-turn 重构清掉主循环地址 ALU/LSU 压力后，全硬件 exp2 的 **MUFU 吞吐瓶颈
反而更暴露**（每 SM 象限 MUFU 仅 4 lanes/cycle，48 次 EX2/线程/迭代成为限流点）；3:1
分流把 1/4 的 exp2 转移到空闲 FMA 管线，恰好填掉这个新暴露的坑。与 `fag3_exp2`
（删软件仿真那 1/4，≈0）互为对照：分流的收益不在省下哪部分计算，而在双管线配平。
印证 V2 行动项"在 fence_cta 基线上重扫 EX2_STRIDE"——重构后 2:1 / 1:1 配比值得重扫。

## 4. FP8 vs FP16 画像（主 kernel Med，同机同协议）

| | FP8 colloop | FP16 FA4 | FP8/FP16 加速比 |
|---|---:|---:|---:|
| @4096 | 219.4 µs（1566 TF） | 276.1 µs（1245 TF） | 1.258× |
| @8192 | 705.2 µs（1948 TF） | 977.0 µs（1406 TF） | 1.386× |
| @16384 | 2649.3 µs（2075 TF） | 3882.4 µs（1416 TF） | **1.465×** |

定版复测（2026-08-16，×3 发射中位）：主 kernel **1562 / 1930 / 2067 TF**
（220.0 / 712.3 / 2660.1 µs，vs FP16 **1.26× / 1.37× / 1.46×**）；3-kernel 合计
**1377 / 1790 / 1964 TF**（colloop = FAG_0816 默认版，两口径波动均 ±0.5%）。

组件暴露对比（@16384，删该组件的 Δt）：

| 组件 | FP8 | FP16 | 解读 |
|---|---:|---:|---|
| MMA（单项/全部） | −1.1%（全部，**完全隐藏**） | −3.6~−9.0%/项 | FP16 是 **MMA 主导**；FP8 的 2× MMA 吞吐让它彻底退出瓶颈榜 |
| corner/DSMEM 交换 | −0.6% | −7.4% | FP16 交换 2× 字节且未做 run 重构；FP8 经 §11 后近乎清零 |
| pack/字节成形 | −3.0% | —（fp16 无需 pack） | FP8 独有的 1B gather 残链 |
| softmax | −2.6% | −2.0% | 相当 |
| exp2 | ≈0 | ≈0 | 相当 |
| **dQ 跨 SM 归约** | **−9.0%**（drain 8.6 + TMA 0.5） | **−9.45%**（§3.1，并列 #1） | **两侧共同的结构性税**：KV-split 设计的代价，构成几乎相同 |
| floor 骨架 | −38.1% | −36.1% | 相当；两侧固定成本占比一致 |

## 5. 结论：MMA 与 SMEM 消掉之后，暴露最多的是 dQ reduce 卸载路径（两侧共有）

1. **新 #1 = dQ reduce**，nsys Med 三 shape −9.32/−9.16/−9.03%（FP8），是重构后唯一 >3% 的
   可行动单项。路径：reduce warps 将 dQ 累加从 TMEM 读出 → 写 SMEM → TMA `REDUCE_ADD` 到 FP32
   workspace（再由 Convert kernel 转 FP8）。V2 时为 #2（−7.0%），基线提速后反超登顶。
   **§3.1 勘误后进一步明确：FP16 侧同样是 −9.45%（并列 #1）——这不是 FP8 特有税，而是
   两个内核共有的 KV-split 结构性成本**；FP8 的 1.47× 领先下其绝对值更小（238 vs 367 µs）。
   §3.2 分解：成本在 drain 串行链（TMEM load + SMEM 暂存 + 栅栏，8.6%），**不在**原子流量
   （0.5%）——优化方向应是缩短/重叠 drain，而非减少 REDUCE_ADD 字节。
2. **#2/#3 = pack 残链（−3.0%）与 softmax（−2.6%）**：pack 剩余部分即 4×STS.128 的端口占用
   本身（寻址已清零）；softmax 为数学必需。
3. **交换家族全部 ≤1.2%**（corner −0.6 + xchg/own ≈±0.5 + fence/bulk ≈0）——§11 重构 +
   fence_cta 后，DSMEM 交换不再是瓶颈。st.async 路线（§11 −11.8%）无需再议。
4. **MMA 全部删除仅 −1.1%**：FP8 的算力供给过剩，整个 GEMM 集合都在阴影里。与 FP16 的
   根本分化：FP16 前三瓶颈 = dQ 归约（−7.7/−9.3/−9.45）+ mma_s（−5.2/−8.2/−8.3）+
   corner（−6.7/−6.5/−7.4）；FP8 只剩 dQ 归约（−9.0~−9.3）+ 两个 ≤4.5% 项。注意 FP16 的
   MMA 成本随 shape 放大（mma_dp @4096 仅 −1.1% → @16384 −9.0%），而 **dq_reduce 在两侧
   三个 shape 都是大项**——两侧共同的地板是 dQ 归约与 TMA/屏障骨架。
5. floor（−38%）两侧一致：剩余优化天花板仍由 TMA 装载 + 屏障骨架决定（roofline 文档同结论）。

### 行动项（按暴露度排序）

1. **reduce drain 路径**（−9.0% FP8 / −9.45% FP16，两侧同构可共享方案）：候选——TMEM load
   加宽（reduce warps 现用 32dp32b 系列，与 compute warps 同族）、SMEM 暂存与栅栏合并
   （fence+named-barrier 每迭代的串行链）、drain 与下一迭代 MMA 的更深重叠、双缓冲
   sdQaccum 的发射重排。注意：减少 REDUCE_ADD 字节量无效（§3.2）。
2. pack 残链（−3.0%）：STS 端口与 TMA 的竞争已被 dup-STS 探针定证（V2 §9），进一步需
   结构性变化（store 时段重排/与 reduce 共用发射窗口）。
3. softmax（−2.6%）：数学必需，仅 f32x2 打包微调空间。
4. **ex2 分流配比重扫**（§3.3：分流现值 +5.4~+7.7%）：`FAG_EX2_EMU_STRIDE ∈ {2,4,8}`
   在新基线上重扫——重构后 MUFU 瓶颈更暴露，最优配比可能从 3:1 移向 2:1/1:1。
5. FP16 侧若要追差距（1.47×）：优先级是 MMA 调度、dQ 归约 drain（同 FP8）、其 2× 交换字节
   的 run 化（FP8 同款重构可平移，corner −7.4% 可回收大半）。

## 6. 复现

```bash
nvidia-smi -lgc 1965,1965
cd temp/test/test_merge/Benchmark/FAG_FP32
cmake --build build --target fag_cand_colloop fag3_reduce fag3_reduce_tma fag3_pack \
  fag3_softmax fag3_corner fag3_bulk fag3_floor fag3_opt_noex2 -j  # + 其余 fag3_* 快筛靶
nsys profile -o /tmp/r ./build/fag3_reduce --b=1 --h=16 --q=16384 --k=16384 \
  --d=128 --d_vo=128 --iterations=20 --mask=no
nsys stats -r cuda_gpu_kern_sum --format csv /tmp/r.nsys-rep   # 主 kernel Med

# FP16 侧（双样本取中位；含新增的 dQ 归约钩子）
cd /home/ubuntu/workspace/oyhj/FAG/flash-attention
nsys profile -o /tmp/x env FAG_ABLATE_MMA_DP=1 /usr/bin/python3 \
  bench_bwd_perf_fp16.py 16384 20
nsys profile -o /tmp/y env FAG_ABLATE_DQ_REDUCE=1 /usr/bin/python3 \
  bench_bwd_perf_fp16.py 16384 20
```

报告：`/tmp/fag3_screen.txt`（FP8 三 shape 快筛 72 行）、`/tmp/fag3_nsys/*.nsys-rep`
（FP8 主 kernel Med）、`/tmp/fp16_perf/*.nsys-rep`（FP16 21+9 份，含 `_v2` 复测）。

## 7. dQ drain 攻坚战（V3.1）：bf16 staging + deep-serial —— 新最优与结构性墙证明

针对 §1/§3.2 定位的 #1 瓶颈（dQ drain 的 STS staging，−8.5%），系统性假设-反驳-收敛过程：

### 7.1 假设反驳链（每步都有实验证据）

| # | 假设 | 反驳实验 | 结果 | 裁决 |
|---|---|---|---|---|
| H1 | 栅栏串行（8×BAR/迭代） | `fag3_dq_batched`（8→2 栅栏） | **−7.1% 更慢**（失去逐段 TMA/STS 流水，制造整迭代排空气泡；与 roofline 文档 L338 旧数据一致） | ✗ 驳回 |
| H2 | 2-stage 深度不足 | `fag3_dq_deep4`（4-stage 串行） | ±0.5% 中性 | ✗ 驳回（f32 下） |
| H3 | LDTM 延迟+release 链 | `fag3_reduce_sts`（保留 LDTM+release，删 STS+TMA）= +9~11% ≈ 全删 `reduce`(+9.5%) | LDTM/release ≈ 0 | ✗ 驳回 |
| H4 | TMA 原子流量 | `fag3_reduce_tma`（只删 TMA） | −0.5% | ✗ 驳回 |
| H5 | TMEM 单缓冲可双缓冲 | TmemAllocation 算术：512 列 = DK128+DV128+DP/DS128+S/P128，dQ 已与 S 上半 triple-alias | 容量 100% 满 | ✗ 结构性不可能 |
| H6 | 字节带宽 | NCU：L1TEX 65% 占用（非饱和），STS 字节占比微小 | 带宽不成立但**管线占用实证** | ✗ 部分驳回 |
| H7 | UCGABAR_WAIT（9.4% 最热指令）是隐藏瓶颈 | SASS 定位：位于内核 **EXIT 尾部**（cluster_arrive/wait+EXIT） | 先完成 warp 等待落后者的采样伪影 | ✗ 驳回 |
| — | **staging STS 本身（发射/wavefront 占用）** | 三方分解（reduce/reduce_sts/reduce_tma）+ NCU wavefront 差分（ST wavefront 119M→44M、ST 冲突 18.3M→10.4M、L1TEX 65%→41%）+ SASS 纯度验证 | **STS = 8.5% 确证** | ✓ 成立 |

### 7.2 解法：`FAG_DQ_BF16_STAGING`（+ `FAG_DQ_DEEP_SERIAL`）

唯一能削减 staging STS 的合法路径是**减半字节数**：dQ partial 以 BF16 暂存/累加
（TMEM load 仍 f32 → `F2FP.BF16.F32.PACK_AB` 打包 → 2×STS.128/段；workspace 与 TMA
reduce-add 同步 bf16，字节全链减半）。**SASS 纯度验证**：相对 colloop 恰好只
`ST.E.128 32→16` + 新增 64 条 `F2FP.BF16.PACK_AB`，LDTM/UTMAREDG/BAR.SYNC/MUFU/其它
STS 全部不变——实验只改了想改的部分。

**精度**（dQ 最终输出本就是 FP8 e4m3）：vs 参考 max=0.082 ≤ 0.1、mean=0.0119 ≤ 0.02
（与 f32 staging 几乎相同，fp8 输入噪声主导）；1SM 逐下标 PASSED；causal PASSED；
2SM-vs-1SM mean 1e-4（门限 0.02 的 1/200）。位稳定 @16384 与 fence_cta 线同源 FAILED
（cta-fence 竞争窗口继承，非 bf16 引入）。

> **约束否决（2026-08-16 追记）**：BF16 方案改变 dQ partial 的累加精度语义，按
> "dQ 精度不可修改"的硬约束被否决（数值影响实测留档于附录 B）。FP32 约束下的完整
> 分析与最终定版见独立文档 `DQ_DRAIN_ANALYSIS_FP32_CONSTRAINED.md`；约束下最优
> 保持 `fag_cand_colloop`（1377/1790/1964，主 kernel 1562/1930/2067）。以下成绩
> 仅作为放宽约束时的政策选项留档。

### 7.3 （已否决方案的）成绩（`fag3_dq_bf16_deep4` = FAG3_BASE + BF16_STAGING + DEEP_SERIAL）

| 口径 | 4096 | 8192 | 16384 |
|---|---:|---:|---:|
| **3-kernel TFLOPS** | **1427** | **1863** | **2048** |
| vs colloop（§4 定版） | +3.6% | +4.1% | +4.3% |
| vs fence_cta-only | +7.7% | +6.7% | +9.3% |
| vs 原始 base | **+17.4%** | **+19.5%** | **+21.8%** |
| 主 kernel Med | 210.5 µs（1633 TF） | 680.6 µs（2019 TF） | 2543.4 µs（**2161 TF**） |
| vs FP16 FA4 主 kernel | **1.31×** | **1.44×** | **1.53×** |

（deep4 在 f32 staging 下中性、在 bf16 下额外 +0.5%——staging 减半后 stage 轮换重新有了
边际价值。）

### 7.4 剩余不可消除项的量化（"为何到此为止"）

bf16 后的 drain 残留 ≈ 4.5%（=8.5% − 4%）：BF16 已是 TMA reduce-add 支持的最窄浮点类型
（f16x2 同宽且动态范围更差）；STS.128 已是最宽存储；TMEM→SMEM 无直通（tcgen05.cp 仅反向）；
TMEM 无双缓冲空间（H5）。其余大项：MUFU.EX2 9.5% stall 样本（s4 已是重扫后最优配比，
全硬件 −5~9%、s2/s8 均劣）；softmax −2.6%（数学必需）；floor −38%（TMA 装载+屏障骨架，
两侧内核一致的结构常数）。**shape 专属分支评估：三 shape 在全部实验中行为一致
（各项 Δ 差异 ≤1%），无需分 shape 配置。**

### 7.5 复现

```bash
cmake --build build --target fag3_dq_bf16 fag3_dq_bf16_deep4 fag3_dq_batched \
  fag3_dq64 fag3_dq128 fag3_dq_deep4 fag3_reduce_sts fag3_reduce_tma \
  fag3_ex2_s2 fag3_ex2_s8 fag3_opt_noex2 -j
# 精度门
./build/fag3_dq_bf16_deep4 --b=1 --h=16 --q=8192 --k=8192 --d=128 --d_vo=128 \
  --iterations=1 --verify --compare-gradients=/tmp/grad_1sm.bin
```
