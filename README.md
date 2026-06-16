# op6 — MXFP8 FlashAttention Forward (B200 / sm_100a)

块缩放 **MXFP8** FlashAttention 前向（推理 FWD only，D=128，NoMask）。
QK 与 PV **均为 block-scaled MXFP8 MMA**（e4m3 数据 + ue8m0 per-32 scale-factor），
从 CUTLASS example 77 改写，针对 B200 (sm_100a) tcgen05 + 2-SM cooperative 深度调优。

**本包 = 甲方确认的目标版本：2-SM cooperative + 静态 P 量化 (SF=1.0) + fp32 softmax。**
（动态量化已确认不需要——P 的 scale 恒为 1，动态在线 amax 是多余开销；fp16 softmax 不需要。）

## 性能（B200 单卡独占实测，graph-replay avg，公式 4·B·H·S²·D / time）

| 验收 shape | TFLOPS |
|---|---|
| **SOW1** (B1 H40 **S170100** D128 NoMask) | **~1424** |
| **SOW2** (B1 H40 **S510300** D128 NoMask) | **~1429** |

> 含 warp-spec reg-down 优化（见下）。未含 reg-down 的前一版为 SOW1 1410.3 / SOW2 1421.0。
> 上限背景：本 kernel 在该 B200 上 exp2(MUFU.EX2)/latency-bound（tcgen05.alloc 锁 1 CTA/SM ⇒ 22% 占用），
> 免-exp2 理论硬顶 ≈ 1477 TFLOPS；1424 ≈ 96% 该硬顶。

## 包内容

```
build.sh                                     # 一键编译 -> build/op6/fmha_mxfp8_pvmx
src/
  common.h                                   # host 工具头（.cu 依赖）
  operators/op6_mxfp8_fmha/
    custom_2sm/                              # 2-SM cooperative kernel（核心代码）
      fmha_mxfp8_pvmx.cu                     # 主程序：host 量化 + 内置 verify + perf 计时 + --dump
      sm100_fmha_fwd_kernel_mxfp8_pvmx.hpp   # kernel 层：16-warp 角色分配 / pipeline 初始化
      sm100_fmha_fwd_mainloop_mxfp8_n128_pvmx.hpp  # mainloop（softmax/correction/MMA，核心）
      sm100_fmha_fwd_mainloop_mxfp8*.hpp     # mainloop 基件
      sm100_fmha_load_tma_mxfp8*.hpp         # TMA load（N128，SF-aware）
      sm100_fmha_fwd_epilogue_mxfp8_n128.hpp
      fmha_mxfp8_n128_schedule.hpp           # warp-spec 调度 / 寄存器预算（含 reg-down 旋钮）
    verify/
      op6_precision_ref.py                   # ★ Python golden：含 per-32 MXFP8 P-量化的紧致参考
      run_op6_precision.sh                   # 50-shape 精度 sweep（kernel --dump vs golden）
```

## thirdparty（自行 clone，不随包）

**CUTLASS v4.5.1**（仅用头文件，无需 cmake 构建）：

```bash
git clone --depth 1 --branch v4.5.1 https://github.com/NVIDIA/cutlass.git thirdparty/cutlass
```

## 编译 / 跑 / 验证

```bash
# 编译（需 CUDA 13.0+ 与 B200 sm_100a）
bash build.sh                 # -> build/op6/fmha_mxfp8_pvmx
# 或:  NVCC=/usr/local/cuda-13.0/bin/nvcc bash build.sh

# 性能
./build/op6/fmha_mxfp8_pvmx --b=1 --h=40 --s=170100 --d=128 --verify=0   # SOW1 ~1424
./build/op6/fmha_mxfp8_pvmx --b=1 --h=40 --s=510300 --d=128 --verify=0   # SOW2 ~1429

# 正确性（内置 vs fp32 SDPA，宽松）
./build/op6/fmha_mxfp8_pvmx --b=1 --h=8 --s=4096 --d=128

# 紧致 Python golden（含在线 per-32 P→e4m3 量化，比内置 verify 更严，能抓真 bug）
bash src/operators/op6_mxfp8_fmha/verify/run_op6_precision.sh   # 50-shape, 全 PASS
```

精度门限：逐 shape `|row|O|ratio − 1| ≤ .03` & `std ≤ .05` & `rel_rmse ≤ .10`
（随机数据，含 S%128≠0 / S%32≠0 / 奇数 / 多头 / B>1）。本版 50/50 sweep PASS，
vs FP32 ref 逐 shape max_abs ≤ 0.0067。

## 实现要点

- **静态 P 量化**（`-DMXFP8_PSTATIC`）：P 的 e8m0 SF = 编译期常数（`MXFP8_PSTATIC_EXP=0` ⇒ SF=1.0），
  在线 amax 链全部编译掉，SFP TMEM 启动时一次性常数填充。⚠️ `MXFP8_PSTATIC_EXP` 勿设负值
  （P>1 过冲撞 e4m3 satfinite 448 致精度崩）。
- **fp32 softmax**：exp2 用 `ex2.approx.f32`，row_sum/rowmax 全 fp32。
- **warp-spec reg-down**（`-DMXFP8_WS_REGSM -DMXFP8_REGSM_SOFTMAX=184`）：softmax warpgroup 的
  setmaxnreg 从 192 降到 184，消除一个 8-byte 临界路径寄存器 spill（192 占满 65280/65536 预算）。
  **数值完全不变**（纯寄存器预算调整），SOW1 +12 / SOW2 +8。去掉此 flag 即前一版 1410/1421。
- 16-warp warp-spec：w0-7 = 2× Softmax warpgroup，w8-11 = Correction，w12 MMA，w13 TMA Load，w14 Epilogue。
- 2-SM cooperative（`cta_group::2`）：M=256 cluster tile，K/V N-split（leader n0-63 / peer n64-127，零重复流量）。
- 其它已固化优化：N128-single QK/PV pipeline、O-rescale exp2-skip（row-max 稳定时跳过）、
  V-tile L2 prefetch、barrier-spin 重排藏延迟。
