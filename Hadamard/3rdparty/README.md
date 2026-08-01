# 第三方依赖说明

`Hadamard/src/doublewarp_fusion.cu` 编译时需要两类第三方头文件依赖。
本目录收录其一（TE 私有头），另一类（CUTLASS）复用仓库顶层的同级目录。

## 1. CUTLASS —— 复用 `../../cutlass`（不在本目录重复存放）

`doublewarp_fusion.cu` 通过 `#include <cutlass/...>` 与 `#include <cute/...>`
引用 CUTLASS（SM100 / Blackwell pipeline、blockscaled layout、numeric conversion 等）。

CUTLASS 体积较大，本工程**不复制**，直接复用仓库根的同级 submodule：

```
<Benchmark>/                 # 即本仓库根（test_merge/Benchmark）
├── cutlass/                 # ← CUTLASS v4.5+，git submodule，FA/FAG/Hadamard 共用
└── Hadamard/                # ← 本工程
```

编译时需要把 `../../cutlass/include` 加入头文件搜索路径
（容器内 TE 的增量编译已自动配置，见根 README）。

## 2. TransformerEngine 私有头 —— 本目录 `te_headers/`

`doublewarp_fusion.cu` 还引用了若干 TransformerEngine（TE）的内部头，
这些头不是 CUTLASS 的一部分，也不随 CUDA Toolkit 发布，属于 TE 私有依赖：

| 引用方式（源码中） | 对应文件 |
|---|---|
| `#include "common/common.h"` | `te_headers/transformer_engine/common/common.h` |
| `#include "common/utils.cuh"` | `te_headers/transformer_engine/common/utils.cuh` |
| `#include "common/util/cuda_runtime.h"` | `te_headers/transformer_engine/common/util/cuda_runtime.h` |
| `#include "common/util/curanddx.hpp"` | `te_headers/transformer_engine/common/util/curanddx.hpp` |
| `#include "common/util/ptx.cuh"` | `te_headers/transformer_engine/common/util/ptx.cuh` |
| `#include "customized_pipeline.cuh"` | `te_headers/transformer_engine/common/hadamard_transform/customized_pipeline.cuh` |
| `#include <transformer_engine/hadamard_transform.h>` | `te_headers/transformer_engine/common/include/transformer_engine/hadamard_transform.h`（installed header，由 `-I common/include` 解析） |

> 说明：源码里 `#include "customized_pipeline.cuh"` 是相对引用——在 TE 工程内，
> `doublewarp_fusion.cu` 与 `customized_pipeline.cuh` 同处 `common/hadamard_transform/` 目录，
> 因此能直接相对引用。本目录保留了与 TE 相同的目录结构，便于对照查阅。

这些头文件从容器内 TE 的 `transformer_engine/` 目录导出，仅含头文件
（`.h` / `.hpp` / `.cuh` / `.inl`），不含 `.cu` / `.cpp` 源文件。
仅供阅读与排查依赖使用——实际编译仍需在容器内 TE 的完整构建环境中进行
（详见根 `README.md` 的「工作方式」一节）。
