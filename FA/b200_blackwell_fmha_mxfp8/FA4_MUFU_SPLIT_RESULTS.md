# FA4-style MUFU split for MXFP8 FA on B200

## Scope

- GPU: NVIDIA B200, 148 SMs
- Build: Release, CUDA 13.3, `CUTLASS_NVCC_ARCHS=100a`
- Baseline target: `op6static2sm_b200_blackwell_fmha_mxfp8`
- Split target: `op6static2sm_fa4mufu_b200_blackwell_fmha_mxfp8`
- Benchmark: `--b=1 --h=40 --h_k=40 --q=170100 --k=170100 --d=128 --mask=no`

## Implementation

The split is independent of the older experimental exp2 helpers in the mainloop. It uses a packed
`float2` Cody-Waite range reduction and a degree-3 polynomial. The original fully-unrolled softmax
loop is retained. Pair selection folds at compile time, so the dispatcher adds no runtime branch,
integer division/remainder, synchronization, or memory operation.

The retained configuration emulates two of the 32 element-pairs handled by each softmax thread
(6.25%). Emulation is placed in the middle of the row so native MUFU and conversion work remains
after the polynomial chain.

## Performance

Higher fractions regressed in interleaved A/B tests:

| Emulated fraction | Result versus paired baseline |
|---:|---:|
| 18.75% | about -0.39% |
| 12.5% | about -0.22% |
| 6.25% | retained; effectively flat to slightly positive |

Final high-precision test used four A/B samples per target, 5 warmups and 20 timed iterations:

| Target | Samples (TFLOPS/s) | Median | Mean |
|---|---|---:|---:|
| Baseline | 1534.96, 1528.91, 1528.00, 1526.90 | 1528.46 | 1529.69 |
| 6.25% split | 1534.39, 1530.25, 1529.76, 1528.43 | 1530.01 | 1530.71 |

Median change is +0.10%; mean change is +0.07%. This is close to run-to-run noise and should be
treated as performance-neutral with a small positive indication, not as a material speedup.

## Binary audit

- Whole-binary `MUFU.EX2` count: 991 -> 935
- Whole-binary branch count: unchanged (3060 -> 3060)
- `IDIV`/`REM`: 0 -> 0
- Main kernel registers: 128 -> 128
- Main kernel local memory: 0 -> 0
- A zero-emulation control target produced a SASS hash identical to the baseline, confirming the
  split framework itself adds no low-level instructions or scheduling changes.

## Correctness

- No-mask MHA: passed O and LSE reference checks.
- Residual-mask MHA: passed O and LSE reference checks.
- The existing 2SM target times out for causal mode in both baseline and split builds.
- The existing 2SM target fails the GQA reference check in both baseline and split builds.

## Reproduction

```bash
cmake -S /home/ubuntu/workspace/oyhj/temp/test/test_merge/Benchmark/FA \
      -B /home/ubuntu/workspace/oyhj/temp/test/test_merge/Benchmark/FA/build \
      -DCMAKE_BUILD_TYPE=Release -DCUTLASS_NVCC_ARCHS=100a
cmake --build /home/ubuntu/workspace/oyhj/temp/test/test_merge/Benchmark/FA/build \
      --target op6static2sm_fa4mufu_b200_blackwell_fmha_mxfp8 -j 16

/home/ubuntu/workspace/oyhj/temp/test/test_merge/Benchmark/FA/build/b200_blackwell_fmha_mxfp8/\
op6static2sm_fa4mufu_b200_blackwell_fmha_mxfp8 \
  --b=1 --h=40 --h_k=40 --q=170100 --k=170100 --d=128 --mask=no \
  --warmup_iterations=5 --iterations=20
```
