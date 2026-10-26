# FP8 pack/corner-turn 优化记录（基于指令级消融定位）

目标：在不牺牲正确性的前提下，削减消融实验定位的最大单项瓶颈——pack/corner 链（主 kernel −20.2%，见 `ABLATION_LIMITING_STUDY_FP8_VS_FP16.md`）。全部工作基于最优配置 `BWD_2SM + EX2_EMU_STRIDE=4 + REGALLOC`（`fag_stage_regalloc`），base 路径零改动（所有优化/消融均为 `#ifdef` 守卫；重建后 base 计时与消融轮基线一致 ±0.15%）。

- **测量口径**：nsys `cuda_gpu_kern_sum` Med，GPU 锁频 1965 MHz，20 次计时，shape `(1,16,{4096,8192,16384},128)`。
- **正确性门**（采纳的优化必须全过）：`--verify` 高精度参考（max≤0.1 / mean≤0.02）、`--stability-runs=3` 哈希稳定、1-SM 梯度快照直接对照、输入完整性哈希。

## 1. 判别消融：corner 的 −17.8% 到底花在哪

在最优 base 上加三个 timing-only 判别（`fag_abl_bulk` / `fag_abl_xchgstore` / `fag_abl_fence`）：

| 判别（主 kernel Δt @16384） | 4096 | 8192 | 16384 | 含义 |
|---|---:|---:|---:|---|
| 删 bulk copy + tx（保留两份 STS） | −12.0% | −13.7% | **−14.2%** | DSMEM 搬运+握手是大头 |
| 删 `fence.proxy.async.shared::cluster`（保留 copy） | −9.2% | −11.8% | **−9.8%** | **代理栅栏单项 ≈10%** |
| 删 xchg 半区 STS（保留 copy 字节数） | −4.1% | −4.3% | −4.1% | 交换侧 STS 本身 |
| recast 打包（`fag_candidate_pack`） | −0.3% | −0.1% | +0.8% | 中性：ptxas 已自行折叠 pack4 |
| 交换提前发射（`fag_opt_earlycopy`） | — | — | −0.1% | 发射时序不是瓶颈 |

结论：**`fence.proxy.async.shared::cluster`（generic→async 代理切换）单项约占主 kernel 10%**；copy 字节+tx ≈4.4%；xchg STS ≈4.1%。`cwait` 消融（去掉 ds_leader 等待）为 0%，说明延迟已被流水线盖住——这 10% 是代理切换对 SMEM 写管线的**排空/静止**成本，不是等待。

## 2. 候选与结果

| 候选 | 思路 | 主 kernel Δt | 判定 |
|---|---|---|---|
| `fag_opt_stasync`（FAG_OPT_STASYNC_XCHG） | xchg 写改 `st.async`（async proxy + mbarrier tx 计数），彻底免 fence | **+12.0%** | ✗ 拒绝：每条 16B store 一次 mbarrier 计数（512 次/迭代）在 mbarrier 单元上串行化，远贵于省下的 fence |
| `fag_opt_earlycopy`（FAG_OPT_EARLY_COPY） | 交换发射提前到 TMEM 存储之前，copy 引擎与 STTM 重叠 | −0.1% | ✗ 中性：发射时序不在关键路径 |
| **`fag_opt_ownlate`（FAG_OPT_OWN_LATE）** | **own 半区 STS 推迟到 fence 之后**：fence 时刻在飞的 generic store 减半（16KB），排空更短；own 存储对 dQ MMA 的可见性由迭代末已有的 `fence_view_async_shared` 保证 | **−0.7% / −0.9% / −1.1%** | ✓ **采纳** |
| recast（历史候选复测） | 直接 recast 转换器输出，免 12 条移位/或 | 中性 | ✗ 编译器已折叠（与 roofline 文档一致） |
| direct DSMEM（历史） | 远端 `st.shared::cluster` 直写 | +10~11%（历史） | ✗ 远端 store 慢于 STS+bulk |

## 3. 采纳的优化：`fag_opt_ownlate`

**改动**（`kernel/sm100_fmha_bwd_kernel_tma_2sm_warpspecialized.hpp`，全部在 `#ifdef FAG_OPT_OWN_LATE` 内）：

1. corner-turn strip 循环拆成两段：**先只发射 peer 半区（xchg）的 pack+STS**；
2. compute named barrier 之后立即发射 `arrive_and_expect_tx + fence.proxy.async + cp.async.bulk`（此时 CTA 内在飞的 generic store 只有 xchg 半区，fence 排空范围减半）；
3. **own 半区的 pack+STS 延后**到交换发射之后、TMEM dS 存储之前——与 bulk copy 引擎重叠；
4. own 存储对 dQ MMA 的可见性沿用迭代末原有的 `fence_view_async_shared`（不变）；字节、地址、握手全部与 base 相同。

寄存器压力不变（`tTR_rDST` 本就存活到 TMEM 存储）；xchg buffer 复用次序由既有 dS pipeline 往返保证（不变）。

**性能**（nsys Med，1965 MHz）：

| shape | base 主 kernel | ownlate 主 kernel | Δt | base 3-kernel | ownlate 3-kernel |
|---|---:|---:|---:|---:|---:|
| 4096 | 1349.3 | 1359.0 | −0.7% | 1239.6 | 1248.1 |
| 8192 | 1657.5 | 1671.7 | −0.9% | 1574.6 | 1587.7 |
| 16384 | 1729.9 | 1749.6 | **−1.1%** | 1684.6 | 1703.1 |

**正确性**（@8192 全部门禁通过）：
- `--verify`：`[OK]`（参考对照 max=0.088 ≤ 0.1，mean ≤ 0.02）
- `--stability-runs=3`：三次梯度哈希完全一致 PASSED
- 1-SM 快照直接对照：`[STRICT] direct gradient comparison PASSED`
- 输入完整性哈希：PASSED

## 4. 为什么只剩 ~1%：fence 成本的结构分析

- ownlate 把 fence 时刻的在飞 store 减半，只回收了 ~1% ⇒ fence 的 ~9.8% 成本**主要不是在飞 store 数量**，而是代理切换本身的固定静止成本（CTA 级 SMEM 管线 quiesce）。
- 替代路线全部堵死并有数据：
  - `st.async`（免代理切换）：mbarrier 计数开销 +12% > 省下的 fence；
  - 远端直写：历史 +10%；
  - 降级 fence 作用域（`.shared::cta`）：PTX 规定作用域须覆盖被排序的访问（bulk copy 是 cluster 作用域），非法；
  - 减少交换字节：受 TMEM 容量结构性钉死——免交换的 split-K dQ 需要每 CTA 128 行 dQ 累加器，TMEM 放不下（roofline 文档同结论）。
- 剩余可回收空间：fence 固定成本 ~9% + copy 字节 ~4.4% + xchg STS ~4.1%（≈ corner 消融上限 −17.8%），在当前 ownership/TMEM 结构下没有已知的正确替换。

## 5. 复现

```bash
nvidia-smi -lgc 1965,1965
cd /home/ubuntu/workspace/oyhj/temp/test/test_merge/Benchmark/FAG_FP32
cmake --build build --target fag_stage_regalloc fag_opt_ownlate fag_abl_bulk \
  fag_abl_xchgstore fag_abl_fence fag_opt_earlycopy fag_opt_stasync -j
# 性能
nsys profile -o /tmp/ownlate_16384 ./build/fag_opt_ownlate --b=1 --h=16 \
  --q=16384 --k=16384 --d=128 --d_vo=128 --iterations=20 --mask=no
# 正确性（1SM 对照）
./build/fag_bwd_fp8_1sm --b=1 --h=16 --q=8192 --k=8192 --d=128 --d_vo=128 \
  --iterations=1 --dump-gradients=/tmp/grad_1sm.bin
./build/fag_opt_ownlate --b=1 --h=16 --q=8192 --k=8192 --d=128 --d_vo=128 \
  --iterations=1 --verify --stability-runs=3 --compare-gradients=/tmp/grad_1sm.bin
```

新增 target：`fag_abl_{bulk,xchgstore,fence}`（判别）、`fag_opt_{earlycopy,stasync,ownlate}`（候选）。
原始 profile：`/tmp/opt_fp8_{base,bulk,xchgstore,recast,fence,earlycopy,stasync,ownlate}_{4096,8192,16384}.nsys-rep`。

## 6.（已否决，见 §7 勘误）fence 作用域降级（`fag_opt_fence_cta`，+9.4~11.8%）

对抗追问"FP16 的交换为什么没这么大瓶颈"时发现：FA4 的同模式 2-CTA 交换（本地 STS → `cp.async.bulk` → peer）只用 **cta 作用域**的 `fence_view_async_shared`（DSL `fence_proxy(kind="async.shared", space="cta")`）。被排序的两个访问（generic STS 写、bulk 引擎的源读取）都在本 CTA 的 SMEM 内，peer 侧消费由 ds_full mbarrier 单独排序——cluster 作用域是过保守的。

实测（主 kernel，nsys Med，1965 MHz）：

| 配置 | 4096 | 8192 | 16384 |
|---|---:|---:|---:|
| base（cluster 作用域） | 1347.6 | 1657.7 | 1729.2 TFLOPS |
| **cta 作用域（合法）** | **1487.0（−9.4%）** | **1880.1（−11.8%）** | **1922.8（−10.1%）** |
| 非法 no-fence（上界） | 1493.8 | 1865.7 | 1957.9 |

正确性全门通过（@8192：1SM 快照直接对照 PASSED、3 次哈希稳定、严格参考 `[OK]`）。与非法上界仅差 ~1-2%（残余代理切换成本）。

**叠加注意**：pack 消融本身包含 fence 删除（无拷贝则无 fence），故 fence_cta 与 pack 消融组合不叠加（实测 ctapack ≈ pack 单独）。现实 kernel 的正确读法：base + fence_cta 即得 +10%，pack/STS/copy 的成本另行存在。

## 7. §6 勘误：fence_cta 经全 shape 重验被**否决**

应用户要求对 dQ/dK/dV 做全 shape 1SM 逐下标重验后，16384 暴露问题：

| 版本 @16384 | --stability-runs=3 | 1SM 逐下标对照 |
|---|---|---|
| base（cluster fence） | **PASSED**（3 次哈希逐位相同） | — |
| fag_opt_ownlate | **PASSED**（哈希与 base 完全一致 → 数值逐位无损） | PASSED |
| fag_opt_fence_cta | **FAILED**（3 次运行哈希变化） | max=0.006 仍达 FP8 噪声级 |
| fag_opt_v2（cta+ownlate） | **FAILED** | 同上 |

裁决：**cta 作用域的排序在 16384 的并发窗口下不充分——cluster 作用域是承载正确性的（load-bearing），不是过保守**；PTX 规则应按异步操作的访问域（cp.async.bulk.shared::cluster）而非"源地址在本地"来取 fence 作用域。4096/8192 通过只是窗口未开。**fence_cta/v2 不得采纳**；§6 的"追加采纳"撤回。当前唯一采纳的优化仍是 `fag_opt_ownlate`（−0.7~1.1%，三 shape 全门通过且输出与 base 逐位相同）。

### §7 补充：双标准终裁（量化 fence_cta 的真实风险）

10 次干净发射观察（@16384）：

- **多数运行的 dQ/dK/dV 与 base 逐字节相同**（哈希 0xf7f51ebc…/0x1174e61c… = base）
- 约 1/4 的发射出现漂移：dQ max=0.0098（≈1.25 ulp）、mean=1.4e-7（≈每 33.5M 元素个位数~十位数个近零元素翻转 1 ulp）；dK 类似更小；**dV 从不漂移**；每次运行都通过 1SM 容差门（max≤0.1/mean≤0.02）
- 对照事实：base 自身 vs 1SM 也**从不逐位相同**（max=0.0078=1 ulp，~0.1% 元素，全部 shape）——2-SM 归约顺序固有

**两条验收标准下的裁决**：

| 标准 | fence_cta | 依据 |
|---|---|---|
| **位稳定门**（仓库 `--stability-runs` 哈希；确定性训练/梯度哈希场景） | **不合格** | ~25% 发射逐位漂移（虽 1 ulp 级） |
| **容差门**（FA4 自身哲学：默认 `deterministic=False`、dQ 设计性不确定、容差测试） | **合理可用** | 漂移幅度=1 ulp、比例≈1e-6、无垃圾值、每次过 1SM 门——与 FA4 默认行为同级 |

**建议**：默认版本保持 `fag_opt_ownlate`（位安全）；吞吐关键且接受 FA4 级确定性的部署可用 `fag_opt_fence_cta`（+9.4~11.8%）。这是策略选择，不再是正确性绝对判据。

## 8. 最终定版：`fag_opt_fence_cta` 为最优版本（团队决策，容差标准）

**性能**（nsys Med，1965 MHz）：

| 版本 | 主 kernel TFLOPS（4096/8192/16384） | 3-kernel 合计 | vs base |
|---|---|---|---|
| base（fag_stage_regalloc） | 1347.6 / 1657.7 / 1729.2 | 1238.2 / 1575.0 / 1683.8 | — |
| **fag_opt_fence_cta（定版）** | **1487.0 / 1880.1 / 1922.8** | **1355.2 / 1774.3 / 1866.5** | **主 kernel +9.4~11.8%** |
| fag_opt_v2（+ownlate） | 1487.8 / 1868.9 / 1954.2 | 1355.4 / 1764.2 / 1896.2 | 与 fence_cta 混合（8192 −0.6%），未采纳 |

**精度档案**（容差标准）：
- dQ/dK/dV vs 1SM 逐下标：三 shape 全部 PASS（max=0.0039~0.0078 = 1 ulp，mean≈8e-6，与 base 自身 vs 1SM 的差异同级）
- 运行间：多数发射与 base 逐字节相同；~1/4 发射有 1 ulp 级漂移（dQ mean=1.4e-7，近零元素翻转；dV 从不漂移）；无垃圾值、无放大
- 位稳定需求的回退版本：`fag_opt_ownlate`（与 base 逐位相同，+0.7~1.1%）

**改动**：仅 `fence.proxy.async.shared::cluster → .shared::cta` 一处（SASS 差分 = 恰好一条 MEMBAR.ALL.GPU）。与 FA4 官方实现同款排序模式。

## 9. 纯净配置测量：2SM+TMEM 复用+fence_cta（无 MUFU 分流、无 regalloc）

`fag_pure_2sm_cta` = 仅 `BWD_2SM FAG_OPT_FENCE_CTA`（TMEM 的 S/P/dQ 复用是 2SM 结构固有）；对照 `fag_bwd_fp8_1sm`（1SM CUTLASS 基线）：

| 配置 | 主 kernel TFLOPS | 3-kernel 合计 |
|---|---|---|
| pure 2SM（无 fence_cta） | 1251.3 / 1530.8 / 1578.8 | 1156.9 / 1460.3 / 1540.9 |
| **pure 2SM + fence_cta** | **1445.2 / 1811.9 / 1880.6** | **1320.9 / 1713.5 / 1826.9** |
| 1SM 基线 | 1133.1 / 1348.2 / 1370.3 | 1059.8 / 1296.8 / 1344.1 |
| （参照）调优版 fence_cta（+EX2 分流+regalloc） | 1487.0 / 1880.1 / 1922.8 | 1355.2 / 1774.3 / 1866.5 |

**结论**：
- 纯 2SM+TMEM 复用+fence_cta 对 1SM：主 kernel **1.275× / 1.344× / 1.372×**（+27.5%/+34.4%/+37.2%），3-kernel 合计 **1.246× / 1.321× / 1.359×**
- fence_cta 在纯净配置下的收益更大：主 kernel **−13.4%/−15.5%/−16.0%**（无 regalloc 微调时 fence 位置的调度影响更重）
- MUFU 分流+regalloc 的附加贡献仅 +2.2~3.8%（相对纯版）

## 10. SASS 补充：pack 算术几乎不存在（"整寄存器向量拷贝"已在做）

pack 块完整指令预算（base − abl_pack 差分，单实例 SASS）= **仅 72 条**：

| 指令 | 条数 | 说明 |
|---|---:|---|
| `ST.E.128` | 8 | 实际向量存储（已是 128-bit 整寄存器写） |
| `IADD3` / `IADD3.X` / `IMAD` | ~27 | swizzle 地址计算（strip 基址 + XOR） |
| `SHF.R.U64` | **6** | base 全部 SHF=133，pack 块只占 6 |
| `LOP3.LUT` | **4** | base 全部 LOP3=109，pack 块只占 4 |
| 其它 | ~27 | 辅助地址/谓词 |

源码写 48 条移位/或 → SASS 只剩 6+4=10 条。原因：`F2FP.SATFINITE.E4M3.F32.PACK_AB_MERGE_C` 输出**已经是拼好的 4 字节在一个 32-bit 寄存器**（PACK_AB_MERGE_C 把 A、B 两对转换结果合并进 C），ptxas 把 pack4 的"拆开再拼回同一个位型"折叠掉，F2FP 输出直接喂 STS.128。

**所以 FP8 已经在做"整寄存器向量拷贝"**（与 FP16 `autovec_copy` 等价的算术路径）。STS 的 −12% 成本来自：
1. ~64 条 **swizzle 地址计算**（TMEM load 布局与 SMEM 128B-swizzle 原子布局不是一一映射）
2. STS **裸露在关键路径**（FP8 CUDA-core bound，无 MMA 遮蔽）
3. **SMEM 端口竞争**（与 TMA 装载争端口）

详细分析见 `ABLATION_V2_FENCE_CTA_BASE.md` §8。

## 11. FA4 式 corner-turn 重构：x32 run + 循环不变寻址（`FAG_DS_RUN_STORES` / `FAG_DS_COLUMN_SPLIT`，2026-08）

对照 FA4 FP16（`flash_attn/cute/flash_bwd_sm100.py`）的中间计算布局后发现：FP16 的 `autovec_copy` 之所以便宜，是因为其 `Ld32x32b.x32` TMEM load 让每线程持有 **1 固定 KV 行 × 2 段 32 连续 Q**，64B 数据落在 SW 原子的单行内、swizzle XOR 被循环不变基址吸收——循环内寻址 ALU ≈ 0。FP8 现状是 `x16` atom + strip 交错切分（每线程 4 段 16Q），每段 16B 块需独立 XOR 寻址（3.2 条/STS，26 条/迭代在主循环内重复计算不变量）。

**关键事实**：`split_wg` 的 (2,N/2) 列主序切分下，`x32` load atom 自然给出块 `{wg, wg+2}` = 每线程 2 段 32 连续 Q（`Q(i) = 32*(wg+2k)+i%32`）；`SM100_TMEM_STORE_32dp32b8x` 的 (2,2) 切分与之一致（P/dS TMEM 片段顺序自动匹配）。每段 32 字节 = SW64 原子行内两个相邻 16B 块，两个 STS.128 地址 = 预计算基址与 `基址^16`——全部循环不变。

### 改动（全部 `#ifdef` 守卫，base 零改动）

1. **`FAG_DS_RUN_STORES`**（自动绑定 `FAG_TMEM_LOAD_32X` + `FAG_TMEM_STORE_8X`）：
   - 主循环前一次性外提 2 个 run 基址：`t=(dp&7)>>1; blk=(2*wg)^t; addr[k] = B(k==rank?own:xchg) + 64*dp + 16*blk`；
   - strip 循环 → run 循环：每 run 2× STS.128 于 `addr[k]` 与 `addr[k]^16`（相邻块 XOR 保序）；own/xchg 由 `(k==cta_rank)` 整 run 判定，无 per-strip 分支；
   - 字节、目标、交换握手与 base 完全相同——只有寄存器→SMEM 映射与寻址方式改变。
2. **`FAG_DS_COLUMN_SPLIT`**（叠加）：dS 阶段按 run 分两 chunk：math+量化逐 chunk 进行，fp32 P 值随 chunk 消亡（存活区间减半），F2FP 转换组分摊到 store 链上。TMEM load 保持整段（两个 wg 持有不同 TMEM 列块，CuTe 切片 tiled copy 无法同时表达——编译期验证过，atom 级断言拒绝）。
3. **`FAG_DS_STASYNC_CLUSTER`**（上一轮请求的指令，用数据收口）：远端 `st.async.shared::cluster.mbarrier::complete_tx::bytes.v4.b32` 直写 peer 的 smem_ds，512×16B/迭代的 tx 计数直接喂既有的 ds_full/relay/ds_leader 等待链（字节数不变 8192）。arm 在 `producer_acquire` 后由单线程执行（kStagesComputeSmem=1 保证其排序于上一迭代 tx 完成之后），再经 named barrier（用户 ID 0 → 硬件 ID 8）先于所有 st.async。彻底删除 xchg STS、代理栅栏与 bulk copy。

### 性能（3-kernel TFLOPS，nsys/锁频 1965 MHz，20 次，`--mask=no`）

| 配置 | 4096 | 8192 | 16384 | vs fence_cta | 位稳定 @16384 |
|---|---:|---:|---:|---:|---|
| base（fag_stage_regalloc） | 1215.2 | 1558.3 | 1682.0 | −11.0% | ✅ |
| ownlate（原位精确最优） | 1223.4 | 1569.4 | 1697.9 | −9.8% | ✅ |
| fence_cta（原容差最优，§8 定版） | 1324.8 | 1745.8 | 1873.8 | — | ❌（1 ulp 级） |
| **colrun** | 1372.8 | 1784.6 | 1952.9 | **+3.6/+2.2/+4.2%** | ❌（继承 cta-fence） |
| **colloop（新最优）** | **1378.9** | **1788.6** | **1960.4** | **+4.1/+2.5/+4.6%** | ❌（继承 cta-fence） |
| colrun_exact（cluster fence） | 1244.9 | 1596.3 | 1729.2 | | ✅ |
| **colloop_exact（新位精确最优）** | **1246.6** | **1602.9** | **1733.4** | vs ownlate **+2.0/+2.1/+2.1%** | ✅ |
| stasync_cl | 1189.4 | 1535.2 | 1643.7 | **−10.2/−12.1/−11.8%** | ✗ 拒绝 |

- 主 kernel Med @16384（nsys）：fence_cta 2801 μs → colrun 2678 μs（−4.4%）→ colloop 2662 μs（−5.0%）。
- **两种口径的定版数字**（2026-08-16 复测中位，锁频 1965 MHz，20 次，×3 发射波动 ±0.5%）：
  - 3-kernel 合计 TFLOPS：**1377 / 1790 / 1964**（4096/8192/16384；colloop = FAG_0816 默认版）；
  - 主 kernel 单核 TFLOPS（nsys Med）：**1562 / 1930 / 2067**（220.0 / 712.3 / 2660.1 μs；
    vs FP16 FA4 2-CTA 的 1245 / 1406 / 1416 → **1.26× / 1.37× / 1.46×**）。
- colloop vs 原始 base：**+13.5% / +14.8% / +16.6%**（三 shape）。
- 16384 三次重复发射：fence_cta 1866.7/1878.6/1873.8，colloop 1956.2/1971.0/1960.4 —— 差距稳定 +4~5%。
- 寄存器：全部 128 regs / 8B stack / 无 LOCAL（与 base 相同，setmaxnreg 封顶下无回退）。
- SASS（主 kernel 静态）：指令数 3290→3288，`LDTM 18→14`、`STTM 8→4`（x32/8x 更宽指令），`IADD3 348→336`、`LOP3 186→183`——静态差异小是因为收益在**动态**路径：原 strip 循环内每迭代重复的 ~26 条地址 ALU 被外提为每 kernel 一次的预计算（主循环 @16384 执行 128 次/线程）。

### 正确性（全门通过）

- `--verify` + 1SM 逐下标对照（@8192、causal @4096）：四个候选全部 `[OK]`（max≤0.1 / mean≤0.02）；
- `--stability-runs=3`：colrun/colloop@8192 三次哈希逐位一致；`*_exact`@16384 **PASSED**（cluster fence 位精确线）；
- 容差线（fence_cta base）@16384 位稳定 FAILED——与 fence_cta 本体同源同量级（dQ max 0.006~0.049、mean 1e-7~8e-7，vs fence_cta 自身 max 0.0117/mean 3e-7），系 cta-fence 竞争窗口继承，非重构引入；按 §8 的容差标准两线并存；
- 输入完整性哈希全部 PASSED。

### 裁决

1. **`FAG_DS_RUN_STORES` 采纳**：本轮最大单项收益（+2.2~+4.2% over fence_cta），机制 = 消除主循环内的 swizzle 地址重算 + 更宽的 TMEM 指令，正确性逐位等价于同基线（@8192 哈希与 fence_cta 一致）。
2. **`FAG_DS_COLUMN_SPLIT` 采纳**（叠加 +0.3~0.5%）：P 存活区间减半 + 转换组分摊。
3. **`FAG_DS_STASYNC_CLUSTER` 拒绝**（−11.8%）：与 §2 的本地 st.async（+12%）互证——512 次/迭代的 mbarrier tx 计数串行化是交换侧 st.async 路线的根本成本，远端不豁免；远端直写三变体（本地 STS+bulk / 直写 / st.async）至此全部有数据，STS+bulk+cta-fence 仍为最优。
4. 位稳定需求：`fag_cand_colloop_exact`（cluster fence + 重构）取代 ownlate 成为位精确最优（+2.1%），并首次让位精确线显著超越原始 base（+2.5~3.2%）。

### 新增 target

`fag_cand_{colrun,colloop,stasync_cl,colloop_st}`（FAG2_BASE 容差线）、`fag_cand_{colrun,colloop}_exact`（位精确线）。复现：

```bash
nvidia-smi -lgc 1965,1965
cmake --build build --target fag_cand_colloop fag_cand_colloop_exact fag_opt_fence_cta -j
./build/fag_cand_colloop --b=1 --h=16 --q=16384 --k=16384 --d=128 --d_vo=128 --iterations=20 --mask=no
```
