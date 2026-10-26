# FP8 FlashAttention Backward

This project contains the B200 FP8 FlashAttention backward implementation.

## Requirements

- NVIDIA B200 (SM100a)
- CUDA Toolkit 13.3 or later
- CMake 3.20 or later
- The repository's `cutlass` checkout available beside `FAG_FP32`

## Build the performance target

From the repository root:

```bash
cmake -S FAG_FP32 -B FAG_FP32/build -DCMAKE_BUILD_TYPE=Release \
  -DCUTLASS_NVCC_ARCHS=100a \
  -DCMAKE_CUDA_COMPILER=/usr/local/cuda-13.3/bin/nvcc

cmake --build FAG_FP32/build --target fag_cand_colloop -j 16
```

`fag_cand_colloop` is the selected 2-SM FP8 backward target with the configured MUFU/software-exp2 split.

## Run

```bash
BIN=FAG_FP32/build/fag_cand_colloop

$BIN --b=1 --h=16 --d=128 --d_vo=128 \
  --q=4096 --k=4096 --iterations=100 --verbose

$BIN --b=1 --h=16 --d=128 --d_vo=128 \
  --q=8192 --k=8192 --iterations=50 --verbose

$BIN --b=1 --h=16 --d=128 --d_vo=128 \
  --q=16384 --k=16384 --iterations=20 --verbose
```

Run on an otherwise idle GPU for representative measurements.
