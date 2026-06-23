# MXFP8 Static-P FA Compile-Flag Ablation Report

## Test Setup

- Source: `/home/ubuntu/workspace/oyhj/staticQuant_mxfp8/Benchmark/FA/b200_blackwell_fmha_mxfp8`
- Build dir: `/home/ubuntu/workspace/oyhj/staticQuant_mxfp8/Benchmark/FA/build_ablation`
- Shape: `--b=1 --h=40 --q=170100 --k=170100 --d=128 --mask=no --warmup_iterations=2 --iterations=5`
- Static quantization kept fixed for every target: `MXFP8_PSTATIC` and `MXFP8_PSTATIC_EXP=0`
- Each variant was run 3 times; the table uses the median TFLOPS/s.

## Compile-Flag Control

The complete target is controlled by `OP6_STATIC_2SM_DEFS` in `CMakeLists.txt`. Each ablation target is generated from that same list with `list(REMOVE_ITEM ...)`, so every target differs from baseline only by the removed macro(s) shown below.

Main baseline definitions include:

```text
FMHA_2CTA MXFP8_N128 MXFP8_OP6_STATIC_2SM MXFP8_KV_STAGES=12
MXFP8_PSTATIC MXFP8_PSTATIC_EXP=0 MXFP8_2SM_CREL MXFP8_E2RSF
MXFP8_2SM_EXITDB MXFP8_2SM_N128SINGLE MXFP8_2SM_VPREFETCH
MXFP8_M2_COMBO MXFP8_G_COMBO MXFP8_R15_ORVLOG
MXFP8_WS_REGSM MXFP8_REGSM_SOFTMAX=184
```

## Main Results

- Baseline median: `1583.20 TFLOPS/s`.
- Worst successful main variant still above `1430 TFLOPS/s`: `no_n128single` at `1466.66 TFLOPS/s`.

| Variant | Removed compile option(s) | Status | Runs TFLOPS/s | Median | Delta vs baseline |
|---|---|---|---:|---:|---:|
| `baseline` | `none` | `ok,ok,ok` | 1584.24, 1582.84, 1583.20 | 1583.20 | +0.00 (+0.00%) |
| `no_crel` | `MXFP8_2SM_CREL` | `ok,ok,ok` | 1579.08, 1583.19, 1580.97 | 1580.97 | -2.23 (-0.14%) |
| `no_e2rsf` | `MXFP8_E2RSF` | `ok,ok,ok` | 1575.97, 1571.68, 1572.73 | 1572.73 | -10.47 (-0.66%) |
| `no_exitdb` | `MXFP8_2SM_EXITDB` | `ok,ok,ok` | 1559.69, 1558.31, 1562.55 | 1559.69 | -23.51 (-1.48%) |
| `no_n128single` | `MXFP8_2SM_N128SINGLE` | `ok,ok,ok` | 1465.59, 1467.14, 1466.66 | 1466.66 | -116.54 (-7.36%) |
| `no_vprefetch` | `MXFP8_2SM_VPREFETCH` | `ok,ok,ok` | 1583.35, 1583.55, 1583.60 | 1583.55 | +0.35 (+0.02%) |
| `no_m2_combo` | `MXFP8_M2_COMBO` | `ok,ok,ok` | 1603.77, 1607.56, 1604.80 | 1604.80 | +21.60 (+1.36%) |
| `no_g_combo` | `MXFP8_G_COMBO` | `ok,ok,ok` | 1575.61, 1575.44, 1574.13 | 1575.44 | -7.76 (-0.49%) |
| `no_r15_orvlog` | `MXFP8_R15_ORVLOG` | `ok,ok,ok` | 1575.61, 1573.82, 1573.64 | 1573.82 | -9.38 (-0.59%) |
| `kv_default` | `MXFP8_KV_STAGES=12` | `ok,ok,ok` | 1535.93, 1532.97, 1532.83 | 1532.97 | -50.23 (-3.17%) |
| `regsm_default` | `MXFP8_WS_REGSM;MXFP8_REGSM_SOFTMAX=184` | `ok,ok,ok` | 1552.19, 1553.45, 1553.15 | 1553.15 | -30.05 (-1.90%) |
| `no_2cta` | `FMHA_2CTA` | `ok,ok,ok` | 1574.82, 1575.34, 1575.14 | 1575.14 | -8.06 (-0.51%) |

## Sanity Results

`MXFP8_AMAXFUSE` and `MXFP8_PSF_VEC16` are weakly relevant under `MXFP8_PSTATIC`: most AMAXFUSE dynamic-scale code is behind `!defined(MXFP8_PSTATIC)`, and the dynamic P-SF write phase is compiled out for static P.

| Variant | Removed compile option(s) | Status | Runs TFLOPS/s | Median | Delta vs baseline |
|---|---|---|---:|---:|---:|
| `no_amaxfuse` | `MXFP8_AMAXFUSE` | `ok,ok,ok` | 1575.53, 1573.33, 1575.26 | 1575.26 | -7.94 (-0.50%) |
| `no_psf_vec16` | `MXFP8_PSF_VEC16` | `ok,ok,ok` | 1573.85, 1574.31, 1574.48 | 1574.31 | -8.89 (-0.56%) |

## Impact Analysis

- `no_crel`: median `1580.97 TFLOPS/s`, -2.23 (-0.14%) vs baseline. 去掉 softmax->correction early release，观察 O wait 是否重新卡住 softmax/correction 链。
- `no_e2rsf`: median `1572.73 TFLOPS/s`, -10.47 (-0.66%) vs baseline. 去掉 E2RSF fused row-sum 路径，观察 P 量化/row-sum 融合的贡献。
- `no_exitdb`: median `1559.69 TFLOPS/s`, -23.51 (-1.48%) vs baseline. 去掉 2SM epilogue/dealloc pair handshake 优化，观察 exit/dealloc 同步形态影响。
- `no_n128single`: median `1466.66 TFLOPS/s`, -116.54 (-7.36%) vs baseline. 去掉 single N128 QK/PV 路径，回到更重的 K/SF staging 形态，是本轮最大性能下降来源。
- `no_vprefetch`: median `1583.55 TFLOPS/s`, +0.35 (+0.02%) vs baseline. 去掉 V tile TMA prefetch hint；当前 shape 下中位数几乎不降，说明它不是主瓶颈。
- `no_m2_combo`: median `1604.80 TFLOPS/s`, +21.60 (+1.36%) vs baseline. 去掉 M2 rescale vote/hoist 组合；本轮反而更快，说明当前编译器/shape 下该组合不是收益项。
- `no_g_combo`: median `1575.44 TFLOPS/s`, -7.76 (-0.49%) vs baseline. 去掉 correction rescale 的 scale pack/address 组合优化，性能小幅下降。
- `no_r15_orvlog`: median `1573.82 TFLOPS/s`, -9.38 (-0.59%) vs baseline. 去掉基于 old/new max 的 rescale exp2 延迟/跳过路径，性能小幅下降。
- `kv_default`: median `1532.97 TFLOPS/s`, -50.23 (-3.17%) vs baseline. 去掉 MXFP8_KV_STAGES=12，回默认 KV stage，load/compute overlap 明显变差。
- `regsm_default`: median `1553.15 TFLOPS/s`, -30.05 (-1.90%) vs baseline. 去掉 WS_REGSM 和 softmax=184 寄存器预算 override，寄存器预算回默认后性能下降。
- `no_2cta`: median `1575.14 TFLOPS/s`, -8.06 (-0.51%) vs baseline. 去掉 FMHA_2CTA 标记；注意 N128 mainloop 仍有 ClusterShape<2,1,1>，因此不是纯 1CTA 对照。
- `no_amaxfuse`: median `1575.26 TFLOPS/s`, -7.94 (-0.50%) vs baseline. sanity 项；在 MXFP8_PSTATIC 下大量 AMAXFUSE 分支被 !MXFP8_PSTATIC 屏蔽。
- `no_psf_vec16`: median `1574.31 TFLOPS/s`, -8.89 (-0.56%) vs baseline. sanity 项；在 MXFP8_PSTATIC 下动态 P-SF 写入阶段被编译掉，影响很弱。

## Best Candidate Above 1430 With Worst Performance

`no_n128single` is the requested slowest successful main variant above `1430 TFLOPS/s`.

- Target: `op6static2sm_no_n128single_b200_blackwell_fmha_mxfp8`
- Removed compile option(s): `MXFP8_2SM_N128SINGLE`
- Median performance: `1466.66 TFLOPS/s`
- Runs: `1465.59, 1467.14, 1466.66`
- Run command:

```bash
cd /home/ubuntu/workspace/oyhj/staticQuant_mxfp8/Benchmark/FA
./build_ablation/b200_blackwell_fmha_mxfp8/op6static2sm_no_n128single_b200_blackwell_fmha_mxfp8 \
  --b=1 --h=40 --q=170100 --k=170100 --d=128 --mask=no \
  --warmup_iterations=2 --iterations=5
```

## Notes

- `no_m2_combo` is faster than baseline in this run. That means `MXFP8_M2_COMBO` should not be assumed beneficial for this exact static-P shape without additional profiling.
- `no_2cta` removes `FMHA_2CTA`, but it is not a pure 1CTA comparison because the `MXFP8_N128` mainloop still defines `ClusterShape<2,1,1>` internally. Treat it as a compile-flag ablation, not a complete structural rewrite.
- Raw logs are in `logs/`, and the machine-readable table is `results.csv` in this same result directory.
