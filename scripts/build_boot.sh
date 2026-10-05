#!/usr/bin/env bash
# Build the headless boot runtime (T-045): lifted image + runtime + Win32 shims -> artifacts/boot/bo1_boot.
#   scripts/build_boot.sh            (the lifted objects come from scripts/difftest.sh / artifacts/lifted/obj)
set -euo pipefail
cd "$(dirname "$0")/.."
OUT=artifacts/boot
LIFT=artifacts/lifted
mkdir -p "$OUT/obj" "$OUT/gen"
ls "$LIFT"/obj/lifted_*.o > /dev/null 2>&1 || { echo "no lifted objects: run scripts/difftest.sh first"; exit 1; }

# import ABI table (re/import_abi.csv -> C initialisers); nargs = stack slots, -1 = cdecl
.venv/bin/python - > "$OUT/gen/import_abi.inc" <<'PY'
import csv
for r in csv.DictReader(open("re/import_abi.csv")):
    n = -1 if r["convention"] == "cdecl" else int(r["arg_bytes"]) // 4
    print(f'    {{{r["iat_va"]}u, "{r["dll"]}", "{r["function"]}", {n}}},')
PY

CXXFLAGS="-std=c++20 -O1 -g -Wall -Wextra -Werror -Wno-unused-parameter -ffp-contract=off -Isrc/runtime -Isrc/platform/win32 -I$LIFT -I$OUT/gen"
CFLAGS="-std=c11 -O1 -g -Wall -Werror -ffp-contract=off -Isrc/runtime -I$LIFT"
pids=()
for c in src/runtime/guest_os.cpp src/platform/win32/*.cpp tests/boot/main.cpp; do
  o="$OUT/obj/$(basename "$c" .cpp).o"
  if [ ! -f "$o" ] || [ "$c" -nt "$o" ] || [ -n "$(find src/runtime src/platform/win32 -name '*.h' -newer "$o" | head -1)" ] \
     || [ "$OUT/gen/import_abi.inc" -nt "$o" -a "$c" = src/runtime/guest_os.cpp ]; then
    clang++ $CXXFLAGS -c "$c" -o "$o" &
    pids+=($!)
  fi
done
o="$OUT/obj/runtime.o"
if [ ! -f "$o" ] || [ src/runtime/runtime.c -nt "$o" ] || [ src/runtime/lifted.h -nt "$o" ]; then
  clang $CFLAGS -c src/runtime/runtime.c -o "$o" &
  pids+=($!)
fi
for p in ${pids[@]+"${pids[@]}"}; do wait "$p"; done
clang++ "$OUT"/obj/*.o "$LIFT"/obj/lifted_*.o -o "$OUT/bo1_boot"
echo "built $OUT/bo1_boot"
