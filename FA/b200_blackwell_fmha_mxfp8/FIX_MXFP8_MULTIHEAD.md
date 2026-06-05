# MXFP8 Flash Attention 多卡头精度问题修复文档

## 问题描述

当 MXFP8 Flash Attention 中 scale factor（缩放因子）不为 1 且 head 数 > 1 时，kernel 输出的 O 与 reference 对比的 `max_diff` 高达 3.5 以上，远超过 MXFP8 的精度阈值（0.1），导致验证失败。

### 复现条件

```bash
# 失败案例：h=40 (多head)
./examples/77_blackwell_fmha/101_blackwell_fmha_mxfp8 \
  --b=1 --h=40 --d=128 --q=1024 --k=1024 --verify=1 \
  --init-style-sfq=r --init-style-sfk=1

# 成功案例：h=1 (单head)
./examples/77_blackwell_fmha/101_blackwell_fmha_mxfp8 \
  --b=1 --h=1 --d=128 --q=1024 --k=1024 --verify=1 \
  --init-style-sfq=r --init-style-sfk=1
```

---

## 修改 1：TMA 的 head/batch 坐标错误（根本原因）

### 文件

`collective/sm100_fmha_load_tma_warpspecialized.hpp`

### 问题分析

在 SF（Scale Factor）的 TMA 加载中，代码使用 `get<2>(blk_coord_q)` 和 `get<2>(blk_coord_kv)` 来获取 head/batch（L 模式）坐标。

```cpp
// 错误的用法（原始代码）
Tensor tQgQ_sf = tQgQ_qdl_sf(_, _, _0{}, get<2>(blk_coord_q));
Tensor tKgK_sf = tKgK_kdl_sf(_, _, _0{}, get<2>(blk_coord_kv));
auto tPgP_sf_view = tPgP_sf(_, _, _0{}, get<2>(blk_coord_kv));
auto tVgV_sf = tVgV_dkl_sf(_, _0{}, _, get<2>(blk_coord_kv));
```

`blk_coord` 中的 L 坐标是一个 **tuple**（如 `(piece_0, piece_1)`），而不是简单的整数。直接使用 `get<2>(bc)` 返回的是这个 tuple，在 tensor 切片时会被错误解释。结果就是 **head > 0 的所有 CTA 都加载了 head 0 的 SF 数据**。

代码中已经定义了正确的坐标转换函数 `sf_l_coord`，但被注释掉了：

```cpp
auto sf_l_coord = [&](auto const& bc) {
    return make_coord(_0{}, crd2idx(get<2>(bc), get<3>(problem_shape)));
};
```

### 修复

将所有 SF TMA 加载中的 `get<2>(blk_coord_*)` 替换为 `sf_l_coord(blk_coord_*)`：

```cpp
// 正确的用法（修复后）
Tensor tQgQ_sf = tQgQ_qdl_sf(_, _, _0{}, sf_l_coord(blk_coord_q));
Tensor tKgK_sf = tKgK_kdl_sf(_, _, _0{}, sf_l_coord(blk_coord_kv));
auto tPgP_sf_view = tPgP_sf(_, _, _0{}, sf_l_coord(blk_coord_kv));
auto tVgV_sf = tVgV_dkl_sf(_, _0{}, _, sf_l_coord(blk_coord_kv));
```

### 修改行

| 行号 | 修改前 | 修改后 |
|------|--------|--------|
| 259 | `get<2>(blk_coord_q)` | `sf_l_coord(blk_coord_q)` |
| 296 | `get<2>(blk_coord_kv)` | `sf_l_coord(blk_coord_kv)` |
| 326 | `get<2>(blk_coord_kv)` | `sf_l_coord(blk_coord_kv)` |
| 355 | `get<2>(blk_coord_kv)` | `sf_l_coord(blk_coord_kv)` |

> **注意**：`sf_l_coord` 函数已在文件中定义（第 206 行），只需取消注释其使用。

---

## 修改 2：Reference 验证的 SF 数据排布修正

### 文件

`examples/77_blackwell_fmha/77_blackwell_fmha.cu`

### 问题分析

CUTLASS kernel 使用 block-scaled layout 存储 SF 数据，其地址映射为：

```
layout_SFQ row stride: ((16,4),512)
模式: 行 % 32 → stride 16, (行/32)%4 → stride 4, (行/128) → stride 512
```

但 reference kernel 期望 SF 数据按 **row-major** 顺序存储：

```
ref[row * SF_D + group + head * SQ * SF_D]
```

这两个布局不一致。对于 h=1 时，由于 buffer 恰好包含完整数据，精度差很小（max_diff ≈ 0.03），但对于 h>1 时错误的排布会导致 reference 读到错误的数据。

### 修复方案

在 `buffer_init_fn` 中添加 `fill_ref` lambda，将 kernel buffer 中的数据从 CUTLASS block-scaled 布局重排为 row-major 布局，存入独立的 `block_ref_SF*` buffer 中供 reference 使用。

```cpp
auto fill_ref = [&](auto& kernel_buf, auto& ref_buf, int rows, int hb, auto& layout_sf) {
    size_t n_kernel = cosize(layout_sf);
    size_t n_ref = (size_t)rows * SF_D_loc * hb;
    std::vector<ElementScale> host_kernel(n_kernel);
    std::vector<ElementScale> host_ref(n_ref, ElementScale(0));
    cudaMemcpy(host_kernel.data(), kernel_buf.get(), n_kernel * sizeof(ElementScale),
               cudaMemcpyDeviceToHost);
    for (int r = 0; r < rows; r++) {
        for (int g = 0; g < SF_D_loc; g++) {
            for (int h = 0; h < hb; h++) {
                int mode0 = (r % 32) * 16 + ((r / 32) % 4) * 4 + (r / 128) * 512;
                int cutlass_idx = mode0 + g + h * rows * SF_D_loc;
                int rm_idx = r * SF_D_loc + g + h * rows * SF_D_loc;
                if (cutlass_idx < n_kernel && rm_idx < n_ref) {
                    host_ref[rm_idx] = host_kernel[cutlass_idx];
                }
            }
        }
    }
    cudaMemcpy(ref_buf.get(), host_ref.data(), n_ref * sizeof(ElementScale),
               cudaMemcpyHostToDevice);
};
```

#### 公式推导

`layout_SFQ` 对坐标 `(row=r, k_elem=g*32, head=h)` 的地址计算：

- **Mode 0**（行维度）：分解为 `r0 = r % 32`（stride 16）、`r1 = (r/32) % 4`（stride 4）、`r2 = r / 128`（stride 512）
- **Mode 1**（K 元素维度）：传入 `g * 32`（即 group 索引 × 向量大小），相当于 K 维度中第 g 组的起始位置，stride 为 1 → offset = g
- **Mode 2**（head 维度）：offset = `h * SQ * SF_D`

> 关键要点：`layout_SFQ(r, g, h)` 将 g 传入 Mode 1（K 元素索引），但 Mode 1 的前 32 个位置 stride 为 0（共享同一个 scale factor）。因此 **g 必须作为 K 元素索引（g*32）传入**，而不是作为 group 索引直接传入。

### 新增成员

在 `DeviceBuffer` 结构体中添加了 row-major 参考 SF buffer：

```cpp
struct DeviceBuffer {
    // ... 原有成员 ...
    DeviceAllocation<ElementScale> block_ref_SFQ;
    DeviceAllocation<ElementScale> block_ref_SFK;
    DeviceAllocation<ElementScale> block_ref_SFP;
    DeviceAllocation<ElementScale> block_ref_SFV;
};
```

---

## 修改 3：命令行选项增强

### 文件

`examples/77_blackwell_fmha/77_blackwell_fmha.cu`

### 新增的初始化风格选项

为 SF 数据添加了独立的初始化风格控制：

| 选项 | 值 | 说明 |
|------|-----|------|
| `--init-style-sfq` | r/1/d/s/n | Q scale factor 初始化风格 |
| `--init-style-sfk` | r/1/d/s/n | K scale factor 初始化风格 |
| `--init-style-sfp` | r/1/d/s/n | P scale factor 初始化风格 |
| `--init-style-sfv` | r/1/d/s/n | V scale factor 初始化风格 |

初始化风格含义：
- `r` = Random（随机高斯分布）
- `1` = One（全部为 1.0）
- `d` = kLinearStride1（每 4 个元素重复 [0,1,2,3]）
- `s` = kLinearStride128（每 128 元素块内常量，块值递增）
- `n` = None（不初始化）

---

## 验证结果

| 测试配置 | SF_Q | SF_K | O max_diff | LSE max_diff | 结果 |
|----------|------|------|-----------|-------------|------|
| H=40, Q=1024, K=1024 | random | 1.0 | **0.043** | **3e-6** | ✅ PASS |
| H=40, Q=1024, K=1024 | random | random | **0.067** | **1e-5** | ✅ PASS |
| H=1, Q=256, K=256 | random | 1.0 | **0.031** | **1e-6** | ✅ PASS |
| H=2, Q=128, K=128 | random | 1.0 | **0.029** | **1e-6** | ✅ PASS |
| H=2, Q=128, K=128 | kLinearStride128 | 1.0 | **0.044** | **2e-6** | ✅ PASS |

所有测试的 `max_diff` 均远低于 MXFP8 阈值（0.1），修复成功。

---

## 文件变更清单

| 文件 | 变更类型 | 说明 |
|------|----------|------|
| `collective/sm100_fmha_load_tma_warpspecialized.hpp` | **修改** | SF TMA head 坐标用 `sf_l_coord` 替换 `get<2>` |
| `examples/77_blackwell_fmha/77_blackwell_fmha.cu` | **修改** | 添加 fill_ref 重排 SF 数据、新增 block_ref buffer、命令行选项 |
