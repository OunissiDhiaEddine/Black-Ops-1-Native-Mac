#!/usr/bin/env bash
# Hand-written replacements (src/native) against the lifted functions they replace (P6).
#   scripts/test_native.sh [--bench]    expected last line: "test_native: <N> cases, 0 failed"
#   --bench adds the time per vertex / per copy of the lifted and the native side (one process, best of 5)
# Needs the lifted objects (scripts/build_lifted.sh). Light: a few seconds.
set -euo pipefail
cd "$(dirname "$0")/.."
LIFT=artifacts/lifted
OUT=artifacts/boot/test_native_obj
mkdir -p "$OUT"
FLAGS="-std=c11 -O2 -g -Wall -Wextra -Werror -ffp-contract=off -Isrc/runtime -Isrc/native"
for c in src/native/*.c tests/native/test_native.c; do
  clang $FLAGS -c "$c" -o "$OUT/$(basename "$c" .c).o"
done
clang -std=c11 -O2 -g -Wall -Werror -ffp-contract=off -Isrc/runtime -I"$LIFT" -c src/runtime/runtime.c -o "$OUT/runtime.o"
clang "$OUT"/*.o "$LIFT"/obj/lifted_*.o -o artifacts/boot/test_native
artifacts/boot/test_native "reference/Call of Duty Black Ops/BlackOps.exe" "$@"
