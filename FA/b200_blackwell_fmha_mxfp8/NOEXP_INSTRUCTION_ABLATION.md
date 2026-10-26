# 在去 exp 版之上的指令级消融实验

**日期**：2026-08-11
**设备**：NVIDIA B200（148 SM，SM100）
**CUDA**：13.3 / nvcc V13.3.73；`-DCMAKE_BUILD_TYPE=Release -DCUTLASS_NVCC_ARCHS=100a`
**GPU 频率**：测试前锁定 1965 MHz（`nvidia-smi -lgc 1965,1965`），每次跑前确认 GPU 空载（无其它 compute 进程）
**被测 shape**：`B=1 H=40 H_K=40 Q=K=170100 D=128 mask=no`
**协议**：每个探针与 noexp 基线做交错 A/B，4 个独立样本，每样本 5 warmup + 20 timed iters

---

## 1. 背景与目标

前置实验（见 `baseline_noexp_add_20260811/README.md`）已经把 softmax 里 **P 值的 exp2 突发**整体换成一次加法（`exp2(out) → out + row_max_safe`），得到去 exp 版
`op6static2sm_no_e2rsf_fa4mufu_noexp_b200_blackwell_fmha_mxfp8`（下称 **noexp**，≈1778 TFLOPS），
相对含 exp 的最优正确版（≈1548 TFLOPS）提升约 **+15%**。说明 exp 是当时的最大瓶颈。

本实验回答：**exp 这个瓶颈消除之后，新的瓶颈出现在哪里？**

方法 = 纯探针消融（不用 NCU，只看前后 TFLOPS）：每个探针在 noexp 之上把**某一类指令"免费化"**，
Δ 越大＝该类越是瓶颈。三个粒度：
1. **单指令类**：每类各一个探针，排序。
2. **指令组合**：把表现最好的几类同时免费化，看互作用（加性＝独立并行路；亚加性＝同条串行链）。
3. **串行链**：综合 1+2 得出新关键路径。

全程以 **noexp 为基线**（不是含 exp 的最优正确版），因为题目就是"exp 去掉之后"。

---

## 2. 方法

### 2.1 探针范式
- 每个探针 = 一个编译期宏 `MXFP8_ABLATE_*`，宏门控；不定义时与 noexp **位级等价**。
- "免费化"= 把目标类指令替换成一个极廉价的操作（identity / 加法 / 常量写）。
- **关键约束（liveness）**：替换必须保留原操作数的使用，让编译器**无法**把"别的计算"（尤其行 max 归约链）DCE 掉；
  保证"只中和了目标类、其余结构不变"。每个探针编译后都用 `cuobjdump --dump-sass` 抽查 NoMask fwd kernel 的指令计数验证这一点。
- 所有探针**数值都是错的**（仅 perf），跑的时候不加 `--verify`。

### 2.2 干净环境
每次基准前：
```bash
nvidia-smi --query-compute-apps=pid --format=csv,noheader   # 确认空载
nvidia-smi -lgc 1965,1965                                    # 锁频
```

### 2.3 测量口径
- 取每个二进制打印的 `TFLOPS/s`（单进程内 5 warmup + 20 iters 的吞吐代表值）。
- 每个探针 4 个独立样本 vs noexp 4 个样本，**交错**采集（round 内先 noexp 后 probe），抵消漂移。
- 报中位数为主、均值为辅；Δ = `probe_median / noexp_median − 1`。

---

## 3. 五个单指令类探针的设计

源文件：`collective/sm100_fmha_fwd_mainloop_mxfp8_n128_pvmx.hpp`
（noexp 配置的激活宏集见前置 README；本实验在其基础上加 `MXFP8_NOEXP_ADD`。）

| 探针 | 宏 | 目标指令类（noexp 计数） | 源码位置 | 中和方式 | liveness 保障 |
|---|---|---|---|---|---|
| **P1 convert** | `MXFP8_ABLATE_CONVERT` | fp32→e4m3 转换（F2FP） | `softmax_step` 行 2537 `tTMEM_STORErS_x4_e[..] = convert_p(in_conv);` | 写一个常量 e4m3 字（4×0.25）到 TMEM，丢弃 `in_conv` | 行 row_sum pass 仍在读 `tTMEM_LOADrS` → burst 输出保持 live，只删转换器 |
| **P2 fmul** | `MXFP8_ABLATE_FMUL` | O 规范化/rescale 乘（FMUL2） | 行 3516 / 3637（活）；3766 / 3780（RSGUARD 死支）`cute::mul` | 宏 `MXFP8_O_MUL` → `out = in`（identity） | O 的 load/store 仍 live；row_sum 经 LSE 输出仍 live |
| **P3 ffma** | `MXFP8_ABLATE_FFMA` | burst scale FMA（FFMA2） | 行 2359 `cute::fma(out, scale, in, -row_max_scale)` | FMA→ADD：`cute::add(out, in, minus_row_max_scale)` | `in` 与 `-row_max_scale` 仍被消费 → 行 max 链保留；`scale` 是 param 无 DCE 风险 |
| **P4 fmax** | `MXFP8_ABLATE_FMAX` | 行 max 归约（FMNMX3） | 行 2140-2149 的 64 元 fmax 扫描 | 折叠成 `row_max = fmax(row_max, rS(0))` | `row_max` 仍 live（chain/stats 仍发布），去掉 ~63 个 fmax |
| **P5 mufu** | `MXFP8_ABLATE_MUFU` | rescale exp2（MUFU.EX2） | 行 2614（row_sum rescale，活）+ 4127（O rescale，活） | 宏 `MXFP8_RESCALE_EXP2(x)` → `x + 1.0f` | `scale*(old-new)` 仍被消费 |

CMake：在 `b200_blackwell_fmha_mxfp8/CMakeLists.txt` 用一个辅助函数批量生成 target：
```cmake
function(add_noexp_ablation SUFFIX)
  set(T op6static2sm_noexp_abl${SUFFIX}_b200_blackwell_fmha_mxfp8)
  ...  # = noexp 那组 DEFS + ${ARGN}（即各 MXFP8_ABLATE_* 宏）
endfunction()
```

> **P1 的局限（重要）**：96 个 F2FP 里只有 32 个来自 P 量化转换器，另 64 个来自 epilogue 的 O→output 转换；
> 本探针只中和了前者（SASS 实测 F2FP 96→64）。另外 HFMA2(51) **不**与该转换器同源（中和后 HFMA2 不降反因重排程略升），
> 因此 P1 测得的是"P 量化转换器"的天花板，**不是**完整 F2FP+HFMA2 类。已在结果里如实标注。

---

## 4. 粒度 1：单指令类结果

### 4.1 原始样本（4 样本/版，TFLOPS/s）

| 探针 | noexp 样本 | probe 样本 | noexp 中位 | probe 中位 | **Δ** |
|---|---|---|---:|---:|---:|
| **fmax (P4)** | 1774.04, 1779.56, 1774.32, 1782.52 | 1845.84, 1850.41, 1849.63, 1847.87 | 1776.94 | 1848.75 | **+4.04%** |
| **convert (P1)** | 1783.28, 1779.14, 1776.10, 1782.13 | 1832.35, 1827.05, 1835.15, 1832.70 | 1780.64 | 1832.53 | **+2.91%** |
| **fmul (P2)** | 1776.09, 1768.12, 1780.79, 1777.72 | 1808.03, 1811.65, 1804.22, 1813.09 | 1776.90 | 1809.84 | **+1.85%** |
| ffma (P3) | 1772.71, 1781.81, 1775.89, 1779.01 | 1776.66, 1778.28, 1780.93, 1776.57 | 1777.45 | 1777.47 | **+0.00%** |
| mufu (P5) | 1776.88, 1779.36, 1773.82, 1779.10 | 1717.23, 1719.46, 1718.60, 1717.47 | 1777.99 | 1718.03 | **−3.37%** |

（均值 Δ 与中位 Δ 一致：fmax +3.98%、convert +2.90%、fmul +1.89%、ffma +0.04%、mufu −3.33%。）
每组样本内部极紧凑、两版完全不交叠（除 ffma 几乎重合）。

### 4.2 排序与新瓶颈定位

| 排名 | 指令类 | Δ | 结论 |
|---|---|---:|---|
| 🥇 1 | 行 max 归约 FMNMX3 | **+4.04%** | **exp 之后的新主瓶颈** |
| 🥈 2 | P 量化转换 F2FP | +2.91% | 第二 |
| 🥉 3 | O 规范化/rescale 乘 FMUL2 | +1.85% | 第三 |
| 4 | burst scale FMA FFMA2 | +0.00% | **非瓶颈**（FP 管有余量） |
| 5 | rescale exp2 MUFU.EX2 | −3.37% | **非瓶颈**（见 4.4） |

### 4.3 SASS 抽查（NoMask fwd kernel 指令计数）

基线 noexp：`F2FP=96 HFMA2=51 FMUL2=128 FFMA2=32 FADD2=67 FMNMX3=34 MUFU.EX2=2`

| 探针 | F2FP | HFMA2 | FMUL2 | FFMA2 | FADD2 | FMNMX3 | MUFU.EX2 | 说明 |
|---|---:|---:|---:|---:|---:|---:|---:|---|
| convert | **64** | 59 | 128 | 32 | 67 | 34 | 2 | 只删 P 量化那 32 个 F2FP；HFMA2 未降（重排程略升） |
| fmul | 96 | 51 | **0** | 32 | 67 | 34 | 1 | FMUL2 全删；顺带 DCE 掉 1 个 rescale exp2 |
| ffma | 96 | 51 | 128 | **0** | **99** | 34 | 2 | FFMA2→FADD2（+32 FADD2） |
| fmax | 96 | 51 | 128 | 32 | 67 | **1** | 2 | 64 元扫描折叠成 1 个 fmax |
| mufu | 96 | 51 | 128 | 32 | 67 | 34 | **0** | 两个 rescale exp2 都去掉 |

各探针只动了目标类（及可解释的连带），没有意外连累别的结构 —— liveness 约束成立。

### 4.4 关于 P5 的负值（必须正确解读）
P5 把 2 个 rescale `MUFU.EX2` 换成 `arg + 1.0f`（FADD2），结果**反而慢 3.37%**。这不是测量噪声（4 样本极紧凑）。
原因：rescale exp2 本来跑在**空闲的 XU(MUFU) 管**上（隐藏在别的工作后面），换成 FADD2 后这部分工作挪到了**繁忙的 FP 管**（已被 67 FADD2 / 128 FMUL2 / 32 FFMA2 占满），并触发 ptxas 全局重排程。
**正确结论 = rescale MUFU 不是瓶颈（隐藏在空闲管上），不该动它。** 这也解释了为什么 C-all（含 P5）会被拖到比 c_top3 还低（见第 5 节）。

---

## 5. 粒度 2：指令组合结果

基于粒度 1 的三强（fmax / convert / fmul），定义组合探针。

### 5.1 原始样本与 Δ

| 组合 | noexp 样本 | probe 样本 | noexp 中位 | probe 中位 | **Δ** |
|---|---|---|---:|---:|---:|
| **c_top3** = fmax+convert+fmul | 1771.95, 1781.75, 1775.46, 1770.57 | 1932.13, 1936.36, 1934.31, 1931.02 | 1773.70 | 1933.22 | **+8.99%** |
| **c_top2** = fmax+convert | 1786.90, 1774.91, 1781.56, 1781.01 | 1911.19, 1900.30, 1905.04, 1902.78 | 1781.28 | 1903.91 | **+6.88%** |
| C-all = 全 5 个 | 1775.49, 1779.73, 1774.17, 1781.25 | 1868.89, 1868.48, 1869.46, 1864.35 | 1777.61 | 1868.68 | +5.12% |

C-all 的 SASS：`F2FP=64 HFMA2=59 FMUL2=0 FFMA2=0 FADD2=99 FMNMX3=1 MUFU.EX2=0`
c_top2 的 SASS：`F2FP=64 FMUL2=128 FFMA2=32 FMNMX3=1 MUFU.EX2=2`
c_top3 的 SASS：`F2FP=64 FMUL2=0 FFMA2=32 FMNMX3=1 MUFU.EX2=1`

### 5.2 互作用分析

| 组合 | Δ | 单项之和 | 差 | 解读 |
|---|---:|---:|---:|---|
| c_top2 (fmax+convert) | +6.88% | 6.95% | −0.07 | **加性 → 两类在独立并行管上** |
| c_top3 (fmax+convert+fmul) | +8.99% | 8.80% | +0.19 | **加性（略超加性）→ 三类相互独立** |

**关键结论**：三强的组合 ≈ 单项之和（加性，不是亚加性）。
若是同一条串行链，组合会**亚加性**（同链只帮一次）；加性意味着它们在**不同的并行管 / 不同资源**上各自是局部瓶颈，互不重叠。

C-all（+5.12%）**低于** c_top3（+8.99%），因为 C-all 额外含 ffma（中性 0%）和 mufu（−3.37% 副作用），
被 mufu 的替换副作用拖低约 3.4%（8.99 − 3.4 ≈ 5.6 ≈ 实测 5.12）。
**所以"softmax 计算全免"的干净天花板应取 c_top3 = +8.99%，不是 C-all。**

---

## 6. 粒度 3：串行链 / 新瓶颈结论

**exp 消除后，softmax warp 的下一个瓶颈不是单一串行链，而是三条相互独立的并行微瓶颈**，按代价排序：

1. **行最大值归约（FMNMX3，+4.04%）** —— 64 元 fmax 扫描，最大头。
2. **P 的 e4m3 量化转换（F2FP，+2.91%）** —— fp32→FP8 的 convert 链。
3. **O 规范化 / rescale 乘（FMUL2，+1.85%）** —— 最后 ×(scale_output/row_sum) 等。

把三类一起免费化（c_top3）= **+8.99%**（noexp 1778 → 1933 TFLOPS），即"softmax 计算全免"的天花板。
从含 exp 的最优正确版（≈1548 TFLOPS）算起：去 exp(+15%) + 去 softmax 全部计算(+9%) ≈ **+24.9%** 上限（1933 TFLOPS）；
再往上就进入访存 / MMA bound 区间，softmax 算力已不是限制因素。

**不值得优化的两类**：burst scale FMA（FFMA2，Δ≈0，FP 管有余量）、rescale exp2（MUFU.EX2，隐藏在空闲 XU 管上，强行换到 FP 管反而变慢）。

---

## 7. 完整数据汇总（一张表）

基线 noexp ≈ 1778 TFLOPS（shape-170100，锁频 1965，GPU 空载）。

| 探针 | 中和指令类(计数) | probe 中位 TFLOPS | Δ vs noexp | 角色 |
|---|---|---:|---:|---|
| fmax | FMNMX3 34→1 | 1848.75 | **+4.04%** | 新主瓶颈 |
| convert | F2FP 96→64 | 1832.53 | **+2.91%** | 第二 |
| fmul | FMUL2 128→0 | 1809.84 | **+1.85%** | 第三 |
| ffma | FFMA2 32→0 | 1777.47 | +0.00% | 非瓶颈 |
| mufu | MUFU.EX2 2→0 | 1718.03 | −3.37% | 非瓶颈（替换副作用） |
| c_top2 | fmax+convert | 1903.91 | +6.88% | 加性（独立） |
| **c_top3** | fmax+convert+fmul | **1933.22** | **+8.99%** | **softmax 计算天花板** |
| C-all | 全 5 个 | 1868.68 | +5.12% | 被 mufu 副作用拖低 |

---

## 8. 复现方式

所有探针 target 都在 `b200_blackwell_fmha_mxfp8/CMakeLists.txt`（`add_noexp_ablation` 函数）；不定义任何 `MXFP8_ABLATE_*` / `MXFP8_NOEXP_FIRST2` / `MXFP8_SOFTMAX_ONEADD` 宏时，与 noexp 基线**位级等价**。

```bash
cd <FA>/build   # 顶层 CMakeLists 所在的 build 目录

# 编译某个探针（target 后缀 <X> 见下表）
cmake --build . --target op6static2sm_noexp_abl_<X>_b200_blackwell_fmha_mxfp8 -j
#   <X> = convert / fmul / ffma / fmax / mufu / fadd         (粒度1：单指令类)
#         c_top2 / c_top3 / all                               (粒度2：组合)
#         softmax1add                                         (整段 softmax → 一次加法, §10)
#         softmax1add_notmem                                  (softmax1add + 去 LDTM copy + STTM, §15)
#         noexp_nosf                                          (noexp + 去全部 SF SMEM->TMEM UTCCP, §16)
#         first2                                              (exp→add 但只加前 2 个元素, §12)
#         first2_max / first2_rsum / first2_conv              (first2 之上单项, §12)
#         first2_max_rsum                                     (first2 之上组合, §12)

# 运行（性能度量，勿加 --verify，输出数值是错的）。以"softmax 整体换成 add"为例：
./b200_blackwell_fmha_mxfp8/op6static2sm_noexp_abl_softmax1add_b200_blackwell_fmha_mxfp8 \
  --b=1 --h=40 --h_k=40 --q=170100 --k=170100 --d=128 --mask=no \
  --warmup_iterations=5 --iterations=20
# 加 --verbose 可看单 kernel GPU 时间 + 共享内存，例如：
#   [--] tma ws n128 acc fp32 individual  : 2355.61 TFLOPS/s
#         t=251.4ms, smem=179632b
# 换 510300 长序列同理：把 --q/--k 改成 510300。

# SASS 抽查（NoMask fwd kernel 的目标指令类计数）
cuobjdump --dump-sass <binary> | \
  awk '/Function :/{f=0} /Sm100FmhaFwdKernel.*NoMask/&&!/VariableLength/{f=1} f' | \
  grep -c FMNMX3
```

基准脚本：`build/bench_ablation.sh`（交错 A/B vs noexp，含每探针 SASS 计数）。
测试前锁频 + 确认 GPU 空载：`nvidia-smi -lgc 1965,1965`、`nvidia-smi --query-compute-apps=pid --format=csv,noheader`。

---

## 9. 局限与注意事项

- **P1（convert）不完整**：只中和了 96 个 F2FP 中的 32 个（P 量化那部分），HFMA2(51) 未被隔离（与该转换器不同源）。
  如需完整 F2FP 天花板，需另中和 epilogue 的 O→output 转换（行 3538-3549）。
- **P4（fmax）是"rowmax pass"天花板**：折叠扫描去掉了 ~63 个 fmax，但连同该 pass 的寄存器读也一并减少；
  实测值是该 pass 的上限（以 fmax 算术为主，寄存器读本身不单独计指令）。
- **P5（mufu）的负 Δ 是替换副作用**，不是"该指令是反向瓶颈"——正确读法是 rescale MUFU 隐藏在空闲管上、非瓶颈、不该动。
- 探针都会触发 ptxas 一定程度的全局重排程，所以 Δ 是"略保守的天花板"（真实指令代价 ≥ 测得 Δ）。每个探针都做了 SASS 抽查确保没意外连累别的类。
- 全程仅测 shape-170100。510300 更偏访存 bound，绝对 Δ 会更小，但**排序预期一致**；如需可在 510300 复测 top-1/top-3 确认。
- 8 个消融 target 与 5 个 `MXFP8_ABLATE_*` 宏均保留在工作树的 `CMakeLists.txt` / 主循环 hpp 里，可复跑。

---

## 10. 终极探针：整个 softmax → 一次加法（`MXFP8_SOFTMAX_ONEADD`）

### 10.1 做法
把 softmax 的**全部计算**掏空，只保留"从 TMEM load 数据 + 一次加法"。具体（宏 `MXFP8_SOFTMAX_ONEADD`，单文件改动）：
- LDTM 之后插一次加法 `tTMEM_LOADrS(0) + tTMEM_LOADrS(1)`（`asm volatile` 防 DCE；LDTM 是单个 atom，不会被裁剪）。
- rowmax 归约 → `row_max = 0.0f`（chain 段的 order_s 握手保留；fmax 因 row_max=0 折叠）。
- exp/add burst 循环 → `continue`（整段跳过；循环内无握手）。
- row_sum 累积循环 → `continue`（row_sum → 0）。
- 量化复用 `MXFP8_ABLATE_CONVERT`（写常量 e4m3 P）。
- **保留**所有 pipeline 握手（`pipeline_s`/`pipeline_c`/`order_s`/`B_FINAL`）、P 与 V-stats 的 STTM、fence、release、wrapper、PSTATIC SFP 填充——保证 PV-MMA/correction 拿到全部信号，**不死锁**。`row_max=0` 使 rescale 三元式走 `0.5f` 分支，无 NaN/Inf。

target：`op6static2sm_noexp_abl_softmax1add_b200_blackwell_fmha_mxfp8`（= noexp DEFS + `MXFP8_SOFTMAX_ONEADD` + `MXFP8_ABLATE_CONVERT`）。

### 10.2 SASS 审计（NoMask fwd kernel）
| 指令类 | noexp | softmax1add | 说明 |
|---|---:|---:|---|
| FADD2 | 67 | **3** | softmax 的 burst/row_sum 加法全去 |
| FFMA2 | 32 | **0** | burst scale FMA 全去 |
| FMNMX3 | 34 | **0** | rowmax 归约 + chain fmax 全去（折叠为 0） |
| F2FP | 96 | 64 | P 量化那 32 个去掉（同 ABLATE_CONVERT） |
| MUFU.EX2 | 2 | 2 | 剩下的是 correction 的 O-rescale（非 softmax） |
| 握手 UCGABAR/UTCCP/FENCE/STTM | 2/12/15/11 | 2/12/15/11 | **完全保留** → 不死锁（冒烟 1-iter 正常退出） |

softmax 的计算确实被掏空，握手/STTM/LDTM 结构完整保留。

### 10.3 实测（4 样本交错 A/B vs noexp，shape-170100，锁频 1965）
| 版本 | 4 样本 TFLOPS/s | 中位 | 单 kernel |
|---|---|---:|---:|
| noexp | 1784.71, 1784.03, 1776.81, 1779.20 | 1781.62 | ~166 ms |
| **softmax1add** | 2356.54, 2351.17, 2348.49, 2343.91 | **2349.83** | ~126 ms |
| **Δ** | — | **+31.89%** | **−40 ms** |

样本极紧凑、两版完全不交叠。

### 10.4 关键发现：指令代价 ≠ 关键路径代价
对照同文件的粒度-1/2 结果：

| 探针 | Δ vs noexp |
|---|---:|
| c_top3（去掉 rowmax+量化+O-norm 的**指令**，但保留 burst/row_sum **循环结构**） | +8.99% |
| **softmax1add（整个 softmax → 一次加法）** | **+31.89%** |

差距巨大（31.89% ≫ 8.99%）。原因：c_top3 只把循环里的**每条指令**变便宜，循环还在跑、关键路径延迟没变；softmax1add 把循环整段跳过 → 从 S-ready 到 P-ready（PV-MMA 等的那个 `consumer_release`）的时间骤降 → PV-MMA 每 tile 提早启动 → 大幅提速。

**结论：softmax 的瓶颈不是它的指令条数，而是它的端到端关键路径延迟**（rowmax→burst→量化→release 这条串到 PV-MMA 的链）。逐类消融（粒度1/2）只能看到指令代价（~+9%），整段掏空才暴露真正的杠杆是**缩短 time-to-P-ready**（+32%）。

### 10.5 全程台阶（shape-170100，中位 TFLOPS）
| 版本 | TFLOPS | 相对 |
|---|---:|---|
| 最优正确版（含 exp） | 1548.6 | 基线 |
| noexp（去 exp） | 1781.6 | +14.95% |
| c_top3（去 softmax 主要计算指令，循环保留） | 1933.2 | +8.99% vs noexp |
| **softmax1add（softmax → 一次加法）** | **2349.8** | **+31.89% vs noexp / +51.7% vs 含 exp** |

到 softmax1add（~2350 TFLOPS）时 softmax 已不构成瓶颈，kernel 进入 MMA/访存 bound 区间。

---

## 11. 补充探针：FADD2（之前逐类消融漏掉的最大一类）

> 起因：复查"逐类消融是否覆盖 softmax 全部指令"时发现 FADD2（noexp 里 67 条，softmax 占 64 条，**是 softmax 最大的一类计算指令**）从未被单独探针过。本节补上。

### 11.1 FADD2 在 softmax 里到底用在哪（行号 = 当前源码）
noexp 的 67 个 FADD2 来自 `softmax_step` 里**两处功能**，加起来正好 67：

| 用途 | 行号 | 数量 | 说明 |
|---|---|---:|---|
| **exp 替代加**（NOEXP 把 exp2 换成的那个加法） | 2417 `cute::add(exp_out, out, row_max_safe_fp32x2)` 在 burst 循环里 | 32 | 每元素对一次 `out + row_max_safe`（即 noexp 下的"softmax 权重"），32 对 = 32 个 FADD2 |
| **row_sum 累加**（配分函数 ΣP） | 2697/2700/2703/2706（循环内 4 个累加器 × 8 轮）+ 2709/2710/2711（3 个跨累加器合并） | 35 | 把 64 个 P 值求和成 row_sum |
| 其余（非 softmax） | — | 3 | correction 等处 |

即 **FADD2 = "算 softmax 权重"（32）+ "算配分函数 row_sum"（35）**。这也解释了为什么 noexp 已经去掉 exp、却仍有 67 个 FADD2：exp 被换成了一个加法，而且 row_sum 的求和本来就是加法。

### 11.2 探针设计（`MXFP8_ABLATE_FADD`）
- 行 2417 的 burst 加 → `exp_out = out`（identity）：去掉这 32 个 FADD2，但**保留 `out`（scale*s 的 FFMA2）和 load**——只删加法，不动 FMA。
- row_sum 累加循环 → `continue`（复用 ONEADD 那个 gate，`#if ONEADD || ABLATE_FADD`）：累加器保持 0，3 个合并 `add(0,0)` 被编译器折叠 → 这 35 个 FADD2 也去掉（row_sum→0，finite，不挂）。

### 11.3 SASS 抽查（NoMask fwd kernel）—— 干净隔离
| 指令类 | noexp | abl_fadd | Δ |
|---|---:|---:|---:|
| **FADD2** | 67 | **3** | **−64（softmax 的 FADD2 全去）** |
| FFMA2 | 32 | 32 | 0（burst scale FMA 保留）|
| FMNMX3 | 34 | 34 | 0（rowmax 归约保留）|
| F2FP | 96 | 96 | 0（量化保留）|
| LDTM | 19 | 19 | 0（S 载入保留）|

只动了 FADD2，其余类纹丝不动 —— 比 P1/P4 更干净的隔离。

### 11.4 实测（4 样本交错 A/B vs noexp，shape-170100，锁频 1965）
| 版本 | 4 样本 TFLOPS/s | 中位 | Δ vs noexp |
|---|---|---:|---:|
| noexp | 1785.22, 1776.45, 1778.94, 1782.59 | 1780.76 | — |
| **abl_fadd** | 1942.83, 1940.22, 1941.40, 1941.77 | **1941.59** | **+9.03%** |

样本极紧凑、两版完全不交叠。

### 11.5 更新后的单类排序（粒度 1）
| 排名 | 指令类 | Δ vs noexp |
|---|---|---:|
| 🥇 1 | **FADD2（exp 替代加 + row_sum 累加）** | **+9.03%** |
| 🥈 2 | FMNMX3（rowmax 归约） | +4.04% |
| 🥉 3 | F2FP（P 量化，32/96） | +2.91% |
| 4 | FMUL2（O-norm，**实为 correction 非 softmax**） | +1.85% |
| 5 | FFMA2（burst scale FMA） | +0.00% |
| 6 | MUFU.EX2（rescale，替换副作用） | −3.37% |

**FADD2 是单类第一瓶颈**。注意两点：
- FADD2 单类（+9.03%）≈ c_top3 三类之和（fmax+convert+fmul = +8.99%）—— 一类顶三类。
- 但 FADD2（+9%）仍只占 softmax 总代价（softmax1add +31.9%）的约 28%。**最大的单类指令也只解释了不到 1/3 的 softmax 开销**，再次印证：softmax 的主导成本是端到端关键路径延迟（第 10 节），不是任何单一指令类的条数。

### 11.6 FADD2 是 add 替代 exp 的产物吗？（带 exp vs noexp 的 SASS 对照）

一半是，一半不是。67 个 FADD2 拆成两段：

| 指令类 | 带 exp 基线 | noexp | Δ | 含义 |
|---|---:|---:|---:|---|
| MUFU.EX2 | 54 | 2 | −52 | burst 的 exp2（26 原生 pair×2 + rescale）被去掉 |
| FFMA2 | 50 | 32 | −18 | 6 个模拟 pair 的多项式 FMA 被去掉 |
| FADD2 | 53 | 67 | +14 | 见下 |

- **burst 里那 32 个 FADD2 —— 是的**：带 exp 时这些位置是 `MUFU.EX2`（硬件 exp2），noexp 把整段 exp 换成 `out + row_max_safe`，新产生 32 个普通 FADD2。FADD 消融去掉这 32 个，本质就是把"替代 exp 的那个加法"也撤掉。
- **row_sum 里那 35 个 FADD2 —— 不是**：配分函数 ΣP 的累加，带 exp（累加 exp2(P)）和不带 exp（累加 P+max）**都是 35 个 FADD2**，跟 exp 替换无关。
- 净变化（FADD2 +14）：noexp 去掉 18 个模拟-poly 的加减法、新增 32 个 burst 加法 → +14。

**读法**：去 exp（+15%）把瓶颈从 MUFU.EX2（XU 管）挪到了 FADD2（FP 管）；FADD2 比 exp2 便宜得多（故净赚 +15%），但它本身仍是单类第一。exp 不是消失了，而是换了个更便宜的形态（FADD2）继续占着 softmax 单类头把交椅。

---

## 12. `first2` 探针：exp→add 但只对前两个元素加

### 12.1 动机与做法
noexp 把 exp 换成 `out + row_max_safe`，但**对全部 64 个元素**都做（32 个 FADD2），给 FP 管加了压。宏 `MXFP8_NOEXP_FIRST2`：burst 循环里 `if (i==0) exp_out = out + row_max_safe; else exp_out = out`——只对第 0 个元素对做加法，其余 31 对跳过。循环是 `#pragma unroll`，`i==0` 编译期折叠，**只发 1 条 FADD2**。其余（FMA、row_sum、convert、rowmax）一律不动。

### 12.2 SASS + 实测（vs noexp）
| 版本 | FADD2 | FFMA2 | FMNMX3 | F2FP | 中位 TFLOPS | Δ vs noexp |
|---|---:|---:|---:|---:|---:|---:|
| noexp | 67 | 32 | 34 | 96 | 1780 | — |
| **first2** | **36** | 32 | 34 | 96 | **1826** | **+2.58%** |

FADD2 67→36（burst 加法 32→1，row_sum 35 保留），其余不变。把 32 个 burst 加法砍到 1 个，快 **+2.58%**——确认"对全部数据做 add 会加剧 FP 管压力"，但幅度有限。

### 12.3 在 first2 之上再消融（base = first2，4 样本交错 A/B）
| 探针 | 去掉的类（SASS 印证） | 中位 TFLOPS | Δ vs first2 |
|---|---|---:|---:|
| first2_max | rowmax FMNMX3 34→1 | 1886 | +3.18% |
| first2_rsum | row_sum FADD2 36→4 | 1948 | **+6.63%** |
| first2_conv | P 量化 F2FP 96→64 | 1926 | +5.57% |
| **first2_max_rsum** | rowmax + row_sum（FMNMX3 34→1, FADD2 36→4） | **2003** | **+9.80%** |

（宏：`MXFP8_ABLATE_FMAX` / `MXFP8_ABLATE_RSUM` / `MXFP8_ABLATE_CONVERT`；`MXFP8_ABLATE_RSUM` 是为本次新加的"只跳 row_sum、保留 burst"的宏，与 `MXFP8_ABLATE_FADD` 区分。）

### 12.4 first2-base 的排序变了
| 排名 | noexp-base（32 个 burst add 还在） | first2-base（1 个 burst add） |
|---|---|---|
| 1 | FADD2 burst-add +9% | **row_sum +6.6%** |
| 2 | rowmax +4% | convert +5.6% |
| 3 | convert +2.9% | rowmax +3.2% |

把 burst 的 32 个 add 砍到 1 个后，**row_sum 累加成了第一**（之前被 burst add 盖住）。这与 §11 的发现一致：row_sum 的 35 个累加比 burst 的 32 个加法更贵。

### 12.5 max+rsum 组合的加性
first2_max(+3.18%) + first2_rsum(+6.63%) = +9.81%；组合 first2_max_rsum = **+9.80%**。**完全加性**。加性的正确解读见 §14（**不是**"不同管/不同链"，而是"同一关键路径上不重叠的串行段相加"）。

### 12.6 全程台阶（中位 TFLOPS）
```
noexp        (32 burst add)     1780
first2       (1 burst add)      1826
first2_max   (+去 rowmax)        1886
first2_conv  (+去量化)           1926
first2_rsum  (+去 row_sum)       1948
first2_max_rsum (去 max+rsum)   2003   ← 本组最高
softmax1add  (整段 softmax 掏空)  2350
```

---

## 13. softmax1add 的 HBM 带宽利用率

公式：`(输入 + 输出) / 时延 / 8 TB/s`。

### 13.1 数据量（手算，不含缩放因子）
每张量元素数 = B×H×seq×D = 1×40×170100×128 = 5120×170100 = **870,912,000**。

| 张量 | 类型 | 字节 |
|---|---|---:|
| Q | e4m3 (1 B) | 870,912,000 |
| K | e4m3 (1 B) | 870,912,000 |
| V | e4m3 (1 B) | 870,912,000 |
| O | fp16 (2 B) | 1,741,824,000 |
| **合计（输入+输出）** | | **4,354,560,000 B = 4.355 GB** |

（Q/K/V 的 e8m0 缩放因子共 ~0.082 GB，<2%，不计入；算进去结论也不变。）

### 13.2 时延
softmax1add `--verbose` 实测单 kernel GPU 时间 **t = 251.4 ms**（noexp 对照 330.9 ms）。

### 13.3 带宽与利用率
```
带宽 = 4,354,560,000 B / 0.2514 s ≈ 17.32 GB/s = 0.0173 TB/s
利用率 = 0.0173 / 8 = 0.22%
```
（noexp 对照：4.355 GB / 0.3309 s ≈ 13.2 GB/s → 0.17%。）

### 13.4 结论
softmax1add 的 HBM 带宽利用率仅 **~0.22%**。算术强度 ≈ 5.9e14 FLOPs / 4.36e9 B ≈ **13.6 万 FLOP/byte**，远超 B200 脊点（~281 FLOP/byte）→ **完全是算力 bound（MMA bound，~2350 TFLOPS 逼近 fp8 峰值）**，HBM 带宽几乎没吃。把 softmax 换成 add 后，瓶颈纯粹是 tensor core 算力，不是带宽。

---

## 14. 硬件资源归属 & "加性"的正确含义

### 14.1 max 和 row_sum 用的是同一类资源：FP32 FMA 管
| 操作 | 指令 | 硬件管 |
|---|---|---|
| max（rowmax 归约） | FMNMX3（fp32 融合 min/max） | **FP32 FMA 管** |
| row_sum（累加） | FADD2（打包 fp32 加） | **FP32 FMA 管**（同上） |

NVIDIA（Ampere/Hopper/Blackwell）**没有专门的 min/max 管**——FMNMX 与 FADD/FFMA/FMUL 共用 FP32 FMA 单元，竞争同一份 FP32 issue/执行吞吐。对比（这些才是不同的管）：
- MUFU.EX2 → **XU / 特殊函数管**；
- tcgen05 MMA（UTCQMMA 等）→ **tensor core**（独立于 FP FMA 管）；
- IMAD / SHF / IADD3（地址/索引整数）→ **ALU / 整数管**。

### 14.2 它们其实在同一条串行链上
数据依赖决定顺序：
```
rowmax(FMNMX3) → row_max → burst FMA → P 写回 tTMEM_LOADrS → row_sum(FADD2) 读 P
```
row_sum 累加的是 P，而 P = f(scale·s − row_max)，所以 **row_sum 依赖 burst，burst 依赖 row_max** → max 和 row_sum **串行、同链**。（倒是 row_sum 与 convert 并行：两者都读 burst 的输出、彼此无关。）

### 14.3 纠正：加性 ≠ 不同管 / 不同链
之前"组合加性 → 它们在不同并行管 / 不同串行链"的推论**是错的过推**。正确含义：

- 内核时间 ≈ softmax warp 从"S 就绪"到"发 P-ready release"的完成时间（这条链 gate 着下游 PV-MMA 启动）。
- max 和 row_sum 是这条链上**不重叠的不同段**。去掉各段，链的完成时间各缩短自己的延迟，**缩短量直接相加** → 组合 Δ = 单项之和（加性）。
- 这与"同管 / 同链"**完全相容**：无论按"串行段延迟相加"还是"同一 FP 管指令吞吐相加"，都是加性。
- 加性只有在"去掉某段后瓶颈转移到别处"时才被打破（变亚加性）。max+rsum 还没到那个拐点。

一句话：**max 和 row_sum 同用 FP32 FMA 管、且在同一条串行链上的不同段；加性来自各段缩短不重叠，不来自资源分离。**

### 14.4 想验证"FP 管是不是 binding"？
做一个探针：在 first2+max+rsum 之上**再去掉 burst 的 FMA（FFMA2）**。若 Δ 很小 → FP 管已非 binding（瓶颈转到关键路径延迟 / convert / MMA）；若 Δ 很大 → FP 管仍是 binding。（未做，留作后续。）

---

## 15. 在 softmax1add 之上再去掉 TMEM load/store（`MXFP8_DROP_TMEM_LDST`）

### 15.1 目的
softmax1add 已经把 softmax 压成"一次加法"。本探针再进一步：**去掉 QK 算完之后的 TMEM load（softmax 读 S 的 LDTM copy）和 softmax 写 P 的 TMEM store（STTM copy）**，但**流水握手保持**（看这两步数据搬运还占多少）。宏 `MXFP8_DROP_TMEM_LDST`，叠加在 softmax1add 之上（SOFTMAX_ONEADD + ABLATE_CONVERT + NOEXP_ADD）。

### 15.2 做法（源码 3 处，单文件）
1. **LDTM copy（S 载入）显式 gate**：`copy(tiled_tmem_load, tTMEM_LOADtS, tTMEM_LOADrS)` 包在 `#if !defined(MXFP8_DROP_TMEM_LDST)` 里。
2. **add 的源改用非 LDTM 数据**：`float _oneadd = params.scale_softmax_log2 + params.scale_softmax_log2;`（运行时常量，不可折叠，且不读 tTMEM_LOADrS）→ 整个 tTMEM_LOADrS 无人用。
3. **STTM copy（P 写回）显式 gate**：`copy(tiled_tmem_store, tTMEM_STORErS_x4, tTMEM_STOREtS_x4)` 包在 `#if !defined(MXFP8_DROP_TMEM_LDST)` 里；**但 `fence_view_async_tmem_store()` 和 `pipeline_s.consumer_release(...)` 原样保留**——PV MMA 仍等这个 release，而 release 又等 QK MMA 的 commit，**所以"第二个 MMA 在第一个之后"的顺序不变**。PV MMA 的 A 操作数（TMEM P0/P1）变成陈旧的，但 tcgen05 MMA 是硬件黑盒、输出流向 HBM，不会被 DCE。

> 一句话：只删两条数据搬运 `copy`，握手（acquire/wait/release）一个不动。

### 15.3 SASS 护栏（NoMask fwd kernel）
| 指令 | softmax1add | notmem | 结论 |
|---|---:|---:|---|
| **UTCQMMA（tensor-core MMA）** | 16 | **16** | **MMA 没被优化掉** ✓ |
| **STTM（P-store）** | 11 | **10** | P-store 真去掉了 ✓ |
| LDTM | 18 | 18 | 见 15.4 |
| exit（60s 超时） | 0 | **0** | **不挂死** ✓（fence+release 保序，PV MMA 读陈旧 TMEM 照常跑）|

### 15.4 关键发现：LDTM 早就被编译器优化了，STTM 才需要手动 gate
- **S-load LDTM：softmax1add 里已经被编译器 DCE 了**。原因：softmax1add 的 add 只用 64 个元素里的 2 个（`tTMEM_LOADrS(0)`、`(1)`），编译器判定全 64 元素的 tile load 没必要，自己删了（noexp LDTM=19 → softmax1add LDTM=18，少的那 1 个就是 S-load）。所以本探针对 LDTM copy 的显式 gate 只额外清掉了它的**残余地址计算**（IMAD −9、MOV −5、HFMA2 −4），LDTM 计数不变。
  - 剩下的 18 个 LDTM（`LDTM.x16` 为主）是 **correction warpgroup 读 O** 做规范化/rescale 用的，是内核必需、不能去（去了就没 O 输出）。
- **P-store STTM：没被编译器优化**（TMEM store 有副作用，编译器不删），**必须显式 gate**（11→10）。这是本探针收益的主要来源。
- 教训：**"让 add 不用 LDTM 数据 → 编译器优化掉 LDTM"这个预期对 S-load 不成立——它在 softmax1add 里早就被优化了；真正需要手动干预的是 STTM。**

### 15.5 实测（4 样本交错 A/B vs softmax1add，shape-170100，锁频 1965）
| 版本 | 4 样本 TFLOPS/s | 中位 | Δ |
|---|---|---:|---:|
| softmax1add | 2349.11, 2350.75, 2351.65, 2344.12 | 2349.93 | — |
| **softmax1add_notmem** | 2520.89, 2516.16, 2513.13, 2508.98 | **2514.64** | **+7.01%** |

样本极紧凑、两版完全不交叠。

### 15.6 全程台阶（中位 TFLOPS）
```
noexp        (完整 softmax 计算)         1780
softmax1add  (softmax→1 add)             2350   +32% vs noexp
softmax1add_notmem (+去 LDTM copy + STTM) 2515   +7% vs softmax1add / +41% vs noexp
```
到 notmem（~2515 TFLOPS）时，softmax 的计算 + 它与 MMA 之间的 TMEM 数据搬运都几乎去掉，内核基本只剩 MMA + correction，逼近 fp8 算力上限。**S-load 本来就几乎免费（早被编译器吃掉），+7% 主要来自去掉 P-store STTM + 两步的残余地址计算。**

### 15.7 产物
- 宏 `MXFP8_DROP_TMEM_LDST`（gate LDTM copy + STTM copy；fence/release 保留）。
- target `op6static2sm_noexp_abl_softmax1add_notmem_b200_blackwell_fmha_mxfp8`（CMakeLists，可复跑）。

---

## 16. 在 noexp 之上再去掉 SF（缩放因子）SMEM→TMEM 搬运（`MXFP8_DROP_SF_UTCCP`）

### 16.1 目的
MXFP8 的 block-scaled MMA 需要 e8m0 缩放因子（SF）。SF 流向：HBM → SMEM（TMA）→ **SMEM → TMEM（`tcgen05.cp` / UTCCP）** → tensor core 的 MMA 用它做 block-scaling。本探针在**仅去 exp 的 noexp 基线**上，把这条 **SF SMEM→TMEM 搬运**整体去掉，看它占多少。注意：这跟 §15 不同——§15 去的是 softmax↔TMEM 的数据搬运（LDTM 读 S、STTM 写 P），本节去的是 **SF↔TMEM 的搬运**（给 MMA 用的 SF）。

共 4 个 SF 流，对应 4 个 `load_sf*` lambda（7 条 `copy(utccp_*)`）：
| SF | 源（SMEM）| 目的（TMEM）| 服务 | 调用 |
|---|---|---|---|---|
| SFA | smem_sfq（Q-SF）| SFA0 | QK MMA（Q 端，一次性）| `load_sfa()` |
| SFB | smem_sfk（K-SF）| SFB0/SFB1 | QK MMA（K 端，每 tile）| `load_sfb(kidx,buf)` |
| SFP | smem_sfp（P-SF）| SFP0/SFP1 | PV MMA（P 端，每 tile）| `load_sfp(buf)` |
| SFV | smem_sfv（V-SF）| SFV0/SFV1 | PV MMA（V 端，每 tile）| `load_sfv(vidx,buf)` |

### 16.2 做法（宏 `MXFP8_DROP_SF_UTCCP`，单文件 4 处 gate）
- `load_sfa`、`load_sfp`：body 包进 `#if !defined(MXFP8_DROP_SF_UTCCP)`（原本未 gate）。
- `load_sfb`、`load_sfv`：原本已有独立 gate（`MXFP8_DROP_QK_SFB_UTCCP` / `MXFP8_DROP_PV_SFV_UTCCP`），加上 `&& !defined(MXFP8_DROP_SF_UTCCP)` 让总开关也覆盖它们。
- 结果：4 个 SF 流的 `copy(utccp_*)` 全部不发射；MMA 仍发（读陈旧/垃圾 SF TMEM，输出乱，perf-only）；pipeline 握手不动。

### 16.3 SASS 护栏（NoMask fwd kernel）
| 指令 | noexp | noexp_nosf | 结论 |
|---|---:|---:|---|
| **UTCCP（SF 搬运）** | 6 | **0** | SF SMEM→TMEM 全去 ✓ |
| **UTCQMMA（tensor-core MMA）** | 16 | **16** | **MMA 没被优化掉** ✓ |
| exit（60s 超时） | 0 | **0** | 不挂死 ✓（SF TMEM 陈旧，MMA 照常跑）|

### 16.4 实测（4 样本交错 A/B vs noexp，shape-170100，锁频 1965）
| 版本 | 4 样本 TFLOPS/s | 中位 | Δ vs noexp |
|---|---|---:|---:|
| noexp | 1787.55, 1778.91, 1775.13, 1771.80 | 1777.02 | — |
| **noexp_nosf** | 2292.29, 2286.87, 2283.45, 2280.60 | **2285.16** | **+28.60%** |

样本极紧凑、两版完全不交叠。

### 16.5 重大发现：SF 搬运是与 softmax 同量级的瓶颈
把本探针放进整个台阶看：

| 探针 | 去掉了什么 | 中位 TFLOPS | Δ vs noexp |
|---|---|---:|---:|
| noexp | —（基线）| 1777 | — |
| **noexp_nosf** | **仅去 6 条 SF UTCCP 搬运**（softmax 全保留）| **2285** | **+28.6%** |
| softmax1add | 整个 softmax 计算掏空（SF 全保留）| 2350 | +31.9% |

**去掉 6 条 SF 搬运指令（+28.6%）≈ 掏空整个 softmax 计算（+31.9%）。** 即 **block-scaled MMA 的 SF 从 SMEM 搬到 TMEM 这一步，本身就是与 softmax 同量级的主导瓶颈**——此前目光一直集中在 softmax，但 SF staging 的代价几乎一样大。

### 16.6 为什么这么贵
UTCCP（`tcgen05.cp`，SF 搬运）和 MMA（`tcgen05.mma`）**共用 tcgen05 / TMEM 端口资源**——源码注释（CMakeLists 上方 mainloop L1253 附近）明说"1-CTA UTCCP 在 2-SM MMA 下会死锁"，印证两者有严格的资源/顺序依赖。每个 tile 要搬 SFB/SFP/SFV 等 512B 量级 SF 进 TMEM，这段搬运与 MMA **串行竞争同一端口** → 每 tile 都拖长 MMA 启动。去掉后 MMA 几乎可连发 → 大幅提速。

### 16.7 含义
- 这解释了为什么 softmax1add（2350，去 softmax、留 SF）只比 noexp_nosf（2285，去 SF、留 softmax）高一点点：**softmax 计算和 SF 搬运是两条几乎独立的大瓶颈**，各自都能拿到 ~+30%。
- 若**同时**去掉两者（`softmax1add + DROP_SF_UTCCP`），预期可叠到 ~+40% 以上，逼近 fp8 峰值。（未做，留作后续。）
- 优化启示：除了继续压 softmax，**SF staging（UTCCP 与 MMA 的端口竞争）是同等重要的另一条优化主线**——例如减少 SF 搬运次数、把 SF staging 与 MMA 更深地重叠、或用更省 SF 的格式。

### 16.8 产物
- 宏 `MXFP8_DROP_SF_UTCCP`（gate 4 个 `load_sf*`：SFA/SFB/SFP/SFV）。
- target `op6static2sm_noexp_abl_noexp_nosf_b200_blackwell_fmha_mxfp8`（CMakeLists，可复跑）。

---

## 17. max / row_sum 消融：带 exp vs 去 exp，两个 shape

**问**：去掉 max（rowmax 归约）和 row_sum（ΣP 累加）各自能省多少？在**带 exp**（完整 softmax）和**去 exp**（noexp）两个基线上分别测，看 exp 在不在时它们的相对代价。

### 17.1 结果（Δ% = 去掉该项相对各自基线）
| 去掉 | 带exp·170100 | 带exp·510300 | 去exp·170100 | 去exp·510300 |
|---|---:|---:|---:|---:|
| **max**（FMNMX3 归约） | +2.37% | +0.94% | +3.68% | +4.22% |
| **row_sum**（FADD2 累加） | +1.85% | +0.19% | **+6.16%** | **+7.23%** |

（基线：带exp 170100=1558.6 / 510300=1523.1；去exp 170100=1784.0 / 510300=1687.0 TFLOPS。target：`fa4_abl_{fmax,rsum}`（带exp）、`noexp_abl_{fmax,rsum}`（去exp）。）

### 17.2 三个发现
1. **带 exp 时 max/row_sum 都是小头**（≤2.4%，510300 上甚至 ~0-1%）——**exp 是 softmax 的绝对主导**，有 exp 时 max/row_sum 被盖住，去掉几乎不提速。
2. **去掉 exp 后 max/row_sum 立刻变大**（max +3.7-4.2%、row_sum +6.2-7.2%）——它们的**绝对代价固定**（指令数不变），但**相对影响取决于 softmax 里还剩什么**：exp 在时是小头，exp 一去就成了下一档瓶颈。
3. **去 exp 后 row_sum > max**（6-7% vs 4%）——row_sum（35 个 FADD2 累加）比 max（34 个 FMNMX3 归约）更贵，与 §11（FADD2 是去 exp 后的单类第一）一致。

### 17.3 跨 shape 趋势
- **去 exp 基线**：max/row_sum 的影响在 510300 上**略增**（max 3.68→4.22%、row_sum 6.16→7.23%）——序列更长、tile 更多，轻量 softmax 逐 tile gate 着 PV-MMA 的开销累积更明显。
- **带 exp 基线**：影响在 510300 上**缩到接近 0**（max 2.37→0.94%、row_sum 1.85→0.19%）——长序列更偏访存 bound，且 exp 占了 softmax 大头，max/row_sum 占比被进一步稀释。

### 17.4 含义
在真实带 exp 内核里优化 max/row_sum 收益很小（≤2.4%）；真正的杠杆还是 **exp 本身**。只有当 exp 被去掉（或大幅压低）后，max（~4%）和 row_sum（~6-7%）才成为下一档值得攻的瓶颈，其中 **row_sum 优先级高于 max**。

---

## 18. noexp 之上加 double-O（depth-2 O pipeline）

**问**：去 exp 后，如果把 O 累加器 pipeline 从 depth-1（单 O）改成 depth-2（double-O，让 PV(t) 不必等 correction 读完 O(t-1) 就能启动），能提多少？

### 18.1 做法
用现成的 `MXFP8_ODEPTH2`：把 `PipelineO` 从 `PipelineUmmaAsync<1>` 改成 `<2>`，使 PV 的 `producer_acquire(k)` 等 `release(k-2)` 而非 `(k-1)` → PV 提前一步启动。这是 **handshake 版**（仍在单一物理 O0 上，PV(t)/correction(t-1) 有 WAR race，~1% 错值，perf-only）。target：`op6static2sm_noexp_abl_noexp_odepth2_...`（= NOEXP_DEFS + `MXFP8_ODEPTH2`）。

> 说明：用户原话是"将第二个 O 放在缩放因子的位置"（真实第二 O buffer，覆盖 SF）。那需要把 O-parity 贯穿 PV-write×2 + correction-read + OLAG + epilogue（多处硬编码 O0）——大型多点手术。本节先用 handshake 版量出 double-O 的性能量级（两者都靠 depth-2 解锁 PV，性能同量级；handshake 因 race 略保守）。

### 18.2 实测（锁频 1965）
| shape | noexp 中位 | +double-O 中位 | Δ |
|---|---:|---:|---:|
| 170100 | 1784.03 | **1909.39** | **+7.03%** |
| 510300 | 1685.29 | **1783.03** | **+5.80%** |

**对照：带 exp（完整 softmax）上加 double-O**（target `op6static2sm_odepth2_noe2rsf_mufu`，= no_e2rsf_fa4mufu + ODEPTH2）：
| shape | 带exp 中位 | +double-O 中位 | Δ |
|---|---:|---:|---:|
| 170100 | 1553.8 | 1606.6 | **+3.40%** |
| 510300 | 1522.2 | 1544.0 | **+1.43%** |

### 18.3 含义
- double-O 在 noexp 上 **+6-7%**，比带 exp 时的 **+3.4%（170100）/ +1.4%（510300）** 大约**翻倍**——去 exp 后 softmax 变轻，O pipeline（PV→correction）的占比上升，double-O 解锁 PV 的收益更显著。这跟 max/row_sum（§17）是同一规律：固定开销的相对影响随 softmax 变轻而放大。
- 对照 noexp 台阶：noexp 1784 → +double-O 1909（+7%）→ +stage-SF-once 1835（+3%）→ softmax→1add 2350（+32%）。double-O 是仅次于 softmax 计算本身的第二档可观杠杆。
- 长序列 510300 收益略小（两个基线都是）——更偏访存 bound，O pipeline 占比下降。

### 18.4 产物
- target `op6static2sm_noexp_abl_noexp_odepth2_b200_blackwell_fmha_mxfp8`（CMakeLists，可复跑）。
- 若要"真实第二 O 在 SF 位置"的干净版（无 race、O1=O0+kSizeO 覆盖 SF），需 O-parity 多点贯穿，留作后续。

---

## 19. double-O（MXFP8_ODEPTH2）运行方式

### 核心宏
`MXFP8_ODEPTH2`——在 `sm100_fmha_fwd_mainloop_mxfp8_n128_pvmx.hpp` ~934 行生效：
```cpp
#elif defined(MXFP8_ODEPTH2)
    using PipelineO = cutlass::PipelineUmmaAsync<2, typename CollectiveMmaQK::AtomThrShapeMNK>;
```
把 PipelineO 从 depth-1（单 O buffer）改成 depth-2（producer_acquire(k) 等 release(k-2) 而非 (k-1)）。这是 **handshake 版**：仍在单一物理 O0 上，PV(t)/correction(t-1) 有 WAR race（~1% 错值），perf-only。

### 各 double-O 版本的 target / 编译宏 / 二进制

| 版本 | target 后缀 | 额外宏 | 二进制名 |
|---|---|---|---|
| **带 exp + doubleO** | `odepth2_noe2rsf_mufu` | `MXFP8_ODEPTH2`（+ FA4 split） | `op6static2sm_odepth2_noe2rsf_mufu_b200_blackwell_fmha_mxfp8` |
| **noexp + doubleO** | `noexp_abl_noexp_odepth2` | `MXFP8_ODEPTH2`（+ NOEXP_ADD） | `op6static2sm_noexp_abl_noexp_odepth2_b200_blackwell_fmha_mxfp8` |
| **noexp_fma + doubleO** | `noexp_abl_noexp_fma_odepth2` | `MXFP8_NOEXP_FMA MXFP8_ODEPTH2` | `op6static2sm_noexp_abl_noexp_fma_odepth2_b200_blackwell_fmha_mxfp8` |
| **noexp_raw + physical doubleO** | `noexp_abl_noexp_raw_odepth2` | `MXFP8_NOEXP_RAW MXFP8_ODEPTH2` | `op6static2sm_noexp_abl_noexp_raw_odepth2_b200_blackwell_fmha_mxfp8` |
| **保留 exp + physical doubleO** | `fa4_abl_physical_odepth2` | `MXFP8_ODEPTH2 MXFP8_PHYSICAL_ODEPTH2`（FA4 exp 保留） | `op6static2sm_fa4_abl_physical_odepth2_b200_blackwell_fmha_mxfp8` |

### 编译与运行
```bash
cd /home/ubuntu/workspace/oyhj/temp/test/test_merge/Benchmark/FA/build

# 编译（以 noexp_raw + doubleO 为例）
cmake --build . --target op6static2sm_noexp_abl_noexp_raw_odepth2_b200_blackwell_fmha_mxfp8 -j

# 运行（性能度量，勿加 --verify，输出数值是错的）
./b200_blackwell_fmha_mxfp8/op6static2sm_noexp_abl_noexp_raw_odepth2_b200_blackwell_fmha_mxfp8 \
  --b=1 --h=40 --h_k=40 --q=170100 --k=170100 --d=128 --mask=no \
  --warmup_iterations=5 --iterations=20
```

### noexp_raw 物理 double-O（2026-08-12）

物理双缓冲由 `MXFP8_PHYSICAL_ODEPTH2` 控制，并要求同时定义 `MXFP8_ODEPTH2`：

- `O0 = TMEM[256:384)`，`O1 = TMEM[384:512)`，O1 覆盖最后 128 列；
- PV 写入、逐 tile correction 和最终 epilogue 都按同一 `PipelineO::PipelineState::index()` 选择 O0/O1；
- O0/O1 各自在首次使用时清零，后续由各自 pipeline stage 的 release/acquire 保护；
- SF 搬运仍保留，SF 地址继续落在 O1 区域；按本实验约定忽略 SF 与 O1 的竞争，因此仍是 perf-only，勿加 `--verify`。
- O0/O1 是两条偶/奇 tile 独立累加链，并未做 O0↔O1 状态复制或最终合并；因此 pipeline/地址逻辑自洽，但不满足真实 attention 的单链 O 递推。
- 除明确忽略的 SF↔O1 外，S/P/V 位于 0..255、O0 位于 256..383、O1 位于 384..511，互不重叠；编译期禁止 physical double-O 与仍含 O0-only 旁路的 OSPLIT/SM12 组合。

170100 实测（B=1, H=H_K=40, D=128, warmup=5, iterations=20）：physical double-O 三次为 2051.31 / 2050.34 / 2047.57 TFLOPS/s，中位数 **2050.34**；同轮 single-O noexp_raw 为 1852.41 TFLOPS/s，提升 **+10.69%**。

其他仅带 `MXFP8_ODEPTH2` 的 target 仍是单 O0 handshake 版。

保留 exp 的 170100 串行实测（2026-08-12，同样 B=1, H=H_K=40, D=128, warmup=5, iterations=20）：

| 版本 | 三次结果（TFLOPS/s） | 中位数 | 相对 single-O |
|---|---|---:|---:|
| single-O FA4/no-E2RSF | 1559.12 / 1556.29 / 1553.13 | 1556.29 | — |
| handshake double-O | 1622.30 / 1614.00 / 1613.94 | 1614.00 | +3.71% |
| physical double-O | 1684.80 / 1682.59 / 1684.50 | **1684.50** | **+8.24%** |

physical double-O 相对 handshake double-O 再提升 **+4.37%**。此前约 660 TFLOPS/s 的一组结果是两个 benchmark 并发争抢同一 GPU，已作废，未计入表格。

510300 串行补测（2026-08-13，同样 B=1, H=H_K=40, D=128, warmup=5, iterations=20；均为 perf-only，未做精度验证）：

| softmax 版本 | O 版本 | 三次结果（TFLOPS/s） | 中位数 | physical double-O 提升 |
|---|---|---|---:|---:|
| 保留 exp（FA4/no-E2RSF） | single-O | 1521.91 / 1520.58 / 1522.26 | 1521.91 | — |
| 保留 exp（FA4/no-E2RSF） | physical double-O | 1660.44 / 1658.63 / 1654.92 | **1658.63** | **+8.98%** |
| noexp_raw | single-O | 1733.75 / 1732.98 / 1734.52 | 1733.75 | — |
| noexp_raw | physical double-O | 1866.62 / 1865.06 / 1865.96 | **1865.96** | **+7.63%** |

因此，“去 exp 后 physical double-O 的相对收益一定更大”并不是跨 shape 的稳定规律：170100 上是 10.69%（noexp）对 8.24%（保留 exp），而 510300 上反转为 7.63% 对 8.98%。长序列下还混入访存、SF staging、调度及两条独立 O 链等效应，不能只用 softmax 串行链长度解释收益。
