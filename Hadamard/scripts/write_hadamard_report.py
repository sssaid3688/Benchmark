#!/usr/bin/env python3
"""Render the final Hadamard ablation tables as a self-contained Chinese report."""

from __future__ import annotations

import argparse
import csv
from pathlib import Path


SHAPES = ["shape8", "shape5", "shape9", "shape6", "shape7", "shape1", "shape2"]
COMPONENT_LABELS = {
    "dq_local_amax_reduction": "局部 absmax 归约",
    "dq_scale_chain": "SF 量化/反量化及倒数链",
    "dq_fp4_scale_convert": "数值缩放与 FP32→E2M1",
    "dynamic_quant_total_compute": "动态量化总计算",
}


def read_rows(path: Path) -> list[dict[str, str]]:
    with path.open(encoding="utf-8-sig", newline="") as handle:
        return list(csv.DictReader(handle))


def by_key(rows: list[dict[str, str]], *fields: str) -> dict[tuple[str, ...], dict[str, str]]:
    return {tuple(row[field] for field in fields): row for row in rows}


def f(value: str | float, digits: int = 3) -> str:
    return f"{float(value):.{digits}f}"


def effect(row: dict[str, str], prefix: str) -> str:
    estimate = f(row[f"{prefix}_us"])
    low = f(row[f"{prefix}_ci95_low_us"])
    high = f(row[f"{prefix}_ci95_high_us"])
    marker = "" if row[f"{prefix}_resolved"].lower() == "true" else "†"
    return f"{estimate}{marker} [{low}, {high}]"


def effect_with_pct(row: dict[str, str], prefix: str) -> str:
    pct = f(row[f"{prefix}_latency_reduction_percent"], 2)
    return f"{effect(row, prefix)}；{pct}%"


def value_with_pct(row: dict[str, str], value_field: str, prefix: str) -> str:
    pct = f(row[f"{prefix}_latency_reduction_percent"], 2)
    return f"{f(row[value_field])}；{pct}%"


def md_table(headers: list[str], rows: list[list[str]]) -> list[str]:
    lines = ["| " + " | ".join(headers) + " |", "|" + "|".join(["---"] * len(headers)) + "|"]
    lines.extend("| " + " | ".join(row) + " |" for row in rows)
    return lines


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--analysis", type=Path, required=True)
    parser.add_argument("--r2-audit", type=Path, required=True)
    parser.add_argument("--anchor-audit", type=Path, required=True)
    parser.add_argument("--final-audit", type=Path, required=True)
    parser.add_argument("--fp4-audit", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()

    main_rows = by_key(read_rows(args.analysis / "main_components.csv"), "shape")
    masking = by_key(read_rows(args.analysis / "component_masking.csv"), "shape", "component")
    breakdown = by_key(read_rows(args.analysis / "dynamic_quant_breakdown.csv"), "shape", "component")
    stability = by_key(read_rows(args.analysis / "baseline_stability.csv"), "shape")
    audit_rows = (
        read_rows(args.r2_audit)
        + read_rows(args.anchor_audit)
        + read_rows(args.final_audit)
        + read_rows(args.fp4_audit)
    )
    audit = {row["variant"]: row for row in audit_rows}

    lines: list[str] = [
        "# B200 最优 Hadamard 融合算子消融实验报告",
        "",
        "## 1. 结论摘要",
        "",
        "本实验把最优 `doublewarp_fusion.cu` 的融合内核拆为三个逻辑部分：Tensor Core Hadamard、动态量化计算、per-tensor scaling。所有最终对照都保留原调度、TMA/TMEM 路径和输出 store；总动态量化对照还在开/关两侧加入相同的 TMEM checksum 锚点，避免 ptxas 因输出常量化删除上游读取。",
        "",
        "表中时间均为微秒。带百分比的单元格写作 `耗时差 [95% CI]；延迟影响%`。`直接` = 基线 − 关闭该部分；`Hadamard 关闭后暴露` = Hadamard-off − 两者同时 off；`交互量` = 暴露 − 直接。延迟影响统一按 `(移除前延迟 − 移除后延迟) / 移除前延迟 × 100%` 计算：正值表示去除后变快，负值表示去除后反而变慢。等价吞吐提升百分比另存于 CSV。交互量为正表示原基线中存在掩盖，为负表示共享瓶颈/调度重排或跨构建漂移，不能解释成负耗时。95% bootstrap CI 跨 0 的结果以 `†` 标记，只作为未分辨趋势。",
        "",
        "## 2. 三个主要部分",
        "",
    ]

    table_rows: list[list[str]] = []
    for shape in SHAPES:
        row = main_rows[(shape,)]
        dq = masking[(shape, "dynamic_quant_total_compute")]
        pts = masking[(shape, "per_tensor_scale_math")]
        table_rows.append(
            [
                shape,
                f"{row['M']}×{row['K']}",
                f(row["baseline_fusion_us"]),
                effect_with_pct(row, "hadamard_tc_visible"),
                effect_with_pct(dq, "component_visible"),
                effect_with_pct(dq, "component_exposed_after_had_removed"),
                effect(dq, "signed_interaction"),
                effect_with_pct(pts, "component_visible"),
                effect_with_pct(pts, "component_exposed_after_had_removed"),
                effect(pts, "signed_interaction"),
            ]
        )
    lines += md_table(
        [
            "shape",
            "M×K",
            "融合基线",
            "Hadamard 直接",
            "DQ 直接",
            "DQ 暴露",
            "DQ 交互",
            "PTS 标量直接",
            "PTS 标量暴露",
            "PTS 交互",
        ],
        table_rows,
    )

    lines += [
        "",
        "上表给出常规基线中的 Hadamard 直接值，并明确展示动态量化和 PTS 在去掉 Hadamard 前后的变化。下面反向列出 Hadamard 在去掉另外两个主要部分前后的变化；同一行采用同一个 matched 四点矩阵，避免把不同控制基线混用。",
        "",
        "### Hadamard 被其他部分掩盖/暴露",
        "",
    ]
    had_rows: list[list[str]] = []
    for shape in SHAPES:
        dq = masking[(shape, "dynamic_quant_total_compute")]
        pts = masking[(shape, "per_tensor_scale_math")]
        had_rows.append(
            [
                shape,
                effect_with_pct(dq, "had_visible"),
                effect_with_pct(dq, "had_exposed_after_component_removed"),
                effect(dq, "signed_interaction"),
                effect_with_pct(pts, "had_visible"),
                effect_with_pct(pts, "had_exposed_after_component_removed"),
                effect(pts, "signed_interaction"),
            ]
        )
    lines += md_table(
        [
            "shape",
            "Had 直接（DQ 矩阵）",
            "去 DQ 后 Had 暴露",
            "Had×DQ 交互",
            "Had 直接（PTS 矩阵）",
            "去 PTS 后 Had 暴露",
            "Had×PTS 交互",
        ],
        had_rows,
    )
    lines += [
        "",
        "不同组件的消融差值不可相加，因为它们本来就并行、共享发射槽和访存瓶颈。完整四点原始值保存在 `component_masking.csv`。",
        "",
        "## 3. 动态量化内部拆分",
        "",
    ]
    breakdown_rows: list[list[str]] = []
    for shape in SHAPES:
        for component in COMPONENT_LABELS:
            row = breakdown[(shape, component)]
            breakdown_rows.append(
                [
                    shape,
                    COMPONENT_LABELS[component],
                    effect_with_pct(row, "visible"),
                    effect_with_pct(row, "exposed_after_had_removed"),
                    effect(row, "signed_interaction"),
                ]
            )
    lines += md_table(
        ["shape", "动态量化子部分", "直接", "Hadamard 关闭后暴露", "交互量"],
        breakdown_rows,
    )
    lines += [
        "",
        "内部三项也不可相加得到总动态量化：分别关闭某一段会改变同一 warp 的依赖链和指令调度。`动态量化总计算` 使用专门的 matched-anchor 四点矩阵，是总量结论；内部三项用于定位重叠和瓶颈。",
        "",
        "## 4. Per-tensor scaling 的完整路径",
        "",
        "该路径不是融合内核里的一条乘法。它由串行的 `ZeroAmax`、扫描输入张量的独立 `HadamardAmaxTmaKernel`，以及融合内核中的 global-amax load/encode-decode 标量数学组成。完整可见开销按三者相加，基线总路径为 `ZeroAmax + Amax + fusion`。",
        "",
    ]
    pts_rows: list[list[str]] = []
    for shape in SHAPES:
        row = main_rows[(shape,)]
        pts_rows.append(
            [
                shape,
                f"{row['M']}×{row['K']}",
                value_with_pct(row, "zero_amax_kernel_us", "zero_amax_kernel"),
                value_with_pct(
                    row,
                    "standalone_tensor_amax_kernel_us",
                    "standalone_tensor_amax_kernel",
                ),
                effect_with_pct(row, "per_tensor_scale_math_visible"),
                value_with_pct(
                    row, "per_tensor_pipeline_visible_us", "per_tensor_pipeline_visible"
                ),
                f(row["baseline_total_path_us"]),
            ]
        )
    lines += md_table(
        ["shape", "M×K", "ZeroAmax", "独立 Amax", "融合内 PTS", "PTS 完整开销", "完整基线路径"],
        pts_rows,
    )

    lines += [
        "",
        "## 5. 防编译优化审计",
        "",
        "下表统计整个目标文件的关键 SASS 指令。判据不是总指令数相同，而是非目标通路必须保持：TMA load、TMEM load、global store、寄存器/SMEM/local memory；目标通路才允许变化。",
        "",
    ]
    audit_variants = [
        "baseline_r2",
        "no_had_r2",
        "baseline_dq_strict_anchor",
        "no_dq_strict_anchor",
        "no_had_dq_strict_anchor",
        "no_had_no_dq_strict_anchor",
        "no_pts_r3",
        "no_had_no_pts_r3",
        "baseline_fp4_anchor",
        "no_fp4_convert_anchor",
        "no_had_fp4_anchor",
        "no_had_no_fp4_convert_anchor",
    ]
    audit_table: list[list[str]] = []
    for variant in audit_variants:
        row = audit[variant]
        audit_table.append(
            [
                variant,
                row["tc_hadamard_utchmma"],
                row["tma_load_utmalgd_2d"],
                row["tmem_load_ldtm_x64"],
                row["global_store_stg"],
                row["global_load_ldg"],
                row["global_load_ldg_32"],
                row["global_load_ldg_64_constant"],
                row["fp4_convert_e2m1"],
                row["mufu_rcp"],
                row["register_max"],
                row["shared_max_bytes"],
                row["local_max_bytes"],
            ]
        )
    lines += md_table(
        ["variant", "UTCHMMA", "UTMALDG", "LDTM", "STG", "LDG总", "LDG32", "LDG64 RNG", "E2M1", "MUFU.RCP", "REG", "SMEM", "LOCAL"],
        audit_table,
    )
    lines += [
        "",
        "严格总 DQ 四点矩阵均保持 `UTMALDG=128、LDTM=64、STG=192、REG=128、SMEM=1024 B、LOCAL=0`，并用 PTS-result anchor 保留 global-amax 读取及 encode/decode 标量结果。总 LDG 的 128→96 仅来自目标 DQ 随机舍入路径的 `LDG.E.64.CONSTANT`（RNG seed/offset）64→32；32 位 global-amax `LDG.E` 保持 64→64。关闭 Hadamard 时 `UTCHMMA` 从 256 降为 0。最早无 TMEM 锚点的 no-DQ 会把 LDTM 从 64 降为 32；后续仅 TMEM 锚定版本仍会让 PTS 标量结果失去消费者，两版均被最终 strict 矩阵取代。PTS 消融的 32 位 `LDG.E` 减少 32，正对应目标 global-amax load。纯 FP4 四点矩阵在两侧加入相同 acc_scale 锚点，使 `MUFU.RCP=832、E4M3=512` 以及 TMA/TMEM/store 全部保持，仅目标 E2M1 `3072→2048` 和相应 value FMUL 被删除。旧版未锚定 FP4 对照会顺带删除 reciprocal/decode 链，已判废。",
        "",
        "## 6. 稳定性与数据取舍",
        "",
    ]
    stable_rows: list[list[str]] = []
    for shape in SHAPES:
        row = stability[(shape,)]
        stable_rows.append(
            [shape, f(row["baseline_r1_us"]), f(row["baseline_r2_us"]), f(row["delta_r2_minus_r1_us"]), f(row["relative_delta_percent"], 2) + "%"]
        )
    lines += md_table(["shape", "baseline r1", "baseline r2", "差值", "相对差"], stable_rows)
    lines += [
        "",
        "### Matched anchor 本身的扰动",
        "",
    ]
    anchor_rows: list[list[str]] = []
    for shape in SHAPES:
        regular = float(main_rows[(shape,)]["baseline_fusion_us"])
        dq_base = float(masking[(shape, "dynamic_quant_total_compute")]["baseline_us"])
        fp4_base = float(masking[(shape, "dq_fp4_scale_convert")]["baseline_us"])
        anchor_rows.append(
            [
                shape,
                f(regular),
                f(dq_base),
                f(dq_base - regular),
                f(fp4_base),
                f(fp4_base - regular),
            ]
        )
    lines += md_table(
        ["shape", "常规基线", "DQ strict-anchor 基线", "DQ anchor 扰动", "FP4 anchor 基线", "FP4 anchor 扰动"],
        anchor_rows,
    )
    lines += [
        "",
        "锚点在四点矩阵两侧完全相同，因此一阶开销在差分中抵消；但它仍可能改变发射/依赖调度。总 DQ 与纯 FP4 的耗时应解释为各自 matched-anchor 调度环境内的可见/暴露成本，而不是与常规基线直接相加。",
        "",
        "每个最终版本采用 80 次 warmup、17 轮、每轮 80 次；表中点估计为 17 个轮中位数的中位数，CI 对轮中位数做 20,000 次 bootstrap。GPU SM 时钟锁为 1965 MHz。运行前要求没有其他 compute process 且利用率连续稳定；首轮中被外部作业污染的版本由同配置重测替换，并以 Amax 控制核和基线复测共同判别。",
        "",
        "微秒级差值存在跨构建漂移，尤其是超大 shape。CI 反映单次版本运行内部波动，不覆盖所有跨构建系统误差；因此应优先看 matched 四点矩阵、符号稳定且明显高于漂移的效应。负值或带 `†` 的项不应解释为真实负耗时。",
        "",
        "## 7. 复现信息",
        "",
        "- 最优源：`/home/ubuntu/workspace/oyhj/temp/test/test_merge/Benchmark/Hadamard/src/doublewarp_fusion.cu`",
        "- 消融模板：`/home/ubuntu/workspace/oyhj/temp/test/test_merge/Benchmark/Hadamard/src/doublewarp_fusion_ablation.cu`",
        "- 运行说明：`/home/ubuntu/workspace/oyhj/temp/test/test_merge/Benchmark/Hadamard/RUNBOOK.md`",
        "- 最终结果目录：`ablation_results_20260829_r2`、`ablation_results_20260829_final_pairs`、`ablation_results_20260830_dq_strict_anchor`、`ablation_results_20260830_fp4_anchor`",
        "- 原始结果清单：`source_manifest.csv`；全量四点结果：`component_masking.csv`；主要结果：`main_components.csv`；DQ 细分：`dynamic_quant_breakdown.csv`。",
        "",
    ]
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text("\n".join(lines), encoding="utf-8")
    print(args.output)


if __name__ == "__main__":
    main()
