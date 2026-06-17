# MXFP8 Static-P 2SM Optimization Path to 1350 TFLOPS

## Goal and Stop Rule

Target shape:

```bash
--b=1 --h=40 --q=170100 --k=170100 --d=128
```

Goal: optimize the static-quant MXFP8 FlashAttention 2SM path to at least
`1350 TFLOPS` while preserving correctness. Per request, optimization stops as
soon as the target is reached.

Final measured result:

```bash
./build2cta/b200_blackwell_fmha_mxfp8/op6static2sm_b200_blackwell_fmha_mxfp8 \
  --b=1 --h=40 --q=170100 --k=170100 --d=128 --mask=no \
  --warmup_iterations=2 --iterations=5
```

Output:

```text
[--] tma ws n128 acc fp32 individual  : 1462.98 TFLOPS/s
```

This exceeds the `1350 TFLOPS` target, so no further optimization was pursued.

## Baseline and Evidence

| Item | Source | Result |
| --- | --- | --- |
| User baseline | User-reported current implementation, 1CTA static quant with P in TMEM | `1287 TFLOPS` |
| op6 reference | `README.md` and `build/op6/op6170100report.ncu-rep` | about `1420-1424 TFLOPS`, XU about `65%` |
| Final implementation | This build, target shape command above | `1462.98 TFLOPS/s` |

Net improvement against the user-reported baseline:

```text
1287 -> 1462.98 TFLOPS/s  (+175.98 TFLOPS/s, +13.7%)
```

XU utilization was treated as a diagnostic, not the stop condition. The op6
report showed high XU utilization because the implementation removes and hides
softmax/P-quant work around the exp2-heavy path. Since the final implementation
already exceeded `1350 TFLOPS`, no additional NCU ablation was run after the
final measurement.

## Implemented Changes

1. Added an op6-style 2SM static-P build target:
   `op6static2sm_b200_blackwell_fmha_mxfp8`.
   The target enables the op6 KING-style flags, including `FMHA_2CTA`,
   `MXFP8_PSTATIC`, `MXFP8_E2RSF`, `MXFP8_2SM_CREL`,
   `MXFP8_2SM_N128SINGLE`, `MXFP8_2SM_VPREFETCH`, `MXFP8_R15_ORVLOG`,
   `MXFP8_M2_COMBO`, `MXFP8_G_COMBO`, and `MXFP8_REGSM_SOFTMAX=184`.

2. Ported the op6 N128 2SM fast-path headers into this benchmark:
   mainloop, TMA load, kernel, schedule, and epilogue.
   This brings in static P-SFP fill, fp32 softmax, E2RSF fused row-sum,
   2SM early release, N128 single pipeline, V prefetch, and O-rescale vote/combo
   logic.

3. Changed static P quantization semantics to match op6:
   P-SFP is fixed to e8m0 byte `127`, i.e. scale `1.0`.
   The old `2^-9` P scale path, `+9` exp2 bias, and row_sum scale-back are not
   used by the new target.

4. Fixed driver wiring for the op6 2SM path:
   host TileShape for `MXFP8_N128` is now per-CTA `M=128`, while cooperative
   `M=256` is handled inside the mainloop via `ClusterShape<2,1,1>`.
   The epilogue uses `Mainloop::TileShapePV`, matching op6.
   Mainloop arguments now explicitly pass `{load, scale_softmax, scale_q,
   scale_k, scale_v, inv_scale_o}`.
   The new target uses op6-style BHSD-contiguous strides.

5. Replaced the local 2CTA-modified `IndividualTileScheduler` with the op6
   scheduler behavior.
   This was the launch-critical fix: the old scheduler used
   `grid.x = num_m * cluster_m` and `block_coord = blockIdx.x / cluster_m`,
   which conflicts with op6's per-CTA `M=128` 2SM design. The op6 scheduler uses
   `grid.x = round_up(num_m, cluster_m)` and `block_coord = blockIdx.x`.

6. Updated the MXFP8 SFP reference path:
   reference P-SFP now uses scale `1.0`, matching `MXFP8_PSTATIC_EXP=0`.
   `mSFP` remains only as an ABI placeholder for this static target.

## Validation Data

Confirmed root-cause A/B for the launch failure:

The launch failure was not guessed. It was reproduced by changing only
`kernel/fmha_tile_scheduler.hpp` back to the old local 2CTA scheduler behavior:

```text
old: grid.x = num_m * cluster_m
old: block_coord = blockIdx.x / cluster_m
```

With the old scheduler, the exact same optimized target and command failed:

```bash
./build2cta/b200_blackwell_fmha_mxfp8/op6static2sm_b200_blackwell_fmha_mxfp8 \
  --b=1 --h=4 --q=512 --k=512 --d=128 --verify --mask=no \
  --warmup_iterations=1 --iterations=1
```

```text
Error running the CUTLASS kernel. Last CUDA error is: unspecified launch failure
[FAIL] tma ws n128 acc fp32 individual  : 0 TFLOPS/s
```

After restoring the op6 scheduler:

```text
new: grid.x = round_up(num_m, cluster_m)
new: block_coord = blockIdx.x
```

The same command passed:

```text
O max_diff:0.008728, mean_diff:0.000954
passed_O: 1
lse max_diff:0.000001, mean_diff:0.000000
passed_LSE: 1
```

Therefore the confirmed launch-failure bug was the old scheduler's 2CTA tile
coordinate model. It grouped both CTAs in a cluster onto the same cooperative
M-tile coordinate (`blockIdx.x / cluster_m`), while the op6 2SM N128 mainloop
expects per-CTA `M=128` tile coordinates and expresses the cooperative `M=256`
work sharing internally through `ClusterShape<2,1,1>`.

Correctness, aligned case:

```bash
./build2cta/b200_blackwell_fmha_mxfp8/op6static2sm_b200_blackwell_fmha_mxfp8 \
  --b=1 --h=4 --q=512 --k=512 --d=128 --verify --mask=no \
  --warmup_iterations=1 --iterations=1
```

```text
O max_diff:0.008728, mean_diff:0.000954
passed_O: 1
lse max_diff:0.000001, mean_diff:0.000000
passed_LSE: 1
```

Correctness, non-128-aligned K tail:

```bash
./build2cta/b200_blackwell_fmha_mxfp8/op6static2sm_b200_blackwell_fmha_mxfp8 \
  --b=1 --h=2 --q=384 --k=300 --d=128 --verify --mask=no \
  --warmup_iterations=1 --iterations=1
```

```text
O max_diff:0.009819, mean_diff:0.001128
passed_O: 1
lse max_diff:0.000001, mean_diff:0.000000
passed_LSE: 1
```

Performance, target shape:

```text
1462.98 TFLOPS/s
```

## Step Log

| Step | Change | Why | Result |
| --- | --- | --- | --- |
| 0 | Baseline | User current static quant implementation | `1287 TFLOPS` reported |
| 1 | Added op6 static 2SM target and ported op6 fast-path headers | Bring in the high-performance static-P softmax/PV pipeline | Compiled, but initially hit launch failure |
| 2 | Aligned TileShape, epilogue tile, mainloop args, and BHSD strides | Match op6 driver/kernel ABI | Still launch failed until scheduler was fixed |
| 3 | Replaced local 2CTA scheduler with op6 scheduler | Old scheduler's tile coordinate model conflicted with op6 per-CTA M=128 clusters | Correctness passed |
| 4 | Measured target shape | Stop when `>=1350 TFLOPS` | `1462.98 TFLOPS/s`, stop |

## Notes

- The new optimized path is isolated in the `op6static2sm_...` target.
- The old baseline target is kept for comparison.
- No polynomial exp2, SM12, OSPLIT, WHYST, or other non-KING experiments were
  enabled.
- The final speed is above the requested threshold, so additional NCU tuning and
  ablation were intentionally skipped.
