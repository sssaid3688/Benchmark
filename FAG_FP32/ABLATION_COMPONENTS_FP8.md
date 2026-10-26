# FP8 FAG Backward — 逐组件消融（limiting study）

对最优 FP8 FAG backward kernel（`fag_stage_regalloc` = 2-SM + `FAG_EX2_EMU_STRIDE=4` + `FAG_REGALLOC_FA4_CURRENT`）做**逐组件 limiting study**：每个组件单独 stub 掉（跳过其计算，**保留所有 pipeline/mbarrier 握手**防死锁；输入用零开销 register-asm 强制 live 防 DCE），测主 kernel 时间，组件成本 = `T_base − T_stub`。

## 方法与口径
- **对象**：FP8 e4m3，2-SM FA4 backward 主 kernel `Sm100FmhaBwdKernelTma2SmWarpSpecialized`。
- **测量**：nsys `cuda_gpu_kern_sum`。本次测量期间 B200 上有他人 co-resident 作业（`v3_E_sfone` 扫描），造成大量长尾（max 可达 3.6× 中位数，stddev ≈ 1.4 ms）。为剔除干扰，取 **Min（30 次里最快一次 ≈ 无干扰真值）**而非 median（base L=8192 Min=825.5µs，与无干扰的 ~824µs 一致）。
- **形状**：`(1,16,{8192,16384},128)`，non-causal，锁频 1965 MHz，30 次迭代。`FLOP=10·b·h·L²·d`。
- **正确性**：stub 故意破坏数值（仅计时），base 路径零影响。每个 stub 目标在 L=4096 已 smoke-test（全部 rc=0，无死锁）。

## 结果（主 kernel，组件成本 = base − stub）

### L=8192（base 825.5 µs / 1665 TFLOPS）
| 组件 | stub后 µs | 成本 µs | %base | FP8特有? |
|---|---:|---:|---:|---|
| **DSMEM corner 交换** | 693.6 | **132.0** | **+16.0%** | FP8 税 |
| **FP8 量化 (FP32→e4m3)** | 729.5 | **96.1** | **+11.6%** | FP8 税 |
| dS = dsoftmax (add+mul) | 772.3 | 53.2 | +6.4% | 共享 |
| dP = dO@V (MMA) | 808.3 | 17.2 | +2.1% | 共享 |
| dV = P@dO (MMA) | 817.2 | 8.3 | +1.0% | 共享 |
| S = K@Q (MMA) | 817.6 | 7.9 | +1.0% | 共享 |
| dQ = dS@Kt (MMA) | 822.9 | 2.6 | +0.3% | 共享 |
| pack4 标量 uint4 pack | 824.6 | 0.9 | +0.1% | FP8 税 |
| exp2 (hw+emu) | 847.9 | −22.4 | −2.7% | FP8 税* |
| dK = dS@Qt (MMA) | 852.8 | −27.2 | −3.3% | 共享 |

### L=16384（base 3160.7 µs / 1739 TFLOPS）
| 组件 | stub后 µs | 成本 µs | %base |
|---|---:|---:|---:|
| **DSMEM corner 交换** | 2611.1 | **549.6** | **+17.4%** |
| **FP8 量化 (FP32→e4m3)** | 2777.9 | **382.8** | **+12.1%** |
| dS = dsoftmax | 2932.5 | 228.2 | +7.2% |
| dP = dO@V (MMA) | 3100.3 | 60.4 | +1.9% |
| dV = P@dO (MMA) | 3120.3 | 40.4 | +1.3% |
| S = K@Q (MMA) | 3139.2 | 21.5 | +0.7% |
| dQ = dS@Kt (MMA) | 3152.0 | 8.7 | +0.3% |
| exp2 (hw+emu) | 3154.8 | 5.9 | +0.2% |
| pack4 | 3163.1 | −2.4 | −0.1% |
| dK = dS@Qt (MMA) | 3296.7 | −136.0 | −4.3% |

## 关键结论

### 1. FP8 backward 是「compute warpgroup 受限」，不是 MMA 受限
5 个 MMA GEMM（S/dP/dV/dK/dQ）单独 stub 后**基本不变甚至变慢**（0~2%，dK 为负）——它们与 compute warpgroup 重叠执行，**不在关键路径上**。真正占时间的是 compute warpgroup 上的**串行标量工作**：corner(+16%)、FP8 量化(+12%)、dS(+6~7%)。三者合计 ≈ 35%。
> 含义：FP8 MMA 吞吐是 BF16 的 2×，5 个 GEMM 很快做完，瓶颈转移到 compute warpgroup 的标量/搬运工作。这正是 FP8 与 FP16 的**结构性差异**——FP16 的 MMA 更慢、更接近瓶颈，compute warpgroup 工作相对更小。

### 2. FP8 相对 FP16 的三项「税」
| FP8 特有开销 | 成本 | BF16 情况 |
|---|---:|---|
| **FP8 量化 (FP32→e4m3 cvt)** | **+11~12%** | BF16 无此 cvt（Element 即 BF16，cvt 更便宜且常折叠） |
| **DSMEM corner 交换** | **+16~17%** | BF16 也有 corner，但 dS 是 2 字节向量化拷贝；FP8 的 1 字节 + 交换机制更贵（见 corner 报告） |
| exp2 软件仿真 (MUFU 分流) | ≈0%（被 MMA 藏住） | BF16 用全硬件 MUFU exp2 |

> 注意：FP8 的 MMA 优势（2× 吞吐）把 GEMM 成本压到接近 0，但 quant(+12%) 把相当一部分优势又吃回去了。**净收益取决于能否把 FP32→FP8 量化也加速/折叠**（例如 fused cvt、或让 cvt 与 MMA 更好重叠）。

### 3. 纠正旧假设：pack4 标量 pack **不是** corner 瓶颈
`FAG_DS_RECAST_PACK`（把 pack4 的 12 条标量移位/或换成 `recast<uint32_t>`）实测 **≈0% 改善**（且它本就是被弃用的 candidate，未进最优配置）。说明 corner 的 +16% 来自**交换机制本身**（DSMEM bulk copy + store 序列化 + pipeline 等待），**不是** pack4 算术。优化 corner 应针对搬运/同步，而非 pack 算术。

### 4. 负值 = 调度扰动，不是「负成本」
`dK`(−3~4%)、`exp2`(L=8192 −2.7%) 为负：这是深度流水线 kernel 上 limiting study 的固有限制——这些组件与关键路径**重叠**，删掉它们反而打破 MMA↔compute 的重叠节奏（删 dK 后 MMA 更早空闲、却要更长等待 compute 的 dS；删 exp2 破坏 MUFU/FMA 调度平衡）。**解读为「该组件被完全隐藏」**，而非真有收益。

## 组件清单（本 kernel 涉及的全部主要操作）
- **MMA（5 个 GEMM，MMA warpgroup）**：S=K@Q、dP=dO@V、dV=P@dO、dK=dS@Qt、dQ=dS@Kt。
- **compute warpgroup 标量**：softmax(scale·S−LSE + exp2)、dS=dsoftmax(add+mul)、FP8 量化(FP32→e4m3)、dS 的 RMEM→SMEM pack + TMEM 写。
- **跨 CTA**：DSMEM corner 交换（dS peer 半 pack+bulk copy）。
- **Reduce warpgroup**：dQ TMEM→RMEM→SMEM→TMA store（本表未单独消融：TMA-store pipeline 难以安全 stub，且非 FP16 差异点）。
- **Load warpgroup**：Q/K/V/dO 的 TMA 加载（mbarrier 门控，被各 consumer 复用，未单独消融）。
- **无** per-tile scale/descale（本 kernel 非 MX-FP8，量化是无 scale 的 RTN cvt）。

## 复现
```bash
cd /home/ubuntu/workspace/oyhj/temp/test/test_merge/Benchmark/FAG_FP32
nvidia-smi -lgc 1965,1965
cmake --build build --target fag_stage_regalloc fag_abl_s fag_abl_dp fag_abl_dv \
  fag_abl_dk fag_abl_dq fag_abl_exp2 fag_abl_softmax fag_abl_quant fag_abl_corner \
  fag_candidate_pack -j 6
# 每个 target：nsys 抓主 kernel，取 Min
nsys profile --force-overwrite=true -o /tmp/cmp_S_8192 ./build/fag_abl_s \
  --b=1 --h=16 --q=8192 --k=8192 --d=128 --d_vo=128 --iterations=30 --mask=no
nsys stats -r cuda_gpu_kern_sum /tmp/cmp_S_8192.nsys-rep   # Sm100FmhaBwdKernelTma2SmWarpSpecialized 的 Min 列
```

## 改动文件
- `kernel/sm100_fmha_bwd_kernel_tma_2sm_warpspecialized.hpp`：加 `FAG_ABLATE_{S,DP,DV,DK,DQ,EXP2,SOFTMAX,QUANT}` 守卫（每个只 stub 目标计算，保留全部握手；EXP2/SOFTMAX 用恒等/寄存器-asm 防 DCE；QUANT 强制输入 live + 常量输出）。base 路径零影响。
- `CMakeLists.txt`：加 `fag_abl_{s,dp,dv,dk,dq,quant}` target（exp2/softmax/corner 已存在）。
- `/tmp/component_ablation_raw.csv`：原始数据；`/tmp/analyze_components.py`：分析脚本。

## 原始数据（Min, ns）
```
component,shape,min_ns
base,8192,825531    base,16384,3160689
S,8192,817627       S,16384,3139217
dP,8192,808347      dP,16384,3100337
dV,8192,817211      dV,16384,3120305
dK,8192,852763      dK,16384,3296656
dQ,8192,822907      dQ,16384,3151985
exp2,8192,847932    exp2,16384,3154833
softmax_dS,8192,772285  softmax_dS,16384,2932498
quant,8192,729469   quant,16384,2777906
corner,8192,693565  corner,16384,2611124
pack_removed,8192,824636  pack_removed,16384,3163120
```
