#!/usr/bin/env python3
"""P2 evidence: which COM classes the exe instantiates and which Steam interface methods it calls.

  .venv/bin/python tools/com_steam_usage.py      -> re/com_steam_usage.json + summary

Method (static, heuristic — tagged LIKELY in RE_NOTES):
 * CoCreateInstance: the GUIDs pushed (or loaded) in the 12 instructions before each call site.
 * Steam: after each call to an accessor import (SteamUser(), SteamFriends(), ...), the vtable
   offset of the first `call [reg+off]` / `mov r,[r+off] ... call r` within the next 14
   instructions. MSVC calls these C++ interfaces with thiscall (this in ecx).
"""
import json
import re
import struct
import sys
import uuid
from collections import Counter, defaultdict
from pathlib import Path

import capstone
import pefile

ROOT = Path(__file__).resolve().parent.parent
EXE = ROOT / "reference" / "Call of Duty Black Ops" / "BlackOps.exe"
KNOWN = {
    "5a508685-a254-4fba-9b82-9a24b00306af": "CLSID_XAudio2 (2.7)",
    "8bcf1f58-9fe7-4583-8ac6-e2adc465c8bb": "IID_IXAudio2",
    "bcde0395-e52f-467c-8e3d-c4579291692e": "CLSID_MMDeviceEnumerator",
    "a95664d2-9614-4f35-a746-de8db63617e6": "IID_IMMDeviceEnumerator",
    "db05ea35-0329-4d4b-a53a-6dead03d3852": "CLSID_XAudio2_Debug",
    "00000000-0000-0000-c000-000000000046": "IID_IUnknown",
}


def main() -> int:
    raw = EXE.read_bytes()
    pe = pefile.PE(data=raw)
    base = pe.OPTIONAL_HEADER.ImageBase
    img = pe.get_memory_mapped_image()
    text = next(s for s in pe.sections if s.Name.rstrip(b"\0") == b".text")
    code = text.get_data()[: text.Misc_VirtualSize]
    va0 = base + text.VirtualAddress
    slots = {}
    for e in pe.DIRECTORY_ENTRY_IMPORT:
        for i in e.imports:
            if i.name:
                slots[i.address] = (e.dll.decode().lower(), i.name.decode())
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
    insns = []
    off, size = 0, len(code)
    while off < size:
        last = off
        for addr, sz, mn, ops in md.disasm_lite(code[off:], va0 + off):
            insns.append((addr, mn, ops))
            last = addr - va0 + sz
        off = last + 1 if last == off or last < size else last
    thunk = {}
    for addr, mn, ops in insns:
        m = re.fullmatch(r"dword ptr \[(0x[0-9a-f]+)\]", ops)
        if mn == "jmp" and m and int(m.group(1), 16) in slots:
            thunk[addr] = int(m.group(1), 16)

    def callee(mn, ops):
        if mn != "call":
            return None
        m = re.fullmatch(r"dword ptr \[(0x[0-9a-f]+)\]", ops)
        if m and int(m.group(1), 16) in slots:
            return slots[int(m.group(1), 16)]
        if ops.startswith("0x") and int(ops, 16) in thunk:
            return slots[thunk[int(ops, 16)]]
        return None

    def guid_at(va):
        if not (base + 0x5A3000 <= va < base + 0x7A6000):
            return None
        b = bytes(img[va - base: va - base + 16])
        return str(uuid.UUID(bytes_le=b)) if len(b) == 16 else None

    com, steam = [], defaultdict(Counter)
    for i, (addr, mn, ops) in enumerate(insns):
        c = callee(mn, ops)
        if not c:
            continue
        if c[1] == "CoCreateInstance":
            guids = []
            for a2, m2, o2 in insns[max(0, i - 12): i]:
                for h in re.findall(r"0x[0-9a-f]{6,8}", o2):
                    g = guid_at(int(h, 16))
                    if g and (g in KNOWN or g[14] in "1234"):
                        guids.append({"va": h, "guid": g, "name": KNOWN.get(g, "?")})
            com.append({"call_site": f"0x{addr:08x}", "guids": guids})
        elif c[0] == "steam_api.dll" and not c[1].startswith(("SteamAPI_", "SteamGameServer_")):
            found = None
            regs_from_eax = {"eax"}
            vt = {}
            for a2, m2, o2 in insns[i + 1: i + 15]:
                m = re.fullmatch(r"(e\w\w), dword ptr \[(e\w\w)\]", o2) if m2 == "mov" else None
                if m and m.group(2) in regs_from_eax:
                    vt[m.group(1)] = True                      # reg now holds the vtable
                    continue
                m = re.fullmatch(r"(e\w\w), (e\w\w)", o2) if m2 == "mov" else None
                if m and m.group(2) in regs_from_eax:
                    regs_from_eax.add(m.group(1))
                    continue
                m = re.fullmatch(r"dword ptr \[(e\w\w)(?: \+ (0x[0-9a-f]+|\d+))?\]", o2) if m2 in ("call", "jmp") else None
                if m and m.group(1) in vt:
                    found = int(m.group(2), 0) if m.group(2) else 0
                    break
                m = re.fullmatch(r"(e\w\w), dword ptr \[(e\w\w)(?: \+ (0x[0-9a-f]+|\d+))?\]", o2) if m2 == "mov" else None
                if m and m.group(2) in vt:
                    found = int(m.group(3), 0) if m.group(3) else 0
                    break
                if m2 in ("call", "ret"):
                    break
            steam[c[1]][found if found is None else found // 4] += 1
    result = {"cocreateinstance": com,
              "steam_vtable_slots": {k: {("unresolved" if s is None else str(s)): n for s, n in sorted(v.items(), key=lambda kv: (kv[0] is None, kv[0] or 0))}
                                     for k, v in sorted(steam.items())}}
    (ROOT / "re" / "com_steam_usage.json").write_text(json.dumps(result, indent=1) + "\n")
    for c in com:
        print("CoCreateInstance at", c["call_site"], [f"{g['name']} {g['guid']}" for g in c["guids"]])
    for k, v in result["steam_vtable_slots"].items():
        print(f"{k:28s} call sites {sum(v.values()):3d}  vtable slots -> sites: {v}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
