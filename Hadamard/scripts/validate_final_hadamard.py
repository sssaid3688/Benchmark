#!/usr/bin/env python3
"""Fail-closed validation for the final Hadamard ablation package."""

from __future__ import annotations

import argparse
import csv
import json
import math
from pathlib import Path


SHAPES = {"shape8", "shape5", "shape9", "shape6", "shape7", "shape1", "shape2"}


def read_csv(path: Path) -> list[dict[str, str]]:
    with path.open(encoding="utf-8-sig", newline="") as handle:
        return list(csv.DictReader(handle))


def keyed(path: Path) -> dict[str, dict[str, str]]:
    return {row["variant"]: row for row in read_csv(path)}


def equal_fields(a: dict[str, str], b: dict[str, str], fields: tuple[str, ...]) -> None:
    for field in fields:
        assert a[field] == b[field], f"{field}: {a[field]} != {b[field]}"


def validate_removal_percentages(row: dict[str, str], prefix: str) -> None:
    before = float(row[f"{prefix}_before_us"])
    after = float(row[f"{prefix}_after_us"])
    latency = float(row[f"{prefix}_latency_reduction_percent"])
    throughput = float(row[f"{prefix}_throughput_gain_percent"])
    expected_latency = (before - after) / before * 100.0
    expected_throughput = (before / after - 1.0) * 100.0
    assert math.isfinite(latency) and math.isfinite(throughput)
    assert math.isclose(latency, expected_latency, rel_tol=1e-12, abs_tol=1e-12)
    assert math.isclose(throughput, expected_throughput, rel_tol=1e-12, abs_tol=1e-12)


def validate_runs(directory: Path, names: tuple[str, ...]) -> None:
    for name in names:
        payload = json.loads((directory / f"{name}.json").read_text(encoding="utf-8"))
        assert payload["rounds"] == 17
        assert payload["iters"] == 80
        assert payload["warmup"] == 80
        assert set(payload["shapes"]) == SHAPES
        for shape in SHAPES:
            assert len(payload["shapes"][shape]["fusion"]["rounds_us"]) == 17


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--results-root", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    root = args.results_root

    final = root / "hadamard_ablation_final"
    r2 = root / "hadamard_ablation_r2"
    pts = root / "hadamard_ablation_final_pairs"
    dq = root / "hadamard_ablation_dq_strict_anchor"
    fp4 = root / "hadamard_ablation_fp4_anchor"

    validate_runs(
        r2,
        (
            "baseline_r2",
            "no_had_r2",
            "no_amax_reduction_r2",
            "no_had_no_amax_reduction_r2",
            "no_scale_chain_r2",
            "no_had_no_scale_chain_r2",
        ),
    )
    validate_runs(pts, ("no_pts_r3", "no_had_no_pts_r3"))
    validate_runs(
        dq,
        (
            "baseline_dq_strict_anchor",
            "no_dq_strict_anchor",
            "no_had_dq_strict_anchor",
            "no_had_no_dq_strict_anchor",
        ),
    )
    validate_runs(
        fp4,
        (
            "baseline_fp4_anchor",
            "no_fp4_convert_anchor",
            "no_had_fp4_anchor",
            "no_had_no_fp4_convert_anchor",
        ),
    )

    memory = (
        "tma_load_utmalgd_2d",
        "tmem_load_ldtm_x64",
        "global_store_stg",
        "register_max",
        "shared_max_bytes",
        "local_max_bytes",
    )

    r2_audit = keyed(r2 / "sass_audit.csv")
    equal_fields(r2_audit["baseline_r2"], r2_audit["no_had_r2"], memory)
    assert r2_audit["baseline_r2"]["tc_hadamard_utchmma"] == "256"
    assert r2_audit["no_had_r2"]["tc_hadamard_utchmma"] == "0"

    dq_audit = keyed(dq / "sass_audit.csv")
    dq_base = dq_audit["baseline_dq_strict_anchor"]
    dq_off = dq_audit["no_dq_strict_anchor"]
    equal_fields(dq_base, dq_off, memory + ("global_load_ldg_32",))
    assert dq_base["global_load_ldg_32"] == dq_off["global_load_ldg_32"] == "64"
    assert dq_base["global_load_ldg_64_constant"] == "64"
    assert dq_off["global_load_ldg_64_constant"] == "32"
    for base_name, off_name in (
        ("baseline_dq_strict_anchor", "no_had_dq_strict_anchor"),
        ("no_dq_strict_anchor", "no_had_no_dq_strict_anchor"),
    ):
        equal_fields(dq_audit[base_name], dq_audit[off_name], memory)
        assert dq_audit[base_name]["tc_hadamard_utchmma"] == "256"
        assert dq_audit[off_name]["tc_hadamard_utchmma"] == "0"

    pts_audit = keyed(pts / "sass_audit.csv")
    equal_fields(pts_audit["no_pts_r3"], pts_audit["no_had_no_pts_r3"], memory)
    assert pts_audit["no_pts_r3"]["global_load_ldg_32"] == "32"

    fp4_audit = keyed(fp4 / "sass_audit.csv")
    for base_name, off_name in (
        ("baseline_fp4_anchor", "no_fp4_convert_anchor"),
        ("no_had_fp4_anchor", "no_had_no_fp4_convert_anchor"),
    ):
        equal_fields(
            fp4_audit[base_name],
            fp4_audit[off_name],
            memory
            + (
                "global_load_ldg_32",
                "global_load_ldg_64_constant",
                "fp8_scale_convert_e4m3",
                "mufu_rcp",
            ),
        )
        assert fp4_audit[base_name]["fp4_convert_e2m1"] == "3072"
        assert fp4_audit[off_name]["fp4_convert_e2m1"] == "2048"

    main_rows = read_csv(final / "main_components.csv")
    masking_rows = read_csv(final / "component_masking.csv")
    breakdown_rows = read_csv(final / "dynamic_quant_breakdown.csv")
    manifest_rows = read_csv(final / "source_manifest.csv")
    assert {row["shape"] for row in main_rows} == SHAPES and len(main_rows) == 7
    assert len(masking_rows) == 35
    assert len(breakdown_rows) == 28
    assert len(manifest_rows) == 17
    for row in masking_rows:
        for prefix in (
            "component_visible",
            "component_exposed_after_had_removed",
            "had_visible",
            "had_exposed_after_component_removed",
        ):
            validate_removal_percentages(row, prefix)
    for row in main_rows:
        for prefix in (
            "hadamard_tc_visible",
            "dynamic_quant_compute_visible",
            "dynamic_quant_compute_exposed_after_had_removed",
            "per_tensor_scale_math_visible",
            "per_tensor_scale_math_exposed_after_had_removed",
            "zero_amax_kernel",
            "standalone_tensor_amax_kernel",
            "per_tensor_pipeline_visible",
        ):
            validate_removal_percentages(row, prefix)
    for row in breakdown_rows:
        validate_removal_percentages(row, "visible")
        validate_removal_percentages(row, "exposed_after_had_removed")
    report_text = (final / "HADAMARD_ABLATION_REPORT.md").read_text(encoding="utf-8")
    assert len(report_text) > 10_000
    assert "延迟影响%" in report_text

    lines = [
        "PASS: final Hadamard ablation package",
        "PASS: 16 final benchmark JSONs have 7 shapes and 17x80 samples",
        "PASS: Hadamard removes UTCHMMA only from the target path",
        "PASS: strict total-DQ preserves TMA/TMEM/store/LDG32/resources",
        "PASS: pure FP4 preserves SF/RCP/memory paths and removes E2M1 only",
        "PASS: 35 masking rows and 28 DQ-breakdown rows are complete",
        "PASS: every component-removal row has formula-checked latency and throughput percentages",
    ]
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text("\n".join(lines) + "\n", encoding="utf-8")
    print("\n".join(lines))


if __name__ == "__main__":
    main()
