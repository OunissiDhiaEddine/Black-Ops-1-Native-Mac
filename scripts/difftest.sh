#!/usr/bin/env bash
# P4 differential test: lift -> generate vectors (Unicorn) -> build lifted code + runner -> compare.
#   scripts/difftest.sh [max_funcs] [per_func] [seed] [leaf|all]
#     leaf (default): functions without calls / indirect transfers / fs:
#     all:            every lifted function; call trees execute on both sides
#   env: JOBS=4 (parallel vector generation / compilation), SKIP_VECTORS=1 (reuse existing vectors),
#        REPORT=re/difftest_report.json (where the summary goes)
# Outputs under artifacts/lifted/ (generated, never committed).
set -euo pipefail
cd "$(dirname "$0")/.."
MAXF="${1:-400}"; PER="${2:-3}"; SEED="${3:-1}"; SELECT="${4:-leaf}"
JOBS="${JOBS:-4}"; REPORT="${REPORT:-re/difftest_report.json}"
OUT=artifacts/lifted
EXE="reference/Call of Duty Black Ops/BlackOps.exe"
mkdir -p "$OUT/obj"
.venv/bin/python tools/lifter/lift.py --out "$OUT" --stats re/lift_coverage.json | tee "$OUT/lift.log" | head -3

if [ "${SKIP_VECTORS:-0}" != 1 ]; then
  rm -f "$OUT"/vectors*.bin "$OUT"/vectors*.bin.json "$OUT"/vectors*.bin.cov
  pids=()
  for ((i = 0; i < JOBS; i++)); do
    .venv/bin/python tests/diff/gen_vectors.py --out "$OUT/vectors_$i.bin" --max-funcs "$MAXF" --per-func "$PER" \
      --seed "$SEED" --select "$SELECT" --shard "$i/$JOBS" > "$OUT/vectors_$i.log" &
    pids+=($!)
  done
  for p in "${pids[@]}"; do wait "$p"; done
  cat "$OUT"/vectors_*.log
fi

# -ffp-contract=off: no fused multiply-add (x86 has none in this code; fusing changes results)
# -O2 (T-064): the lifted SIMD loops run 3.3x faster than at -O1, compile time is the same; the differential
# tests run on exactly these objects. A change of the flags recompiles everything (stamp file).
CFLAGS="-std=c11 -O2 -g -Wall -Werror -Wno-unused-label -Wno-unused-function -ffp-contract=off -Isrc/runtime -I$OUT"
[ "$(cat "$OUT/obj/.cflags" 2>/dev/null)" = "$CFLAGS" ] || { rm -f "$OUT"/obj/*.o; echo "$CFLAGS" > "$OUT/obj/.cflags"; }
todo=()
for c in "$OUT"/lifted_*.c src/runtime/runtime.c tests/diff/difftest.c; do
  o="$OUT/obj/$(basename "$c" .c).o"
  if [ ! -f "$o" ] || [ "$c" -nt "$o" ] || [ src/runtime/lifted.h -nt "$o" ] || [ src/runtime/lifted_fp.h -nt "$o" ] \
     || [ src/runtime/runtime.h -nt "$o" ] || [ "$OUT/lifted_protos.h" -nt "$o" ]; then
    todo+=("$c")
  fi
done
echo "compiling ${#todo[@]} file(s)"
if [ ${#todo[@]} -gt 0 ]; then
  printf '%s\n' "${todo[@]}" | xargs -P "$JOBS" -I{} sh -c 'clang '"$CFLAGS"' -c "$1" -o "'"$OUT"'/obj/$(basename "$1" .c).o"' _ {}
fi
for o in "$OUT"/obj/lifted_*.o; do [ -f "$OUT/$(basename "$o" .o).c" ] || rm -f "$o"; done
# src/native defines the functions it replaces; BO1_NATIVE=off below makes them run their lifted originals,
# which is what the vectors test (the replacements have their own test: scripts/test_native.sh)
for c in src/native/*.c; do
  clang -std=c11 -O2 -g -Wall -Wextra -Werror -ffp-contract=off -Isrc/runtime -c "$c" -o "$OUT/obj/native_$(basename "$c" .c).o"
done
clang "$OUT"/obj/*.o -o "$OUT/difftest"

set +e
BO1_NATIVE=off "$OUT/difftest" "$EXE" --verbose "$OUT"/vectors_*.bin > "$OUT/difftest.log" 2>&1
RC=$?
set -e
tail -1 "$OUT/difftest.log"
.venv/bin/python - "$OUT" "$PER" "$SEED" "$SELECT" "$REPORT" <<'PY'
import glob, json, re, struct, sys
from collections import Counter
out, per, seed, select, report = sys.argv[1:6]
log = open(f"{out}/difftest.log", errors="replace").read().strip().splitlines()
m = re.search(r"vectors (\d+): pass (\d+), approx (\d+), nan-only (\d+), fp-range (\d+), fail (\d+) \(of which trapped (\d+)\); functions with a failure: (\d+)", log[-1])
if not m:
    sys.exit("difftest did not finish: " + (log[-1] if log else "no output"))
cov = json.load(open("re/lift_coverage.json"))
stats, covered, skipped = Counter(), set(), Counter()
for f in glob.glob(f"{out}/vectors_*.bin.json"):
    s = json.load(open(f))
    skipped.update(s.pop("skipped"))
    stats.update(s)
for f in glob.glob(f"{out}/vectors_*.bin.cov"):
    b = open(f, "rb").read()
    covered.update(struct.unpack(f"<{len(b) // 4}I", b))
# functions where the REFERENCE is known to be wrong (tests/diff/reference_deviations.json): reported, not counted as failures
known = {int(k, 16): v for k, v in json.load(open("tests/diff/reference_deviations.json")).items()}
failing = {}
for line in log[:-1]:
    k = re.match(r"FAIL ([0-9a-f]{8}):", line)
    if k:
        failing.setdefault(int(k[1], 16), []).append(line[15:])
unexplained = {f: v for f, v in failing.items() if f not in known}
kinds = Counter()
for line in log[:-1]:
    k = re.match(r"FAIL [0-9a-f]{8}: (trap [a-z ()/]+?|x87 top|st\(\d\)|xmm\d|mm\d|memory|e[a-z]{2})\b", line)
    if k:
        kinds[re.sub(r"\d", "N", k[1])] += 1
rep = {"selection": select, "vectors_per_function": int(per), "seed": int(seed),
       "functions_total": cov["functions"], "functions_lifted": cov["lifted"], "instructions_lifted": cov["instructions_lifted"],
       "functions_tried": stats["functions_tried"], "functions_with_vectors": stats["functions_with_vectors"],
       "instructions_executed": len(covered),
       "instructions_in_tried_functions": stats["insns_in_tried_functions"],
       "vectors_skipped_by_unicorn": dict(skipped.most_common()),
       "vectors": int(m[1]), "pass": int(m[2]), "pass_x87_within_1e-12": int(m[3]), "nan_sign_or_payload_only": int(m[4]),
       "differs_after_double_overflow_or_underflow": int(m[5]),
       "fail": int(m[6]), "trapped": int(m[7]), "functions_with_failure": int(m[8]),
       "functions_failing_reference_known_wrong": {f"0x{f:08x}": known[f] for f in sorted(failing) if f in known},
       "functions_failing_unexplained": {f"0x{f:08x}": v[:4] for f, v in sorted(unexplained.items())},
       "failure_lines_by_kind": dict(kinds.most_common())}
json.dump(rep, open(report, "w"), indent=1)
print(json.dumps({k: rep[k] for k in ("functions_tried", "functions_with_vectors", "instructions_executed", "vectors", "pass",
                                      "pass_x87_within_1e-12", "nan_sign_or_payload_only",
                                      "differs_after_double_overflow_or_underflow", "fail", "trapped")}))
print(f"functions failing: {len(failing)} ({len(failing) - len(unexplained)} with a known-wrong reference, {len(unexplained)} unexplained)")
sys.exit(1 if unexplained else 0)
PY
