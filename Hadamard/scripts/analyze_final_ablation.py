#!/usr/bin/env python3
"""Assemble the clean Hadamard ablation runs and derive masking effects."""

from __future__ import annotations

import argparse
import csv
import json
import random
import statistics
from dataclasses import dataclass
from pathlib import Path


SHAPES = ["shape8", "shape5", "shape9", "shape6", "shape7", "shape1", "shape2"]


@dataclass(frozen=True)
class Run:
    name: str
    path: Path
    payload: dict

    def samples(self, shape: str, kernel: str = "fusion") -> list[float]:
        return list(self.payload["shapes"][shape][kernel]["rounds_us"])

    def median(self, shape: str, kernel: str = "fusion") -> float:
        return float(self.payload["shapes"][shape][kernel]["median_us"])


@dataclass(frozen=True)
class Effect:
    estimate: float
    low: float
    high: float

    @property
    def resolved(self) -> bool:
        return self.low > 0.0 or self.high < 0.0


def load_run(path: Path) -> Run:
    payload = json.loads(path.read_text(encoding="utf-8"))
    return Run(payload["label"], path, payload)


def bootstrap_expression(
    terms: list[tuple[float, Run]], shape: str, *, seed: int, samples: int
) -> Effect:
    estimate = sum(sign * run.median(shape) for sign, run in terms)
    raw = [(sign, run.samples(shape)) for sign, run in terms]
    rng = random.Random(seed)
    values: list[float] = []
    for _ in range(samples):
        total = 0.0
        for sign, observations in raw:
            resampled = [observations[rng.randrange(len(observations))] for _ in observations]
            total += sign * statistics.median(resampled)
        values.append(total)
    values.sort()
    return Effect(
        estimate,
        values[int(0.025 * (samples - 1))],
        values[int(0.975 * (samples - 1))],
    )


def add_effect(row: dict[str, object], prefix: str, effect: Effect) -> None:
    row[f"{prefix}_us"] = effect.estimate
    row[f"{prefix}_ci95_low_us"] = effect.low
    row[f"{prefix}_ci95_high_us"] = effect.high
    row[f"{prefix}_resolved"] = effect.resolved


def add_removal_percentages(
    row: dict[str, object], prefix: str, before_us: float, after_us: float
) -> None:
    """Add latency reduction and equivalent throughput gain for one removal."""
    row[f"{prefix}_before_us"] = before_us
    row[f"{prefix}_after_us"] = after_us
    row[f"{prefix}_latency_reduction_percent"] = (before_us - after_us) / before_us * 100.0
    row[f"{prefix}_throughput_gain_percent"] = (
        before_us / after_us - 1.0 if after_us > 0.0 else float("nan")
    ) * 100.0


def write_csv(path: Path, rows: list[dict[str, object]]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    fields: list[str] = []
    for row in rows:
        for field in row:
            if field not in fields:
                fields.append(field)
    with path.open("w", newline="", encoding="utf-8-sig") as handle:
        writer = csv.DictWriter(handle, fieldnames=fields)
        writer.writeheader()
        writer.writerows(rows)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--r1", type=Path, required=True)
    parser.add_argument("--r2", type=Path, required=True)
    parser.add_argument("--dq-strict-anchor", type=Path, required=True)
    parser.add_argument("--final-pairs", type=Path, required=True)
    parser.add_argument("--fp4-anchor", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--bootstrap-samples", type=int, default=20000)
    args = parser.parse_args()

    runs = {
        "baseline_r1": load_run(args.r1 / "baseline.json"),
        "baseline": load_run(args.r2 / "baseline_r2.json"),
        "no_had": load_run(args.r2 / "no_had_r2.json"),
        "no_amax": load_run(args.r2 / "no_amax_reduction_r2.json"),
        "no_had_no_amax": load_run(args.r2 / "no_had_no_amax_reduction_r2.json"),
        "no_scale": load_run(args.r2 / "no_scale_chain_r2.json"),
        "no_had_no_scale": load_run(args.r2 / "no_had_no_scale_chain_r2.json"),
        "baseline_dq_strict_anchor": load_run(args.dq_strict_anchor / "baseline_dq_strict_anchor.json"),
        "no_had_dq_strict_anchor": load_run(args.dq_strict_anchor / "no_had_dq_strict_anchor.json"),
        "no_dq_strict_anchor": load_run(args.dq_strict_anchor / "no_dq_strict_anchor.json"),
        "no_had_no_dq_strict_anchor": load_run(args.dq_strict_anchor / "no_had_no_dq_strict_anchor.json"),
        "no_pts": load_run(args.final_pairs / "no_pts_r3.json"),
        "no_had_no_pts": load_run(args.final_pairs / "no_had_no_pts_r3.json"),
        "baseline_fp4_anchor": load_run(args.fp4_anchor / "baseline_fp4_anchor.json"),
        "no_had_fp4_anchor": load_run(args.fp4_anchor / "no_had_fp4_anchor.json"),
        "no_fp4_anchor": load_run(args.fp4_anchor / "no_fp4_convert_anchor.json"),
        "no_had_no_fp4_anchor": load_run(args.fp4_anchor / "no_had_no_fp4_convert_anchor.json"),
    }

    # (component A, baseline, A-off, Had-off, both-off, boundary note)
    pairs = [
        (
            "dynamic_quant_total_compute",
            runs["baseline_dq_strict_anchor"],
            runs["no_dq_strict_anchor"],
            runs["no_had_dq_strict_anchor"],
            runs["no_had_no_dq_strict_anchor"],
            "Matched TMEM and PTS-result anchors; LDTM/TMA/store/global-amax path preserved",
        ),
        (
            "per_tensor_scale_math",
            runs["baseline"],
            runs["no_pts"],
            runs["no_had"],
            runs["no_had_no_pts"],
            "Global-amax load plus encode/decode scalar math inside fused kernel",
        ),
        (
            "dq_local_amax_reduction",
            runs["baseline"],
            runs["no_amax"],
            runs["no_had"],
            runs["no_had_no_amax"],
            "16-value local absmax tree replaced by live one-lane surrogate",
        ),
        (
            "dq_scale_chain",
            runs["baseline"],
            runs["no_scale"],
            runs["no_had"],
            runs["no_had_no_scale"],
            "FP8 SF round-trip/global decode/reciprocal-clamp chain",
        ),
        (
            "dq_fp4_scale_convert",
            runs["baseline_fp4_anchor"],
            runs["no_fp4_anchor"],
            runs["no_had_fp4_anchor"],
            runs["no_had_no_fp4_anchor"],
            "Matched acc_scale anchor; only value FMUL plus FP32-to-E2M1 conversion removed",
        ),
    ]

    pair_rows: list[dict[str, object]] = []
    pair_effects: dict[tuple[str, str], dict[str, Effect]] = {}
    pair_points: dict[tuple[str, str], dict[str, float]] = {}
    for shape_i, shape in enumerate(SHAPES):
        dims = runs["baseline"].payload["shapes"][shape]
        for pair_i, (component, base, a_off, had_off, both_off, boundary) in enumerate(pairs):
            seed = 2026082900 + shape_i * 100 + pair_i
            a_visible = bootstrap_expression(
                [(1.0, base), (-1.0, a_off)], shape, seed=seed, samples=args.bootstrap_samples
            )
            a_exposed = bootstrap_expression(
                [(1.0, had_off), (-1.0, both_off)],
                shape,
                seed=seed + 10000,
                samples=args.bootstrap_samples,
            )
            had_visible = bootstrap_expression(
                [(1.0, base), (-1.0, had_off)],
                shape,
                seed=seed + 20000,
                samples=args.bootstrap_samples,
            )
            had_exposed = bootstrap_expression(
                [(1.0, a_off), (-1.0, both_off)],
                shape,
                seed=seed + 30000,
                samples=args.bootstrap_samples,
            )
            signed_interaction = bootstrap_expression(
                [(1.0, had_off), (-1.0, both_off), (-1.0, base), (1.0, a_off)],
                shape,
                seed=seed + 40000,
                samples=args.bootstrap_samples,
            )
            row: dict[str, object] = {
                "shape": shape,
                "M": dims["M"],
                "K": dims["K"],
                "component": component,
                "boundary": boundary,
                "baseline_run": base.name,
                "component_off_run": a_off.name,
                "had_off_run": had_off.name,
                "both_off_run": both_off.name,
                "baseline_us": base.median(shape),
                "component_off_us": a_off.median(shape),
                "had_off_us": had_off.median(shape),
                "both_off_us": both_off.median(shape),
            }
            add_effect(row, "component_visible", a_visible)
            add_effect(row, "component_exposed_after_had_removed", a_exposed)
            add_effect(row, "had_visible", had_visible)
            add_effect(row, "had_exposed_after_component_removed", had_exposed)
            add_effect(row, "signed_interaction", signed_interaction)
            add_removal_percentages(
                row, "component_visible", base.median(shape), a_off.median(shape)
            )
            add_removal_percentages(
                row,
                "component_exposed_after_had_removed",
                had_off.median(shape),
                both_off.median(shape),
            )
            add_removal_percentages(
                row, "had_visible", base.median(shape), had_off.median(shape)
            )
            add_removal_percentages(
                row,
                "had_exposed_after_component_removed",
                a_off.median(shape),
                both_off.median(shape),
            )
            pair_rows.append(row)
            pair_effects[(shape, component)] = {
                "visible": a_visible,
                "exposed": a_exposed,
                "interaction": signed_interaction,
            }
            pair_points[(shape, component)] = {
                "visible_before_us": base.median(shape),
                "visible_after_us": a_off.median(shape),
                "exposed_before_us": had_off.median(shape),
                "exposed_after_us": both_off.median(shape),
            }
    write_csv(args.output / "component_masking.csv", pair_rows)

    main_rows: list[dict[str, object]] = []
    for shape_i, shape in enumerate(SHAPES):
        dims = runs["baseline"].payload["shapes"][shape]
        had = bootstrap_expression(
            [(1.0, runs["baseline"]), (-1.0, runs["no_had"])],
            shape,
            seed=2026090000 + shape_i,
            samples=args.bootstrap_samples,
        )
        pts_scalar = pair_effects[(shape, "per_tensor_scale_math")]["visible"]
        dq_total = pair_effects[(shape, "dynamic_quant_total_compute")]["visible"]
        zero = runs["baseline"].median(shape, "zero_amax")
        standalone_amax = runs["baseline"].median(shape, "per_tensor_amax")
        fusion = runs["baseline"].median(shape)
        row: dict[str, object] = {
            "shape": shape,
            "M": dims["M"],
            "K": dims["K"],
            "baseline_fusion_us": fusion,
            "zero_amax_kernel_us": zero,
            "standalone_tensor_amax_kernel_us": standalone_amax,
            "per_tensor_pipeline_visible_us": zero + standalone_amax + pts_scalar.estimate,
            "baseline_total_path_us": zero + standalone_amax + fusion,
        }
        add_effect(row, "hadamard_tc_visible", had)
        add_removal_percentages(
            row,
            "hadamard_tc_visible",
            runs["baseline"].median(shape),
            runs["no_had"].median(shape),
        )
        add_effect(row, "dynamic_quant_compute_visible", dq_total)
        add_effect(row, "dynamic_quant_compute_exposed_after_had_removed", pair_effects[(shape, "dynamic_quant_total_compute")]["exposed"])
        dq_points = pair_points[(shape, "dynamic_quant_total_compute")]
        add_removal_percentages(
            row,
            "dynamic_quant_compute_visible",
            dq_points["visible_before_us"],
            dq_points["visible_after_us"],
        )
        add_removal_percentages(
            row,
            "dynamic_quant_compute_exposed_after_had_removed",
            dq_points["exposed_before_us"],
            dq_points["exposed_after_us"],
        )
        add_effect(row, "per_tensor_scale_math_visible", pts_scalar)
        add_effect(row, "per_tensor_scale_math_exposed_after_had_removed", pair_effects[(shape, "per_tensor_scale_math")]["exposed"])
        pts_points = pair_points[(shape, "per_tensor_scale_math")]
        add_removal_percentages(
            row,
            "per_tensor_scale_math_visible",
            pts_points["visible_before_us"],
            pts_points["visible_after_us"],
        )
        add_removal_percentages(
            row,
            "per_tensor_scale_math_exposed_after_had_removed",
            pts_points["exposed_before_us"],
            pts_points["exposed_after_us"],
        )
        baseline_total = zero + standalone_amax + fusion
        add_removal_percentages(row, "zero_amax_kernel", baseline_total, baseline_total - zero)
        add_removal_percentages(
            row,
            "standalone_tensor_amax_kernel",
            baseline_total,
            baseline_total - standalone_amax,
        )
        add_removal_percentages(
            row,
            "per_tensor_pipeline_visible",
            baseline_total,
            baseline_total - (zero + standalone_amax + pts_scalar.estimate),
        )
        main_rows.append(row)
    write_csv(args.output / "main_components.csv", main_rows)

    breakdown_rows: list[dict[str, object]] = []
    for shape in SHAPES:
        dims = runs["baseline"].payload["shapes"][shape]
        for component in (
            "dq_local_amax_reduction",
            "dq_scale_chain",
            "dq_fp4_scale_convert",
            "dynamic_quant_total_compute",
        ):
            effect = pair_effects[(shape, component)]
            row: dict[str, object] = {
                "shape": shape,
                "M": dims["M"],
                "K": dims["K"],
                "component": component,
            }
            add_effect(row, "visible", effect["visible"])
            add_effect(row, "exposed_after_had_removed", effect["exposed"])
            add_effect(row, "signed_interaction", effect["interaction"])
            points = pair_points[(shape, component)]
            add_removal_percentages(
                row, "visible", points["visible_before_us"], points["visible_after_us"]
            )
            add_removal_percentages(
                row,
                "exposed_after_had_removed",
                points["exposed_before_us"],
                points["exposed_after_us"],
            )
            breakdown_rows.append(row)
    write_csv(args.output / "dynamic_quant_breakdown.csv", breakdown_rows)

    stability_rows: list[dict[str, object]] = []
    for shape in SHAPES:
        old = runs["baseline_r1"].median(shape)
        new = runs["baseline"].median(shape)
        stability_rows.append(
            {
                "shape": shape,
                "baseline_r1_us": old,
                "baseline_r2_us": new,
                "delta_r2_minus_r1_us": new - old,
                "relative_delta_percent": (new / old - 1.0) * 100.0,
            }
        )
    write_csv(args.output / "baseline_stability.csv", stability_rows)

    manifest_rows = [
        {
            "logical_name": key,
            "json_label": run.name,
            "source_path": str(run.path.resolve()),
            "rounds": run.payload["rounds"],
            "iters": run.payload["iters"],
            "warmup": run.payload["warmup"],
        }
        for key, run in runs.items()
    ]
    write_csv(args.output / "source_manifest.csv", manifest_rows)

    for name in (
        "component_masking.csv",
        "main_components.csv",
        "dynamic_quant_breakdown.csv",
        "baseline_stability.csv",
        "source_manifest.csv",
    ):
        print(args.output / name)


if __name__ == "__main__":
    main()
