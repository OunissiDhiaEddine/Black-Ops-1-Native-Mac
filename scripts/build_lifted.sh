#!/usr/bin/env bash
# Lift the game image and compile what changed (the lifting half of scripts/difftest.sh, without the tests).
#   scripts/build_lifted.sh          env: JOBS=2 (parallel compiles; each needs ~1 GB)
# Generated C goes to artifacts/lifted/, objects to artifacts/lifted/obj/. Objects are written under a
# temporary name and renamed, so a build of another thread that links them at the same time never sees
# a half-written file. Afterwards: scripts/build_boot.sh.
set -euo pipefail
cd "$(dirname "$0")/.."
OUT=artifacts/lifted
JOBS="${JOBS:-2}"
mkdir -p "$OUT/obj"
.venv/bin/python tools/lifter/lift.py --out "$OUT" --stats re/lift_coverage.json | head -1
CFLAGS="-std=c11 -O1 -g -Wall -Werror -Wno-unused-label -Wno-unused-function -ffp-contract=off -Isrc/runtime -I$OUT"
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
