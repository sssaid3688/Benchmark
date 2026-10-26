# Baseline: softmax P-value exp2 removed (exp → 加法)

**日期**：2026-08-11
**设备**：NVIDIA B200（148 SM，SM100），GPU 锁频 1965 MHz
**CUDA**：13.3 / nvcc V13.3.73，`-DCMAKE_BUILD_TYPE=Release -DCUTLASS_NVCC_ARCHS=100a`

这是一个**性能探针基线**（数值错误，仅用于度量 exp 代价），用于后续一系列 exp 实现实验的对照起点。

---

## 1. 这个版本是什么

以 **最优正确版** `op6static2sm_no_e2rsf_fa4mufu_b200_blackwell_fmha_mxfp8`
（E2RSF 关 + 18.75% MUFU 分流）为底，把 **softmax 里 P 值的 exp2 突发**整体换成一次加法：

```
原:  P = exp2( scale * s - row_max_scale )           // out = FMA(scale, s, -row_max_scale)
新:  P =        out + row_max_safe                     // 一次 FADD2
```

其中 `out` 就是原来喂给 exp2 的那个 FMA 结果，`row_max_safe` 是行最大值。
**只替换了 exp2→加法这一步**；row_sum / O 的 rescale exp2 保留为硬件 `MUFU.EX2`（数值有界、时序干净）。

## 2. 编译宏

target 名：`op6static2sm_no_e2rsf_fa4mufu_noexp_b200_blackwell_fmha_mxfp8`

= `OP6_FA4_MUFU_NO_E2RSF_DEFS`（= `OP6_STATIC_2SM_DEFS` 去掉 `MXFP8_E2RSF`）
+ `MXFP8_FA4_MUFU_SPLIT` `MXFP8_FA4_MUFU_BEGIN_PAIR=4` `MXFP8_FA4_MUFU_END_PAIR=28`
`MXFP8_FA4_MUFU_PERIOD=4` `MXFP8_FA4_MUFU_SLOT=0`
+ **`MXFP8_NOEXP_ADD`**（本基线新增的门控宏）

## 3. 源码改动（宏门控，对其它 target 零影响）

文件 `collective/sm100_fmha_fwd_mainloop_mxfp8_n128_pvmx.hpp`：

- `minus_row_max_scale_fp32x2` 定义之后新增：
  ```cpp
  #if defined(MXFP8_NOEXP_ADD)
      float2 row_max_safe_fp32x2 = make_float2(row_max_safe, row_max_safe);
  #endif
  ```
- softmax 突发循环里（`cute::fma(out, scale_fp32x2, in, minus_row_max_scale_fp32x2);` 之后），
  `MXFP8_NOEXP_ADD` 分支优先于 `MXFP8_FA4_MUFU_SPLIT`：
  ```cpp
  #if defined(MXFP8_NOEXP_ADD)
        float2 exp_out;
        cute::add(exp_out, out, row_max_safe_fp32x2);
        tTMEM_LOADrS(i+0) = exp_out.x;
        tTMEM_LOADrS(i+1) = exp_out.y;
  #elif defined(MXFP8_FA4_MUFU_SPLIT)
        ...原代码不动
  #else
        ...原代码不动
  #endif
  ```

## 4. 防编译器优化审计（NoMask fwd kernel SASS，cuobjdump）

关键担忧：去掉 exp 后编译器会不会把 row-max 归约链也优化掉？**结论：没有。**

| 指标 | 基线(no_e2rsf_fa4mufu) | 本 noexp 版 | Δ | 说明 |
|---|---:|---:|---:|---|
| `MUFU.EX2` | 54 | 2 | **−52** | = 26 个原生 P 突发 pair × 2 lane；剩 2 个是 rescale exp2（保留） |
| `FFMA2` | 50 | 32 | −18 | = 6 个模拟分流 pair × 3 poly-FMA（exp 的一部分） |
| `FADD2` | 53 | 67 | +14 | 替换用的 `out + row_max_safe` 加法 |
| **`FMNMX3`** | **34** | **34** | **0** | **行 max 归约 + 所有 min/max 完全保留** |
| 主 kernel 寄存器 | 128 | 128 | 0 | 无 spill 变化 |
| SASS 指令总数 | 3864 | 3784 | −80 | 仅 exp 机器被去掉 |

依据：`row_max_safe` 被那次加法消费 → 数据流上编译器**不可能**删掉生成它的归约；
SASS 里 `FMNMX3` 计数 34→34 完全不变，实证这一点。**只有 exp→加法这一步变了。**

## 5. 性能实测（4 样本交错 A/B，每样本 5 warmup + 20 iters，锁频 1965）

### shape-170100（B=1 H=40 Q=K=170100 D=128 mask=no）

| 版本 | 4 样本 TFLOPS/s | 中位 | 均值 | 单 kernel 时间 |
|---|---|---:|---:|---:|
| 基线 no_e2rsf_fa4mufu | 1553.50, 1549.08, 1548.06, 1547.60 | 1548.57 | 1549.56 | ~191 ms |
| 本 noexp 版 | 1783.57, 1780.82, 1775.41, 1779.33 | **1780.07** | 1779.78 | ~166 ms |
| **noexp 相对基线** | — | **+14.95%** | +14.86% | **−25 ms/kernel** |

### shape-510300（B=1 H=40 Q=K=510300 D=128 mask=no）

| 版本 | 4 样本 TFLOPS/s | 中位 | 均值 | 单 kernel 时间 |
|---|---|---:|---:|---:|
| 基线 no_e2rsf_fa4mufu | 1517.84, 1519.49, 1517.60, 1518.52 | 1518.18 | 1518.36 | ~1758 ms |
| 本 noexp 版 | 1677.91, 1678.27, 1678.94, 1676.98 | **1678.09** | 1678.03 | ~1590 ms |
| **noexp 相对基线** | — | **+10.53%** | +10.52% | **−168 ms/kernel** |

**结论：softmax P 值的 exp2 是非常大的瓶颈**——去掉它后 170100 提速约 **+15%**，
510300 提速约 **+10.5%**（序列越长 softmax 占比越低，收益递减）。两组样本各自极紧凑、
两版完全不交叠，方向一致、可复现。

## 6. 复现方式

```bash
# 编译（本目录已含编译此基线所需的全部源码快照 + CMakeLists）
cmake -S <FA 顶层> -B <FA>/build -DCMAKE_BUILD_TYPE=Release -DCUTLASS_NVCC_ARCHS="100a"
cmake --build <FA>/build --target op6static2sm_no_e2rsf_fa4mufu_noexp_b200_blackwell_fmha_mxfp8 -j

# 跑（性能度量，勿加 --verify，输出数值是错的）
./op6static2sm_no_e2rsf_fa4mufu_noexp_b200_blackwell_fmha_mxfp8 \
  --b=1 --h=40 --h_k=40 --q=170100 --k=170100 --d=128 --mask=no \
  --warmup_iterations=5 --iterations=20
```

## 7. 后续实验用法

本基线 = "softmax 里 P 值的 exp 被完全去掉、换成一个加法占位" 的参照点。
后续替换 exp 的实验（多项式 / 查表 / 近似 / 分流 ……）都应以 `op6static2sm_no_e2rsf_fa4mufu`
为正确性 + 性能对照，并以本 noexp 版作为**性能上界参照**（任何 exp 实现都不应超过本版的吞吐，
因为本版完全没有 exp 计算）。

## 8. 本目录文件清单

- `b200_blackwell_fmha.cu` — 入口源码快照
- `sm100_fmha_fwd_mainloop_mxfp8_n128_pvmx.hpp` — 含 `MXFP8_NOEXP_ADD` 改动的主循环
- `fa4_mufu_split_sm100.hpp` — MUFU 分流实现（本基线因 NOEXP 优先而不调用它）
- `CMakeLists.txt` — 含本 target 的定义
- `op6static2sm_no_e2rsf_fa4mufu_noexp_b200_blackwell_fmha_mxfp8` — 编译产物

## 9. 在 noexp 之上的指令级消融实验（2026-08-11）

目的：exp 去掉之后，新的瓶颈在哪里。方法 = 纯探针（每个探针把 noexp 的某一类指令"免费化"，测 ΔTFLOPS，越大＝该类越是瓶颈）。shape-170100，4 样本交错 A/B vs noexp，5 warmup + 20 iters，锁频 1965，GPU 空载。

### 9.1 粒度1：单指令类（noexp 中位 ≈ 1778 TFLOPS）

| 探针 | 中和的指令类(计数) | SASS 变化 | Δ vs noexp | 结论 |
|---|---|---:|---:|---|
| **fmax (P4)** | row-max 归约 FMNMX3(34) | FMNMX3 34→1 | **+4.04%** | **新主瓶颈** |
| **convert (P1)** | P 量化 fp32→e4m3 (F2FP 96 中 32) | F2FP 96→64 | **+2.91%** | 第二 |
| **fmul (P2)** | O 规范化/rescale FMUL2(128) | FMUL2 128→0 | **+1.85%** | 第三 |
| ffma (P3) | burst scale FFMA2(32) | FFMA2 32→0（+FADD2） | +0.00% | 非瓶颈 |
| mufu (P5) | rescale MUFU.EX2(2) | MUFU.EX2 2→0 | −3.37% | 非瓶颈（见下） |

注：
- P1 只去掉 96 个 F2FP 里的 32 个（P 量化那部分；另 64 个来自 epilogue O 转换，未中和）；HFMA2(51) 与该转换器**不**同源，未被单独隔离。P1 测得的是"P 量化转换器"的天花板。
- P5 的负值是**替换副作用**：rescale exp2 本跑在空闲的 XU(MUFU) 管上，被 `arg+1.0f`(FADD2) 替换后挤占了繁忙的 FP 管，并触发 ptxas 重排程 → 反而变慢。正确解读：rescale MUFU **不是瓶颈**（隐藏在空闲管上），别动它。

### 9.2 粒度2：指令组合（interaction）

| 组合 | Δ vs noexp | 单项之和 | 解读 |
|---|---:|---:|---|
| c_top2 = fmax+convert | +6.88% | 6.95% | **加性**（独立） |
| c_top3 = fmax+convert+fmul | **+8.99%** | 8.80% | **加性**（独立，softmax 计算天花板） |
| C-all = 全 5 个 | +5.12% | — | 被 mufu 替换副作用(−3.4%) 拖低，低于 c_top3 |

组合 ≈ 单项之和（加性）→ 三类在**不同的并行管**上，**不是**同一条串行链。

### 9.3 粒度3：串行链 / 新瓶颈结论

exp 消除后，softmax warp 的下一个瓶颈**不是单一串行链，而是三条独立的并行微瓶颈**：
1. **行最大值归约（FMNMX3，+4.04%）**——64 元 fmax 扫描，最大头。
2. **P 的 e4m3 量化转换（F2FP，+2.91%）**。
3. **O 规范化/rescale 乘（FMUL2，+1.85%）**。

把这三类一起免费化（c_top3）达到 **+8.99%**（noexp 1778→1933 TFLOPS）= "softmax 计算全免"天花板。
从最优正确版(1548，含 exp)算起，去 exp(+15%) + 去 softmax 全部计算(+9%) ≈ **+24.9%** 上限（1933 TFLOPS）；
再往上就是访存/MMA bound。burst 的 scale FMA(FFMA2) 和 rescale exp2(MUFU) 都不是瓶颈，不值得优化。

### 9.4 探针复现

探针宏在 `sm100_fmha_fwd_mainloop_mxfp8_n128_pvmx.hpp`（`MXFP8_ABLATE_CONVERT/_FMUL/_FFMA/_FMAX/_MUFU`），
target 在 `CMakeLists.txt`（`add_noexp_ablation(...)`，如 `op6static2sm_noexp_abl_fmax_...`、`..._abl_c_top3_...`）。
本目录快照是 noexp 基线（消融宏之前的版本）；消融 target 在工作树里、需在 FA/build 下重编译。
