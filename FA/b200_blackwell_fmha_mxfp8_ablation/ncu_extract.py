#!/usr/bin/env python3
"""Extract metrics from .ncu-rep files via --page details --csv.
Usage: ncu_extract.py <report.ncu-rep> [--report <r2.ncu-rep> ...] [filter_substring ...]
If filter substrings given, only metrics whose (Section+Metric Name) contain any are printed.
Otherwise prints all metrics with values.
"""
import sys, subprocess, csv, io

NCU = "/usr/local/cuda-13.3/bin/ncu"

def extract(report):
    cmd = [NCU, "--import", report, "--page", "details", "--csv"]
    res = subprocess.run(cmd, capture_output=True, text=True)
    rows = list(csv.reader(io.StringIO(res.stdout)))
    if not rows: return []
    hdr = rows[0]
    idx = {h:i for i,h in enumerate(hdr)}
    out = []
    for r in rows[1:]:
        try:
            sec = r[idx['Section Name']]
            mn  = r[idx['Metric Name']]
            mu  = r[idx['Metric Unit']]
            mv  = r[idx['Metric Value']]
        except (KeyError, IndexError):
            continue
        if mv and mv.strip() and mv.strip()!=' ':
            out.append((f"{sec} :: {mn}", mv, mu))
    # dedupe keeping first
    seen=set(); dd=[]
    for t in out:
        if t[0] not in seen:
            seen.add(t[0]); dd.append(t)
    return dd

def main():
    reports, filters = [], []
    mode=None
    for a in sys.argv[1:]:
        if a=="--report": mode="report"; continue
        if mode=="report": reports.append(a); mode=None; continue
        if a.endswith(".ncu-rep") and not filters: reports.append(a); continue
        filters.append(a)
    if not reports:
        print(__doc__); sys.exit(1)
    labels = [r.split("/")[-1].replace(".ncu-rep","") for r in reports]
    data = {l:extract(r) for l,r in zip(labels,reports)}
    # union of metric names preserving order of first report
    allnames=[]
    for l in labels:
        for n,_,_ in data[l]:
            if n not in allnames: allnames.append(n)
    def keep(name):
        if not filters: return True
        return any(f.lower() in name.lower() for f in filters)
    allnames=[n for n in allnames if keep(n)]
    # table
    print(f"{'metric':58s} " + " ".join(f"{l:>20s}" for l in labels))
    print("-"*(58+21*len(labels)))
    for n in allnames:
        rowvals=[]
        for l in labels:
            v=u=""
            for nn,vv,uu in data[l]:
                if nn==n: v=vv; u=uu; break
            rowvals.append((v or "-",u or ""))
        # align units: put value, show unit only on first
        cells=[]
        for (v,u) in rowvals:
            cells.append(v)
        print(f"{n:58s} " + " ".join(f"{c:>20s}" for c in cells))

if __name__ == "__main__":
    main()
