#!/usr/bin/env python3
"""
op6 full-MXFP8 FlashAttention — CPU Python golden reference + precision check.

Follows the op6_comparison.png RIGHT-figure flow EXACTLY (full MXFP8), and crucially
includes the online P->MXFP8 per-32 quantization step that the kernel's built-in
--verify (a plain fp32 SDPA) omits. That makes this a TIGHT check that can expose real
kernel bugs (e.g. per-row multiplicative errors), not just bound quant noise.

Inputs: a dump prefix written by `fmha_mxfp8_pvmx --dump=<prefix>`:
  <p>.q <p>.k <p>.v : fp32 [B,H,S,D] = the DEQUANTIZED Q/K/V the kernel actually consumed
                      (already mxfp8 e4m3+e8m0 round-tripped) -> we do NOT requant these.
  <p>.o            : fp32 [B,H,S,D] = kernel output O (half_t upcast to fp32).
  <p>.meta         : "B H S D".

Flow per (b,h)  [matches kernel mainloop sm100_fmha_fwd_mainloop_mxfp8_n128_pvmx.hpp]:
  scores = (Q @ K^T) * (1/sqrt(D))            # blockscaled QK already baked into .q/.k
  rowmax = max_kv scores
  P      = exp(scores - rowmax)               # standard softmax, no folding (fig: "无折叠")
  row_sum= sum_kv P                           # normalize uses UNQUANTIZED P (kernel L915)
  P_q    = mxfp8_quant_per32_along_kv(P)      # online: per-32 amax -> e8m0 SF, e4m3 data
  O      = (P_q @ V_q) / row_sum              # V_q = .v (already dequantized), fp32 accum
  O      = O.astype(fp16)                     # kernel output is half_t (NOT bf16)
"""
import sys, struct, numpy as np

try:
    import torch
    def to_e4m3_rt(x):  # RNE e4m3fn, saturating to +-448 (== __nv_cvt SATFINITE / cutlass converter)
        return torch.from_numpy(np.ascontiguousarray(x)).to(torch.float8_e4m3fn).to(torch.float32).numpy()
    def to_fp16(x):
        return torch.from_numpy(np.ascontiguousarray(x.astype(np.float32))).to(torch.float16).to(torch.float32).numpy()
except Exception:
    import ml_dtypes
    def to_e4m3_rt(x):
        return x.astype(ml_dtypes.float8_e4m3fn).astype(np.float32)
    def to_fp16(x):
        return x.astype(np.float16).astype(np.float32)

E4M3_MAX = np.float32(448.0)

def read_meta(p):
    with open(p + ".meta") as f:
        B, H, S, D = (int(x) for x in f.read().split())
    return B, H, S, D

def read_f32(path, shape):
    a = np.fromfile(path, dtype=np.float32)
    assert a.size == int(np.prod(shape)), f"{path}: {a.size} != {np.prod(shape)}"
    return a.reshape(shape)

def e8m0_exp_bitexact(amax):
    """e = ceil(log2(amax/448)) via float-exponent bits — EXACTLY matches the kernel
    P-SF bit-trick (sm100_..._pvmx.hpp L906-912): exp(bits)-127 + (mantissa!=0)."""
    x = (amax.astype(np.float32) * np.float32(1.0 / 448.0)).astype(np.float32)
    bits = x.view(np.uint32)
    exp = ((bits >> 23) & np.uint32(0xFF)).astype(np.int32) - 127
    frac_nz = (bits & np.uint32(0x7FFFFF)) != 0
    e = exp + frac_nz.astype(np.int32)
    e = np.where(amax > 0, e, -127)          # amax<=0 -> e=-127 (block is all-zero -> P_q=0)
    return np.clip(e, -127, 127).astype(np.int32)

def mxfp8_quant_p(P, block=32):
    """Quantize P (Sq,Skv) per row, per-32-block along kv: e8m0 pow2 SF + e4m3 data, dequantized.
    Handles Skv % 32 != 0 (final partial block uses amax over its valid entries only)."""
    Sq, Skv = P.shape
    Pq = np.empty_like(P, dtype=np.float32)
    for s in range(0, Skv, block):
        e = min(s + block, Skv)
        blk = P[:, s:e]                                   # (Sq, b)
        amax = np.abs(blk).max(axis=1)                    # (Sq,)
        ex = e8m0_exp_bitexact(amax)                      # (Sq,)
        scale = np.exp2(ex.astype(np.float32))            # 2^e  (>= amax/448 -> blk/scale <= 448)
        scale_safe = np.where(amax > 0, scale, np.float32(1.0))[:, None]
        Pq[:, s:e] = to_e4m3_rt(blk / scale_safe) * scale_safe
    return Pq

def ref_one(Q, K, V, D):
    scale = np.float32(1.0 / np.sqrt(np.float32(D)))
    scores = (Q.astype(np.float32) @ K.astype(np.float32).T) * scale       # (S,S)
    rowmax = scores.max(axis=1, keepdims=True)
    P = np.exp(scores - rowmax).astype(np.float32)                         # (S,S)
    row_sum = P.sum(axis=1, keepdims=True).astype(np.float32)              # unquantized P sum
    Pq = mxfp8_quant_p(P)
    O = (Pq @ V.astype(np.float32)) / row_sum                             # (S,D), fp32 accum
    return to_fp16(O)

def compare(p, verbose=True):
    B, H, S, D = read_meta(p)
    Q = read_f32(p + ".q", (B, H, S, D))
    K = read_f32(p + ".k", (B, H, S, D))
    V = read_f32(p + ".v", (B, H, S, D))
    Ok = read_f32(p + ".o", (B, H, S, D))
    max_abs = 0.0; sse = 0.0; ssref = 0.0; nbad = 0; ntot = 0
    worst_row_rel = 0.0; row_ratios = []
    for b in range(B):
        for h in range(H):
            Oref = ref_one(Q[b, h], K[b, h], V[b, h], D)
            ok = Ok[b, h]
            d = np.abs(ok - Oref)
            max_abs = max(max_abs, float(d.max()))
            sse += float((d * d).sum()); ssref += float((Oref * Oref).sum())
            nbad += int((d > 0.1).sum()); ntot += d.size
            # per-row diagnostics (catch per-row multiplicative bugs, cf. op5)
            rn = np.linalg.norm(Oref, axis=1); kn = np.linalg.norm(ok, axis=1)
            rel = np.linalg.norm(ok - Oref, axis=1) / np.maximum(rn, 1e-12)
            worst_row_rel = max(worst_row_rel, float(rel.max()))
            nz = rn > 1e-6
            row_ratios.append((kn[nz] / rn[nz]))
    rel_rmse = float(np.sqrt(sse / max(ssref, 1e-30)))
    rr = np.concatenate(row_ratios) if row_ratios else np.array([1.0])
    stats = dict(B=B, H=H, S=S, D=D, max_abs=max_abs, rel_rmse=rel_rmse,
                 worst_row_rel=worst_row_rel, bad=nbad, tot=ntot,
                 row_ratio_mean=float(rr.mean()), row_ratio_std=float(rr.std()),
                 row_ratio_min=float(rr.min()), row_ratio_max=float(rr.max()))
    if verbose:
        print("[ref] B=%d H=%d S=%d D=%d | max_abs=%.5f rel_rmse=%.5f worst_row_rel=%.5f | "
              "row|O|ratio mean=%.4f std=%.4f [%.3f,%.3f] | bad(>0.1)=%d/%d"
              % (B, H, S, D, max_abs, rel_rmse, worst_row_rel, stats["row_ratio_mean"],
                 stats["row_ratio_std"], stats["row_ratio_min"], stats["row_ratio_max"], nbad, ntot))
    return stats

if __name__ == "__main__":
    if len(sys.argv) < 2:
        print("usage: op6_precision_ref.py <dump_prefix> [<dump_prefix> ...]"); sys.exit(1)
    for p in sys.argv[1:]:
        compare(p)
