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

The program's `tma` line is an end-to-end CUDA-event result for the main BWD kernel plus the `SumOdO` and `Convert` helper kernels. It is useful for three-kernel throughput, but it is not a main-kernel-only measurement.

## Reproduce the main-kernel nsys measurement

The following sequence measures the fused 2-SM backward main kernel. The `fag3_reduce` target is a timing-only dQ-reduce ablation: it deliberately removes the reduce data path and must not be used as a correctness or final-performance target.

```bash
nvidia-smi -lgc 1965,1965

cd FAG_FP32
cmake --build build --target \
  fag_cand_colloop fag3_reduce fag3_reduce_tma fag3_pack \
  fag3_softmax fag3_corner fag3_bulk fag3_floor fag3_opt_noex2 \
  -- -j 16

nsys profile --force-overwrite=true -o /tmp/fag_colloop \
  ./build/fag_cand_colloop \
  --b=1 --h=16 --q=16384 --k=16384 \
  --d=128 --d_vo=128 --iterations=20 --mask=no
nsys stats -r cuda_gpu_kern_sum --format csv /tmp/fag_colloop.nsys-rep

nsys profile --force-overwrite=true -o /tmp/fag_reduce \
  ./build/fag3_reduce \
  --b=1 --h=16 --q=16384 --k=16384 \
  --d=128 --d_vo=128 --iterations=20 --mask=no
nsys stats -r cuda_gpu_kern_sum --format csv /tmp/fag_reduce.nsys-rep
```

In `cuda_gpu_kern_sum`, select the row named `Sm100FmhaBwdKernelTma2SmWarpSpecialized`; its `Med (ns)` is the main-kernel median. Ignore initialization, reference, conversion, and SumOdO rows when reporting this metric.

After the measurement, check the actual clock under load. A configured 1965 MHz application clock can still be reduced by power or thermal limits.
