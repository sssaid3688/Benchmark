# MXFP8 关闭 E2RSF 后的 MUFU 分流实验

日期：2026-08-04  
设备：NVIDIA B200（148 SM，SM100）  
CUDA：13.3

## 结论

关闭 `MXFP8_E2RSF` 后，MUFU 分流可以获得小幅但可复现的正收益。最佳实测配置为 18.75% 分流：每个 softmax group 的 32 个 float2 pair 中，选择 6 个 pair 使用 packed Cody-Waite 三次多项式，其余继续使用硬件 `MUFU.EX2`。

该版本已保留为正式独立目标：

```text
op6static2sm_no_e2rsf_fa4mufu_b200_blackwell_fmha_mxfp8
```

对应编译定义：

```text
不定义 MXFP8_E2RSF
MXFP8_FA4_MUFU_SPLIT
MXFP8_FA4_MUFU_BEGIN_PAIR=4
MXFP8_FA4_MUFU_END_PAIR=28
MXFP8_FA4_MUFU_PERIOD=4
MXFP8_FA4_MUFU_SLOT=0
```

## 最终平衡顺序复测

输入：`B=1, H=H_K=40, SQ=SK=170100, D=128, mask=no`。GPU 核心频率锁定为 1965 MHz；每个样本 5 次 warmup、20 次计时，共 4 个交替样本。

| 版本 | 4 个样本 TFLOPS/s | 中位数 | 均值 | 相对默认中位数 | 相对 no-E2RSF 中位数 |
|---|---|---:|---:|---:|---:|
| 默认：E2RSF 开、无分流 | 1540.33, 1534.86, 1533.66, 1534.15 | 1534.51 | 1535.75 | 基线 | — |
| 仅关闭 E2RSF | 1538.58, 1539.60, 1540.56, 1534.28 | 1539.09 | 1538.26 | +0.30% | 基线 |
| 关闭 E2RSF + 12.5% 分流 | 1545.37, 1541.79, 1543.63, 1543.42 | 1543.53 | 1543.55 | +0.59% | +0.29% |
| 关闭 E2RSF + 18.75% 分流 | 1545.58, 1549.95, 1548.41, 1545.82 | **1547.12** | **1547.44** | **+0.82%** | **+0.52%** |

以均值计算，18.75% 版本相对默认提升约 0.76%，相对 no-E2RSF 提升约 0.60%。收益不大，但中位数和均值方向一致，并在独立比例扫描中重复出现。

## 分流比例扫描

另一轮相同长序列、4 样本交替扫描结果如下。相对变化以同一轮 no-E2RSF 中位数 1539.20 TFLOPS/s 为基准。

| 分流比例 | 中位 TFLOPS/s | 相对 no-E2RSF |
|---:|---:|---:|
| 0% | 1539.20 | 基线 |
| 6.25% | 1538.07 | -0.07% |
| 12.5% | 1546.35 | +0.46% |
| 18.75% | **1547.84** | **+0.56%** |
| 25% | 1515.18 | -1.56% |

分流收益呈明显的窄区间：太少不足以明显缓解 MUFU，太多则让 FMA/ALU 指令和依赖链压过收益。18.75% 是当前测试形状下的最佳点，12.5% 是较保守的次优点。

## NCU 指标

诊断形状：`B=1, H=40, SQ=SK=16384, D=128, mask=no`。比较 no-E2RSF 基线与保留的 18.75% 版本。

| 指标 | no-E2RSF | no-E2RSF + 18.75% | 变化 |
|---|---:|---:|---:|
| XU 指令 | 342,315,156 | 279,400,600 | **-18.4%** |
| FMA 指令 | 498,066,656 | 713,024,736 | +43.2% |
| ALU 指令 | 477,987,207 | 582,844,819 | +21.9% |
| wait stall | 205.02% | 142.71% | **-30.4%** |
| long scoreboard | 429.38% | 341.73% | **-20.4%** |
| short scoreboard | 44.26% | 32.96% | **-25.5%** |
| MIO throttle | 92.31% | 54.33% | **-41.1%** |
| math-pipe throttle | 9.75% | 17.51% | +79.6% |
| not-selected stall | 36.18% | 45.44% | +25.6% |
| NCU replay cycles | 1.080B | 1.036B | -4.1% |

关闭 E2RSF 后，分流仍会提高 math-pipe 压力，但 `wait`、长/短 scoreboard 和 MIO throttle 的下降幅度更大。与 E2RSF 开启时的激进分流相比，关键区别是 row-sum 不再逐 pair 插入 exp2 burst，因此减少 MUFU 后释放的等待更容易缩短 P 生成关键路径。

NCU 多 pass replay 的 cycle 降幅大于独立端到端收益，不能直接作为性能数字；最终结论仍以锁频独立计时为准。

## 正确性与低级开销审计

无 mask 小形状验证：

```text
B=1, H=H_K=4, Q=K=512, D=128
passed_O: 1
passed_LSE: 1
O max_diff: 0.002495
LSE max_diff: 0.000007
```

SASS/资源对比：

| 指标 | no-E2RSF | 保留的 18.75% 版本 |
|---|---:|---:|
| `MUFU.EX2` | 991 | 823 |
| `FFMA2` | 448 | 700 |
| `FADD2` | 490 | 742 |
| 分支 `BRA/BRX/JMX` | 3060 | 3060 |
| `IDIV/REM` | 0 | 0 |
| 主内核寄存器 | 128 | 128 |
| 主内核 local memory | 0 | 0 |

分流仍是纯编译期选择，没有引入运行时分支、整数除法/取模、寄存器增长或 local-memory spill。

## 保留文件和运行方式

- CMake 配置：`/home/ubuntu/workspace/oyhj/temp/test/test_merge/Benchmark/FA/b200_blackwell_fmha_mxfp8/CMakeLists.txt`
- 分流实现：`/home/ubuntu/workspace/oyhj/temp/test/test_merge/Benchmark/FA/b200_blackwell_fmha_mxfp8/collective/fa4_mufu_split_sm100.hpp`
- 主循环接入：`/home/ubuntu/workspace/oyhj/temp/test/test_merge/Benchmark/FA/b200_blackwell_fmha_mxfp8/collective/sm100_fmha_fwd_mainloop_mxfp8_n128_pvmx.hpp`
- 保留二进制：`/home/ubuntu/workspace/oyhj/temp/test/test_merge/Benchmark/FA/build/b200_blackwell_fmha_mxfp8/op6static2sm_no_e2rsf_fa4mufu_b200_blackwell_fmha_mxfp8`

运行命令：

```bash
/home/ubuntu/workspace/oyhj/temp/test/test_merge/Benchmark/FA/build/b200_blackwell_fmha_mxfp8/op6static2sm_no_e2rsf_fa4mufu_b200_blackwell_fmha_mxfp8 \
  --b=1 --h=40 --h_k=40 --q=170100 --k=170100 --d=128 --mask=no \
  --warmup_iterations=5 --iterations=20
```

原默认目标和原 E2RSF-on 分流目标都没有被覆盖，仍可用于回归比较。
