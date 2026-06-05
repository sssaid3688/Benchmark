# Fix MXFP8 Large Shape Runtime Failure

## Background

`b200_blackwell_fmha_mxfp8` previously failed on large shapes such as:

```bash
./b200_blackwell_fmha_mxfp8/b200_blackwell_fmha_mxfp8 --b=1 --h=40 --d=128 --q=170100 --k=16384
```

The failure was:

```text
terminate called after throwing an instance of 'std::length_error'
  what():  cannot create std::vector larger than max_size()
Aborted (core dumped)
```

The direct cause was host-side allocation and layout sizing for large MXFP8 FA shapes. Some size and stride calculations still used 32-bit `int`, and the global `SFP` scale buffer was sized as if P scale were a full global tensor.

## Root Cause

There were two related problems:

1. Q/K/O/LSE strides and some host-side size calculations used 32-bit math.
   For `--q=510300 --h=40 --d=128`, `Q * H * D` is larger than `2^31`, so any 32-bit stride or element-count calculation can overflow.

2. `SFP` was allocated as a global scale tensor.
   P is an intermediate tile-local value, and the kernel already generates its scale in SMEM. A global `SFP` layout grows roughly with `Q * ceil(K / 32) * H`, which is far too large for the target shapes and can also produce invalid TMA descriptor dimensions.

## Code Changes

All changes are in:

```text
b200_blackwell_fmha.cu
```

### 1. Use 64-bit strides

Changed dynamic stride types from `int` to `int64_t`:

```cpp
StrideQ
StrideK
StrideV
StrideO
StrideLSE
```

Stride construction now uses explicit 64-bit intermediates:

```cpp
SQ64, SK64, D64, H64, H_K64, H_Q64
```

This prevents overflow in expressions such as:

```cpp
H * D * SQ
H_K * D * SK
SQ * H
```

### 2. Use explicit `size_t` allocation sizes

Q/K/V/O/LSE allocation sizes are now computed with `size_t`:

```cpp
q_elems
kv_elems
lse_elems
varlen_output_offset
```

This avoids relying on `cute::size(...)` for large host-side element counts when the shape is still represented with `int` modes.

### 3. Remove global SFP allocation path

`block_SFP` is now only a 1-element dummy allocation:

```cpp
buffer.block_SFP.reset(1);
```

The real P scale path remains tile-local in the kernel SMEM path. `block_ref_SFP` was removed, and host initialization no longer calls:

```cpp
initialize_block(buffer.block_SFP, ...)
fill_ref(buffer.block_SFP, ...)
```

### 4. Use a small dummy SFP layout

`layout_SFP` no longer uses the full `Q x K x H` PV problem shape. It is built from one PV tile-sized dummy problem so the unused SFP TMA descriptor can still be constructed safely:

```cpp
layout_SFP = Sm1xxBlkScaledConfigPV::tile_atom_to_shape_SFA(dummy_sfp_problem_size);
```

This removes the previous TMA descriptor error for the largest target shape.

### 5. Allocate reference buffers only when verifying

Reference buffers are now only allocated under:

```cpp
if (options.verify) { ... }
```

This avoids unnecessary extra memory use for large benchmark-only runs.

### 6. Promote reference scale index math

Reference scale conversion indices now use `size_t` for values such as:

```cpp
head_stride
cutlass_idx
rm_idx
sfv_per_hb
```

This avoids another class of overflow when running larger verified cases.

### 7. Current diff also tightens MXFP8 verify threshold

The current file diff changes:

```cpp
kMaxDiffThresh: 4.0e-1 -> 1e-1
```

The small-shape verification still passes with this stricter threshold.

## Verification

Build:

```bash
cmake --build /home/ubuntu/workspace/oyhj/FA/build --target b200_blackwell_fmha_mxfp8 -j
```

Small-shape accuracy:

```bash
/home/ubuntu/workspace/oyhj/FA/build/b200_blackwell_fmha_mxfp8/b200_blackwell_fmha_mxfp8 \
  --b=1 --h=40 --d=128 --q=512 --k=512 --verify --iterations=1
```

Result:

```text
passed_O: 1
passed_LSE: 1
```

Original failing shape:

```bash
/home/ubuntu/workspace/oyhj/FA/build/b200_blackwell_fmha_mxfp8/b200_blackwell_fmha_mxfp8 \
  --b=1 --h=40 --d=128 --q=170100 --k=16384 --iterations=1
```

Result:

```text
completed, no std::length_error
```

Target shape 1:

```bash
/home/ubuntu/workspace/oyhj/FA/build/b200_blackwell_fmha_mxfp8/b200_blackwell_fmha_mxfp8 \
  --b=1 --h=40 --d=128 --q=170100 --k=170100 --iterations=1
```

Result:

```text
completed, no std::length_error
```

Target shape 2:

```bash
/home/ubuntu/workspace/oyhj/FA/build/b200_blackwell_fmha_mxfp8/b200_blackwell_fmha_mxfp8 \
  --b=1 --h=40 --d=128 --q=510300 --k=510300 --iterations=1
```

Result:

```text
completed, no std::length_error, no TMA descriptor error
```

## Notes

- Large target shapes were intentionally tested without `--verify`.
- `--verify` on very large shapes is still expected to be expensive because reference O/LSE and row-major scale buffers require significant memory and time.
- This fix is scoped to the local example and does not modify CUTLASS public headers.
