#!/usr/bin/env python3
"""Audit instruction/resource preservation across Hadamard ablation cubins."""

from __future__ import annotations

import argparse
import csv
import re
from pathlib import Path


PATTERNS = {
    "functions": r"^\s*Function\s*:",
    "instructions": r"^\s*/\*[0-9a-f]+\*/",
    "tc_hadamard_utchmma": r"\bUTCHMMA\b",
    "tma_load_utmalgd_2d": r"\bUTMALDG\.2D\b",
    "tmem_load_ldtm_x64": r"\bLDTM\.x64\b",
    "global_store_stg": r"\bSTG(?:\.[A-Z0-9]+)*\b",
    "global_load_ldg": r"\bLDG(?:\.[A-Z0-9]+)*\b",
    "global_load_ldg_32": r"\bLDG\.E\b(?!\.64)",
    "global_load_ldg_64_constant": r"\bLDG\.E\.64\.CONSTANT\b",
    "fp4_convert_e2m1": r"\bF2FP\.[^\s;]*E2M1[^\s;]*",
    "fp8_scale_convert_e4m3": r"\bF2FP\.[^\s;]*E4M3[^\s;]*",
    "mufu_rcp": r"\bMUFU\.RCP\b",
    "float_minmax": r"\bFMNMX\b",
    "float_abs": r"\bFABS\b",
    "float_mul": r"\bFMUL\b",
    "float_add": r"\bFADD\b",
    "float_fma": r"\bFFMA\b",
    "barrier": r"\b(?:BAR|UTCBAR)(?:\.[A-Z0-9]+)*\b",
}


def write_csv(path: Path, rows: list[dict[str, object]]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    fields: list[str] = []
    for row in rows:
        for key in row:
            if key not in fields:
                fields.append(key)
    with path.open("w", newline="", encoding="utf-8-sig") as handle:
        writer = csv.DictWriter(handle, fieldnames=fields)
        writer.writeheader()
        writer.writerows(rows)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("sass_dir", type=Path)
    parser.add_argument("output_csv", type=Path)
    args = parser.parse_args()

    rows: list[dict[str, object]] = []
    for sass_path in sorted(args.sass_dir.glob("*.sass")):
        if sass_path.stat().st_size == 0:
            continue
        text = sass_path.read_text(encoding="utf-8", errors="replace")
        row: dict[str, object] = {
            "variant": sass_path.stem,
            "sass_bytes": sass_path.stat().st_size,
        }
        for name, pattern in PATTERNS.items():
            row[name] = len(re.findall(pattern, text, flags=re.MULTILINE))

        resource_path = sass_path.with_suffix(".resources.txt")
        if resource_path.exists():
            resource_text = resource_path.read_text(encoding="utf-8", errors="replace")
            usage = re.findall(
                r"REG:(\d+) STACK:(\d+) SHARED:(\d+) LOCAL:(\d+)", resource_text
            )
            if usage:
                regs = [int(item[0]) for item in usage]
                stacks = [int(item[1]) for item in usage]
                shared = [int(item[2]) for item in usage]
                local = [int(item[3]) for item in usage]
                row.update(
                    {
                        "resource_records": len(usage),
                        "register_min": min(regs),
                        "register_max": max(regs),
                        "stack_max_bytes": max(stacks),
                        "shared_min_bytes": min(shared),
                        "shared_max_bytes": max(shared),
                        "local_max_bytes": max(local),
                    }
                )
        rows.append(row)

    write_csv(args.output_csv, rows)
    print(f"wrote {args.output_csv} ({len(rows)} variants)")


if __name__ == "__main__":
    main()
