#!/usr/bin/env python3
"""Compute per-variant TFLOPS delta vs baseline from the timing CSVs.
Higher variant TFLOPS = the removed component was on the critical path (un-masked).
Largest positive delta = the biggest un-masked bottleneck.
Usage: analyze_deltas.py <tag>
"""
import sys, csv, os, statistics

TAG = sys.argv[1] if len(sys.argv)>1 else "S1"
D = f"/home/ubuntu/workspace/oyhj/temp/test/test_merge/Benchmark/FA/b200_blackwell_fmha_mxfp8_ablation/timings"

def median(csvf):
    vals=[]
    with open(csvf) as f:
        for r in csv.DictReader(f):
            try: vals.append(float(r['tflops']))
            except: pass
    return statistics.median(vals) if vals else None

rows=[]
for fn in sorted(os.listdir(D)):
    if not fn.endswith(f"_{TAG}.csv"): continue
    name = fn.replace(f"_{TAG}.csv","").replace("abl_","").replace("_b200_blackwell_fmha_mxfp8","").replace("op6static2sm","baseline")
    m = median(os.path.join(D,fn))
    if m: rows.append((name,m))

base = next((m for n,m in rows if n=="baseline"), None)
if base is None:
    print("no baseline!"); sys.exit(1)

print(f"\n=== Component-removal ablation — TAG={TAG} (clocks locked) ===")
print(f"Baseline median: {base:.2f} TFLOPS/s\n")
print(f"{'variant':28s} {'TFLOPS':>10s} {'Δ TFLOPS':>10s} {'Δ %':>8s}  interpretation")
print("-"*85)
# sort by delta descending (most-unmasked first)
ranked = sorted([(n,m,m-base,100*(m-base)/base) for n,m in rows if n!="baseline"], key=lambda x:-x[2])
for n,m,d,dp in ranked:
    interp = "BIGGEST un-masked bottleneck" if dp>15 else ("un-masked" if dp>3 else ("~masked" if abs(dp)<3 else "slows down (bottleneck is elsewhere)"))
    print(f"{n:28s} {m:10.2f} {d:>+10.2f} {dp:>+7.1f}%  {interp}")
# also show the ones that went DOWN (removing them made it slower -> they were helping)
print(f"\nNote: positive Δ = removing that component SPEEDS UP the kernel")
print(f"      => that component's time was NOT masked (sat on critical path).")
print(f"      The LARGEST positive Δ is the root bottleneck.")
