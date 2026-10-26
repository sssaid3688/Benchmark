#!/usr/bin/env python3
"""Analyze per-source-line stall samples from .ncu-rep (--page source --csv).
Prints: (1) total stall-sample breakdown across the kernel, (2) top source lines
by total samples, with their dominant stall reason.
Usage: ncu_stalls.py <report.ncu-rep> [N_top_lines]
"""
import sys, subprocess, csv, io

NCU = "/usr/local/cuda-13.3/bin/ncu"
STALL_COLS = ["stall_long_sb","stall_short_sb","stall_mio","stall_barrier","stall_math",
              "stall_wait","stall_not_selected","stall_no_inst","stall_drain","stall_membar",
              "stall_dispatch","stall_tex","stall_lg","stall_misc","stall_selected",
              "stall_branch_resolving","stall_sleep"]
# friendly map
FRIENDLY = {"stall_long_sb":"Long Scoreboard (mem dep)","stall_short_sb":"Short Scoreboard",
            "stall_mio":"MIO Throttle (math-in-out)","stall_barrier":"Barrier",
            "stall_math":"Math Pipe Throttle","stall_wait":"Wait (sync)",
            "stall_not_selected":"Not Selected","stall_no_inst":"No Instruction",
            "stall_drain":"Drain","stall_membar":"Membar","stall_dispatch":"Dispatch Stall",
            "stall_tex":"TEX","stall_lg":"Long Scoreboard(LG)","stall_misc":"Misc",
            "stall_selected":"Selected(fixed)","stall_branch_resolving":"Branch Resolving",
            "stall_sleep":"Sleep"}

def load(report):
    cmd = [NCU,"--import",report,"--page","source","--csv"]
    res = subprocess.run(cmd, capture_output=True, text=True)
    rows = list(csv.reader(io.StringIO(res.stdout)))
    # find header
    hdr=None
    for r in rows:
        if len(r)>5 and r[0]=="Address":
            hdr=r; break
    if not hdr: return [],{}
    idx={c:i for i,c in enumerate(hdr)}
    recs=[]
    for r in rows:
        if len(r)<len(hdr): continue
        try:
            addr=r[idx["Address"]]; src=r[idx["Source"]]; nsamp=r[idx["# Samples"]]
        except KeyError: continue
        if not nsamp or nsamp=="0" or not addr or addr=="0x0": continue
        stalls={}
        for s in STALL_COLS:
            if s in idx:
                try: stalls[s]=int(float(r[idx[s]]))
                except: stalls[s]=0
        recs.append((addr,src,nsamp,stalls))
    return hdr, recs

def main():
    report=sys.argv[1]
    ntop=int(sys.argv[2]) if len(sys.argv)>2 else 15
    hdr,recs=load(report)
    if not recs:
        print("no source records"); return
    total={}
    for _,_,_,st in recs:
        for k,v in st.items(): total[k]=total.get(k,0)+v
    tot_samples=sum(total.values())
    print(f"=== Stall sample breakdown (kernel total, {tot_samples} samples) ===")
    for k in sorted(total, key=lambda x:-total[x]):
        if total[k]<=0: continue
        pct=100.0*total[k]/tot_samples if tot_samples else 0
        print(f"  {FRIENDLY.get(k,k):35s} {total[k]:8d}  {pct:5.1f}%")
    print(f"\n=== Top {ntop} source lines by stall samples ===")
    for addr,src,ns,st in sorted(recs,key=lambda x:-int(x[2] or 0))[:ntop]:
        dom=max(st, key=st.get) if any(st.values()) else "-"
        domv=st.get(dom,0)
        sline=src.replace("\n"," ").strip()[:90]
        print(f"  {addr:>12s} samp={ns:>6s}  dom={FRIENDLY.get(dom,dom)[:25]:25s}({domv})  | {sline}")

if __name__=="__main__":
    main()
