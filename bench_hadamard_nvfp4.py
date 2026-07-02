"""Benchmark col-only Hadamard + NVFP4 quantization operator (single-tensor path).

Configuration: NVFP4Quantizer(rowwise=False, columnwise=True, with_rht=True,
                              with_post_rht_amax=True, with_random_sign_mask=True,
                              stochastic_rounding=True)
This triggers the col-only RHT+NVFP4 path with 3 CUDA kernels:
  1. ZeroAmaxKernel             (hadamard_transform.cu:596)
  2. HadamardAmaxTmaKernel      (hadamard_transform.cu:662)
  3. row_col_rht_gemm_device    (row_cast_col_hadamard_transform_cast_fusion.cu:1207)

Total operator time is measured with cuda Events.
Per-kernel breakdown is obtained via a separate nsys profile run (see run_nsys.sh).
"""
import argparse
import torch
import transformer_engine.pytorch as te  # must be first
import transformer_engine_torch as tex
from transformer_engine.pytorch import NVFP4Quantizer


def bench_one_round(q, x, iters=100, warmup=50):
    """One round: median total operator latency (us) over `iters` calls."""
    for _ in range(warmup):
        q(x)
    torch.cuda.synchronize()
    s = [torch.cuda.Event(enable_timing=True) for _ in range(iters)]
    e = [torch.cuda.Event(enable_timing=True) for _ in range(iters)]
    for i in range(iters):
        s[i].record()
        q(x)
        e[i].record()
    torch.cuda.synchronize()
    times = sorted(se.elapsed_time(ee) for se, ee in zip(s, e))
    return times[len(times) // 2] * 1e3  # us (median of this round)


def bench(q, x, iters=100, warmup=50, repeats=5):
    """Run multiple rounds, return (mean_us, std_us) of per-round medians."""
    vals = [bench_one_round(q, x, iters, warmup) for _ in range(repeats)]
    mean = sum(vals) / len(vals)
    var = sum((v - mean) ** 2 for v in vals) / len(vals)
    return mean, var ** 0.5


def make_quantizer():
    """col-only RHT+NVFP4 quantizer."""
    q = NVFP4Quantizer(
        fp4_dtype=tex.DType.kFloat4E2M1,
        rowwise=False,            # col-only path
        columnwise=True,
        with_amax_reduction=False,
        with_rht=True,
        with_post_rht_amax=True,
        with_random_sign_mask=True,
        stochastic_rounding=True,
    )
    # Enable gemm-swizzled scale output (fusion kernel path)
    q.optimize_for_gemm = True
    return q


def align(x, m=64):
    """Round up to multiple of m."""
    return ((x + m - 1) // m) * m


# Alignment constraint: fusion kernel requires M%64==0 and N%64==0
# (NVTE_CHECK in row_cast_col_hadamard_transform_cast_fusion.cu:1173-1174).
# This also satisfies the NVFP4 block-scaling (16) and Hadamard (16) constraints.
ALIGN_TO = 64


# (label, M, K). Use the REAL (possibly unaligned) shapes.
SHAPES = [
    ("shape1", 170100, 5120),
    ("shape2", 510300, 5120),
    ("shape3", 170100, 5120),
    ("shape4", 510300, 5120),
    ("shape5", 8192, 4096),
    ("shape6", 16384, 4096),
    ("shape7", 32768, 4096),
    ("shape8", 8192, 2048),
    ("shape9", 16384, 2048),
]


def run_one(label, M, K, iters, profile, repeats):
    # Real shape; padded shape computed for display only
    Ma, Ka = align(M, ALIGN_TO), align(K, ALIGN_TO)
    needs_pad = not (Ma == M and Ka == K)
    torch.manual_seed(0)
    torch.cuda.manual_seed(0)
    # Input is the REAL (unaligned) shape; pad ONCE before timing (pad cost excluded)
    x_real = torch.randn((M, K), dtype=torch.bfloat16, device="cuda")
    if needs_pad:
        x = torch.nn.functional.pad(x_real, (0, Ka - K, 0, Ma - M), value=0)
    else:
        x = x_real
    q = make_quantizer()
    # Only the quantize operator is timed (padding done once, outside timing)
    if profile:
        # Sufficient warmup before profiling so first-call/clock-boost overhead is excluded
        for _ in range(50):
            q(x)
        torch.cuda.synchronize()
        # Multiple instrumented calls wrapped in an NVTX range for nsys per-shape segmentation
        torch.cuda.profiler.start()
        torch.cuda.nvtx.range_push(f"{label}_{M}x{K}")
        for _ in range(profile):
            q(x)
        torch.cuda.nvtx.range_pop()
        torch.cuda.profiler.stop()
        torch.cuda.synchronize()
        return label, M, K, Ma, Ka, needs_pad, None, None
    mean_us, std_us = bench(q, x, iters=iters, repeats=repeats)
    return label, M, K, Ma, Ka, needs_pad, mean_us, std_us


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--iters", type=int, default=100)
    ap.add_argument("--repeats", type=int, default=5,
                    help="number of timing rounds (mean+std over rounds)")
    ap.add_argument("--profile", type=int, default=0,
                    help="if >0, run this many instrumented calls under nvtx for nsys")
    ap.add_argument("--only", default=None, help="comma-separated shape labels to run")
    args = ap.parse_args()

    shapes = SHAPES
    if args.only:
        wanted = set(args.only.split(","))
        shapes = [s for s in SHAPES if s[0] in wanted]

    if args.profile:
        # warmup GPU
        _ = torch.empty(1, device="cuda")

    if args.profile:
        print(f"{'label':<10}{'shape(MxK)':<20}{'padded':<20}{'pad':>6}{'calls':>8}")
        print("-" * 66)
        for label, M, K in shapes:
            lab, rM, rK, Ma, Ka, needs_pad, _, _ = run_one(
                label, M, K, args.iters, args.profile, args.repeats)
            pad_str = f"{Ma-rM}x{Ka-rK}" if needs_pad else "no"
            print(f"{lab:<10}{f'{rM}x{rK}':<20}{f'{Ma}x{Ka}':<20}{pad_str:>6}{args.profile:>8}")
    else:
        print(f"{'label':<10}{'shape(MxK)':<20}{'padded':<20}{'pad':>6}"
              f"{'mean_us':>12}{'std_us':>10}")
        print("-" * 80)
        for label, M, K in shapes:
            lab, rM, rK, Ma, Ka, needs_pad, mean_us, std_us = run_one(
                label, M, K, args.iters, args.profile, args.repeats)
            pad_str = f"{Ma-rM}x{Ka-rK}" if needs_pad else "no"
            print(f"{lab:<10}{f'{rM}x{rK}':<20}{f'{Ma}x{Ka}':<20}{pad_str:>6}"
                  f"{mean_us:>12.2f}{std_us:>10.2f}")


if __name__ == "__main__":
    main()
