# 指令级消融实验：FP8 FAG vs FP16 FA4 Backward 瓶颈定位（Limiting Study）

对 FP8（FAG，2-SM + MUFU 分流最优版）与 FP16（FA4 CuTe-DSL，默认 2-CTA）的 backward 主 kernel 做**逐组件、细化到指令类别**的消融：消除每个 MMA、corner 交换与其同步等待、pack 打包、FP32→FP8 量化、exp2、softmax 微分、TMEM load/store、dQ reduce 等，共 **27 个 FP8 配置 + 10 个 FP16 配置 × 3 个 shape = 111 份 nsys profile**，并用 **SASS 指令差分**验证每次消融"只删了想删的指令"。

- **测量口径**：nsys `cuda_gpu_kern_sum` Med（中位数），GPU 锁频 1965 MHz，每配置 20 次计时；shape `(1,16,{4096,8192,16384},128)`，full mask。
- **FLOP**：`10·b·h·L²·d`。"主 kernel" = backward 主体；"3-kernel 合计" = 主 + 2 辅助（FP8: sumOdO+convert；FP16: preprocess+postprocess）。
- **采集日期**：2026-08-15，B200 (sm_100a, 148 SM)，CUDA 13.3。

## 1. 实验设计：如何保证"只消除目标部分"

深度流水线 kernel 的消融有两个陷阱：(a) 删掉一个组件后，**依赖它的上游计算被编译器 DCE**；(b) 删掉 mbarrier/TMA 字节期望会**死锁**。本实验的 guard 设计：

1. **保留全部 pipeline 握手**：所有 `producer_acquire/commit`、`consumer_wait/release`、named barrier、fence 原样保留，只包裹数据计算本身 → 无死锁。
2. **上游 liveness 用零成本 `asm volatile("" : "+f"/"+r")` 标记**：被消融组件的输入寄存器以 read-write 内联汇编钉住，编译器无法删除产生它们的上游指令。
3. **SASS 差分验证**（第 6 节）：对每个消融二进制统计 `MUFU.EX2 / F2FP.e4m3 / UTCQMMA.2CTA` 计数，确认未消融的类别一条不少。
4. **base 路径零影响**：所有 guard 均为 `#ifdef`/环境变量分支；改动后重编的 base 与改动前 base 计时一致（±1%，1228 vs 1212 / 1689 vs 1678 TFLOPS E2E @4096/16384）。
5. 消融版本**故意破坏数值正确性**（读旧值/垃圾值），仅用于计时——与既有 `ABLATION_COMPONENTS_FP8.md` 的 limiting-study 约定一致。

## 2. FP8 消融矩阵（FAG 2-SM + EX2_STRIDE=4 + regalloc）

### 2.1 主 kernel TFLOPS（Med）

| 消融对象 | 移除的指令 | 4096 | 8192 | 16384 | Δt@16384 |
|---|---|---:|---:|---:|---:|
| **base** | — | 1351.5 | 1657.9 | 1727.7 | — |
| MMA-S（Q@Kᵀ） | UTCQMMA×11 | 1375.5 | 1675.9 | 1749.2 | −1.2% |
| MMA-dP（dO@Vᵀ） | UTCQMMA | 1388.4 | 1687.0 | 1766.3 | −2.2% |
| MMA-dV（Pᵀ@dO） | UTCQMMA | 1377.5 | 1677.1 | 1753.9 | −1.5% |
| MMA-dK（dSᵀ@Q） | UTCQMMA | 1315.7 | 1607.3 | 1662.0 | **+4.0%**⚠ |
| MMA-dQ（dS@K） | UTCQMMA | 1367.0 | 1662.0 | 1741.3 | −0.8% |
| **全部 5 个 MMA** | UTCQMMA×176 | 1400.3 | 1680.5 | 1736.5 | **−0.5%** |
| S TMEM load | LDTM | 1242.8 | 1517.5 | 1566.6 | **+10.3%**⚠ |
| dP TMEM load | LDTM | 1168.9 | 1422.5 | 1466.4 | **+17.8%**⚠ |
| P TMEM store | STTM | 1264.1 | 1546.7 | 1604.8 | **+7.7%**⚠ |
| dS TMEM store | STTM | 1361.4 | 1665.8 | 1742.3 | −0.8% |
| scale·S−LSE FMA | FFMA2×32 | 1369.2 | 1686.2 | 1763.6 | −2.0% |
| exp2（MUFU 75%+软仿 25%） | MUFU×48+多项式 | 1342.7 | 1642.6 | 1731.4 | −0.2% |
| **softmax 微分 (dP+sum)·P** | FADD2/FMUL2×32 | 1441.3 | 1772.2 | 1835.6 | −5.9% |
| **FP32→e4m3 量化站点整链†** | F2FP+FMUL.FTZ+死链 | 1542.5 | 1875.3 | 1975.2 | **−12.5%** |
| †严谨重测：仅删转换器（v3 仪器） | F2FP 链 | 1331.5 | 1637.1 | 1702.4 | **+1.5%（≈0，见第 10 节）** |
| **corner：peer 半 pack+DSMEM 交换** | bulk copy+pack | 1584.6 | 1961.5 | 2101.1 | **−17.8%** |
| **pack：全部 pack4+SMEM 存储** | SHF/OR+ST.E.128 | 1651.4 | 2013.7 | 2165.5 | **−20.2%** |
| corner 同步等待（ds_leader） | SYNCS×2 | 1351.6 | 1657.2 | 1727.0 | −0.0% |
| dQ reduce（TMEM→SMEM→TMA） | LDS/ST.E.128 | 1450.8 | 1770.5 | 1839.9 | −6.1% |
| compute_all（pack+scale_lse+softmax+quant） | | 1752.5 | 2107.3 | 2326.4 | −25.7% |
| **floor（MMA+全部数据通路）** | | 2414.5 | 2858.6 | 3206.5 | −46.1% |
| **pack+quant 组合** | | 1757.6 | 2104.3 | 2325.1 | −25.7% |
| pack+quant+softmax 组合 | | 1758.2 | 2096.4 | 2321.2 | −25.6% |
| pq_reduce（pack+quant+reduce） | | 2003.3 | 2384.2 | 2645.0 | −34.7% |
| compute_reduce（pack+quant+softmax+scale_lse+reduce） | | 2002.5 | 2376.6 | 2642.9 | −34.6% |
| compute_mma（compute_all+全部 MMA） | | 2065.3 | 2400.1 | 2665.0 | −35.2% |
| **compute_reduce_mma（上述全部）** | | 2437.0 | 2852.5 | 3240.8 | −46.7% |

### 2.2 3-kernel 合计 TFLOPS（主 + sumOdO + convert）

| 消融 | 4096 | 8192 | 16384 |
|---|---:|---:|---:|
| base | 1241.5 | 1575.3 | 1682.4 |
| quant | 1401.9 | 1770.4 | 1916.3 |
| softmax | 1316.8 | 1678.0 | 1784.4 |
| corner | 1435.9 | 1846.4 | 2034.5 |
| **pack** | 1490.7 | 1892.7 | 2094.5 |
| reduce | 1322.0 | 1670.4 | 1788.8 |
| mma_all | 1283.5 | 1595.9 | 1690.9 |
| **pack_quant** | **1577.4** | **1973.1** | **2243.8** |
| compute_all | 1573.5 | 1975.7 | 2245.1 |
| pq_reduce | 1766.9 | **2206.1** | 2540.5 |
| compute_reduce | 1766.9 | 2199.4 | 2538.5 |
| compute_mma | 1821.5 | 2230.6 | 2559.0 |
| compute_reduce_mma | **2095.9** | **2601.4** | **3085.1** |
| floor | 2080.5 | 2605.9 | 3053.7 |

> 辅助 kernel 时间在全部消融下不变（±3%，例：16384 时 sumOdO 41.1–41.6 µs、convert 44.2–44.7 µs），证明消融只作用于主 kernel。

## 3. FP16 消融矩阵（FA4 CuTe-DSL 2-CTA，torch.float16）

### 3.1 主 kernel TFLOPS（Med）

| 消融对象 | 4096 | 8192 | 16384 | Δt@16384 |
|---|---:|---:|---:|---:|
| **base** | 1246.8 | 1412.0 | 1398.8 | — |
| MMA-S | 1314.3 | 1540.6 | 1536.4 | −9.0% |
| MMA-dP | 1254.8 | 1521.1 | 1549.3 | −9.7% |
| MMA-dV | 1269.8 | 1511.1 | 1520.4 | −8.0% |
| MMA-dK | 1286.7 | 1516.0 | 1524.5 | −8.2% |
| MMA-dQ | 1291.0 | 1527.7 | 1517.2 | −7.8% |
| **全部 5 个 MMA** | 1444.6 | 1740.3 | 1788.7 | **−21.8%** |
| corner（DSMEM 交换） | 1330.2 | 1508.3 | 1518.5 | −7.9% |
| exp2 | 1221.6 | 1397.9 | 1334.0 | **+4.9%**⚠ |
| softmax 微分 | 1281.2 | 1442.8 | 1415.9 | −1.2% |

### 3.2 3-kernel 合计 TFLOPS

| 消融 | 4096 | 8192 | 16384 |
|---|---:|---:|---:|
| base | 1144.5 | 1346.6 | 1367.7 |
| mma_s / mma_dp / mma_dv / mma_dk / mma_dq | 1200.5/1151.5/1164.3/1178.3/1181.6 | 1465.0/1447.8/1440.9/1443.2/1453.9 | 1500.7/1513.2/1485.9/1489.3/1483.2 |
| mma_all | 1312.2 | 1647.1 | 1742.3 |
| corner | 1214.5 | 1433.3 | 1481.8 |
| exp2 | 1123.1 | 1334.4 | 1305.4 |
| softmax | 1173.0 | 1374.8 | 1383.4 |

## 4. 核心结论：FP8 与 FP16 的瓶颈完全不同

### 4.1 FP16 是 Tensor-Core bound，FP8 是 CUDA-Core（转换/打包）bound

| 口径@16384 主 kernel | FP8 | FP16 |
|---|---:|---:|
| 删除全部 5 个 MMA 的收益 | **−0.5%**（完全隐藏） | **−21.8%**（最大单项） |
| 单个 MMA 的收益 | −0.8%~−2.2% | −7.8%~−9.7% |
| 最大非 MMA 单项 | pack **−20.2%** | corner −7.9% |

FP8 的 5 个 UMMA 在当前配置下**几乎全部被 compute warpgroup 的转换工作掩盖**（mma_all 仅 −0.5%~−3.5%）；主 kernel 的时间由 **CUDA core 上的 FP8 数据通路**决定。FP16 正相反：5 个 MMA 串行占据 tensor core，每个删掉都省 ~8-10%，是典型的 tensor-core-throughput 受限（FP16 base 主 kernel 1398.8，mma_all 后 1788.7）。

### 4.2 FP8 的瓶颈排序（@16384，主 kernel 时间降幅；经第 10 节勘误修订）

1. **pack4 打包 + SMEM corner turn：−20.2%**（2094.5 → 全 kernel 合计）。其中判别分解：`fence.proxy.async.shared::cluster` 代理栅栏 −9.8%、DSMEM bulk copy+tx −4.4%、xchg 半区 STS −4.1%（见 `PACK_OPTIMIZATION_FP8.md`）；pack4 移位/或算术本身被 ptxas 折叠、免费。这是 FP8 独有税——FP16 的 dS 是 2 字节，直接向量化拷贝。
2. **corner 交换（peer 半 pack + DSMEM bulk copy）：−17.8%**。其中**同步等待本身免费**（cwait −0.0%）：瓶颈是数据搬运与打包，不是 barrier。
3. **量化站点整链：−12.5%（勘误后的归因）**。严谨重测（第 10 节 v3 仪器）证明 **F2FP.SATFINITE.E4M3 转换器本身 ≈0%（完全被隐藏）**；−12.5% 实为该站点的**整条 FP32 生产链**（scale·S−LSE FMA −2.0% + softmax 微分 −5.9% + exp2 ≈0 + 转换器输入范围缩放 `FMUL.FTZ`×196 等）的联合成本——旧消融因空 asm 钉活失效、链被连带 DCE 而误归因于转换器。
4. softmax 微分 −5.9%、dQ reduce −6.1%（两侧相近，非 FP8 特有）。
5. exp2 ≈0%（3:1 MUFU/软件分流已在局部最优，与 `FP8_BWD_2SM_MICROARCH_ROOFLINE.md` 微基准一致）。

### 4.3 两侧共同/相反的现象

- **exp2 消融两侧都无收益甚至变慢**（FP8 −0.2%~+0.9%，FP16 +1.0%~+4.9%）：MUFU.EX2 与 FMA 管线的调度平衡被破坏。解释为"被完全隐藏"，不是优化方向。
- **TMEM load/store 消融（FP8 sload/dpload/pstore）反而变慢 +7~18%**：这些异步拷贝承载着调度重叠（其 scoreboard 延迟为其它指令让出发射窗口）；删除后 ptxas 重排代码（SASS 反而多 72–96 条），节奏被打乱。同为"被完全隐藏的组件"。
- **删 dK MMA（FP8）变慢 +4.0%**：dK 与 compute 的 dS 生产重叠，删除打破 MMA↔compute 节奏（与旧报告一致）。
- **FP16 有寄存器 spill**：base 直方图含 `MOV.SPILL`，mma_all 后消失 30 条（寄存器压力来自 MMA 驱动）；FP8 无 spill。

## 5. 深挖：为什么 FP8 的 pack4 这么贵、FP16 为什么不用 pack

### 5.1 pack4 在做什么

dS 量化成 e4m3 后，每线程的字节必须凑成 `uint4` 才能一条 `STS.128` 写 SMEM（`sm100_fmha_bwd_kernel_tma_2sm_warpspecialized.hpp` 主循环内）：

```cpp
auto pack4 = [&](int i) {            // 4 个字节 → 1 个 uint32
  return uint32_t(tTR_rDST(i+0).storage) |
         (uint32_t(tTR_rDST(i+1).storage) << 8) |
         (uint32_t(tTR_rDST(i+2).storage) << 16) |
         (uint32_t(tTR_rDST(i+3).storage) << 24);   // 3×SHF + 3×OR
};
uint4 lane_q16 = { pack4(src), pack4(src+4), pack4(src+8), pack4(src+12) };  // 12 条标量/16B
```

耗时原因——**经判别消融与全量 SASS 审计修正后的指令级归因**（@16384，128 个 K-tile 迭代，主 kernel 3179.3 µs；每项均通过"计算/TMA/TMEM/数据装载类逐一相同"的反 DCE 审计，见 6.1 节）：

| 指令类 | 条数/迭代 | 时间成本 | 占比 | ns/迭代 |
|---|---|---:|---:|---:|
| **`fence.proxy.async.shared::cluster`**（leader 线程） | **1 条** | **−371.4 µs** | **−11.7%** | **2902**（≈5700 周期/条） |
| `STS.128` own 半区（+其 pack 算术，已被折叠） | 2 条/线程 | −149.8 µs | −4.7% | 1170 |
| `STS.128` xchg 半区 | 2 条/线程 | −141.4 µs | −4.4% | 1104 |
| `cp.async.bulk` 8KB + `arrive_and_expect_tx` | 1+1 条 | −82.9 µs | −2.6% | 648 |
| `ds_leader` 同步等待 | 2 条 | ≈0 | +0.1% | 0 |
| pack4 移位/或算术（SHF/LOP3） | ~48 条/线程 | **≈0** | — | 0 |
| **合计（pack 全消融）** | | **−632.8 µs** | **−19.9%** | 4943 |

四层解读（按修正后的重要性）：

1. **大头是代理栅栏，不是拼装算术**。`fence.proxy.async.shared::cluster` 一条指令/迭代却占 −11.7%（≈5700 周期）：它是 generic→async 代理切换，语义上要求 CTA 的 SMEM 写管线**排空静止**后异步引擎才能读 STS 的结果——成本是管线级 quiesce，不是指令延迟。pack4 的 12 条移位/或反而**免费**：`FAG_DS_RECAST_PACK`（recast 直取位型）实测中性，证明 ptxas 已把 pack4 折叠进 `F2FP` 转换器的输出寄存器——"拆包→重包"的表象指令在最终 SASS 里并不存在。
2. **两份 STS.128（own+xchg，各 2 条/线程/迭代）合计 −9.1%**：SMEM 写发射与带宽，与持续到达的 TMA 装载争端口。ownstore/corner 直接测量与差分（pack−corner）不完全可加（−149.8 vs −77.2 µs），属深度流水线消融的固有交互。
3. **bulk copy+tx 只有 −2.6%**：8KB DSMEM 搬运本身便宜（异步引擎）；`cwait` 证明等待本身免费（延迟被流水线盖住）。
4. **为什么必须 pack**：dQ 的 2-CTA UMMA 要求 A 操作数（dS）在 SMEM 满足 descriptor 的 128B-swizzle 原子布局（`SmemLayoutDSHalf = tile_to_shape(SmemLayoutAtomA, Shape<_64,_128>)`），即"同一 KV 坐标的 4 个相邻 Q 字节"必须物理连续、32-bit 对齐；而 TMEM load 给每 lane 的是"固定 KV 列 × 4 段 16 行 Q"——这 4 个字节**散在不同寄存器**，必须 gather。不 pack 则退化为 4 条单字节 store + SMEM bank conflict（kernel 注释原话）。

### 5.2 FP16 为什么不用 pack

FP16 的 corner turn（`flash_bwd_sm100.py` compute_loop）：

```python
utils.cvt_f16(tdPrdP_cur, tdPrdS_cvt)          # F2FP.F16.F32.PACK_AB → half2
cute.autovec_copy(tdPrdS_cvt, tRS_sdS[...])    # 寄存器 → SMEM，整寄存器搬
cute.autovec_copy(tdPrdS_cvt, tdPrdS_xchg)     # 交换半区先留寄存器
# 迭代末：dump 到 sdS_xchg → 一条 bulk DSMEM copy 发 peer
```

本质差别是**元素宽度 2B vs 1B**：

- **2 字节天然自对齐**：`F2FP.F16.F32.PACK_AB` 原生输出 packed half2——一个 32 位寄存器 = 恰好 2 个相邻元素 = SMEM 一个完整 word，**转换器输出形态与 SMEM 原子布局直接重合**，"拼包"免费。FP8 要 4 个字节凑一个 word，而转换器给好的 packed 形态又被 Array 逐元素语义拆散，才需手工 gather。
- **布局映射交给 copy atom**：`autovec_copy` 使寄存器 fragment 与 SMEM slice 一一对应，16-bit 元素的重排退化为整寄存器 MOV + `STS.128`。
- **SASS 证据**：FP16 corner 消融仅 −80 条（`IMAD.MOV −20、MOV −13、STS.128 −4、SHF 仅 −3`）、时间 −7.9%；FP8 pack 消融 −72 条（STS/SHF/IADD 等）但时间 −19.9%（审计轮）。
- **字节量反证**：两边都做同结构的 2-CTA dS 半区交换（FP16 也是 exchange stage → 寄存器 → sdS_xchg → bulk copy）。FP16 每迭代交换 **16KB**/CTA（`tma_copy_bytes["dS"]//2`，64×128 元素 × 2B）是 FP8 **8KB** 的两倍，但 corner 消融只省 7.9% vs FP8 17.5%——**搬运带宽不是瓶颈**；FP8 的额外成本经判别定位为**代理栅栏（−11.7%）+ 字节成形触发的双份 STS/栅栏结构**，FP16 的 −7.9% 基本就是 bulk copy + barrier 本身。

一句话：**结构相同（都要 DSMEM 交换），差别在 FP8 的 1B 元素迫使 corner turn 走"逐字节 gather → STS → 代理栅栏 → bulk"的完整链，其中代理栅栏一项就占 −11.7%；FP16 的转换器输出即 SMEM 形态（整寄存器直拷），无成形税。** 这也解释了 2200 TFLOPS 的最小组合为什么是 pack+quant站点链（−25.7%，二者串行占据同一路径）。

## 6. SASS 指令归因（静态指令差分，主 kernel 函数）

### 6.1 消融"只删目标指令"的验证（FP8，全部 4 份 fatbin 实例合计）

**第一轮验证**（3 个哨兵 opcode）：

| 二进制 | MUFU.EX2 | F2FP.e4m3 | UTCQMMA.2CTA | 判定 |
|---|---:|---:|---:|---|
| base | 192 | 488 | 176 | — |
| abl_pack / corner / dsstore / reduce / sload / dpload | 192 ✓ | 488 ✓ | 176 ✓ | 干净 |
| abl_softmax | 192 ✓ | 488 ✓ | 176 ✓ | 干净 |
| abl_exp2 | 4（by design） | 488 ✓ | 176 ✓ | 干净 |
| abl_scale_lse | 4（by design） | 488 ✓ | 176 ✓ | 干净 |
| abl_quant | 4 ⚠ | 40 ⚠ | 176 ✓ | 见注/第 10 节 |
| abl_pstore | 192 ✓ | 360 ⚠ | 176 ✓ | 见注 |

**第二轮全量审计**（应第 10 节勘误要求重做：pack 家族 7 个消融全部从当前源码重建，做**完整 opcode 直方图差分**而非 3 个哨兵）：

| 语义类（4 实例合计） | base | pack | corner | bulk | xchgstore | fence | cwait | ownstore |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| MUFU.EX2（exp2） | 192 | 192 | 192 | 192 | 192 | 192 | 192 | 192 |
| F2FP.e4m3（转换器） | 488 | 488 | 488 | 488 | 488 | 488 | 488 | 488 |
| UTCQMMA.2CTA（MMA） | 176 | 176 | 176 | 176 | 176 | 176 | 176 | 176 |
| UTMALDG（TMA 装载） | 44 | 44 | 44 | 44 | 44 | 44 | 44 | 44 |
| LDS.128（LSE/dPsum 数据装载） | 128 | 128 | 128 | 128 | 128 | 128 | 128 | 128 |
| LDTM+STTM（TMEM 读写） | 94 | 94 | 94 | 94 | 94 | 94 | 94 | 94 |
| 全部 FP 算术类合计 | 904 | 903 | 903 | 903 | 905 | 903 | 902 | 905 |
| **ST.E（SMEM 存储）** | **128** | **96** | **112** | **128** | **112** | **128** | **128** | **112** |

结论：**所有计算/装载/TMEM 类逐一相同，零数据工作被 DCE**；ST.E 的减少与声明精确对应（corner −16 = 交换半区、ownstore −16 = own 半区、pack −32 = 两者、bulk/fence/cwait −0）。残差 ±2~6 条的 MOV/PRMT/标量 LDS 变化为寄存器分配洗牌（数据装载 LDS.128 与全部消费端计数不变，证明只是冗余装载被 CSE）。**pack 家族全部干净**。

> **注 1（quant）**：旧版 `FAG_ABLATE_QUANT` 的输入-liveness asm 未能保住 exp2/FMA 链（MUFU 192→4、FMUL.FTZ 196→0），即该消融 = 量化 + exp2 + scale-FMA。但 `abl_exp2`（−0.2%）与 `abl_scale_lse`（−2.0%）单独计时证明这两类≈免费，故 quant 的 −12.5% 仍归属量化链。
> **注 2（pstore）**：P 路 `F2FP` 被部分 DCE（−128/488）；其结果本来就是负增益（+7.7%，调度扰动），不影响任何瓶颈结论。

### 6.2 FP8 逐组件指令增量（base − ablated，单实例 SASS）

| 组件 | 指令增量（主要 opcode） |
|---|---|
| quant（量化链） | **−528**：F2FP.SATFINITE.E4M3 −128、FMUL.FTZ −65、FFMA2 −32、FADD2 −32、FMUL2 −32、LDS.128 −32、LDTM −16、（附带 exp2 −128） |
| exp2 | −128：MUFU.EX2 −48、FFMA2.FTZ −24、FADD2 −16、FMNMX −16、LEA −16（软件仿真多项式） |
| softmax | −88：FADD2 −32、FMUL2 −32、LDS.128 −16 |
| pack（含 SMEM 存） | −72：IADD3 −10、ST.E.128 −8、SHF.R.U64 −6、IMAD −8、LOP3/PLOP3 −8、LDS −4 |
| corner（peer 交换） | −16：地址/副本编排（bulk copy 为异步指令，静态数少） |
| reduce | −88：SHF.R.U64 −32、ST.E.128 −32、LDS −32、LOP3 −37、BAR −16 |
| scale_lse（FMA+exp2） | −176：含 exp2 的 −128 + FFMA2 −32 + LDS.128 −16 |
| mma_all | −168：UTCQMMA −44、R2UR −93、UMOV −67、UIADD3 −40、SYNCS −54、BRA −45（MMA 驱动开销 > MMA 本身） |

FP8 base 主 kernel（单实例）opcode 前几名：`BRA 239、UMOV 224、UIADD3 168、F2FP.SATFINITE.E4M3 138、R2UR 127、SYNCS…TRYWAIT 116、LOP3 109、MOV 102、LDS 70、FMUL.FTZ 65、UTCQMMA.2CTA 48、MUFU.EX2 48` —— **转换类（F2FP+FMUL）指令数超过 MMA 指令 4 倍以上**。

### 6.3 FP16 逐组件指令增量（cubin：`CUTE_DSL_KEEP=cubin` 导出）

| 组件 | 增量 | 主要 opcode |
|---|---:|---|
| mma_all | **−776** | UTCHMMA.2CTA −80、R2UR −205、UMOV −162、UIADD3 −116、MOV −94、**MOV.SPILL −30** |
| corner | −80 | IMAD.MOV −20、MOV −13、STS.128 −4、SHF −3 |
| exp2 | −56 | MUFU.EX2 −64 |
| softmax | −88 | FADD2 −32、FMUL2 −32、LDS.128 −16（与 FP8 结构相同） |

FP16 base：总 3200 条；`UMOV 324、UIADD3 249、R2UR 224、BRA 188、F2FP.F16.F32 128、UTCHMMA.2CTA 80、MUFU.EX2 64、FMUL2 64`。对比 FP8：**FP16 的转换只有 128 条 F2FP 且无辅助算术；FP8 的转换链 528 条**。这就是 quant 消融在 FP8 值 −12.5%、在 FP16 无对应大项的结构性原因。

## 7. 可达性：2200 TFLOPS 与「FP16 对应 shape 的 1.6×」

### 7.1 2200 TFLOPS 目标（3-kernel 合计，@16384）

base@16384 = **1682.4**，要过 2200 需再省 **−23.5%** 主 kernel 时间：

| 移除的操作集 | 3-kernel 合计@16384 | 达标? |
|---|---:|---|
| pack 单独 | 2094.5 | ✗ |
| corner 单独 | 2034.5 | ✗ |
| quant 单独 | 1916.3 | ✗ |
| **pack + quant** | **2243.8** | **✓（+2.0%）** |
| pack + quant + softmax | 2240.1 | ✓（饱和） |
| compute_all（pack+quant+softmax+scale_lse） | 2245.1 | ✓（饱和） |
| compute_reduce_mma / floor | 3085.1 / 3053.7 | 理论上限 |

**结论：必须同时消除两类 FP8 特有操作——① dS 的 SMEM corner turn（判别后主要是 `fence.proxy.async` 代理栅栏 −11.7% + 双份 STS −9.1%）、② 量化站点的 FP32 生产链（−12.5%；注意转换器本身 ≈0，见第 10 节勘误）——才能把全 kernel 推过 2200 TFLOPS**。两者收益近似可加（−19.9% 与 −12.5% → 合计 −25.7%），说明它们**串行**占据 compute warpgroup 的同一条关键路径，互不重叠；再叠加 softmax/scale_lse 只剩 ±2 TFLOPS 的空间（路径已饱和于 ~2325 主 kernel）。反之，任何不触碰这两类的组合（如删 MMA、删等待、删 exp2）最多只能到 ~1789。

现实优化方向的对应候选（非消融、保正确性）：`FAG_DS_RECAST_PACK`（免 shift/or 的 recast 打包）、`fag_candidate_direct_ds`（dS 直写 peer DSMEM，免 bulk copy）、以及把量化与 pack 融合为单条 `F2FP+PRMT` 路径——这些正是本实验指向的两个瓶颈点。

### 7.2 每个 shape 达到 FP16 对应性能 1.6×（经第 10/12 节勘误后用干净 guard 重做）

FP16 基线（3-kernel 合计）：1144.5 / 1346.6 / 1367.7 → **1.6× 目标：1831.2 / 2154.6 / 2188.3**。

> **重要勘误**：下表"旧组合"（pack_quant/pq_reduce/compute_* 等）全部含旧污染 quant guard——其"quant"成分实为**整条 FP32 生产链的 DCE 连带删除**（exp2/scale-FMA/softmax 等，见第 10 节）。这些链在现实 kernel 中**数学必需**（P/dS 必须计算），因此旧表的"最低充分集"是假象。下表用**干净 guard 重建**（pack_quantv3 = pack+仅转换器；pack_reduce / pack_red_soft 无任何污染成分），SASS 审计确认（§6.1 第二轮）。

| 操作集 | @4096 | @8192 | @16384 | 现实可优化？ |
|---|---:|---:|---:|---|
| base | 1238.2（1.082✗） | 1575.0（1.170✗） | 1683.8（1.231✗） | — |
| **pack（干净）** | 1485.5（1.298✗） | 1891.9（1.405✗） | 2088.1（1.527✗） | ✓（corner 方案重构） |
| pack+仅转换器（pack_quantv3，干净） | 1484.3（1.297✗） | 1891.7（1.405✗） | 2075.0（1.517✗） | — 与 pack 相同：**组合层面再次证明转换器 ≈0** |
| **pack+reduce（干净）** | 1632.1（1.426✗） | 2069.4（1.537✗） | **2378.2（1.739✓）** | ✓ |
| pack+reduce+softmax（干净；softmax 数学必需，仅作上限） | 1667.6（1.457✗） | 2124.3（1.578✗） | 2424.1（1.772✓） | ✗（参考上限） |
| floor（绝对上限，含删数学必需链） | 2080.5（1.818） | 2605.9（1.935） | 3053.7（2.233） | ✗ |
| （旧）pack_quant（污染，含链删除） | 1577.4（1.378） | 1973.1（1.465） | 2243.8（1.641） | 假象，仅存档 |

**修正后的结论**：

| shape | 现实可达 1.6×？ | 依据 |
|---|---|---|
| **16384** | **✓ 能**：优化 pack 机器（fence/STS/corner）+ 精简 dQ reduce → 1.739× | pack_reduce 干净组合实测 |
| **8192** | **✗ 不能（在不动数学必需链的前提下）**：干净可达上限 1.537×（pack+reduce）；即使再叠 softmax 删除（不现实）也只有 1.578×；距 1.6× 还差 ~4% | 干净阶梯 + 门槛 2154.6 |
| **4096** | **✗ 不能**：干净上限 1.457×，距 1.6× 差 ~10%；缺口由固定开销主导 | 见下条固定开销分析 |

固定开销的 shape 依赖（floor 主 kernel 时间 ÷ 工作单元数 = 每 (tile×K-iter) 的骨架成本）：

| shape | floor 主 kernel | 骨架 ns/单元 | 相对 16384 |
|---|---:|---:|---:|
| 4096 | 142.3 µs | **8.69** | **+33%** |
| 8192 | 480.8 µs | 7.34 | +12% |
| 16384 | 1714.5 µs | 6.54 | — |

4096 的额外固定开销来源：wave 量化（256 cluster / 74 slot ≈ 3.46 wave 的尾波浪费）、辅助 kernel 占比最高（~8%）、流水线建立/排空的摊薄不足——这解释了为什么 4096 即使删光可优化项也只到 1.457×，而其绝对 floor（1.818×）与 8192/16384 的差距远小于现实可达集的差距。

## 8. 消融实现清单

| 文件 | 改动（全部 `#ifdef` 守卫，base 路径零影响） |
|---|---|
| `FAG_FP32/kernel/sm100_fmha_bwd_kernel_tma_2sm_warpspecialized.hpp` | 新增 guard：`FAG_ABLATE_PACK`（整条 SMEM corner turn，quantize 输出用 `+r` asm 保活）、`FAG_ABLATE_CORNER_WAIT`（MMA 侧两处 `ds_leader.wait`，relay 链保留）、`FAG_ABLATE_REDUCE`（TMEM load/SMEM staging/TMA store，握手保留）、`FAG_ABLATE_S_LOAD`、`FAG_ABLATE_DP_LOAD`、`FAG_ABLATE_P_STORE`（asm 保活）、`FAG_ABLATE_DS_TMEM_STORE`、`FAG_ABLATE_SCALE_LSE`（FMA+exp2，acc/lse asm 保活） |
| `FAG_FP32/CMakeLists.txt` | 新增 17 个 target：`fag_abl_{sload,dpload,pstore,dsstore,scale_lse,pack,cwait,reduce,mma_all,compute_all,floor,pack_quant,pack_quant_softmax,compute_reduce,compute_mma,compute_reduce_mma,pq_reduce}`（全部基于最优配置 `BWD_2SM EX2=4 REGALLOC`） |
| `FAG/flash-attention/flash_attn/cute/flash_bwd_sm100.py` | `__init__` 读环境变量 `FAG_ABLATE_{MMA_S,MMA_DP,MMA_DV,MMA_DK,MMA_DQ,EXP2,SOFTMAX}`；mma helper 换 no-op（helper 无返回值/无分配器副作用）；exp2/softmax 两处 `const_expr` gate |
| `FAG/flash-attention/bench_bwd_perf_fp16.py` | fp16 bwd bench 脚本（沿用上轮） |

既有 guard 复用：`FAG_ABLATE_{S,DP,DV,DK,DQ,QUANT,EXP2,SOFTMAX,CORNER}`。

## 9. 复现

```bash
nvidia-smi -lgc 1965,1965
cd /home/ubuntu/workspace/oyhj/temp/test/test_merge/Benchmark/FAG_FP32
cmake --build build --target fag_stage_regalloc fag_abl_s fag_abl_dp fag_abl_dv fag_abl_dk \
  fag_abl_dq fag_abl_quant fag_abl_exp2 fag_abl_softmax fag_abl_corner fag_abl_sload \
  fag_abl_dpload fag_abl_pstore fag_abl_dsstore fag_abl_scale_lse fag_abl_pack fag_abl_cwait \
  fag_abl_reduce fag_abl_mma_all fag_abl_compute_all fag_abl_floor \
  fag_abl_pack_quant fag_abl_pack_quant_softmax fag_abl_compute_reduce \
  fag_abl_compute_mma fag_abl_compute_reduce_mma fag_abl_pq_reduce -j
for L in 4096 8192 16384; do
  for b in fag_stage_regalloc fag_abl_pack fag_abl_quant fag_abl_pack_quant ...; do
    nsys profile --force-overwrite=true -o /tmp/abl_fp8_<cfg>_$L ./build/$b \
      --b=1 --h=16 --q=$L --k=$L --d=128 --d_vo=128 --iterations=20 --mask=no
  done
done
# FP16 侧（env 触发，FAG/flash-attention 目录）
FAG_ABLATE_MMA_S=1 nsys profile -o /tmp/abl_fp16_mma_s_$L /usr/bin/python3 bench_bwd_perf_fp16.py $L 20
# 分析与 SASS
python3 /tmp/analyze_abl.py both        # 中位数 + TFLOPS + Δt
python3 /tmp/sass_count.py              # FP8 per-component 指令差分
python3 /tmp/sass_fp16.py               # FP16 cubin 指令差分
```

原始数据：`/tmp/abl_fp8_*.nsys-rep`（81 份）、`/tmp/abl_fp16_*.nsys-rep`（30 份）、汇总 `/tmp/abl_results.json`、SASS `/tmp/sass_results.txt`。

## 10. 勘误：quant 消融的严谨重做（三轮仪器迭代）

**问题**：原 `FAG_ABLATE_QUANT`（量化消融，报告为 −12.5%）的输入钉活用的是空 body 内联汇编 `asm volatile("" : "+f"(v))`。证据链证明它失效：主 kernel PTX 中 `ex2.approx` 两边都是 112（nvcc 前端保住了），但 SASS 中 `MUFU.EX2` 192→4、`FMUL.FTZ` 196→0（**ptxas 删掉了**）——空 asm 不发射任何 PTX 指令，量化输出换成零常量后整条 FP32 生产链在 ptxas 眼里变成死代码。即旧 −12.5% 实为「转换器 + 整条上游链」的联合成本，归因错误。

### 10.1 三轮仪器设计

| 仪器 | 设计 | SASS 隔离性 | 计时 | 判定 |
|---|---|---|---|---|
| 旧（空 asm） | 零常量输出 + `asm volatile("" : "+f")` | ✗ 链被 ptxas DCE | −12.5%（污染） | 弃用 |
| **v2**（不透明分支） | 零常量输出 + `if (blockIdx.x == gridDim.x) sink ^= xor(v)`（真实全局 RMW，编译器无法证明不可达） | ✓ 全链保留（MUFU 192/FMUL.FTZ 196/FFMA2 全在），仅 F2FP 488→40 | **+4.5~5.4%** | **仪器不合格**：分支未外提，热循环复制 ~150 条指令（LOP3 +125/STG +50/BRA +27），配对照（ctrl=分支+转换器都保留）测得钉活开销 +43~52% |
| **v3**（最小仪器，采纳） | 转换器换成 **3 条 XOR 位归约**消耗 4 个 FP32 输入，输出 union 位译为 e4m3——**真实数据流**（无可 DCE 之处）、**运行时值**（下游 pack/store 不折叠）、**零分支/零常量/零膨胀** | ✓ 全链保留（MUFU 192、FMUL.FTZ 196、FMUL2 128、UTCQMMA 176），仅 F2FP 488→40；新增 LOP3 +103/实例 = 设计的 3-XOR×32 组 | **+1.3~1.5%** | ✓ 采纳 |

### 10.2 修正后的结论

| 版本 | 主 kernel TFLOPS@16384 | Δt vs base | 含义 |
|---|---:|---:|---|
| base | 1727.7 | — | — |
| 旧污染 quant | 1975.2 | −12.5% | 「量化站点整链消失」的假设情形上限 |
| **v3 严谨** | 1702.4 | **+1.5%** | **F2FP 转换器本身 ≈0%——完全被隐藏**（+1.5% 中含 ~96 LOP3/迭代的仪器成本与调度扰动） |

1. **转换器不是瓶颈**。`F2FP.SATFINITE.E4M3` 及其范围缩放链与其它「被完全隐藏的组件」（TMEM load、dK MMA、exp2）同类：删除它们打破调度节奏反而变慢。
2. **−12.5% 的真实语义**：quant 站点的整条 FP32 生产链（scale·S−LSE −2.0% + softmax 微分 −5.9% + exp2 ≈0 + `FMUL.FTZ`×196 输入缩放等）。这条链在现实 kernel 中不可删除（P/dS 必须算），**现实可优化的只有其中的缩放辅助指令**（FP16 的 `F2FP.F16.F32` 无此负担）。
3. **对第 7 节 2200/1.6× 结论的影响**：`pack_quant` 组合实测 2243.8 的事实不变，但其 "quant" 成分的语义修正为「量化站点整链」（含不可删的 softmax/exp2）；**pack 仍是最大且唯一可独立攻关的现实杠杆**（ownlate 已回收其中 ~1%，fence/copy 的剩余 ~14% 见 `PACK_OPTIMIZATION_FP8.md` 的结构性分析）。
4. **方法学教训**（已沉淀到 guard 注释）：深度流水线消融的钉活必须（a）产生真实 PTX 指令、（b）不向热循环引入分支/膨胀、（c）让保留组件形成真实数据流消费；且**必须**做事后 SASS opcode 审计（本节正是被第 6.1 节的审计表抓出的）。

新增 target：`fag_abl_quant_v2`（分支钉活，仪器不合格，留档）、`fag_ctrl_quantpin`（配对对照，留档）、`fag_abl_quant_v3`（**严谨版，引用此数据**）。原始 profile：`/tmp/abl_fp8_{quant,quant_v2,quant_v3,ctrl_quantpin}_{4096,8192,16384}.nsys-rep`。

## 11. FP8 vs FP16：MMA/SMEM 减半之后暴露的新旧瓶颈（纯数据推导，@16384 主 kernel）

FP8 相对 FP16 的两个硬件事实：**MMA 吞吐 2×**（同规模 UMMA 时间减半）、**操作数字节减半**（1B vs 2B：TMA 装载与 dS 交换字节都减半）。这两项"减负"如何改变瓶颈结构，全部用消融数据回答：

### 11.1 同一消融在两边的对照

| 消融（同一指令类） | FP16 Δt（绝对） | FP8 Δt（绝对，审计轮） | 比值 |
|---|---:|---:|---:|
| 删全部 5 个 MMA | **−856.0 µs（−21.8%）** | −13.4 µs（−0.4%） | 64× |
| 删 corner 交换 | −309.8 µs（−7.9%），16KB/迭代 | **−555.6 µs（−17.5%）**，8KB/迭代 | 0.56× |
| 删 softmax 微分（两侧 SASS delta 相同：FADD2 −32、FMUL2 −32、LDS.128 −16） | −47.0 µs（−1.2%） | **−187.6 µs（−5.9%）** | 0.25× |
| 删 exp2 | +191.3 µs（**+4.9% 变慢**） | ≈0（−0.2%） | — |
| 量化站点 | 未消融（cvt_f16 仅 128 条 F2FP） | 站点链 −397 µs（−12.5%）；转换器本身 ≈0（§10 v3） | FP8 特有 |
| 删 dQ reduce | — | −194 µs（−6.1%） | — |

### 11.2 解释一：MMA 时间份额坍缩，隐藏关系整体翻转

- FP16 中 MMA 串行占 21.8%（856µs），是第一瓶颈；CUDA 侧工作（softmax 仅 1.2%）躲在 MMA 阴影下。
- FP8 中同样的 5 个 MMA 只占 0.4%——**MMA 反过来躲在 CUDA 工作阴影下**。最直接的证据是 softmax：**完全相同的 SASS 指令类**，在 FP16 里删掉只省 1.2%，在 FP8 里删掉省 5.9%——不是指令变了，是"谁遮蔽谁"翻转了。
- 旁证：删掉全部 MMA 后，FP8（3165.9 µs）反而比 FP16（3073.4 µs）**慢 3%**——FP8 的非 MMA 骨架（量化链 + corner 成形）本身比 FP16 重，即使 tensor core 全空闲也追不回。

### 11.3 解释二：字节减半没有兑现成带宽收益，反而引入成形税

- 输入侧：FP8 的 TMA 装载字节减半且完全无压力——删 TMEM load（sload/dpload）反而变慢 +10~18%，装载完全重叠。
- dS 交换侧：FP8 每迭代只交换 8KB（FP16 是 16KB），**字节少一半却贵一倍**（−17.5% vs −7.9%）。判别消融定位（§5.1）：FP8 的成本大头是 `fence.proxy.async` 代理栅栏（−11.7%）——1B 元素的 gather→STS→异步搬运链强制每迭代一次 generic→async 代理切换；FP16 的 2B 元素转换器输出即 SMEM 形态，无此结构。
- 结论：**FP8 的 SMEM 减半在输入侧兑现（无压力），在 dS 侧被字节成形/代理切换的结构性成本反超**。

### 11.4 新旧瓶颈清单与余量核算

| | FP16（旧世界） | FP8（新世界） |
|---|---|---|
| 第一瓶颈 | MMA issue/吞吐 −21.8% | corner/pack 链 −19.9%（**新**，其中代理栅栏 −11.7%） |
| 第二瓶颈 | corner −7.9%（16KB 搬运） | 量化站点 FP32 链 −12.5%（**新**；转换器本身 ≈0） |
| 被暴露的旧瓶颈 | — | softmax 1.2%→5.9%、reduce −6.1%（从 MMA 阴影下露出） |
| 寄存器 | 有 spill（MOV.SPILL，删 MMA 后 −30） | 无 spill（1B 元素省寄存器） |
| exp2 | 删除变慢 +4.9%（MUFU/FMA 平衡敏感） | ≈0（3:1 分流已最优） |

余量核算：FP8 base 3179.3 µs vs floor 1713.6 µs（−46%）→ 可回收 1465.7 µs；逐一相加 pack 632.8 + 量化链 397 + reduce 194 + softmax 188 + （scale_lse/exp2/dsstore 等零头）≈ 1412 µs，**与 floor 差距吻合（96%）**——消融清单基本完备。FP8 对 FP16 的实际加速 1.236×（1729.2/1398.8），而结构上限 2.29×（floor/FP16 base）；**未兑现的 1.85× 全部押在 corner/pack 与量化链这两个 FP8 特有瓶颈上**。

补充数据点：FP16 侧"全计算移除"（MMA×5+corner+exp2+softmax，环境变量组合）主 kernel = 185.2 / 610.8 / 2363.9 µs，vs FP8 floor 142.3 / 480.8 / 1714.5 µs。注意两者删除深度不同（FP16 版保留 load/store/reduce，FP8 floor 删得更深），只能方向性使用：**FP16 的骨架（含 2× 字节的装载/存储）本身就占其 base 的 ~60%，加上 21.8% 的 MMA，CUDA 计算类在 FP16 里被完全盖住**——这就是"为什么 FP16 没暴露这些开销"的骨架层证据。

## 12. 对抗验证与硬件层面"为何提不上去"

### 12.1 对抗实验：逐条反驳直到无法反驳

**R1「fence −11.7% 只是删代码带来的调度扰动（像 pstore/sload 那样）」→ 已反驳，无法再反驳。**
设计：`fag_adv_doublefence`——base 上**再加一条**相同的 `fence.proxy.async.shared::cluster`。若成本真实，应再加 ~10%；若是扰动，加倍应中性。实测（主 kernel）：

| 配置 | 4096 | 8192 | 16384 |
|---|---:|---:|---:|
| base（1 条 cluster fence） | 255.0 µs | 829.1 µs | 3179.3 µs |
| 删 fence（abl_fence，非法） | −9.8% | −11.2% | −11.7% |
| **2 条 fence（doublefence）** | **+19.0%** | **+21.4%** | **+23.0%** |
| **降级 cta 作用域（fence_cta，合法，全部门通过）** | **−9.4%** | **−11.8%** | **−10.1%** |

增益/代价随 fence 条数**线性**（1 条 ≈ −10~−12%，2 条 ≈ +19~+23% ≈ 2×），SASS 差分显示 fence 编码为 `MEMBAR` 类指令。调度扰动不会产生这种线性剂量响应。**结论修正：fence 成本真实且线性，但 ~10% 源自 cluster 作用域的过度排序——降级到 cta 作用域（FA4 同款）即合法回收；真墙只剩 ~1-2% 的代理切换残余。**

**R2「转换器可能在组合里仍有贡献」→ 已反驳。** `pack_quantv3`（pack+仅删转换器，v3 干净仪器）与 `pack` 单独在三 shape 比值完全相同（1.297/1.405/1.517 vs 1.298/1.405/1.527，差异 ≤0.7% 在噪声内）。组合层面独立复现了 v3 的"转换器 ≈0"。

**R3「FP16 骨架更重可能只是删除深度不同的伪影」→ 部分成立，降级为方向性证据。** FP16 allcompute 保留 load/store/reduce 而 FP8 floor 全删，不能做严格等价比较（已在 §11 标注 caveat）。可严格主张的只剩：FP16 删光可删计算后仍剩 2363.9 µs（16384），其中 2× 字节的装载/存储是主要成分——FP16 的限制从来不是 CUDA core。

**R4「ownstore/xchgstore 的 −4.7%/−4.4% 可能是地址计算而非 STS」→ 弱反驳后接受。** 二者 SASS delta 均以 ST.E.128 −4 为核心、地址运算为伴生（删 store 必然连带其地址链），成本对称（1170/1104 ns/迭代），无单向偏差迹象。不确定性 ±1%。

**R5「量化链 −12.5% 的归因仍有 ~4.6% 未定位（FMUL.FTZ×196 的归属）」→ 诚实保留为开放项。** 该 196 条标量 FMUL.FTZ 只在整链死亡时消失、在 v3（仅删转换器）中存活——说明它是链的上游生产者而非转换器内部；单独消融它需要先定位其源码来源，本轮未做。其成本被界定在 0 ~ −4.6% 之间，不影响排序结论（pack 632.8 µs > 链 397 µs ≥ ...）。

**R6「floor 可能还能更低，绝对上限被高估」→ 成立但不影响结论。** floor 仍含 TMA 装载、全部 barrier、epilogue 存储与 MMA 驱动骨架；更深的理论 floor 只会让"绝对上限"更高，不改变现实可达集（§7.2）的判定。

### 12.2 硬件层面：四堵墙（每堵都有消融数据背书）

1. **代理栅栏墙（−11.7%，真墙——R7 对抗实验两轮裁决）**：`fence.proxy.async.shared::cluster` 编译为 `MEMBAR` 类指令，剂量线性（R1）。第一轮对抗（R7a）：降级到 `.shared::cta` 在 4096/8192 拿回 ~10% 且全门通过，一度判定"作用域过保守"；第二轮对抗（R7b，应精度重验要求补 16384 稳定性）：**base 逐位稳定而 cta 版 3 次哈希漂移**——cluster 作用域是承载正确性的：PTX 的 fence 作用域应按异步操作的访问域（`cp.async.bulk.shared::cluster`）选取，而非"源地址在本地"。墙完整保留：**只要 dS 走 SMEM 集群交换，每迭代 ~2900ns 的集群作用域代理栅栏就是法律刚需**。ownlate（在飞 store 减半）只回收 ~1%，说明成本是集群冲刷的固定部分而非排空量。
2. **字节成形/双 STS 墙（−9.1%）**：1B 元素的 gather → 每线程每迭代 4×STS.128（own+xchg），与 TMA 入口争 SMEM 端口。根因是 TMEM 容量：免交换的 split-K dQ 需要每 CTA 128 行 dQ 累加器，TMEM 放不下（roofline 文档同结论）——**corner turn 是 TMEM 预算换来的结构性存在**。
3. **FP32 数学链墙（−12.5%，其中可优化 ≤4.6%）**：exp2/softmax/scale-FMA 数学必需且都在 CUDA core；FP8 的 MMA（−0.4%）全部躲在它后面。链中唯一可动的部分是转换器输入缩放（FMUL.FTZ，见 R5）。
4. **固定骨架墙（shape 相关）**：每工作单元 6.5~8.7 ns；4096 因波量化（3.46 wave）+辅助 kernel 占比高而 +33%——这是 4096 无法现实达到 1.6× 的直接原因（§7.2）。

四堵墙合计解释了 floor 差距的 96%（§11.4 余量核算），且每堵墙都有"删除后变慢/加倍后变贵"的对抗数据——**在当前 tile/ownership/TMEM 结构下，FP8 主 kernel 的可验证上限就是 ~2100（干净 pack+reduce 后）到 ~3200（绝对 floor）TFLOPS 之间，其中只有 pack 机器和 reduce 路径是现实可攻的**。
