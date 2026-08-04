# E2RSF 开启 vs. 关闭 E2RSF + MUFU 分流 编译与测试文档

日期：2026-08-04
设备：NVIDIA B200（148 SM，SM100）
CUDA：13.3 / nvcc V13.3.73
cmake：4.4.0，gcc 11.4.0
GPU 核心频率：测试前锁定为 1965 MHz（`nvidia-smi -lgc 1965,1965`）

本文档说明如何编译两个对比版本，并给出两个长序列 shape 下的实测结果：

- 版本 A（基线）：**开启 E2RSF**，softmax 使用硬件 `MUFU.EX2`
- 版本 B：**关闭 E2RSF** 且对 softmax 的部分 `MUFU.EX2` 做软件分流（保留的 18.75% 版本）

被测 shape：

| 名称 | B | H / H_K | Q | K | D | mask |
|---|---:|---|---:|---:|---:|---|
| shape-170100 | 1 | 40 | 170100 | 170100 | 128 | no |
| shape-510300 | 1 | 40 | 510300 | 510300 | 128 | no |

---

## 1. 目录结构

```text
/home/ubuntu/workspace/oyhj/temp/test/test_merge/Benchmark/
├── cutlass/                         # 第三方库（CUTLASS，header-only + tools）
└── FA/
    ├── CMakeLists.txt                # 顶层：add_subdirectory(../cutlass) + add_subdirectory(b200_blackwell_fmha_mxfp8)
    ├── build/                        # 构建目录（cmake 输出，已存在）
    │   └── b200_blackwell_fmha_mxfp8/   # 编译出的可执行文件
    └── b200_blackwell_fmha_mxfp8/    # 源码与 CMakeLists.txt
        ├── CMakeLists.txt            # 定义全部 target（含两个对比目标）
        ├── b200_blackwell_fmha.cu    # 入口
        ├── collective/               # mainloop / 分流实现
        │   ├── sm100_fmha_fwd_mainloop_mxfp8_n128_pvmx.hpp
        │   └── fa4_mufu_split_sm100.hpp
        └── ...
```

顶层 `FA/CMakeLists.txt` 通过 `add_subdirectory(../cutlass cutlass_build EXCLUDE_FROM_ALL)` 引入 CUTLASS；CUTLASS 源码位于 `Benchmark/cutlass`。**两者是兄弟目录**，不可移动其一而不移动另一个。

---

## 2. 编译两个对比版本

### 2.1 编译选项

```bash
cd /home/ubuntu/workspace/oyhj/temp/test/test_merge/Benchmark/FA

# 从零构建（推荐独立目录）
rm -rf build
mkdir build && cd build
cmake .. \
  -DCMAKE_BUILD_TYPE=Release \
  -DCUTLASS_NVCC_ARCHS="100a"
make -j
```

说明：

- `-DCUTLASS_NVCC_ARCHS="100a"`：必须指定，对应 B200 的 SM100a（cluster / TMA 需要 `100a` 而非 `100`）。`CMakeLists.txt` 里只有匹配 `100a` 或 `103a` 才会生成这些 target。
- cmake 必须在 `FA/` 目录下执行（顶层 `CMakeLists.txt` 所在目录），它会用相对路径 `../cutlass` 找到 CUTLASS。
- 编译耗时较长（每个 target 都重新编译 `b200_blackwell_fmha.cu`），首次全量约 10–20 分钟。

### 2.2 只编译两个对比目标（节省时间）

上面 `CMakeLists.txt` 一次性生成几十个 target。如果**只需要本次对比的两个版本**，单独 make 它们即可：

```bash
cd /home/ubuntu/workspace/oyhj/temp/test/test_merge/Benchmark/FA/build

make -j op6static2sm_b200_blackwell_fmha_mxfp8
make -j op6static2sm_no_e2rsf_fa4mufu_b200_blackwell_fmha_mxfp8
```

这两个 target 在 `b200_blackwell_fmha_mxfp8/CMakeLists.txt` 中已分别定义好对应的编译宏（见第 3 节），无需手动指定任何 `-D`。

### 2.3 编译产物

```text
build/b200_blackwell_fmha_mxfp8/
├── op6static2sm_b200_blackwell_fmha_mxfp8                # 版本 A：E2RSF 开
└── op6static2sm_no_e2rsf_fa4mufu_b200_blackwell_fmha_mxfp8   # 版本 B：E2RSF 关 + MUFU 分流 18.75%
```

### 2.4 增量修改源码后重新编译

修改了 `b200_blackwell_fmha_mxfp8/` 下的 `.cu`/`.hpp` 后：

```bash
cd /home/ubuntu/workspace/oyhj/temp/test/test_merge/Benchmark/FA/build
make -j op6static2sm_b200_blackwell_fmha_mxfp8
make -j op6static2sm_no_e2rsf_fa4mufu_b200_blackwell_fmha_mxfp8
```

cmake 会自动跟踪头文件依赖，无需重新 `cmake ..`。

---

## 3. 两个版本的编译宏对照

两个 target 共用同一个源文件 `b200_blackwell_fmha.cu`，区别完全由编译期宏决定。完整定义见 `b200_blackwell_fmha_mxfp8/CMakeLists.txt`，关键差异如下。

公共基础宏 `OP6_STATIC_2SM_DEFS`（两个 target 都有）：

```text
MXFP8
FMHA_2CTA
MXFP8_FULL
MXFP8_OP6_STATIC_2SM
MXFP8_N128
MXFP8_KV_STAGES=12
MXFP8_AMAXFUSE
MXFP8_PSF_VEC16
MXFP8_PSTATIC
MXFP8_PSTATIC_EXP=0
MXFP8_2SM_CREL
MXFP8_E2RSF                  # <-- 注意：基线包含它
MXFP8_2SM_EXITDB
MXFP8_2SM_N128SINGLE
MXFP8_2SM_VPREFETCH
MXFP8_M2_COMBO
MXFP8_G_COMBO
MXFP8_R15_ORVLOG
MXFP8_WS_REGSM
MXFP8_REGSM_SOFTMAX=184
```

| 宏 | 版本 A（基线，E2RSF 开） | 版本 B（关 E2RSF + MUFU 分流） |
|---|---|---|
| `MXFP8_E2RSF` | **定义** | **不定义**（被 `list(REMOVE_ITEM ...)` 移除） |
| `MXFP8_FA4_MUFU_SPLIT` | 不定义 | **定义** |
| `MXFP8_FA4_MUFU_BEGIN_PAIR` | — | `4` |
| `MXFP8_FA4_MUFU_END_PAIR` | — | `28` |
| `MXFP8_FA4_MUFU_PERIOD` | — | `4` |
| `MXFP8_FA4_MUFU_SLOT` | — | `0` |
| target 名 | `op6static2sm_b200_blackwell_fmha_mxfp8` | `op6static2sm_no_e2rsf_fa4mufu_b200_blackwell_fmha_mxfp8` |

### 关于分流比例（18.75% 是怎么来的）

`MXFP8_FA4_MUFU_*` 这组宏控制：在 softmax group 的 32 个 float2 pair 中，区间 `[BEGIN_PAIR, END_PAIR)` 内每 `PERIOD` 个 pair 选 `SLOT` 偏移处的那一个 pair，用 packed Cody–Wait 三次多项式软件模拟 `exp2`，其余仍用硬件 `MUFU.EX2`。

版本 B 的 `BEGIN_PAIR=4, END_PAIR=28, PERIOD=4, SLOT=0` ⇒ 选中 pair = `{4, 8, 12, 16, 20, 24}`，共 6 / 32 = **18.75%**。这是前期分流比例扫描的保留胜出配置（详见 `MXFP8_NO_E2RSF_MUFU_SPLIT_RESULTS.md`）。

> 注意：CMakeLists 里还有一个 target `op6static2sm_fa4mufu_b200_blackwell_fmha_mxfp8`，它是「**保留 E2RSF** 的分流」版本，分流比例更小（约 6.25%），不在本次对比范围内。别和版本 B 搞混——版本 B 的特征是「**先关 E2RSF** 再分流」。

---

## 4. 运行测试

### 4.1 锁频（保证结果可复现）

```bash
sudo nvidia-smi -lgc 1965,1965      # 锁定 SM/Graph 频率到 1965 MHz
nvidia-smi --query-gpu=clocks.applications.graphics,clocks.current.sm --format=csv
# 解锁：sudo nvidia-smi -rgc
```

### 4.2 单次手动运行

```bash
cd /home/ubuntu/workspace/oyhj/temp/test/test_merge/Benchmark/FA/build/b200_blackwell_fmha_mxfp8

# shape-170100
./op6static2sm_b200_blackwell_fmha_mxfp8 \
  --b=1 --h=40 --h_k=40 --q=170100 --k=170100 --d=128 --mask=no \
  --warmup_iterations=5 --iterations=20

./op6static2sm_no_e2rsf_fa4mufu_b200_blackwell_fmha_mxfp8 \
  --b=1 --h=40 --h_k=40 --q=170100 --k=170100 --d=128 --mask=no \
  --warmup_iterations=5 --iterations=20

# shape-510300（单 kernel 约 1.7 s，整体更慢）
./op6static2sm_b200_blackwell_fmha_mxfp8 \
  --b=1 --h=40 --h_k=40 --q=510300 --k=510300 --d=128 --mask=no \
  --warmup_iterations=5 --iterations=20

./op6static2sm_no_e2rsf_fa4mufu_b200_blackwell_fmha_mxfp8 \
  --b=1 --h=40 --h_k=40 --q=510300 --k=510300 --d=128 --mask=no \
  --warmup_iterations=5 --iterations=20
```

每个二进制会输出一行 TFLOPS/s，例如：

```text
###### B 1 H 40 H_K 40 Q 170100 K 170100 D 128 Forward None #SM 148
 [--] tma ws n128 acc fp32 individual  : 1547.91 TFLOPS/s
```

常用命令行参数：

| 参数 | 含义 |
|---|---|
| `--b / --h / --h_k / --q / --k / --d` | batch / Q 头数 / KV 头数 / Q 序列长 / KV 序列长 / 头维度 |
| `--mask=<no\|residual\|causal>` | 掩码类型；本次均用 `no` |
| `--warmup_iterations` | 预热次数（不计入计时） |
| `--iterations` | 计时迭代次数 |
| `--verify` | 与参考实现比对正确性（性能测试时不加） |
| `--verbose` | 打印每个 kernel 的共享内存与单次时间 |

### 4.3 批量 A/B 脚本（多样本）

为避免单次抖动，本次实测用脚本对每个版本独立取多个样本，再算中位数/均值。脚本位于：

```text
/home/ubuntu/workspace/oyhj/temp/test/test_merge/Benchmark/FA/build/bench_e2rsf_vs_mufu.sh
```

用法：

```bash
cd /home/ubuntu/workspace/oyhj/temp/test/test_merge/Benchmark/FA/build

# shape-170100：每个版本 4 个独立样本，每样本 5 warmup + 20 iters
./bench_e2rsf_vs_mufu.sh --q=170100 --k=170100 --samples=4 --warmup=5 --iters=20

# shape-510300
./bench_e2rsf_vs_mufu.sh --q=510300 --k=510300 --samples=4 --warmup=5 --iters=20
```

脚本内部对两个二进制各跑 `--samples` 次，逐样本打印 TFLOPS/s，最后汇总样本列表，便于离线计算中位数/均值。

---

## 5. 实测结果

测试条件：GPU 锁频 1965 MHz；每个版本 4 个独立样本；每样本 5 次 warmup + 20 次计时；mask=no。

### 5.1 shape-170100：`B=1,H=40,Q=K=170100,D=128`

| 版本 | 4 个样本 TFLOPS/s | 中位数 | 均值 |
|---|---|---:|---:|
| A：E2RSF 开（基线） | 1542.41, 1539.28, 1534.61, 1535.16 | 1537.22 | 1537.87 |
| B：关 E2RSF + MUFU 18.75% | 1549.50, 1546.37, 1545.14, 1547.48 | 1546.92 | 1547.12 |
| **相对变化** | — | **+0.63%** | **+0.60%** |

- 单 kernel 时间：基线 ≈ 192.74 ms，分流版 ≈ 191.53 ms（≈ -1.2 ms）。
- 两版本样本完全不交叠（基线最高 1542.41 < 分流最低 1545.14），方向一致、可复现。

### 5.2 shape-510300：`B=1,H=40,Q=K=510300,D=128`

| 版本 | 4 个样本 TFLOPS/s | 中位数 | 均值 |
|---|---|---:|---:|
| A：E2RSF 开（基线） | 1515.74, 1514.43, 1514.45, 1516.25 | 1515.10 | 1515.22 |
| B：关 E2RSF + MUFU 18.75% | 1519.02, 1521.68, 1518.54, 1517.87 | 1518.78 | 1519.28 |
| **相对变化** | — | **+0.24%** | **+0.27%** |

- 单 kernel 时间：基线 ≈ 1759.99 ms，分流版 ≈ 1755.72 ms（≈ -4.3 ms）。
- 基线最高 1516.25 与分流最低 1517.87 仅相差约 1.6 TFLOPS/s，区间几乎贴合但方向仍为正，4 个样本互不交叠。

### 5.3 小结

- 在两个 shape 上，**关闭 E2RSF + 18.75% MUFU 分流版相对 E2RSF 开启的基线均有小幅正收益**：shape-170100 约 +0.6%，shape-510300 约 +0.25%。
- 收益随序列变长而变小：长序列下 softmax 的 MUFU 等待在端到端占比下降，内存/全局规约更主导，分流可压缩的关键路径变短。
- shape-510300 的两版本样本区间已接近贴合（差距约 0.1%），可视为「基本持平、略偏正」；shape-170100 的差距更明显且样本不交叠，结论更稳健。
- 这与既有结论一致（见 `MXFP8_NO_E2RSF_MUFU_SPLIT_RESULTS.md`）：关闭 E2RSF 后，分流在「干净的 exp2 临界段」上能拿到小幅可复现正收益，但不是数量级提升。

> 性能结论以锁频独立计时为准。NCU 多 pass replay 的 cycle 变化可作为诊断信号，但不作为性能数字（详见同目录 `FA4_MUFU_SPLIT_COMPARATIVE_ANALYSIS.md` 第 4 节）。

---

## 6. 附：本次测试的原始数据

shape-170100（顺序：基线 4 样本 → 分流 4 样本）：

```text
E2RSF-on baseline      : 1542.41, 1539.28, 1534.61, 1535.16   (median 1537.22 / mean 1537.87)
no-E2RSF + MUFU 18.75% : 1549.50, 1546.37, 1545.14, 1547.48   (median 1546.92 / mean 1547.12)
```

shape-510300（顺序：基线 4 样本 → 分流 4 样本）：

```text
E2RSF-on baseline      : 1515.74, 1514.43, 1514.45, 1516.25   (median 1515.10 / mean 1515.22)
no-E2RSF + MUFU 18.75% : 1519.02, 1521.68, 1518.54, 1517.87   (median 1518.78 / mean 1519.28)
```

每个样本 = 单进程内 `5 warmup + 20 timed iterations`，二进制打印的 TFLOPS/s 即为该样本代表值。
