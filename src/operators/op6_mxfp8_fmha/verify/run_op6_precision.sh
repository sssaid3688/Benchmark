#!/usr/bin/env bash
# op6 full-MXFP8 precision sweep: kernel --dump vs CPU Python golden (op6 flow w/ P-quant).
# 50 small shapes (D=128 fixed by the build; vary B,H,S incl. odd / S%128!=0 / S%32!=0).
# Run on the B200 box (needs the built fmha_mxfp8_pvmx + torch). Usage: bash run_op6_precision.sh
set -uo pipefail
ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../../../.." && pwd)
BIN=$ROOT/build/op6/fmha_mxfp8_pvmx
REF=$ROOT/src/operators/op6_mxfp8_fmha/verify/op6_precision_ref.py
OUT=/tmp/op6v; mkdir -p "$OUT"
[ -x "$BIN" ] || { echo "missing $BIN — build via custom/build_pvmx.sh"; exit 1; }

# 50 shapes: "B H S"  (D=128). Coverage: tiny, single-tile, multi-tile, odd S,
# S%128!=0, S%32!=0, multi-head, B>1.
SHAPES=(
 "1 1 128"  "1 1 129"  "1 1 130"  "1 1 160"  "1 1 192"
 "1 1 200"  "1 1 224"  "1 1 255"  "1 1 256"  "1 1 257"
 "1 2 288"  "1 2 320"  "1 2 333"  "1 2 384"  "1 2 400"
 "1 2 448"  "1 2 500"  "1 2 512"  "1 2 513"  "1 2 576"
 "1 4 640"  "1 4 700"  "1 4 768"  "1 4 777"  "1 4 896"
 "1 4 1000" "1 4 1024" "1 4 1025" "1 4 1152" "1 4 1280"
 "1 8 256"  "1 8 384"  "1 8 512"  "1 8 640"  "1 8 768"
 "2 1 256"  "2 1 384"  "2 2 256"  "2 2 333"  "2 2 512"
 "2 4 256"  "2 4 384"  "2 4 500"  "1 2 1333" "1 2 1500"
 "1 1 1536" "1 1 2000" "1 1 2048" "1 4 1333" "1 2 1600"
)

pass=0; fail=0; i=0
printf "%-3s %-12s %-9s %-9s %-9s %-22s %s\n" "#" "shape(BxHxS)" "max_abs" "rel_rmse" "wrowrel" "row|O|ratio(mean/std)" "status"
for s in "${SHAPES[@]}"; do
  read B H S <<< "$s"; i=$((i+1)); pfx="$OUT/s${i}"
  "$BIN" --b=$B --h=$H --s=$S --verify=0 --dump=$pfx >/dev/null 2>&1
  if [ ! -f "$pfx.o" ]; then printf "%-3s %-12s  DUMP FAILED\n" "$i" "${B}x${H}x${S}"; fail=$((fail+1)); continue; fi
  # python prints one [ref] line; capture + classify
  line=$(python3 "$REF" "$pfx" 2>&1 | grep "^\[ref\]")
  ma=$(echo "$line"   | sed -nE 's/.*max_abs=([0-9.eE+-]+).*/\1/p')
  rr=$(echo "$line"   | sed -nE 's/.*rel_rmse=([0-9.eE+-]+).*/\1/p')
  wr=$(echo "$line"   | sed -nE 's/.*worst_row_rel=([0-9.eE+-]+).*/\1/p')
  rm=$(echo "$line"   | sed -nE 's/.*ratio mean=([0-9.eE+-]+).*/\1/p')
  rs=$(echo "$line"   | sed -nE 's/.*mean=[0-9.eE+-]+ std=([0-9.eE+-]+).*/\1/p')
  # PASS: per-row |O| ratio ~1 (no systematic/per-row bug) AND modest rel_rmse.
  ok=$(python3 - "$rr" "$rm" "$rs" <<'PY'
import sys
rr,rm,rs=(float(x) for x in sys.argv[1:4])
print("PASS" if (abs(rm-1.0)<=0.03 and rs<=0.05 and rr<=0.10) else "FAIL")
PY
)
  if [ "$ok" = PASS ]; then pass=$((pass+1)); else fail=$((fail+1)); fi
  printf "%-3s %-12s %-9s %-9s %-9s %-22s %s\n" "$i" "${B}x${H}x${S}" "$ma" "$rr" "$wr" "${rm}/${rs}" "$ok"
done
echo "------------------------------------------------------------"
echo "TOTAL: $pass PASS / $fail FAIL  of $((pass+fail))"
