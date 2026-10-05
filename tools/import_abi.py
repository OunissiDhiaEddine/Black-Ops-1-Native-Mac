#!/usr/bin/env python3
"""Calling convention and stack-argument size of every import of BlackOps.exe (T-045 input).

  .venv/bin/python tools/import_abi.py            # -> re/import_abi.csv

Source of truth: the decorated stdcall names (`_Name@N`) in the mingw-w64 i686 import libraries
(read with nm), i.e. the same headers/defs the d3d9 proxy is built against — no hand-counted
stacks. binkw32 imports carry the decoration in the import table itself. A symbol that exists only
undecorated is cdecl (caller pops). Anything not found is listed as `unknown` and must be resolved
by hand before its shim is written.
"""
import csv
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
LIBDIRS = sorted(Path("/opt/homebrew/Cellar/mingw-w64").glob("*/toolchain-i686/i686-w64-mingw32/lib"))
NM = "i686-w64-mingw32-nm"
CDECL_DLLS = {"steam_api.dll"}          # Steamworks flat API: __cdecl (S_API), CONFIRMED by the SDK headers
# Imports by ordinal. LIKELY: from the well-known export tables of these system DLLs (dsound.dll,
# xinput1_3.dll); to be CONFIRMED against the DLLs in the Wine prefix when their shims are written.
ORDINALS = {("dsound.dll", 6): "DirectSoundCaptureCreate", ("dsound.dll", 11): "DirectSoundCreate8",
            ("xinput1_3.dll", 2): "XInputGetState", ("xinput1_3.dll", 3): "XInputSetState",
            ("xinput1_3.dll", 4): "XInputGetCapabilities"}


def lib_symbols(dll):
    stem = dll.lower().rsplit(".", 1)[0]
    for d in LIBDIRS:
        lib = d / f"lib{stem}.a"
        if lib.exists():
            out = subprocess.run([NM, str(lib)], capture_output=True, text=True).stdout
            std, plain = {}, set()
            for m in re.finditer(r"^\S*\s+[TI]\s+(?:__imp_)?_?([A-Za-z0-9_]+?)(?:@(\d+))?$", out, re.M):
                if m.group(2) is not None:
                    std[m.group(1)] = int(m.group(2))
                else:
                    plain.add(m.group(1))
            return std, plain
    return None, None


def main() -> int:
    rows = [r for r in csv.DictReader((ROOT / "re" / "imports.csv").open()) if r["file"] == "BlackOps.exe"]
    cache, out, unknown = {}, [], 0
    for r in rows:
        dll, fn = r["dll"], r["function"]
        by_ordinal = not fn
        if by_ordinal:
            fn = ORDINALS.get((dll.lower(), int(r["ordinal"])), "")
        if dll not in cache:
            cache[dll] = lib_symbols(dll)
        std, plain = cache[dll]
        m = re.match(r"^_(\w+)@(\d+)$", fn)
        if m:
            conv, nbytes, src = "stdcall", int(m.group(2)), "decorated import name"
        elif dll.lower() in CDECL_DLLS:
            conv, nbytes, src = "cdecl", "", "Steamworks S_API = cdecl"
        elif std is not None and fn in std:
            conv, nbytes, src = "stdcall", std[fn], "mingw-w64 import library"
        elif plain is not None and fn in plain:
            conv, nbytes, src = "cdecl", "", "mingw-w64 import library (undecorated)"
        else:
            conv, nbytes, src = "unknown", "", ""
            unknown += 1
        if by_ordinal and fn:
            src += f"; ordinal {r['ordinal']} -> name LIKELY"
        out.append({"dll": dll.lower(), "function": fn or f"#{r['ordinal']}", "iat_va": r["iat_va"], "convention": conv,
                    "arg_bytes": nbytes, "source": src})
    with (ROOT / "re" / "import_abi.csv").open("w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=list(out[0]))
        w.writeheader()
        w.writerows(out)
    print(f"imports {len(out)}: stdcall {sum(1 for o in out if o['convention'] == 'stdcall')}, "
          f"cdecl {sum(1 for o in out if o['convention'] == 'cdecl')}, unknown {unknown}")
    for o in out:
        if o["convention"] == "unknown":
            print("  unknown:", o["dll"], o["function"])
    return 0


if __name__ == "__main__":
    sys.exit(main())
