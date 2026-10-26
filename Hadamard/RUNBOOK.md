# doublewarp Hadamard+NVFP4 最优版本 运行手册（宿主机版）

> 本手册对应**最优版本** `src/doublewarp_fusion.cu`（md5 `e6ff5d9c1896c34dbbbafbe02bdea0ca`），
> kernel 为 `row_col_rht_gemm_device`（col-only RHT+NVFP4 融合量化，MUFU fast_math 分流）。
> 原 README 的容器方案（`te-base` + docker）已失效（容器/镜像被删），本手册为**宿主机直跑方案**，
> 已于 2026-08-29 在本机（B200 / Ubuntu 22.04 / driver 580 / CUDA 13.3）全流程验证。

---

## 0. 一键速跑（环境已就绪时）

```bash
export PYBIN=/home/ubuntu/workspace/wzg/gjl/env/bin/python
export PYTHONPATH=/workspace/hadamard_base/TransformerEngine
export LD_LIBRARY_PATH=/home/ubuntu/workspace/wzg/gjl/env/lib/python3.12/site-packages/torch/lib

cd /home/ubuntu/workspace/oyhj/temp/test/test_merge/Benchmark/Hadamard

# 1) CUDA event 计时（全部 7 个 shape）
NVTE_USE_FAST_MATH=1 $PYBIN scripts/benchmark.py --fm 1

# 2) 逐 kernel 带宽分析（主 fusion / 全局 amax / 合并利用率）
$PYBIN scripts/kernel_bw_breakdown.py 1        # 参数为 fm 值，可传 0 1
```

---

## 1. 版本说明

| 版本 | 位置 | md5 |
|---|---|---|
| **doublewarp（最优，本工程）** | `src/doublewarp_fusion.cu` | `e6ff5d9c1896c34dbbbafbe02bdea0ca` |
| triplewarp_v2（历史中间版） | TE 树内 `*.triplewarp_v2_bak` | `9be3ca6b2eeb97b0b2a1f4de179f15b3` |
| original baseline | TE 树内 `*.original_baseline_bak` | `aef0216152c0a33afa2e8fcec87374b5` |

- 部署目标路径（TE 树内）：
  `/workspace/hadamard_base/TransformerEngine/transformer_engine/common/hadamard_transform/row_cast_col_hadamard_transform_cast_fusion.cu`
- `NVTE_USE_FAST_MATH=1`（fm1）= MUFU 分流开启：`reciprocal_approximate_ftz` → 裸 `MUFU.RCP`，
  替代 IEEE 除法的 Newton 修正序列。SASS 实测：col-only 路径 fm0=1720 条指令 → fm1=1400 条。
  收益：小 shape 快 20-25%，大 shape 持平。

## 2. 环境架构（宿主机，无 docker）

```
/home/ubuntu/workspace/oyhj/hadamard_base/TransformerEngine   # TE 源码+build 树（= /workspace/hadamard_base）
├── transformer_engine/
│   ├── libtransformer_engine.so           # 主库（含 doublewarp kernel，sm_100a/sm_103a）
│   └── transformer_engine_torch.so        # pybind 扩展（宿主机 torch 2.13 重建版）
│                                          #   旧容器版备份为 *.container_bak
├── build/cmake/                           # ninja 增量编译目录（build.ninja 内是 /workspace 路径）
└── 3rdparty/{cutlass,cudnn-frontend}      # 头文件依赖

/home/ubuntu/workspace/wzg/gjl/env         # python 3.12.13 + torch 2.13.0+cu130
/usr/local/cuda-13.3                       # nvcc（编译用，只编 sm_100a + sm_103a）
```

### 一次性环境配置（仅在环境丢失时需要重做）

1. **`/workspace` 符号链接**（build.ninja 内嵌容器路径，必须还原）：
   ```bash
   mkdir -p /workspace
   ln -sfn /home/ubuntu/workspace/oyhj /workspace/oyhj          # （可选）
   ln -sfn /home/ubuntu/workspace/oyhj/hadamard_base /workspace/hadamard_base
   ```
2. **cmake 路径符号链接**（ninja 重新生成 build.ninja 时调用的是容器内 cmake 路径）：
   ```bash
   mkdir -p /usr/local/lib/python3.12/dist-packages/cmake/data/bin
   ln -sfn /usr/local/bin/cmake /usr/local/lib/python3.12/dist-packages/cmake/data/bin/cmake
   ```
3. **pip 依赖**（gjl env）：
   ```bash
   /home/ubuntu/workspace/wzg/gjl/env/bin/pip install pybind11 ninja pydantic onnxscript einops
   ```
4. **pip 元数据 stub**（TE 的 `sanity_checks_for_pypi_installation` 需要；无 WHEEL 文件即跳过 PyPI 校验）：
   ```
  gjl_env/lib/python3.12/site-packages/transformer_engine-2.18.0.dev0.dist-info/
   ├── METADATA    # Metadata-Version: 2.1 / Name: transformer_engine / Version: 2.18.0.dev0
   ├── INSTALLER
   └── RECORD      # 三行：METADATA,, / INSTALLER,, / RECORD,,
   ```
5. **pybind 扩展重建**（仅当 torch 版本变化或 `transformer_engine_torch.so` 报 undefined symbol 时）：
   ```bash
   LD_LIBRARY_PATH=<gjl_env>/lib/python3.12/site-packages/torch/lib \
       <gjl_env>/bin/python scripts/build_te_torch_ext.py
   # 产物自动部署到 TE/transformer_engine/transformer_engine_torch.so
   ```
   > 注意：不能跑 `python setup.py build_ext`——它会传入全架构列表（75~120f）触发数小时的全量重编。
   > 手动增量编译/重配 cmake 时务必带 `-DCMAKE_CUDA_ARCHITECTURES="100a;103a"`（只用 sm100！）。

## 3. 编译 / 切换版本

```bash
# 部署 doublewarp 源码到 TE 树 + 单文件增量编译 + 重部署 .so（约 1-2 分钟）
bash scripts/swap_and_build.sh src/doublewarp_fusion.cu

# 验证
md5sum /workspace/hadamard_base/TransformerEngine/transformer_engine/common/hadamard_transform/row_cast_col_hadamard_transform_cast_fusion.cu
# 期望 e6ff5d9c1896c34dbbbafbe02bdea0ca
```

等价的手动步骤：
```bash
TE=/workspace/hadamard_base/TransformerEngine
cp src/doublewarp_fusion.cu $TE/transformer_engine/common/hadamard_transform/row_cast_col_hadamard_transform_cast_fusion.cu
cd $TE/build/cmake && ninja transformer_engine           # 只重编 1 个 .o + relink
cp libtransformer_engine.so $TE/transformer_engine/
```

## 4. 运行 benchmark

### 4.1 CUDA event 计时（粗粒度，含每次调用的 launch/dispatch 开销）
```bash
$PYBIN scripts/benchmark.py --fm 1            # 全部 shape
$PYBIN scripts/benchmark.py --fm 1 --shape-idx 4   # 只跑 shape7（idx 表见下）
```
> 本机宿主 python dispatch 较慢（~9us/call），event 模式对短 kernel 偏高；kernel 级数据以 4.2 为准。

### 4.2 逐 kernel 带宽分析（推荐，等价 README 的 nsys 口径）
```bash
$PYBIN scripts/kernel_bw_breakdown.py 1        # fm1
$PYBIN scripts/kernel_bw_breakdown.py 0 1      # fm0 和 fm1 都跑
```
输出列：主 fusion（row_cast_col_hadamard_transform_cast_fusion TU）/ 全局 amax
（HadamardAmaxTmaKernel，读一遍 X）/ 合并利用率。字节口径：
**搬入 X(bf16, 2B) + 搬出 Q(fp4, 0.5B) + 搬出 SF(fp8, M·K/16) = M·K × 2.5625**，理论带宽 8.0 TB/s。

### 4.3 shape 索引表（benchmark.py 用）

| idx | name | M(padded) | K | 用户 shape 编号 |
|---|---|---|---|---|
| 0 | shape8 | 8192 | 2048 | shape 8 |
| 1 | shape5 | 8192 | 4096 | shape 5 |
| 2 | shape9 | 16384 | 2048 | shape 9 |
| 3 | shape6 | 16384 | 4096 | shape 6 |
| 4 | shape7 | 32768 | 4096 | shape 7 |
| 5 | shape1 | 170112 | 5120 | shape 1 / 3（170100 padded） |
| 6 | shape2 | 510336 | 5120 | shape 2 / 4（510300 padded） |

### 4.4 nsys（可选，host 已装 2026.1.4）
```bash
nsys profile -t cuda,nvtx -o /tmp/shape8_fm1 \
    env PYTHONPATH=$PYTHONPATH LD_LIBRARY_PATH=$LD_LIBRARY_PATH NVTE_USE_FAST_MATH=1 \
    $PYBIN scripts/benchmark.py --nsys --shape-idx 0 --fm 1
nsys stats --report cuda_gpu_kern_sum /tmp/shape8_fm1.nsys-rep | grep -i rht
```

## 5. 参考结果（2026-08-29 实测，fm1，kernel 级中位数）

| shape | M×K | fusion μs | fusion TB/s | 主利用率 | amax TB/s | amax利用率 | 合并利用率 |
|---|---|---|---|---|---|---|---|
| shape1/3 | 170112×5120 | 340.9 | 6.55 | **81.8%** | 7.05 | 88.1% | 84.2% |
| shape2/4 | 510336×5120 | 1044.2 | 6.41 | **80.2%** | 7.28 | 91.0% | 84.5% |
| shape5 | 8192×4096 | 12.53 | 6.86 | **85.8%** | 6.51 | 81.3% | 79.4% |
| shape6 | 16384×4096 | 27.33 | 6.29 | 78.7% | 4.99 | 62.3% | 68.5% |
| shape7 | 32768×4096 | 52.47 | 6.55 | **81.9%** | 5.66 | 70.8% | 75.4% |
| shape8 | 8192×2048 | 7.39 | 5.82 | 72.7% | 6.99 | 87.4% | 71.3% |
| shape9 | 16384×2048 | 12.52 | 6.87 | **85.9%** | 6.63 | 82.9% | 80.1% |

- 与 README 的 nsys 参考数据（容器环境）误差 <4%，版本部署正确。
- shape8（grid 128、波次 0.86，SM 未填满）与 shape6（amax kernel 拖累）低于 80%，
  与 README 文档记录（73.7%/79.6%）一致，属 shape 并行度限制。
- 本机实测 torch copy 峰值 6.65 TB/s；shape5/9 主 kernel（6.86/6.87）已超拷贝吞吐。
- 锁频 1965MHz 复测无差异（本机无 DVFS 降频问题，421W@1852MHz）。

## 6. 常见问题

| 现象 | 原因 / 解决 |
|---|---|
| `Could not find transformer-engine` | dist-info stub 丢失，见 §2.4 |
| `Multiple files found: transformer_engine_torch.*.so` | TE 目录下有多个 pybind .so，只留宿主重建版 |
| `undefined symbol: materialize_cow_storage` | pybind 与当前 torch ABI 不符，重跑 §2.5 重建脚本 |
| `GLIBC_2.38 not found` | 用到容器编译的 .so，须在宿主机重编（§3） |
| ninja 触发全量重编 | cmake 配置被改动过；重配时必须带 `-DCMAKE_CUDA_ARCHITECTURES="100a;103a"`，且不要跑 `setup.py build_ext` |
| 想恢复容器方案 | TE 官方镜像 `nvcr.io/nvidia/pytorch:26.01-py3`（见 temp/TransformerEngine/README.rst），但需重新搭 TE build 环境，不推荐 |

## 7. 归档脚本清单（scripts/）

| 脚本 | 用途 |
|---|---|
| `deploy_and_build.sh` | 原 docker 方案（已失效，留档） |
| `swap_and_build.sh` | **宿主机版**：部署 .cu + 单文件增量编译 + 重部署 .so |
| `build_te_torch_ext.py` | 宿主机重建 pybind 扩展（torch cpp_extension，仅 .cpp） |
| `benchmark.py` | CUDA event 计时 / nsys 采集 |
| `kernel_bw_breakdown.py` | 逐 kernel 带宽：主 fusion / amax / 合并 |
