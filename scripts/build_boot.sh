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

# D3D9 vtable layout (re/d3d9_abi.csv) and the adapter answers recorded from the baseline (re/d3d9_adapter_probe.json)
.venv/bin/python - > "$OUT/gen/d3d9_abi.inc" <<'PY'
import csv
for r in csv.DictReader(open("re/d3d9_abi.csv")):
    print(f'    {{"{r["interface"]}", {r["slot"]}, "{r["method"]}", {r["words"]}}},')
PY

# Steam C++ interface methods the game calls (re/steam_abi.csv: slot + stack words read off the game's call sites)
.venv/bin/python - > "$OUT/gen/steam_abi.inc" <<'PY'
import csv
for r in csv.DictReader(open("re/steam_abi.csv")):
    print(f'    {{"{r["interface"]}", {r["slot"]}, "{r["method"]}", {r["words"]}}},')
PY
.venv/bin/python - > "$OUT/gen/d3d9_probe.inc" <<'PY'
import json
d = json.load(open("re/d3d9_adapter_probe.json"))
def arr(name, hexstr):
    b = bytes.fromhex(hexstr)
    print(f"static const uint8_t {name}[{len(b)}] = {{" + ",".join(str(x) for x in b) + "};")
arr("kProbeCaps", d["caps_hex"])
arr("kProbeIdentifier", d["identifier_hex"])
print("static const ProbeCheck kProbeChecks[] = {")
for c in d["checks"]:
    a = (c["args"] + [0] * 6)[:6]
    print(f'    {{"{c["method"]}", {{{", ".join(str(x) + "u" for x in a)}}}, {c["hr"]}u, {c.get("quality_levels", 0)}u}},')
print("};\nstatic const ProbeMode kProbeModes[] = {")
for fmt, modes in d["modes"].items():
    for idx, (w, h, hz, f) in sorted(modes.items(), key=lambda kv: int(kv[0])):
        print(f"    {{{fmt}, {idx}, {w}, {h}, {hz}, {f}}},")
m = d["display_mode"]
print(f'}};\nstatic const uint32_t kProbeDisplayMode[4] = {{{m["width"]}, {m["height"]}, {m["refresh"]}, {m["format"]}}};')
print(f'static const uint32_t kProbeAdapterCount = {d["adapter_count"]};')
PY

# libunicorn (x86 interpreter for the emulated steam_api.dll, ADR-010) comes with the Python package in the venv
UNICORN="$(ls -d .venv/lib/python3*/site-packages/unicorn | head -1)"
CXXFLAGS="-std=c++20 -O1 -g -Wall -Wextra -Werror -Wno-unused-parameter -ffp-contract=off -Isrc/runtime -Isrc/platform/win32 -Isrc/rhi -Itools/d3d9_proxy -I$LIFT -I$OUT/gen -I$UNICORN/include"
CFLAGS="-std=c11 -O1 -g -Wall -Werror -ffp-contract=off -Isrc/runtime -I$LIFT"
pids=()
for c in src/runtime/guest_os.cpp src/platform/win32/*.cpp tests/boot/main.cpp tests/boot/steam_selftest.cpp; do
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
# the Metal renderer (ADR-004) comes from the CMake build: the game's live Direct3D calls go to it (d3d9_live.cpp)
bash scripts/build.sh bo1_metal bo1_shaders >/dev/null
clang++ "$OUT"/obj/*.o "$LIFT"/obj/lifted_*.o build/libbo1_metal.a build/libbo1_shaders.a "$UNICORN/lib/libunicorn.2.dylib" \
  /opt/homebrew/lib/libSDL3.dylib -framework Metal -framework Foundation -framework CoreGraphics -framework ImageIO -framework QuartzCore \
  -Wl,-rpath,"$PWD/$UNICORN/lib" -o "$OUT/bo1_boot"
echo "built $OUT/bo1_boot"
