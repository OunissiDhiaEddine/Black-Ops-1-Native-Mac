#!/usr/bin/env bash
# Find where the lifted code and the reference diverge on one recorded vector.
#   tests/diff/debug_vector.sh <func_va> [nth=0]
# Lifts the function (and what it calls) with per-instruction tracing, runs the vector through it and
# through Unicorn, and prints the first differing trace lines side by side.
set -euo pipefail
cd "$(dirname "$0")/../.."
FN="$1"; NTH="${2:-0}"
DBG=artifacts/lifted_dbg
EXE="reference/Call of Duty Black Ops/BlackOps.exe"
mkdir -p "$DBG"
.venv/bin/python tools/lifter/lift.py --out "$DBG" --only "$FN" --closure --trace --no-native > /dev/null
CFLAGS="-std=c11 -O1 -g -DLIFT_TRACE -Wno-unused-label -Wno-unused-function -ffp-contract=off -Isrc/runtime -I$DBG"
clang $CFLAGS "$DBG"/lifted_*.c src/runtime/runtime.c tests/diff/difftest.c -o "$DBG/difftest"
"$DBG/difftest" "$EXE" --verbose --func "${FN#0x}" --nth "$NTH" artifacts/lifted/vectors_*.bin > "$DBG/lifted.trace" || true
.venv/bin/python tests/diff/trace_vector.py "$FN" --nth "$NTH" --plain > "$DBG/ref.trace"
.venv/bin/python - "$DBG" <<'PY'
import sys
d = sys.argv[1]
ref = [l.rstrip() for l in open(f"{d}/ref.trace")]
got = [l.rstrip() for l in open(f"{d}/lifted.trace") if l[:8].strip() and all(c in "0123456789abcdef" for c in l[:8])]
tail = [l.rstrip() for l in open(f"{d}/lifted.trace") if l.startswith(("FAIL", "NANDIFF", "vectors"))]
print(f"reference: {len(ref)} instructions, lifted: {len(got)}")
for k, (a, b) in enumerate(zip(ref, got)):
    if a != b:
        print(f"first difference at step {k}:")
        for j in range(max(0, k - 6), k):
            print("   both   ", ref[j])
        print("   REF    ", a)
        print("   LIFTED ", b)
        for j in range(k + 1, min(len(ref), len(got), k + 4)):
            print("   ref    ", ref[j], "\n   lifted ", got[j])
        break
else:
    print("traces agree on their common prefix" + ("" if len(ref) == len(got) else " (different lengths)"))
print("\n".join(tail))
PY
