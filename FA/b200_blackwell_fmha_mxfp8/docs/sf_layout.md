# CUTLASS SF (Scaling Factor) Layout for Blackwell FMHA

## Overview

In MXFP8 Flash Attention, each 32 consecutive E4M3 values share one FP32 scaling factor (SF).
The SF tensors (SFQ, SFK, SFP, SFV) are stored on GPU using a CUTLASS-specific **bit-swizzled layout**
(`Sm1xxBlockScaledConfig::SfAtom`), NOT simple row-major. This document explains the mapping between
logical coordinates `(row, group, hb)` and the linear GPU memory index.

## Shared Atom Layout

All four SF buffers use the same `SfAtom` as their atomic building block:

```
SfKMajorAtom = Layout<
    Shape< Shape<_32, _4>, Shape<SFVecSize, _4> >,
    Stride<Stride<_16, _4>, Stride<       _0, _1> >
>
```

Where:
- **Mode 0 (row/seqlen dimension)**: `Shape<_32, _4>` = 128 rows per atom tile.
  The stride `(_16, _4)` means rows are **bit-swizzled** — not stored contiguously.
- **Mode 1 (SF group dimension)**: `Shape<SFVecSize, _4>` where `SFVecSize = 32`.
  The stride `(_0, _1)` means the first 32 consecutive groups **all map to the same offset**
  (broadcast/replicated), and only the outer `_4 = 4` blocks have distinct offsets.
  → **Each atom tile provides only 4 distinct SF group positions.**

### Visual: Row layout within one atom tile (128 rows × 4 groups)

For row `r` (0..127) and group `g` (0..3) within one tile:

```
Row offset: mode0(r) = (r % 32) * 16 + (r / 32) * 4
```

| r | mode0(r) | r | mode0(r) | r | mode0(r) | r | mode0(r) |
|---|----------|---|----------|---|----------|---|----------|
| 0 | 0        | 32 | 4        | 64 | 8        | 96 | 12       |
| 1 | 16       | 33 | 20       | 65 | 24       | 97 | 28       |
| 2 | 32       | 34 | 36       | 66 | 40       | 98 | 44       |
| ...| ...     | ...| ...      | ...| ...      | ...| ...      |
| 31| 496      | 63 | 500      | 95 | 504      | 127| 508      |

Group offset within tile = `g` (0, 1, 2, 3).

**Linear index within one atom tile** (128 rows × 4 groups = 512 elements):
```
tile_idx(r, g) = mode0(r) + g
```

## SFQ — Scale Factor for Q

**Phase**: QK MMA (A = Q matrix)

**Purpose**: Dequantizes Q: `Q_fp32[q][d] = Q_e4m3[q][d] * SFQ[q][d/32]`

**Layout computation**:
```cpp
layout_SFQ = Sm1xxBlkScaledConfigQK::tile_atom_to_shape_SFA(problem_size);
// SFA uses (M, K) = (Q, D) → logical shape (Q rows, D/32 groups, H*B hb)
```

**Parameters**:
| Parameter  | Value           |
|------------|-----------------|
| row        | Q seqlen (0..SQ-1) |
| group      | D/32 = 4 (for D=128) |
| hb         | H × B           |
| num_tiles  | 1 (4 groups fit in one atom tile) |
| per_hb     | SQ × 4          |

**Formula** (matches existing `fill_ref`):
```cpp
int mode0 = (r % 32) * 16 + ((r / 32) % 4) * 4 + (r / 128) * 512;
int cutlass_idx = mode0 + g + h * SQ * 4;
int ref_idx     = r * 4 + g + h * SQ * 4;
// Both refer to the same logical (r, g, h) position
// ref_idx == cutlass_idx only if mode0(r) == r*4 (which is generally NOT true)
```

## SFK — Scale Factor for K

**Phase**: QK MMA (B = K matrix)

**Purpose**: Dequantizes K: `K_fp32[k][d] = K_e4m3[k][d] * SFK[k][d/32]`

**Layout computation**:
```cpp
layout_SFK = Sm1xxBlkScaledConfigQK::tile_atom_to_shape_SFB(problem_size);
// SFB uses (N, K) = (K, D) → logical shape (K rows, D/32 groups, H_K*B hb)
```

**Parameters**:
| Parameter  | Value             |
|------------|-------------------|
| row        | K seqlen (0..SK-1) |
| group      | D/32 = 4 (for D=128) |
| hb         | H_K × B           |
| num_tiles  | 1                 |
| per_hb     | SK × 4            |

**Formula** (identical pattern to SFQ):
```cpp
int mode0 = (r % 32) * 16 + ((r / 32) % 4) * 4 + (r / 128) * 512;
int cutlass_idx = mode0 + g + h * SK * 4;
int ref_idx     = r * 4 + g + h * SK * 4;
```

## SFP — Scale Factor for P (Softmax Output)

**Phase**: PV MMA (A = P matrix)

**Purpose**: Dequantizes P. **Always set to 1.0** — P stays in FP32 (from TMEM).

**Layout computation**:
```cpp
auto problem_size_pv = select<0,2,1,3>(problem_size); // (Q, D, K, HB)
layout_SFP = Sm1xxBlkScaledConfigPV::tile_atom_to_shape_SFA(problem_size_pv);
// SFA uses (M, K) = (Q, K) → logical shape (Q rows, K/32 groups, H*B hb)
```

**Parameters**:
| Parameter  | Value              |
|------------|--------------------|
| row        | Q seqlen (0..SQ-1) |
| group      | **K/32** (not D/32!) |
| hb         | H × B              |
| num_tiles  | ceil(K/32 / 4)     |
| per_hb     | 512 × num_tiles    |

**Formula** (same structure but with K-based groups):
```cpp
int mode0 = (r % 32) * 16 + ((r / 32) % 4) * 4 + (r / 128) * 512;
int group_offset = (g % 4) + (g / 4) * 512;  // multi-tile when K/32 > 4
int cutlass_idx = mode0 + group_offset + h * per_hb;
int ref_idx     = r * (K/32) + g + h * SQ * (K/32);
```

> **Note**: SFP is always 1.0, so the layout mismatch with the reference convention
> `(Q, D/32, HB)` is masked. If SFP were ever made non-1.0, the same fill_ref fix
> applied to SFV would be needed.

## SFV — Scale Factor for V

**Phase**: PV MMA (B = V matrix)

**Purpose**: Dequantizes V: `V_fp32[k][d] = V_e4m3[k][d] * SFV[d][k/32]`

**Key difference**: SFV groups along **K-seqlen** (not D like SFQ/SFK), because the PV MMA's
contracting K-dimension is K_seqlen, matching the MMA hardware behavior.

**Layout computation**:
```cpp
auto problem_size_pv = select<0,2,1,3>(problem_size); // (Q, D, K, HB)
layout_SFV = Sm1xxBlkScaledConfigPV::tile_atom_to_shape_SFB(problem_size_pv);
// SFB uses (N, K) = (D, K) → logical shape (D=128 rows, K/32 groups, H_K*B hb)
```

**Parameters**:
| Parameter  | Value              |
|------------|--------------------|
| row        | D = head dim (0..127) |
| group (kg) | K/32 = groups along K-seqlen |
| hb         | H_K × B            |
| num_tiles  | ceil(K/32 / 4)     |
| per_hb     | 512 × num_tiles    |

**Formula** (multi-tile when K/32 > 4):
```cpp
int mode0 = (d % 32) * 16 + (d / 32) * 4;         // D=128, 1 tile, so no (d/128)*512
int group_offset = (kg % 4) + (kg / 4) * 512;      // each tile = 4 groups × 128 rows
int cutlass_idx = mode0 + group_offset + h * per_hb;
int ref_idx     = d * (K/32) + kg + h * D * (K/32);
```

### Why SFV differs from SFQ/SFK

| | MMA Phase | K-dim | Logical shape | Reference convention | Match? |
|---|---|---|---|---|---|
| SFQ | QK: A=Q, B=K | D=128 | (SQ, D/32, H×B) | (SQ, D/32, H×B) | ✅ Same |
| SFK | QK: A=Q, B=K | D=128 | (SK, D/32, H_K×B) | (SK, D/32, H_K×B) | ✅ Same |
| SFP | PV: A=P, B=V | **K_seqlen** | (SQ, **K/32**, H×B) | (SQ, D/32, H×B) | ❌ (SFP=1 masked) |
| SFV | PV: A=P, B=V | **K_seqlen** | (D=128, **K/32**, H_K×B) | (SK, D/32, H_K×B) | ❌ **Must fix** |

**The reference kernel must group SFV along K-seqlen** (not D), matching the MMA:
```cpp
// Reference Phase 3: P * V
for (int d = 0; d < head_v; d++) {
    for (int k = 0; k < K; k++) {
        int gk = k / 32;  // group along K-seqlen
        v_fp32 = V[k][d] * SFV[d][gk];  // NOT V[k][d] * SFV[k][d/32]
    }
}
```

## Summary Table

| Tensor | row dim | groups | hb dim | num_tiles | per_hb stride | fill_ref formula |
|--------|---------|--------|--------|-----------|---------------|------------------|
| SFQ | SQ | D/32=4 | H×B | 1 | SQ×4 | `mode0(r) + g + h*SQ*4` |
| SFK | SK | D/32=4 | H_K×B | 1 | SK×4 | `mode0(r) + g + h*SK*4` |
| SFP | SQ | K/32 | H×B | ceil(K/32/4) | 512×num_tiles | `mode0(r) + (g%4)+(g/4)*512 + h*per_hb` |
| SFV | D=128 | K/32 | H_K×B | ceil(K/32/4) | 512×num_tiles | `mode0(d) + (kg%4)+(kg/4)*512 + h*per_hb` |

## Reference Row-Major Layout

The reference buffers use a simple row-major layout for each SF tensor:

```cpp
auto make_mxfp8_sf_tensor(T* ptr, int rows, int sf_groups, int hb) {
    return make_tensor(make_gmem_ptr(ptr),
        make_layout(make_shape(rows, sf_groups, hb),
                    make_stride(sf_groups, _1{}, sf_groups * rows)));
}
```

| Tensor | make_mxfp8_sf_tensor call | Shape | Stride |
|--------|---------------------------|-------|--------|
| SFQ | `(ptr, SQ, D/32, H×B)` | (SQ, 4, H×B) | (4, 1, SQ×4) |
| SFK | `(ptr, SK, D/32, H_K×B)` | (SK, 4, H_K×B) | (4, 1, SK×4) |
| SFP | `(ptr, SQ, K/32, H×B)` | (SQ, K/32, H×B) | (K/32, 1, SQ×K/32) |
| SFV | `(ptr, D, K/32, H_K×B)` | (128, K/32, H_K×B) | (K/32, 1, 128×K/32) |

## Verification Threshold

For MXFP8 (E4M3 data), the numerical precision is limited to ~0.9 decimal digits
(3 mantissa bits). The O output comparison uses:

```cpp
// In verify():
const double kMaxDiffThresh = sizeof(Element) == 1 ? 3.5e-1 : 1e-2;
const double kMeanDiffThresh = sizeof(Element) == 1 ? 1e-1 : 1e-3;
```

The threshold of 0.35 accounts for precision differences between the kernel's
block-scaled MMA accumulation and the reference's FP32 loop, which are inherent
to the MXFP8 computation path (E4M3 → FP32 accumulate → FP16 output).
