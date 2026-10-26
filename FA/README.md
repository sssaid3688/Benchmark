# MXFP8 FlashAttention Forward

This project contains the B200 MXFP8 FlashAttention forward implementation.

## Requirements

- NVIDIA B200 (SM100a)
- CUDA Toolkit 13.3 or later
- CMake 3.20 or later
- The repository's `cutlass` checkout available beside `FA`

## Build the performance target

From the repository root:

```bash
cmake -S FA -B FA/build -DCMAKE_BUILD_TYPE=Release \
  -DCUTLASS_NVCC_ARCHS=100a \
  -DCMAKE_CUDA_COMPILER=/usr/local/cuda-13.3/bin/nvcc

cmake --build FA/build \
  --target op6static2sm_no_e2rsf_fa4mufu_b200_blackwell_fmha_mxfp8 \
  -j 16
```

The target uses the 2-SM MXFP8 forward path with the selected MUFU/software-exp2 split.

## Run

```bash
BIN=FA/build/b200_blackwell_fmha_mxfp8/op6static2sm_no_e2rsf_fa4mufu_b200_blackwell_fmha_mxfp8

$BIN --b=1 --h=40 --h_k=40 --d=128 \
  --q=170100 --k=170100 --mask=no \
  --warmup_iterations=5 --iterations=20

$BIN --b=1 --h=40 --h_k=40 --d=128 \
  --q=510300 --k=510300 --mask=no \
  --warmup_iterations=5 --iterations=20
```

Run on an otherwise idle GPU for representative measurements.
