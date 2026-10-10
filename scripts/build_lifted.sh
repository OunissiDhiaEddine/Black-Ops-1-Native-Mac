#!/usr/bin/env bash
# Lift the game image and compile what changed (the lifting half of scripts/difftest.sh, without the tests).
#   scripts/build_lifted.sh [--a2-check]    env: JOBS=2 (parallel compiles; each needs ~1 GB)
# Generated C goes to artifacts/lifted/, objects to artifacts/lifted/obj/. Objects are written under a
# temporary name and renamed, so a build of another thread that links them at the same time never sees
# a half-written file. Afterwards: scripts/build_boot.sh.
set -euo pipefail
cd "$(dirname "$0")/.."
OUT=artifacts/lifted
JOBS="${JOBS:-2}"
if [ "${1:-}" = "--a2-check" ]; then
  # audit build of assumption A2 (docs/LIFTER.md): a second tree whose every `ret` checks its return address.
  # Link it with BO1_LIFT=artifacts/lifted_a2 scripts/build_boot.sh -> artifacts/boot/bo1_boot_a2
  OUT=artifacts/lifted_a2
  mkdir -p "$OUT/obj"
  .venv/bin/python tools/lifter/lift.py --a2-check --out "$OUT" | head -1
else
  mkdir -p "$OUT/obj"
  .venv/bin/python tools/lifter/lift.py --out "$OUT" --stats re/lift_coverage.json | head -1
fi
# -O2 (T-064): the lifted SIMD loops run 3.3x faster than at -O1, compile time is the same; the differential
# tests run on exactly these objects. A change of the flags recompiles everything (stamp file).
CFLAGS="-std=c11 -O2 -g -Wall -Werror -Wno-unused-label -Wno-unused-function -ffp-contract=off -Isrc/runtime -I$OUT"
[ "$(cat "$OUT/obj/.cflags" 2>/dev/null)" = "$CFLAGS" ] || { rm -f "$OUT"/obj/*.o; echo "$CFLAGS" > "$OUT/obj/.cflags"; }
todo=()
for c in "$OUT"/lifted_*.c; do
  o="$OUT/obj/$(basename "$c" .c).o"
  if [ ! -f "$o" ] || [ "$c" -nt "$o" ] || [ src/runtime/lifted.h -nt "$o" ] || [ src/runtime/lifted_fp.h -nt "$o" ] \
     || [ src/runtime/runtime.h -nt "$o" ] || [ "$OUT/lifted_protos.h" -nt "$o" ] || [ "$OUT/lifted_jmp.h" -nt "$o" -a -n "$(grep -l lifted_jmp.h "$c")" ]; then
    todo+=("$c")
  fi
done
echo "compiling ${#todo[@]} file(s)"
if [ ${#todo[@]} -gt 0 ]; then
  printf '%s\n' "${todo[@]}" | xargs -P "$JOBS" -I{} sh -c \
    'o="'"$OUT"'/obj/$(basename "$1" .c).o"; clang '"$CFLAGS"' -c "$1" -o "$o.tmp.$$" && mv "$o.tmp.$$" "$o"' _ {}
fi
for o in "$OUT"/obj/lifted_*.o; do [ -f "$OUT/$(basename "$o" .o).c" ] || rm -f "$o"; done
echo "lifted objects up to date"
