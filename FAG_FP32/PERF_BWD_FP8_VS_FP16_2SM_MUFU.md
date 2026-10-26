# Backward 性能采集：FP8 FAG（2-SM + MUFU 分流最优版）vs FP16 FA4（2-CTA 最优版）

两侧均使用 **各自的最优 2-SM 版本**，shape 为 `(1,16,{4096,8192,16384},128)`，full（non-causal），bwd-only。
每个数据点同时给出 **主 kernel（bwd）TFLOPS** 与 **三个 kernel 合计**（主 backward + 2 个辅助 kernel）的 TFLOPS。

- **测量口径**：nsys `cuda_gpu_kern_sum` 的 **Med（中位数，ns）**；GPU 锁频 1965 MHz；每 shape 计时 20 次（另有预热启动，不剔除、中位数鲁棒）。
- **FLOP 数**：`FLOP = 10·b·h·L²·d`（full mask backward：`2·Q·K·(3·D+2·D_VO)·H·B`）。
- **采集日期**：2026-08-15，B200（sm_100a，148 SM），CUDA 13.3，Nsight Systems 2026.1.3。

## 实验对象与配置

| 版本 | 路径 | 精度 | 2-SM 最优配置 | 三个 kernel |
|---|---|---|---|---|
| **FP8** | `temp/test/test_merge/Benchmark/FAG_FP32/`（CUTLASS）| FP8 e4m3 | `fag_stage_regalloc` = `BWD_2SM FAG_EX2_EMU_STRIDE=4 FAG_REGALLOC_FA4_CURRENT`（EX2_STRIDE=4 即 3:1 MUFU/软件分流）| `Sm100FmhaBwdKernelTma2SmWarpSpecialized` + `FmhaKernelBwdSumOdO` + `FmhaKernelBwdConvert` |
| **FP16** | `FAG/flash-attention/flash_attn/cute/flash_bwd_sm100.py`（FA4 CuTe-DSL）| FP16 (`torch.float16`) | `flash_attn_func` 默认 2-CTA（head_dim≥128 自动开启，MUFU 硬件 exp2）| `FlashAttentionBackwardSm100` + `…Preprocess` + `…Postprocess` |

## 原始中位数（µs）

### FP8：主 kernel `Sm100FmhaBwdKernelTma2SmWarpSpecialized` + `FmhaKernelBwdSumOdO` + `FmhaKernelBwdConvert`

| shape | 主 | sumOdO | convert | 三 kernel 合计 |
|---|---:|---:|---:|---:|
| (1,16,4096,128) | 253.8 | 11.6 | 10.9 | 276.2 |
| (1,16,8192,128) | 827.9 | 22.0 | 21.4 | 871.3 |
| (1,16,16384,128) | 3172.3 | 41.3 | 44.3 | 3258.0 |

### FP16：主 kernel `FlashAttentionBackwardSm100` + `…Preprocess` + `…Postprocess`

| shape | 主 | preprocess | postprocess | 三 kernel 合计 |
|---|---:|---:|---:|---:|
| (1,16,4096,128) | 276.6 | 12.1 | 12.5 | 301.2 |
| (1,16,8192,128) | 969.7 | 21.4 | 25.6 | 1016.7 |
| (1,16,16384,128) | 3897.8 | 41.9 | 47.7 | 3987.4 |

> 合计时间 = 三个 kernel 中位数之和（三 kernel 顺序执行，逐次中位数求和即单次 backward 的代表时间）。

## 结果 A：主 kernel（bwd）TFLOPS

| shape | FP8 (2-SM+MUFU) | FP16 (2-CTA) | FP8 vs FP16 |
|---|---:|---:|---:|
| (1,16,4096,128) | **1353.9** | 1242.0 | **+9.0%** |
| (1,16,8192,128) | **1660.2** | 1417.4 | **+17.1%** |
| (1,16,16384,128) | **1733.0** | 1410.4 | **+22.9%** |

## 结果 B：三个 kernel 合计 TFLOPS（主 + 2 辅助）

| shape | FP8 (2-SM+MUFU) | FP16 (2-CTA) | FP8 vs FP16 |
|---|---:|---:|---:|
| (1,16,4096,128) | **1243.9** | 1140.6 | **+9.1%** |
| (1,16,8192,128) | **1577.4** | 1351.8 | **+16.7%** |
| (1,16,16384,128) | **1687.4** | 1378.7 | **+22.4%** |

## 观察与交叉验证

1. **主 kernel 与三 kernel 合计两个口径结论一致**：FP8 最优版相对 FP16 FA4 2-CTA 的优势随 shape 增大而扩大（+9% → +23%）。辅助 kernel 占比随 L 增大而稀释（4096 时约 8–9%，16384 时约 2–3%），因此两个口径的相对差距基本相同。
2. **与历史数据吻合**：FP8 主 kernel 本轮 1353.9 / 1660.2 / 1733.0 TFLOPS，`ABLATION_CORNER_2SM_MUFU.md` 的 base 为 1361 / 1668 / 1744（±1% 以内的运行波动）；FP16 侧此前文档以 BF16 采集（1230 / 1455 / 1479），本轮按需求改用 **FP16**，数值同量级（1242 / 1417 / 1410）。
3. **E2E 事件计时交叉校验（FP8）**：`fag_stage_regalloc` 自带的 event 计时（含全部 kernel，二进制输出行）为 1212.04 / 1554.10 / 1678.06 TFLOPS，与三 kernel 合计口径（1243.9 / 1577.4 / 1687.4）一致，event 值略低符合 host 端 launch 间隙的预期。
4. 两个口径都以 `10·b·h·L²·d` 计 FLOP（与 `77_blackwell_fmha_bwd.cu` 内的统计公式一致，非因果 mask）。

## 复现

```bash
nvidia-smi -lgc 1965,1965   # GPU 锁频
cd /home/ubuntu/workspace/oyhj/temp/test/test_merge/Benchmark/FAG_FP32
# FP8（nsys 抓主/sumOdO/convert kernel 中位数）
for L in 4096 8192 16384; do
  nsys profile --force-overwrite=true -o /tmp/perf_fp8_${L} \
    ./build/fag_stage_regalloc --b=1 --h=16 --q=$L --k=$L --d=128 --d_vo=128 \
    --iterations=20 --mask=no
done

cd /home/ubuntu/workspace/oyhj/FAG/flash-attention   # 确保 import 解析到本份源码
# FP16（bench 脚本：bench_bwd_perf_fp16.py，fp16、默认 2-CTA、非因果、bwd-only）
for L in 4096 8192 16384; do
  nsys profile --force-overwrite=true -o /tmp/perf_fp16_${L} \
    /usr/bin/python3 bench_bwd_perf_fp16.py $L 20
done

# 取 Med 列
nsys stats -r cuda_gpu_kern_sum /tmp/perf_fp8_4096.nsys-rep
```

- 中位数提取 + TFLOPS 计算：`/tmp/analyze_perf_bwd.py`（原始 JSON：`/tmp/perf_bwd_results.json`）。
- FP16 bench 脚本：`FAG/flash-attention/bench_bwd_perf_fp16.py`（本轮新增，基于 `bench_ablate_bwd.py` 改为 fp16、无消融）。
