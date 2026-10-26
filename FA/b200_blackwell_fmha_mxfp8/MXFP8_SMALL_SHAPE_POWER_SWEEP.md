# MXFP8 FA 小 shape 功耗扫描（B200）

日期：2026-08-30
设备：NVIDIA B200（148 SM / 74 个 2SM cluster，功耗上限 1000 W，应用时钟锁定 1965 MHz）
被测二进制：`op6static2sm_no_e2rsf_fa4mufu_b200_blackwell_fmha_mxfp8`（`--measure-energy`，NVML `nvmlDeviceGetTotalEnergyConsumption`）
固定配置：`B=1, H=H_K=40, D=128, q=k=S, mask=no`，warmup 20–50，iterations 按 shape 缩放使能量窗口 ≥1 s。
脚本与原始数据：`FA/agent_space/run_small_shape_power_sweep2.sh`、`run_shape_power_gated.sh`、`small_shape_power_sweep*/`、`shape_power_v3/`、`analyze_shape_power_v3.py`。

## 结论

**功耗强烈依赖 shape，并非任何 shape 都打满。** 只有 q=k ≳ 16K–32K 时功耗才达到 ~990–1000 W（1000 W 上限附近）；小 shape 显著低于上限：

| q=k | CTA 波数 ((q/256)·40/74) | 执行期功耗 (E/t_gpu) | 能量/次 | 延迟/次 | 数据来源 |
|---:|---:|---:|---:|---:|---|
| 256 | 0.54 | **~255 W** (223–295) | ~3 mJ | ~10 µs | v2 |
| 512 | 1.1 | **~320 W** (311–422) | ~7 mJ | ~20 µs | v2 |
| 1024 | 2.2 | **~445 W** (380–471) | ~21 mJ | ~46 µs | v2 |
| 2048 | 4.3 | **~600 W** (568–659) | ~74 mJ | ~124 µs | v2 |
| 4096 | 8.6 | **~720 W** (680–834) | ~271 mJ | ~325 µs | v2 |
| 8192 | 17 | **~815 W** (775–870) | ~844 mJ | ~1.04 ms | v1（干净） |
| 16384 | 35 | **~975 W** (916–1018) | ~3.77 J | ~3.8 ms | v1（干净，±25% 量化） |
| 32768 | 70 | **~990–1000 W** | ~14.8 J | ~14.8 ms | 用户数据 + v1 |
| 65536 | 140 | ~990 W (983–1004) | ~58 J | ~58 ms | 用户数据 |
| ≥131072 | — | ~988–1007 W | — | — | 用户数据 |

两个机制分段解释：

1. **q ≤ 2048：SM 填充率不足。** CTA 数 = (q/256)·40，q=256 只有 40 个 CTA（74 个 2SM cluster 的 54%），大量 SM 空闲，功耗只有 ~255 W；随 CTA 波数增加功耗线性爬升。
2. **q ≥ 4096：SM 已满，功耗随 K/V 工作集超出 L2 继续上升。** K+V 字节数 ≈ 10.24 KB·q：q=4096 时 42 MB（基本 L2 常驻），q=8192 时 84 MB，q≥16384 时 ≥168 MB 超出 L2，每次迭代都要从 HBM 流式读取，功耗从 ~720 W 爬到 ~975–990 W，接近 1000 W 上限（v3 块级采样最大 996.7 W，与用户独占时 973–1007 W 一致）。

## 方法学发现：NVML 能量计数器 100 ms 更新粒度

用 1 ms 轮询探测（`/tmp/nvml_block_probe.c`）发现 `nvmlDeviceGetTotalEnergyConsumption` **每 ~100 ms 更新一次**，每次更新 ΔE = 该周期平均功率 × 100 ms。因此：

- 计时窗口远大于 100 ms 且功率平稳（大 shape、GPU 有界）时，两次读取求 delta 是准的——用户大 shape 数据稳定（973–1007 W）正是这个原因。
- 窗口只有几个 100 ms 块、且块内功率混合（空闲+负载）时，边界块的归属带来 ±1 块误差：第一轮 sweep（窗口 75–750 ms）同 shape 重复测量出现 890 W vs 1040 W 的双峰抖动。
- **建议：以后测小 shape 功耗，iterations 要保证能量窗口 ≥1 s**（或用块级 probe 连续记录取中位数）。

## 测量污染与数据可信度

测试期间另一用户的 python 任务（4 个 multiprocessing worker）持续占用 GPU（29/30 秒有计算活动，时间片抢占使大 shape TFLOPS 从干净的 ~1490 掉到 900–1200）。各数据源可信度：

- **v1**（`small_shape_power_sweep/`，GPU 干净，TFLOPS 与独占参考一致 1.49e3@32K/1.45e3@16K/1.33e3@8K）：8192/16384/32768 的能量数据可信，但窗口短（100 ms 量化 ±25%）。
- **v2**（`small_shape_power_sweep_v2/`，窗口 ≥1 s）：**小 shape（256–4096）延迟与 TFLOPS 无抢占劣化**（这些 shape kernel 短、host 侧 launch 有空隙，外来任务填空隙不改我的延迟，只轻微抬高能量 ≤~10 W），数值可用；8192 以上被抢占，偏低。
- **v3 块级 probe**（`shape_power_v3/`）：受污染但验证了 32768 的 100 ms 块最大功率 996.7 W，与用户独占数据吻合。
- 用户自己的大 shape 独占数据作为 ≥32K 的基准。

综合后 8192–16384 区间数值仍有 ±5–10% 不确定度；如需精确定稿，可在 GPU 空闲时重跑 `run_shape_power_gated.sh`（约 5 分钟）。

## 复现

```bash
# 单个 shape（窗口 ≥1 s）
/path/op6static2sm_no_e2rsf_fa4mufu_b200_blackwell_fmha_mxfp8 \
  --b=1 --h=40 --h_k=40 --q=1024 --k=1024 --d=128 --mask=no \
  --warmup_iterations=50 --iterations=20000 --measure-energy

# 整套扫描 + 块级能量记录 + TFLOPS 门控
bash FA/agent_space/run_shape_power_gated.sh
python3 FA/agent_space/analyze_shape_power_v3.py
```

功耗取法：`P = E_total / gpu_total_latency_ms`（mJ/ms = W），即执行期平均功率；与用户 `power = energy(J)/latency(s)` 一致。
