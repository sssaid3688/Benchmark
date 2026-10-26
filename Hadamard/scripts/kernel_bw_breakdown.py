#!/usr/bin/env python3
"""Per-kernel bandwidth breakdown: fusion / global-amax / combined.

Kernel classification (per call of NVFP4Quantizer with with_rht+post_rht_amax):
  fusion : row_cast_col_hadamard_transform_cast_fusion TU (row_col_rht_gemm_device)
           traffic = M*K*(2 [bf16 in] + 0.5 [fp4 out] + 0.0625 [fp8 SF out])
  amax   : HadamardAmaxTmaKernel — reads X again (M*K*2), writes negligible amax
  zero   : ZeroAmaxKernel — negligible traffic (few floats)

combined = (fusion bytes + amax bytes) / (fusion + amax + zero time)
"""
import statistics
import sys

import torch
import transformer_engine.pytorch as te  # noqa: F401
import transformer_engine_torch as tex
from transformer_engine.pytorch import NVFP4Quantizer

SHAPES = [
    ("shape8", 8192,   2048),
    ("shape5", 8192,   4096),
    ("shape9", 16384,  2048),
    ("shape6", 16384,  4096),
    ("shape7", 32768,  4096),
    ("shape1", 170112, 5120),
    ("shape2", 510336, 5120),
]
THEO = 8.0  # TB/s theoretical


def make_quantizer():
    q = NVFP4Quantizer(
        fp4_dtype=tex.DType.kFloat4E2M1,
        rowwise=False, columnwise=True,
        with_amax_reduction=False, amax_reduction_group=None,
        with_rht=True, with_post_rht_amax=True,
        with_random_sign_mask=True, stochastic_rounding=False,
    )
    q.optimize_for_gemm = False
    return q


def classify(name):
    if "row_cast_col_hadamard_transform_cast_fusion" in name:
        return "fusion"
    if "HadamardAmaxTmaKernel" in name:
        return "amax"
    if "ZeroAmaxKernel" in name:
        return "zero"
    return "other"


def bench(M, K, fm, rounds=8, iters=50):
    import os
    os.environ["NVTE_USE_FAST_MATH"] = str(fm)
    x = torch.randn((M, K), dtype=torch.bfloat16, device="cuda")
    q = make_quantizer()
    for _ in range(50):
        q(x)
    torch.cuda.synchronize()

    from torch.profiler import ProfilerActivity, profile
    acc = {"fusion": [], "amax": [], "zero": [], "other": []}
    for _ in range(rounds):
        with profile(activities=[ProfilerActivity.CUDA]) as prof:
            for _ in range(iters):
                q(x)
            torch.cuda.synchronize()
        t = {"fusion": 0.0, "amax": 0.0, "zero": 0.0, "other": 0.0}
        for e in prof.events():
            if e.device_type == torch.autograd.DeviceType.CUDA and e.self_device_time_total > 0:
                t[classify(e.name)] += e.self_device_time_total
        for k in acc:
            acc[k].append(t[k] / iters)

    med = lambda k: statistics.median(acc[k])
    b_fusion = M * K * 2.5625
    b_amax = M * K * 2.0
    t_fus, t_amx, t_zer = med("fusion"), med("amax"), med("zero")
    t_all = t_fus + t_amx + t_zer + med("other")
    bw_fus = b_fusion / t_fus / 1e6      # us -> TB/s: bytes/us/1e6
    bw_amx = b_amax / t_amx / 1e6
    bw_all = (b_fusion + b_amax) / t_all / 1e6
    return t_fus, t_amx, t_zer, med("other"), t_all, bw_fus, bw_amx, bw_all


def main():
    torch.cuda.set_device(0)
    fms = [int(a) for a in sys.argv[1:]] or [1]
    for fm in fms:
        print(f"\n===== NVTE_USE_FAST_MATH={fm} =====")
        hdr = (f"{'shape':<8}{'M':>7}{'K':>6} | {'fus_us':>8}{'amax_us':>8}{'zero_us':>8}"
               f" | {'fus_TB/s':>8}{'util%':>6} | {'amax_TB/s':>9}{'util%':>6} | {'comb_TB/s':>9}{'util%':>6}")
        print(hdr)
        print("-" * len(hdr))
        for name, M, K in SHAPES:
            t_f, t_a, t_z, t_o, t_all, bwf, bwa, bwc = bench(M, K, fm)
            print(f"{name:<8}{M:>7}{K:>6} | {t_f:8.2f}{t_a:8.2f}{t_z:8.2f}"
                  f" | {bwf:8.2f}{bwf/THEO*100:6.1f} | {bwa:9.2f}{bwa/THEO*100:6.1f}"
                  f" | {bwc:9.2f}{bwc/THEO*100:6.1f}")


if __name__ == "__main__":
    main()
