#!/usr/bin/env python3
import csv
import pathlib
import statistics
from collections import defaultdict

OUT = pathlib.Path("/home/ubuntu/workspace/oyhj/staticQuant_mxfp8/Benchmark/FA/b200_blackwell_fmha_mxfp8/ablation_results_20260622_045904")
CSV = OUT / "results.csv"
REPORT = OUT / "MXFP8_STATIC_P_ABLATION_REPORT.md"

INTENTS = {
    "baseline": "完整 op6 static-P 2SM 版本。",
    "no_crel": "去掉 softmax->correction early release，观察 O wait 是否重新卡住 softmax/correction 链。",
    "no_e2rsf": "去掉 E2RSF fused row-sum 路径，观察 P 量化/row-sum 融合的贡献。",
    "no_exitdb": "去掉 2SM epilogue/dealloc pair handshake 优化，观察 exit/dealloc 同步形态影响。",
    "no_n128single": "去掉 single N128 QK/PV 路径，回到更重的 K/SF staging 形态，是本轮最大性能下降来源。",
    "no_vprefetch": "去掉 V tile TMA prefetch hint；当前 shape 下中位数几乎不降，说明它不是主瓶颈。",
    "no_m2_combo": "去掉 M2 rescale vote/hoist 组合；本轮反而更快，说明当前编译器/shape 下该组合不是收益项。",
    "no_g_combo": "去掉 correction rescale 的 scale pack/address 组合优化，性能小幅下降。",
    "no_r15_orvlog": "去掉基于 old/new max 的 rescale exp2 延迟/跳过路径，性能小幅下降。",
    "kv_default": "去掉 MXFP8_KV_STAGES=12，回默认 KV stage，load/compute overlap 明显变差。",
    "regsm_default": "去掉 WS_REGSM 和 softmax=184 寄存器预算 override，寄存器预算回默认后性能下降。",
    "no_2cta": "去掉 FMHA_2CTA 标记；注意 N128 mainloop 仍有 ClusterShape<2,1,1>，因此不是纯 1CTA 对照。",
    "no_amaxfuse": "sanity 项；在 MXFP8_PSTATIC 下大量 AMAXFUSE 分支被 !MXFP8_PSTATIC 屏蔽。",
    "no_psf_vec16": "sanity 项；在 MXFP8_PSTATIC 下动态 P-SF 写入阶段被编译掉，影响很弱。",
}

rows = list(csv.DictReader(CSV.open()))
by_variant = defaultdict(list)
for row in rows:
    by_variant[row["variant"]].append(row)

summary = {}
for variant, group in by_variant.items():
    vals = [float(r["perf_tflops"]) for r in group if r["status"] == "ok" and r["perf_tflops"]]
    summary[variant] = {
        "target": group[0]["target"],
        "category": group[0]["category"],
        "removed": group[0]["removed_defs"],
        "statuses": [r["status"] for r in group],
        "values": vals,
        "median": statistics.median(vals) if vals else None,
    }

baseline = summary["baseline"]["median"]
main_variants = [
    "baseline",
    "no_crel",
    "no_e2rsf",
    "no_exitdb",
    "no_n128single",
    "no_vprefetch",
    "no_m2_combo",
    "no_g_combo",
    "no_r15_orvlog",
    "kv_default",
    "regsm_default",
    "no_2cta",
]
sanity_variants = ["no_amaxfuse", "no_psf_vec16"]

eligible = [
    (v, s["median"])
    for v, s in summary.items()
    if s["category"] == "main" and v != "baseline" and s["median"] is not None and s["median"] > 1430
]
worst_gt_1430 = min(eligible, key=lambda item: item[1])

def fmt_values(vals):
    return ", ".join(f"{v:.2f}" for v in vals) if vals else "-"

def delta_cell(median):
    if median is None:
        return "-"
    delta = median - baseline
    pct = delta / baseline * 100.0
    sign = "+" if delta >= 0 else ""
    return f"{sign}{delta:.2f} ({sign}{pct:.2f}%)"

def row_for(variant):
    s = summary[variant]
    med = "-" if s["median"] is None else f"{s['median']:.2f}"
    return (
        f"| `{variant}` | `{s['removed']}` | `{','.join(s['statuses'])}` | "
        f"{fmt_values(s['values'])} | {med} | {delta_cell(s['median'])} |"
    )

lines = []
lines.append("# MXFP8 Static-P FA Compile-Flag Ablation Report")
lines.append("")
lines.append("## Test Setup")
lines.append("")
lines.append("- Source: `/home/ubuntu/workspace/oyhj/staticQuant_mxfp8/Benchmark/FA/b200_blackwell_fmha_mxfp8`")
lines.append("- Build dir: `/home/ubuntu/workspace/oyhj/staticQuant_mxfp8/Benchmark/FA/build_ablation`")
lines.append("- Shape: `--b=1 --h=40 --q=170100 --k=170100 --d=128 --mask=no --warmup_iterations=2 --iterations=5`")
lines.append("- Static quantization kept fixed for every target: `MXFP8_PSTATIC` and `MXFP8_PSTATIC_EXP=0`")
lines.append("- Each variant was run 3 times; the table uses the median TFLOPS/s.")
lines.append("")
lines.append("## Compile-Flag Control")
lines.append("")
lines.append("The complete target is controlled by `OP6_STATIC_2SM_DEFS` in `CMakeLists.txt`. Each ablation target is generated from that same list with `list(REMOVE_ITEM ...)`, so every target differs from baseline only by the removed macro(s) shown below.")
lines.append("")
lines.append("Main baseline definitions include:")
lines.append("")
lines.append("```text")
lines.append("FMHA_2CTA MXFP8_N128 MXFP8_OP6_STATIC_2SM MXFP8_KV_STAGES=12")
lines.append("MXFP8_PSTATIC MXFP8_PSTATIC_EXP=0 MXFP8_2SM_CREL MXFP8_E2RSF")
lines.append("MXFP8_2SM_EXITDB MXFP8_2SM_N128SINGLE MXFP8_2SM_VPREFETCH")
lines.append("MXFP8_M2_COMBO MXFP8_G_COMBO MXFP8_R15_ORVLOG")
lines.append("MXFP8_WS_REGSM MXFP8_REGSM_SOFTMAX=184")
lines.append("```")
lines.append("")
lines.append("## Main Results")
lines.append("")
lines.append(f"- Baseline median: `{baseline:.2f} TFLOPS/s`.")
lines.append(f"- Worst successful main variant still above `1430 TFLOPS/s`: `{worst_gt_1430[0]}` at `{worst_gt_1430[1]:.2f} TFLOPS/s`.")
lines.append("")
lines.append("| Variant | Removed compile option(s) | Status | Runs TFLOPS/s | Median | Delta vs baseline |")
lines.append("|---|---|---|---:|---:|---:|")
for variant in main_variants:
    lines.append(row_for(variant))
lines.append("")
lines.append("## Sanity Results")
lines.append("")
lines.append("`MXFP8_AMAXFUSE` and `MXFP8_PSF_VEC16` are weakly relevant under `MXFP8_PSTATIC`: most AMAXFUSE dynamic-scale code is behind `!defined(MXFP8_PSTATIC)`, and the dynamic P-SF write phase is compiled out for static P.")
lines.append("")
lines.append("| Variant | Removed compile option(s) | Status | Runs TFLOPS/s | Median | Delta vs baseline |")
lines.append("|---|---|---|---:|---:|---:|")
for variant in sanity_variants:
    lines.append(row_for(variant))
lines.append("")
lines.append("## Impact Analysis")
lines.append("")
for variant in main_variants[1:] + sanity_variants:
    s = summary[variant]
    med = s["median"]
    if med is None:
        result = "did not produce a valid performance number"
    else:
        result = f"median `{med:.2f} TFLOPS/s`, {delta_cell(med)} vs baseline"
    lines.append(f"- `{variant}`: {result}. {INTENTS[variant]}")
lines.append("")
lines.append("## Best Candidate Above 1430 With Worst Performance")
lines.append("")
winner = worst_gt_1430[0]
ws = summary[winner]
lines.append(f"`{winner}` is the requested slowest successful main variant above `1430 TFLOPS/s`.")
lines.append("")
lines.append(f"- Target: `{ws['target']}`")
lines.append(f"- Removed compile option(s): `{ws['removed']}`")
lines.append(f"- Median performance: `{ws['median']:.2f} TFLOPS/s`")
lines.append(f"- Runs: `{fmt_values(ws['values'])}`")
lines.append("- Run command:")
lines.append("")
lines.append("```bash")
lines.append(f"cd /home/ubuntu/workspace/oyhj/staticQuant_mxfp8/Benchmark/FA")
lines.append(f"./build_ablation/b200_blackwell_fmha_mxfp8/{ws['target']} \\")
lines.append("  --b=1 --h=40 --q=170100 --k=170100 --d=128 --mask=no \\")
lines.append("  --warmup_iterations=2 --iterations=5")
lines.append("```")
lines.append("")
lines.append("## Notes")
lines.append("")
lines.append("- `no_m2_combo` is faster than baseline in this run. That means `MXFP8_M2_COMBO` should not be assumed beneficial for this exact static-P shape without additional profiling.")
lines.append("- `no_2cta` removes `FMHA_2CTA`, but it is not a pure 1CTA comparison because the `MXFP8_N128` mainloop still defines `ClusterShape<2,1,1>` internally. Treat it as a compile-flag ablation, not a complete structural rewrite.")
lines.append("- Raw logs are in `logs/`, and the machine-readable table is `results.csv` in this same result directory.")

REPORT.write_text("\n".join(lines) + "\n")
print(REPORT)
