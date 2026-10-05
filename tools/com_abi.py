#!/usr/bin/env python3
"""Vtable layout and stack-argument size of the Direct3D 9 COM interfaces (T-045 input).

  .venv/bin/python tools/com_abi.py            # -> re/d3d9_abi.csv

Source: the interface declarations in the mingw-w64 d3d9.h (the header the d3d9 proxy is built
against). `words` = 32-bit stack slots including `this`; every D3D9 method parameter is one slot
(pointers, DWORDs, floats, enums). Cross-checks: slot numbers against tools/d3d9_proxy/d3d9_methods.h
(which the proxy static_asserts against the SDK vtable structs).
"""
import csv
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
HEADERS = sorted(Path("/opt/homebrew/Cellar/mingw-w64").glob("*/toolchain-i686/i686-w64-mingw32/include/d3d9.h"))


def parse(text):
    """-> {interface: (parent, [(name, words)])} for the C++ (DECLARE_INTERFACE) half of the header."""
    out = {}
    for m in re.finditer(r"DECLARE_INTERFACE_IID_\((\w+),\s*(\w+),[^)]*\)\s*\{(.*?)\n\};", text, re.S):
        name, parent, body = m.group(1), m.group(2), m.group(3)
        if name in out:
            continue
        methods = []
        for mm in re.finditer(r"STDMETHOD_?\(([^)]*)\)\s*\((.*?)\)\s*PURE;", body, re.S):
            mname = mm.group(1).split(",")[-1].strip()
            params = " ".join(mm.group(2).split())
            if params == "THIS":
                n = 0
            else:
                assert params.startswith("THIS_"), params
                n = params[5:].count(",") + 1
            methods.append((mname, 1 + n))
        out[name] = (parent, methods)
    return out


def main() -> int:
    if not HEADERS:
        print("mingw-w64 d3d9.h not found")
        return 1
    ifaces = parse(HEADERS[-1].read_text(errors="replace"))
    # slot lists the proxy already verified against the SDK
    proxy = {}
    txt = (ROOT / "tools" / "d3d9_proxy" / "d3d9_methods.h").read_text()
    shared = {m.group(1): m.group(2) for m in re.finditer(r"#define (\w+_METHODS)\(X\)((?:.*\\\n)*.*)\n", txt)}
    for m in re.finditer(r"#define (\w+)_METHODS\(X\)(.*?)\n#define \w+_COUNT (\d+)", txt, re.S):
        body = m.group(2)
        for _ in range(4):                               # helper macros such as BASETEXTURE_METHODS(X) nest
            for k, v in shared.items():
                body = body.replace(k + "(X)", v)
        proxy[m.group(1)] = (re.findall(r"X\((\d+), (\w+)\)", body), int(m.group(3)))
    rows, bad = [], 0
    for name, (parent, methods) in ifaces.items():
        # the header repeats inherited methods inside each interface, so the list is the full vtable
        key = name.upper()
        for slot, (mname, words) in enumerate(methods):
            check = ""
            if key in proxy:
                p = dict((int(s), n) for s, n in proxy[key][0])
                check = "ok" if p.get(slot) == mname else f"MISMATCH (proxy has {p.get(slot)})"
                bad += check != "ok"
            rows.append({"interface": name, "slot": slot, "method": mname, "words": words, "proxy_slot_check": check})
        if key in proxy and proxy[key][1] != len(methods):
            print(f"{name}: {len(methods)} methods here, {proxy[key][1]} in the proxy table")
            bad += 1
    with (ROOT / "re" / "d3d9_abi.csv").open("w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=list(rows[0]))
        w.writeheader()
        w.writerows(rows)
    print(f"{len(ifaces)} interfaces, {len(rows)} methods, {sum(1 for r in rows if r['proxy_slot_check'] == 'ok')} slot checks ok, {bad} problems")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
