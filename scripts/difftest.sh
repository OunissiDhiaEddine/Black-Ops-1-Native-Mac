#!/usr/bin/env bash
# P4 differential test: lift -> generate vectors (Unicorn) -> build lifted code + runner -> compare.
#   scripts/difftest.sh [max_funcs] [per_func] [seed]
# Outputs under artifacts/lifted/ (generated, never committed); summary in re/difftest_report.json.
set -euo pipefail
cd "$(dirname "$0")/.."
MAXF="${1:-400}"; PER="${2:-3}"; SEED="${3:-1}"
OUT=artifacts/lifted
EXE="reference/Call of Duty Black Ops/BlackOps.exe"
.venv/bin/python tools/lifter/lift.py --out "$OUT" --stats re/lift_coverage.json | tee "$OUT/lift.log" | head -3
# SKIP_VECTORS=1 reuses the existing vectors (they depend only on the original binary and the function selection)
if [ "${SKIP_VECTORS:-0}" != 1 ]; then
  .venv/bin/python tests/diff/gen_vectors.py --out "$OUT/vectors.bin" --max-funcs "$MAXF" --per-func "$PER" --seed "$SEED" | tee "$OUT/vectors.log"
fi
rm -rf "$OUT/obj"; mkdir -p "$OUT/obj"
CFLAGS="-std=c11 -O1 -g -Wall -Werror -Wno-unused-label -Wno-unused-function -ffp-contract=off -Isrc/runtime -I$OUT"
ls "$OUT"/lifted_*.c | xargs -P 6 -I{} sh -c 'clang '"$CFLAGS"' -c "$1" -o "'"$OUT"'/obj/$(basename "$1" .c).o"' _ {}
clang $CFLAGS -c src/runtime/runtime.c -o "$OUT/obj/runtime.o"
clang $CFLAGS -c tests/diff/difftest.c -o "$OUT/obj/difftest.o"
clang "$OUT"/obj/*.o -o "$OUT/difftest"
set +e
"$OUT/difftest" "$EXE" "$OUT/vectors.bin" --verbose > "$OUT/difftest.log" 2>&1
RC=$?
set -e
tail -1 "$OUT/difftest.log"
.venv/bin/python - "$OUT" "$MAXF" "$PER" "$SEED" <<'PY'
import json, re, sys
out = sys.argv[1]
line = open(f"{out}/difftest.log").read().strip().splitlines()[-1]
m = re.search(r"vectors (\d+): pass (\d+), approx (\d+), fail (\d+) \(of which trapped (\d+)\); functions with a failure: (\d+)", line)
cov = json.load(open("re/lift_coverage.json"))
v = re.search(r"functions tried (\d+), with at least one vector (\d+); vectors \d+; instructions executed (\d+) of (\d+)", open(f"{out}/vectors.log").read())
rep = {"functions_total": cov["functions"], "functions_lifted": cov["lifted"], "instructions_lifted": cov["instructions_lifted"],
       "leaf_functions_lifted": cov["leaf_lifted"], "functions_tried": int(v[1]), "functions_with_vectors": int(v[2]), "instructions_executed": int(v[3]),
       "instructions_in_tried_functions": int(v[4]), "vectors_per_function": int(sys.argv[3]),
       "seed": int(sys.argv[4]), "vectors": int(m[1]), "pass": int(m[2]), "pass_x87_within_1e-12": int(m[3]), "fail": int(m[4]),
       "trapped": int(m[5]), "functions_with_failure": int(m[6])}
json.dump(rep, open("re/difftest_report.json", "w"), indent=1)
PY
exit $RC
