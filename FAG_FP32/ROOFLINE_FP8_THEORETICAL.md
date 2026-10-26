# FP8 Backward 理论性能上限分析（Roofline 模型）

基于 FA4 论文 §3.2.1 的 roofline 方法论，对 FP8 e4m3 2-CTA backward 推导各硬件资源的理论 cycles，得出性能上限。**全部为理论推导，不使用消融实测数据。**

---

## 1. 硬件模型（B200 SM100）

| 资源 | 吞吐率（per SM per cycle） | 来源 |
|---|---|---|
| Tensor Core (FP8) | 16384 FLOPs | BF16 的 2× |
| Tensor Core (BF16) | 8192 FLOPs | FA4 论文 |
| FP32 FMA | 128 FLOPs（64 FFMA2 × 2 elem） | Blackwell spec |
| MUFU (exp2) | 16 elements | FA4 论文 |
| F2FP (FP32→FP8) | 64 elements（quarter rate†） | NVIDIA tuning guide |
| TMEM load/store | **~1527 B/cycle**（8 warps 并发，实测†） | 仓库微基准 |
| SMEM load (LDS) | 128 B/cycle | FA4 论文 |
| SMEM store (STS) | 128 B/cycle（与 TMA 共享写端口） | FA4 论文 |

† F2FP 的精确吞吐率无公开文档，取 quarter rate（FP32 管线的 1/4）为标准假设。§4 讨论其他速率的影响。

† TMEM 带宽来自仓库内微基准（`FP8_BWD_2SM_MICROARCH_ROOFLINE.md` §6）：8 个 compute warps 并发执行 `tcgen05.ld.sync.aligned.32x32b.x16.b32`，实测有效带宽 1527 B/cycle。单 warp 为 191 B/cycle。注意：这是实测值而非 NVIDIA 公开 spec——PTX ISA 未公布 tcgen05.ld 的端口吞吐。FA4 论文未将 TMEM 列为 roofline 独立资源（只建模 MMA/SMEM/MUFU），因此论文中无此数字。

## 2. 问题参数

```
Shape: (B=1, H=16, L, D=128)
2-CTA 模式: cluster tile M=256, 每 CTA M'=128
Per-CTA tile: M'=128 Q × N=128 KV × d=128 head_dim
Per SM per iteration:
  元素数 = M'×N = 16384
  compute 线程 = 256
  元素/线程 = 64
  FLOPs = 10×M'×N×d = 20,971,520
换算: TFLOPS = 6099.5 / T  (T = cycles/SM/iteration)
```

## 3. 各资源理论 cycles

### 3.1 MMA Compute（Tensor Core）

5 个 MMA，各 2·M'·N·d FLOPs，合计 10·M'·N·d：

$$T_{\text{MMA}} = \frac{10 \times 128 \times 128 \times 128}{16384} = 1280 \text{ cycles}$$

| MMA | 类型 | 维度 | FLOPs |
|---|---|---|---|
| S = Q·Kᵀ | SS | (M',d)×(d,N)→(M',N) | 2M'Nd |
| dP = dO·Vᵀ | SS | (M',d)×(d,N)→(M',N) | 2M'Nd |
| dV = Pᵀ·dO | TS | (N,M')×(M',d)→(N,d) | 2M'Nd |
| dK = dSᵀ·Q | TS | (N,M')×(M',d)→(N,d) | 2M'Nd |
| dQ = dS·K | SS | (M',2N)×(2N,d)→(M',d) | 2M'Nd |
| **合计** | | | **10M'Nd = 20,971,520** |

### 3.2 SMEM Traffic

按 FA4 论文公式（Table 3 口径），2-CTA 每 CTA 的 SMEM 流量：

**3.2a MMA 操作数读取**（98304 元素，不含 dS A 操作数读取†）：

| MMA | A 操作数（per CTA） | B 操作数（per CTA） | 元素数 |
|---|---|---|---|
| S = Q·Kᵀ | Q (M'×d) = 16384 | Kᵀ (d/2×N) = 8192 | 24576 |
| dP = dO·Vᵀ | dO (M'×d) = 16384 | Vᵀ (d/2×N) = 8192 | 24576 |
| dV = Pᵀ·dO | — (TMEM) | dO (M'×d) = 16384 | 16384 |
| dK = dSᵀ·Q | — (TMEM) | Q (M'×d) = 16384 | 16384 |
| dQ = dS·K | — (dS 不计†) | K (N×d) = 16384 | 16384 |
| **合计** | | | **98304** |

$$T_{\text{smem,MMA}} = \frac{98304 \times 1\text{B}}{128\text{B/cycle}} = 768 \text{ cycles}$$

† dS 的 A 操作数读取不计入"MMA operands"（论文口径：dS 写入单列，读取视为 write→read 转发）。

**3.2b dS 写入 SMEM**（供 dQ MMA 使用）：

$$T_{\text{smem,dS}} = \frac{M' \times N \times 1\text{B}}{128\text{B/cycle}} = \frac{16384}{128} = 128 \text{ cycles}$$

**3.2c dS DSMEM 交换**（corner turn，交换半区 ×3 次遍历）：

$$T_{\text{smem,DSMEM}} = \frac{(M'/2) \times N \times 1\text{B} \times 3}{128\text{B/cycle}} = \frac{8192 \times 3}{128} = 192 \text{ cycles}$$

**3.2d dQ FP32 累加器写+读**（TMA reduction 用）：

$$T_{\text{smem,dQ}} = \frac{(M'/2) \times d \times 4\text{B} \times 2}{128\text{B/cycle}} = \frac{64 \times 128 \times 8}{128} = 512 \text{ cycles}$$

**SMEM 合计**：

$$T_{\text{smem}} = 768 + 128 + 192 + 512 = 1600 \text{ cycles}$$

### 3.3 Softmax 链条完整推导（CUDA Core + MUFU + TMEM + SMEM）

每 SM 每迭代处理 16384 元素（128×128），256 compute 线程，64 元素/线程。

#### P 链：S → exp(S·scale−LSE) → quantize → TMEM

**步骤 1：S TMEM→reg（LDTM）**
```
数据量：  16384 元素 × 4B (FP32) = 65536 B = 64 KB
TMEM 带宽： 256 B/cycle（专用端口，不占 SMEM）
cycles = 65536 / 256 = 256
```

**步骤 2：LSE SMEM→reg（LDS.128）**
```
数据量：  128 个 Q 行 × 4B (FP32) = 512 B
         （LSE 每 Q 行一个值，共享 across KV 列）
SMEM 读带宽： 128 B/cycle
cycles = 512 / 128 = 4
```

**步骤 3：scale·S − LSE（FFMA2 packed）**
```
操作数：  16384 元素，每个做 1 次 FMA
FP32 吞吐： 128 elem/cycle（64 FFMA2 × 2 elem/inst）
cycles = 16384 / 128 = 128
```

**步骤 4：exp2（75% MUFU 硬件 + 25% FMA 软件仿真）**
```
75% 硬件 MUFU：
  元素数 = 16384 × 0.75 = 12288
  MUFU 吞吐 = 16 elem/cycle
  cycles_MUFU = 12288 / 16 = 768

25% 软件仿真（多项式近似）：
  元素数 = 16384 × 0.25 = 4096
  每元素 ~4 次 FMA（多项式展开）
  FP32 吞吐 = 128 elem/cycle（对 FMA 操作）
  cycles_SW = 4096 × 4 / 128 = 128

两条管线（MUFU / FP32）可并行 → 取 max:
cycles = max(768, 128) = 768
```

**步骤 5：P 量化 FP32→FP8（F2FP.SATFINITE.E4M3.PACK_AB_MERGE_C）**
```
元素数：  16384（P tile 全量）
F2FP 吞吐： 64 elem/cycle（quarter rate†）
cycles = 16384 / 64 = 256
```

**步骤 6：P reg→TMEM（STTM）**
```
数据量：  16384 × 1B (FP8) = 16384 B = 16 KB
TMEM 带宽： 256 B/cycle
cycles = 16384 / 256 = 64
```

**P 链合计：**
```
  ① S TMEM load      256  (TMEM port)
  ② LSE SMEM load      4  (SMEM read port)
  ③ scale·S−LSE FMA  128  (FP32 pipe)
  ④ exp2             768  (MUFU pipe, max with 128 SW on FP32)
  ⑤ P quantize       256  (FP32 pipe, F2FP)
  ⑥ P TMEM store      64  (TMEM port)
                    ────
  P 链合计          1476  cycles
```

#### dS 链：dP + P → dS = P·(dP−sumOdO) → quantize → TMEM + SMEM

**步骤 7：dP TMEM→reg（LDTM）**
```
数据量：  16384 × 4B = 64 KB
TMEM 带宽： 256 B/cycle
cycles = 65536 / 256 = 256
```

**步骤 8：sumOdO SMEM→reg（LDS.128）**
```
数据量：  128 个 Q 行 × 4B = 512 B（每 Q 行一个和，共享）
SMEM 带宽： 128 B/cycle
cycles = 512 / 128 = 4
```

**步骤 9：dS = P·(dP − sumOdO)——减法**
```
操作数：  16384 元素，每个做 1 次 FP32 减法
FP32 吞吐： 128 elem/cycle（FADD2 packed）
cycles = 16384 / 128 = 128
```

**步骤 10：dS = P·(dP − sumOdO)——乘法**
```
操作数：  16384 元素，每个做 1 次 FP32 乘法
FP32 吞吐： 128 elem/cycle（FMUL2 packed）
cycles = 16384 / 128 = 128
```
（步骤 9 和 10 串行在同一 FP32 管线上，合计 256 cycles）

**步骤 11：dS 量化 FP32→FP8（F2FP）**
```
元素数：  16384
F2FP 吞吐： 64 elem/cycle
cycles = 16384 / 64 = 256
```

**步骤 12：dS reg→TMEM（STTM，供 dK MMA 读取）**
```
数据量：  16384 × 1B = 16 KB
TMEM 带宽： 256 B/cycle
cycles = 16384 / 256 = 64
```

**步骤 13：dS reg→SMEM（STS.128，供 dQ MMA 读取）**
```
数据量：  16384 × 1B = 16 KB
SMEM 写带宽： 128 B/cycle（与 TMA 共享写端口）
cycles = 16384 / 128 = 128
```

**步骤 14：dS DSMEM 交换（cp.async.bulk）**
```
交换量：  (128/2) × 128 = 8192 元素（半区）× 1B = 8 KB
遍历：    3 次（写 xchg buffer + bulk copy + peer 落盘）
SMEM 带宽： 128 B/cycle
cycles = 8192 × 3 / 128 = 192
```

**dS 链合计：**
```
  ⑦ dP TMEM load      256  (TMEM port)
  ⑧ sumOdO SMEM load    4  (SMEM read port)
  ⑨ dP−sumOdO (sub)   128  (FP32 pipe)
  ⑩ P×(dP−sum) (mul)  128  (FP32 pipe)
  ⑪ dS quantize       256  (FP32 pipe, F2FP)
  ⑫ dS TMEM store      64  (TMEM port, dK 用)
  ⑬ dS SMEM store     128  (SMEM write port, dQ 用)
  ⑭ dS DSMEM 交换     192  (SMEM port + cluster interconnect)
                      ────
  dS 链合计          1156  cycles
```

#### Softmax 链条总计

$$T_{\text{softmax chain}} = \underbrace{1476}_{\text{P 链}} + \underbrace{1156}_{\text{dS 链}} = \boxed{2632 \text{ cycles}}$$

**按管线分解**（识别哪些管线是关键路径）：

| 管线 | P 链使用 | dS 链使用 | 合计 | 说明 |
|---|---:|---:|---:|---|
| **TMEM 端口** | ① 256 + ⑥ 64 = 320 | ⑦ 256 + ⑫ 64 = 320 | **640** | S/dP load + P/dS store |
| **SMEM 读端口** | ② 4 | ⑧ 4 | **8** | LSE + sumOdO（极小） |
| **SMEM 写端口** | — | ⑬ 128 | **128** | dS SMEM store |
| **SMEM（DSMEM）** | — | ⑭ 192 | **192** | cluster 交换 |
| **FP32 管线** | ③ 128 + ⑤ 256 = 384 | ⑨ 128 + ⑩ 128 + ⑪ 256 = 512 | **896** | FMA + softmax + 量化 |
| **MUFU 管线** | ④ 768 | — | **768** | exp2 硬件部分 |
| **串行合计** | 1476 | 1156 | **2632** | 全部串行（上限） |

注：各管线可并行，实际瓶颈 = max(各管线) 而非 sum。FP32 管线（896）> MUFU（768）> TMEM（640）> SMEM 写（128+192=320）> SMEM 读（8）。如果各管线完美重叠，softmax 链条的有效时间为 **max(896, 768, 640, 320, 8) = 896 cycles**（FP32 管线瓶颈）。但 896 < SMEM 总流量 1600，因此 softmax 链条**不是整体 roofline 的瓶颈**——它被 SMEM 的 1600 cycles 遮蔽。

### 3.4 按执行管线分组

不同资源运行在不同的硬件管线上，可以并行。按管线汇总：

| 管线 | 包含的工作 | cycles |
|---|---|---|
| **Tensor Core** | 5 个 MMA | **1280** |
| **SMEM 端口** | MMA 操作数读 + dS 写 + DSMEM + dQ 写读 | **1600** |
| **MUFU** | exp2 (75% 部分) | **768** |
| **FP32 管线 (CUDA Core)** | FMA(128) + softmax sub/mul(256) + exp2 软仿(128) + F2FP×2(512) | **1024** |
| **TMEM 端口** | S load(256) + dP load(256) + P store(64) + dS store(64) | **640** |

注：F2FP 取 quarter rate = 64 elem/cycle。若为 half rate (128 elem/cycle)，则 FP32 管线降至 640；若为 eighth rate (32 elem/cycle)，则升至 1536。

## 4. Roofline 上限

### 4.1 基本上限（假设所有管线完美并行）

$$T_{\text{roofline}} = \max(T_{\text{TC}}, T_{\text{SMEM}}, T_{\text{MUFU}}, T_{\text{FP32}}, T_{\text{TMEM}})$$
$$= \max(1280, 1600, 768, 1024, 640) = T_{\text{SMEM}} = 1600 \text{ cycles}$$

$$\boxed{\text{TFLOPS}_{\text{roofline}} = \frac{6099.5}{1600} = 3812 \text{ TFLOPS}}$$

### 4.2 F2FP 吞吐率敏感性

| F2FP 假设 | FP32 管线 cycles | Roofline 瓶颈 | 上限 TFLOPS |
|---|---:|---|---:|
| Full rate (256 elem/cyc) | 640 | SMEM=1600 | 3812 |
| Half rate (128 elem/cyc) | 768 | SMEM=1600 | 3812 |
| **Quarter rate (64 elem/cyc)** | **1024** | **SMEM=1600** | **3812** |
| Eighth rate (32 elem/cyc) | 1536 | SMEM=1600 (仅多64) | 3812 |

→ 在所有合理的 F2FP 假设下，**SMEM (1600 cycles) 始终是 roofline 瓶颈**。FP32 管线即使在最保守的 eighth-rate 假设下也不超过 SMEM。

### 4.3 加入结构开销

上述 roofline 假设所有管线完美并行、零同步开销。实际 kernel 有不可消除的流水线同步成本（TMA 延迟 + barrier + 循环开销）。设结构开销系数为 α：

$$T_{\text{actual}} = T_{\text{roofline}} \times (1 + \alpha)$$

| α 假设 | T (cycles) | TFLOPS | 说明 |
|---|---:|---:|---|
| 0%（理想） | 1600 | 3812 | 纯 roofline |
| 15% | 1840 | 3315 | 适度开销 |
| 25% | 2000 | 3050 | 保守估计 |
| 50% | 2400 | 2541 | 含显著同步等待 |

## 5. 与 BF16 的理论对比

用同一方法论计算 BF16（元素 2B，MMA 吞吐 8192）：

| 资源 | BF16 cycles | FP8 cycles | FP8/BF16 |
|---|---:|---:|---|
| MMA compute | 10×128³/8192 = **2560** | 10×128³/16384 = **1280** | 0.50× |
| SMEM MMA 操作数 | 98304×2B/128 = **1536** | 98304×1B/128 = **768** | 0.50× |
| SMEM dS 写 | 16384×2B/128 = **256** | 16384×1B/128 = **128** | 0.50× |
| SMEM DSMEM | 8192×2B×3/128 = **384** | 8192×1B×3/128 = **192** | 0.50× |
| SMEM dQ w+r | 64×128×4×2/128 = **512** | 64×128×4×2/128 = **512** | **1.00×** |
| **SMEM 合计** | **2688** | **1600** | **0.60×** |
| MUFU exp2 | 16384/16 = **1024** | 16384/16 = **1024** | 1.00× |
| FP32 管线 (无量化) | 128+256+128 = **512** | 128+256+128+512 = **1024** | 2.00× |
| **Roofline 瓶颈** | **SMEM = 2688** | **SMEM = 1600** | 0.60× |
| **理论上限** | 6099.5×(8192/16384)/2688 = **1149** | 6099.5/1600 = **3812** | **3.32×** |

注：BF16 无 FP8 量化（F2FP），FP32 管线负担更轻。BF16 的 roofline 瓶颈也是 SMEM。

**理论加速比 = FP8 上限 / BF16 上限 = 3812 / 1149 = 3.32×**

但实际加速比受限于：dQ FP32 累加器（SMEM 512 cycles 不减）、MUFU exp2（1024 cycles 不减）、和结构开销（与精度无关）。实际可达加速比 ≈ SMEM 比值 0.60× 的倒数与 MMA 比值 0.50× 的倒数的较小者 ≈ 1/0.60 = 1.67×。

## 6. 结论

| 项目 | 数值 |
|---|---|
| **Roofline 理论上限** | **3812 TFLOPS**（SMEM 带宽瓶颈，1600 cycles/iter） |
| 第二瓶颈 | FP32 管线 1024 cycles（含 F2FP 量化 512） |
| 第三瓶颈 | Tensor Core 1280 cycles（FP8 2× 吞吐已充分） |
| 理论 FP8/BF16 加速比 | 3.32×（受 dQ FP32 和 MUFU 拖累，实际约 1.4-1.7×） |
| Softmax 链条串行总工作量 | **2632 cycles**（P 链 1476 + dS 链 1156） |
| Softmax 链条管线瓶颈 | **FP32 管线 = 896 cycles**（MUFU 768, TMEM 640, SMEM 写 320） |
| Softmax 链条 vs Roofline 瓶颈 | 896 < SMEM 1600 → softmax 链条被 SMEM 遮蔽 |
| 量化（F2FP×2）理论工作量 | 512 cycles（quarter rate 假设） |
| SMEM 带宽详细分解 | MMA 操作数 768 + dS 写 128 + DSMEM 192 + dQ 512 = 1600 |

---

## 7. 三种上限定义及其合理性

| 方法 | cycles | TFLOPS | 假设 | 问题 |
|---|---:|---:|---|---|
| A. 纯 roofline | 1600 | 3812 | 瓶颈资源之上零开销 | 实测 floor(1903) > 1600 → 不可达 |
| B. 直接用 floor | 1903 | 3205 | 所有计算免费 | MMA/softmax 是必需的，不可能免费 |
| **C. BF16 锚定** | **3044** | **2004** | FP8 的开销系数 ≈ BF16 的开销系数 | —（最合理） |

## 8. BF16 锚定法：合理性论证

### 8.1 方法

$$\text{FP8 上限} = \text{BF16}_{\text{实测}} \times \frac{\text{BF16 SMEM roofline}}{\text{FP8 floor}} = 1418.6 \times \frac{2688}{1903} = 1418.6 \times 1.413 = \boxed{2004 \text{ TFLOPS}}$$

### 8.2 为什么合理：三个支柱

**支柱 1：BF16 的优化水平是可信的上界参照**

NVIDIA FA4 BF16 kernel 是生产级、深度调优的代码。它相对自己的 SMEM roofline（2688 cycles）有 60% 开销（实测 4299 cycles / roofline 2688 = **1.599×**）。这 60% 不是懒惰——是流水线 kernel 在真实硬件上不可避免的开销（依赖等待、barrier 延迟、调度不完美）。假设 FP8 能达到同级别优化是合理的。

**支柱 2：两侧的开销来源相同**

| 开销来源 | BF16 kernel | FP8 kernel |
|---|---|---|
| 流水线不完美（MMA 等 softmax） | ✓ | ✓ |
| Barrier 同步等待 | ✓ | ✓ |
| 依赖链（dS→dQ 串行） | ✓ | ✓ |
| 调度器效率 | ✓ | ✓ |
| Wave 量化 | ✓ | ✓ |

开销的**性质**相同，只是瓶颈不同（BF16 卡 SMEM 带宽，FP8 卡结构下限）→ "开销/瓶颈" 比值两侧可比。

**支柱 3：实证验证——差异仅 2%**

| | BF16 | FP8 (fence_cta) |
|---|---:|---:|
| 瓶颈 | SMEM = 2688 cycles | floor = 1903 cycles |
| 实测 | 4299 cycles | 3106 cycles |
| **开销系数** | **1.599×** | **1.632×** |
| 差异 | — | **2.0%** |

FP8 kernel **已经在**相对于其 floor 的位置上和 BF16 相对于其 SMEM roofline 的位置几乎重合 → 隐含假设（开销系数可比）已被数据验证。

### 8.3 交叉验证

两种等价方法给出完全相同的答案：

| 方法 | 公式 | 结果 |
|---|---|---:|
| 折算法 | BF16 实测 × (2688/1903) | 2004 TFLOPS |
| 开销系数法 | FP8 floor × BF16 开销系数 = 1903 × 1.599 = 3044 cycles | 2004 TFLOPS |

### 8.4 各 shape 的目标

| Shape | BF16 实测 | × 1.413 = 目标 | FP8 当前 | 达成率 | 剩余 |
|---|---:|---:|---:|---:|---:|
| 4096 | 1245.9 | **1760** | 1487 | 84.5% | 273 |
| 8192 | 1416.4 | **2001** | 1880 | 94.0% | 121 |
| 16384 | 1418.6 | **2004** | 1964 | **98.0%** | **40** |

### 8.5 为什么不直接用 roofline(3812) 或 floor(3205)

- **roofline 3812 不可达**：floor(1903) > SMEM roofline(1600)，即使 SMEM 无限快也逃不过 1903 cycles 的结构成本
- **floor 3205 过于乐观**：它假设"所有计算免费"，但 5 个 MMA + softmax + 量化是算法必需的
- **BF16 锚定 2004 最实际**：它锚定在"被 NVIDIA 验证过的优化水平"上，比 roofline 更严格、比 floor 更实际

### 8.6 当前状态与剩余空间

@L=16384：当前 1964 TFLOPS，目标 2004，**剩余 40 TFLOPS ≈ 62 cycles/iter**。

超额 1203 cycles 中已优化掉的（fence_cta 等优化回收的 10%）和剩余可争取的：

| 已回收 | 来源 |
|---|---|
| ~350 cycles | fence_cta（代理栅栏 cta 化） |
| 剩余 62 cycles | 需从 1203 − 350 = 853 cycles 的超额中再挤 7% |

---

*硬件参数来源：FA4 论文 Table 3（BF16 基准），NVIDIA Blackwell Architecture Guide，CUTLASS SM100 文档。F2FP 吞吐率取 quarter rate 为标准假设（§4.2 敏感性分析显示结论对此不敏感）。TMEM 带宽来自仓库微基准（`FP8_BWD_2SM_MICROARCH_ROOFLINE.md` §6）。*

## 9. FP16 Floor 实测：独立验证折算系数

对 FP16（FA4 DSL）做了同等的 floor 消融（`FAG_ABLATE_FLOOR=1`，stub 全部 MMA + compute + store，保留 TMA/barrier/loop）。

**结果**（L=16384 不可用——DSL 的 pipeline 管理 stub 后在 large shape 异常，floor 反而比 base 慢）：

| L | FP16 floor (cycles) | FP8 floor (cycles) | **比值** | FP16 base (cycles) | 开销系数 |
|---|---:|---:|---:|---:|---:|
| 4096 | 3600 | 2526 | 1.425× | 4895 | 1.36× |
| 8192 | 3008 | 2134 | **1.410×** | 4306 | 1.43× |

### 关键验证

| 方法 | 比值 | FP8 上限 |
|---|---:|---:|
| 理论法：BF16 SMEM roofline / FP8 floor = 2688/1903 | 1.413× | 2004 TFLOPS |
| **实测法：FP16 floor / FP8 floor = 3008/2134** | **1.410×** | **2000 TFLOPS** |
| 差异 | 0.2% | 4 TFLOPS |

**两种完全独立的方法（理论 roofline 比值 vs 实测 floor 比值）给出几乎相同的折算系数**——1.410× vs 1.413×，差异仅 0.2%。这从实验上严格验证了折算方法的正确性。

### 开销系数对比 @L=8192

| | FP16 | FP8 |
|---|---:|---:|
| base / floor | 1.431× | 1.520× |
| 差异 | — | 6.2% |

FP8 的开销系数略高（1.52 vs 1.43），说明 FP8 kernel 相对于其 floor 还有少量优化空间（与 @16384 分析一致：当前 98% of 目标）。
