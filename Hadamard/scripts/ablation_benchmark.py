#!/usr/bin/env python3
"""Kernel-level Hadamard/NVFP4 ablation benchmark.

The loaded TransformerEngine .so determines the compile-time ablation variant.
This script records every round so masking deltas can be recomputed without
rounding loss.
"""
import argparse
import json
import os
import statistics
from pathlib import Path

import torch
import transformer_engine.pytorch as te  # noqa: F401
import transformer_engine_torch as tex
from transformer_engine.pytorch import NVFP4Quantizer


SHAPES = [
    ("shape8", 8192, 2048),
    ("shape5", 8192, 4096),
    ("shape9", 16384, 2048),
    ("shape6", 16384, 4096),
    ("shape7", 32768, 4096),
    ("shape1", 170112, 5120),
    ("shape2", 510336, 5120),
]


def make_quantizer():
    q = NVFP4Quantizer(
        fp4_dtype=tex.DType.kFloat4E2M1,
        rowwise=False,
        columnwise=True,
        with_amax_reduction=False,
        amax_reduction_group=None,
        with_rht=True,
        with_post_rht_amax=True,
        with_random_sign_mask=True,
        stochastic_rounding=False,
    )
    q.optimize_for_gemm = False
    return q


def classify(name):
    if "row_cast_col_hadamard_transform_cast_fusion" in name:
        return "fusion"
    if "HadamardAmaxTmaKernel" in name:
        return "per_tensor_amax"
    if "ZeroAmaxKernel" in name:
        return "zero_amax"
    return "other"


def stats(values):
    ordered = sorted(values)
    n = len(ordered)
    return {
        "median_us": statistics.median(ordered),
        "mean_us": statistics.mean(ordered),
        "min_us": ordered[0],
        "max_us": ordered[-1],
        "q1_us": ordered[n // 4],
        "q3_us": ordered[(3 * n) // 4],
        "rounds_us": values,
    }


def bench_shape(name, m, k, rounds, iters, warmup):
    x = torch.randn((m, k), dtype=torch.bfloat16, device="cuda")
    q = make_quantizer()
    for _ in range(warmup):
        q(x)
    torch.cuda.synchronize()

    samples = {key: [] for key in ("fusion", "per_tensor_amax", "zero_amax", "other")}
    from torch.profiler import ProfilerActivity, profile

    for _ in range(rounds):
        with profile(activities=[ProfilerActivity.CUDA]) as prof:
            for _ in range(iters):
                q(x)
            torch.cuda.synchronize()
        totals = {key: 0.0 for key in samples}
        for event in prof.events():
            if event.device_type == torch.autograd.DeviceType.CUDA and event.self_device_time_total > 0:
                totals[classify(event.name)] += event.self_device_time_total
        for key in samples:
            samples[key].append(totals[key] / iters)

    result = {key: stats(values) for key, values in samples.items()}
    result["combined_median_us"] = sum(result[key]["median_us"] for key in samples)
    return result


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--label", required=True)
    parser.add_argument("--output", required=True)
    parser.add_argument("--shape-indices", default="0,1,2,3,4,5,6")
    parser.add_argument("--rounds", type=int, default=11)
    parser.add_argument("--iters", type=int, default=60)
    parser.add_argument("--warmup", type=int, default=60)
    args = parser.parse_args()

    os.environ["NVTE_USE_FAST_MATH"] = "1"
    torch.cuda.set_device(0)
    torch.manual_seed(20260829)
    indices = [int(item) for item in args.shape_indices.split(",") if item]
    payload = {
        "label": args.label,
        "fast_math": 1,
        "rounds": args.rounds,
        "iters": args.iters,
        "warmup": args.warmup,
        "shapes": {},
    }
    for index in indices:
        name, m, k = SHAPES[index]
        print(f"[{args.label}] {name} M={m} K={k}", flush=True)
        payload["shapes"][name] = {
            "M": m,
            "K": k,
            **bench_shape(name, m, k, args.rounds, args.iters, args.warmup),
        }

    output = Path(args.output)
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps(payload, indent=2), encoding="utf-8")
    print(json.dumps({
        name: {
            "fusion_us": data["fusion"]["median_us"],
            "per_tensor_amax_us": data["per_tensor_amax"]["median_us"],
        }
        for name, data in payload["shapes"].items()
    }, indent=2), flush=True)


if __name__ == "__main__":
    main()
