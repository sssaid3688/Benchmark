# `softmax_step` FP16 实验修改片段

目标：

```text
QK MMA / TMEM accumulator：保持 FP32
TMEM score 读入后：先舍入到 FP16
row_max：基于 FP16 score，但使用 FP32 累加
scale、subtract、exp2：使用 FP16x2
row_sum、online-softmax 状态：保持 FP32
P 输出：保持 E4M3
```

这种修改不会改变 QK MMA、TMEM layout、pipeline 接口和 `softmax_step`
函数签名。它适合用于快速验证 FP16 softmax 的精度和性能。

## 1. 增加头文件

在 `sm100_fmha_fwd_mainloop_mxfp8_n128_pvmx.hpp` 顶部增加：

```cpp
#include <cuda_fp16.h>
```

## 2. 修改位置一：Mask 后将 Score 舍入到 FP16

在当前代码：

```cpp
if constexpr (need_apply_mask) {
  Mask{}.apply_mask(tTMEM_LOADrS, tTMEM_LOADcS, problem_shape);
}

ElementQK old_row_max = row_max;
```

中间插入：

```cpp
// Experimental FP16 softmax path:
// QK remains FP32 in TMEM, but all subsequent softmax math observes scores
// rounded to FP16. Keep the tensor storage type float so existing masking,
// coordinate handling and E4M3 conversion code remain unchanged.
CUTLASS_PRAGMA_UNROLL
for (int i = 0; i < size(tTMEM_LOADrS); i += 2) {
  __half2 hs = __floats2half2_rn(
      tTMEM_LOADrS(i + 0),
      tTMEM_LOADrS(i + 1));
  float2 rounded = __half22float2(hs);
  tTMEM_LOADrS(i + 0) = rounded.x;
  tTMEM_LOADrS(i + 1) = rounded.y;
}
```

修改后的上下文：

```cpp
if constexpr (need_apply_mask) {
  Mask{}.apply_mask(tTMEM_LOADrS, tTMEM_LOADcS, problem_shape);
}

// QK FP32 score -> FP16 -> FP32 representation.
// From this point onward, softmax sees FP16-rounded scores.
CUTLASS_PRAGMA_UNROLL
for (int i = 0; i < size(tTMEM_LOADrS); i += 2) {
  __half2 hs = __floats2half2_rn(
      tTMEM_LOADrS(i + 0),
      tTMEM_LOADrS(i + 1));
  float2 rounded = __half22float2(hs);
  tTMEM_LOADrS(i + 0) = rounded.x;
  tTMEM_LOADrS(i + 1) = rounded.y;
}

ElementQK old_row_max = row_max;
```

原有 row-max 代码可以保持不变。因为 `tTMEM_LOADrS` 中的值已经被舍入到
FP16，所以它计算的是 FP16 score 的最大值，但 `row_max` 本身仍是 FP32。

## 3. 修改位置二：使用 FP16x2 计算 Scale/Subtract/Exp2

保留当前 SFP 读取、bias 计算和以下变量：

```cpp
float bias_g0 = -row_max_scale - float(int(sfp_e0) - 127);
float bias_g1 = -row_max_scale - float(int(sfp_e1) - 127);
```

删除或替换：

```cpp
float2 scale_fp32x2 = make_float2(scale, scale);
float2 bias_g0_f32x2 = make_float2(bias_g0, bias_g0);
float2 bias_g1_f32x2 = make_float2(bias_g1, bias_g1);
```

改为：

```cpp
// Scale and bias are also rounded to FP16 for the half2 softmax path.
__half2 scale_f16x2 = __float2half2_rn(scale);
__half2 bias_g0_f16x2 = __float2half2_rn(bias_g0);
__half2 bias_g1_f16x2 = __float2half2_rn(bias_g1);
```

然后将当前 exp 循环：

```cpp
CUTLASS_PRAGMA_UNROLL
for (int i = 0; i < size(tTMEM_LOADrS); i += kConversionsPerStep) {
  CUTLASS_PRAGMA_UNROLL
  for (int j = 0; j < kConversionsPerStep; j += 2) {
    float2 in = make_float2(
      tTMEM_LOADrS(i + j + 0),
      tTMEM_LOADrS(i + j + 1)
    );
    float2 out;
    cute::fma(out, scale_fp32x2, in,
              (i < 32) ? bias_g0_f32x2 : bias_g1_f32x2);
    tTMEM_LOADrS(i + j + 0) = fast_exp2f(out.x);
    tTMEM_LOADrS(i + j + 1) = fast_exp2f(out.y);
  }

  // Existing E4M3 conversion...
}
```

替换为：

```cpp
NumericArrayConverter<Element, ElementQK, kConversionsPerStep> convert;

CUTLASS_PRAGMA_UNROLL
for (int i = 0; i < size(tTMEM_LOADrS); i += kConversionsPerStep) {
  CUTLASS_PRAGMA_UNROLL
  for (int j = 0; j < kConversionsPerStep; j += 2) {
    // Scores were already rounded to FP16 after masking. Pack two scores into
    // one half2, perform scale+bias in FP16x2, then evaluate two exp2 values.
    __half2 score_f16x2 = __floats2half2_rn(
        tTMEM_LOADrS(i + j + 0),
        tTMEM_LOADrS(i + j + 1));

    __half2 bias_f16x2 = (i < 32) ? bias_g0_f16x2 : bias_g1_f16x2;
    __half2 exp_arg_f16x2 = __hfma2(score_f16x2, scale_f16x2, bias_f16x2);
    __half2 p_f16x2 = h2exp2(exp_arg_f16x2);

    // Keep P as FP32 values in the existing register tensor. This allows the
    // existing E4M3 converter and FP32 row-sum reduction to remain unchanged.
    float2 p_f32x2 = __half22float2(p_f16x2);
    tTMEM_LOADrS(i + j + 0) = p_f32x2.x;
    tTMEM_LOADrS(i + j + 1) = p_f32x2.y;
  }

  // Existing FP32 -> E4M3 conversion and shared-memory store remain unchanged.
  Array<ElementQK, kConversionsPerStep> in_conv;
  CUTLASS_PRAGMA_UNROLL
  for (int j = 0; j < kConversionsPerStep; ++j) {
    in_conv[j] = tTMEM_LOADrS(i + j);
  }

  Array<Element, kConversionsPerStep> out_conv = convert(in_conv);
  auto out_words = reinterpret_cast<uint32_t const*>(&out_conv);
  int k0 = p_kbase + i;
  Element& dst_byte0 =
      sP_st(make_coord(p_row, k0 % 32), _0{}, k0 / 32);
  uint4 packed =
      make_uint4(out_words[0], out_words[1], out_words[2], out_words[3]);
  *reinterpret_cast<uint4*>(&dst_byte0) = packed;
}
```

## 4. 不要修改的部分

以下变量和计算必须继续使用 FP32：

```cpp
float& row_max;
float& row_sum;
float old_row_max;
float row_max_safe;
float acc_scale;
float local_row_sum;
__shared__ float smem_sm_exch[2][128];
```

特别是不要将函数签名改成：

```cpp
softmax_step(half& row_max, half& row_sum, ...)
```

`row_sum` 和 online-softmax rescale 会跨越大量 KV tile。使用 FP16 会明显增加累计误差，
并可能造成溢出。

## 5. 这个最小方案的局限

这个版本实现了 FP16 数值路径，但不一定降低寄存器数量：

```text
tTMEM_LOADrS 的存储类型仍然是 float
FP16 score 被转换回 float 后保存在原寄存器 tensor 中
```

这样设计的目的是先隔离验证：

- FP16 score 舍入带来的精度变化；
- FP16x2 FMA/exp2 是否有性能收益；
- `h2exp2` 在 B200 上是否真的降低 MUFU/issue 开销。

如果该版本验证通过且性能提升，再进行第二阶段优化：将 score fragment 真正改为 packed
`__half2` 数组，避免 FP16 转回 FP32 保存。但第二阶段需要重写 mask、row-max 和 P
conversion，风险和改动范围更大。

## 6. 推荐验证

正确性：

```bash
--b=1 --h=40 --q=512  --k=512  --d=128 --mask=no --verify
--b=1 --h=40 --q=1024 --k=1013 --d=128 --mask=no --verify
--b=1 --h=40 --q=4096 --k=4096 --d=128 --mask=no --verify
```

性能与 NCU 重点观察：

```text
Duration
MUFU/XU utilization
Issue Active
Eligible Warps
MIO Throttle
Long Scoreboard
Registers/thread
Local-memory spilling
```

注意：此前代码注释记录过 B200 上 `ex2.approx.f16x2` 实验可能没有提升 MUFU 的
element throughput，并增加 pack/unpack 开销。因此该方案应作为消融实验，不应预设一定更快。
