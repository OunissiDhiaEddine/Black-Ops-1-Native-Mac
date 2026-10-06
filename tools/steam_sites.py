#!/usr/bin/env python3
"""Steam call sites of the game, for checking the stand-in's interface tables (T-024).

  .venv/bin/python tools/steam_sites.py [--context N] [name ...]   -> listing on stdout

For every call to a steam_api.dll import this prints the surrounding instructions, so the vtable
slot (`call [reg+off]` -> off/4), the stack arguments pushed for it and what the game does with
the result can be read directly. Evidence source for re/steam_abi.csv.
"""
import sys
from pathlib import Path

import capstone
import pefile

ROOT = Path(__file__).resolve().parent.parent
EXE = ROOT / "reference" / "Call of Duty Black Ops" / "BlackOps.exe"


def main() -> int:
    args = sys.argv[1:]
    ctx = 14
    if args[:1] == ["--context"]:
        ctx = int(args[1]); args = args[2:]
    pe = pefile.PE(str(EXE), fast_load=True)
    pe.parse_data_directories(directories=[pefile.DIRECTORY_ENTRY["IMAGE_DIRECTORY_ENTRY_IMPORT"]])
    base = pe.OPTIONAL_HEADER.ImageBase
    iat = {}
    for d in pe.DIRECTORY_ENTRY_IMPORT:
        if d.dll.decode().lower() == "steam_api.dll":
            for i in d.imports:
                iat[i.address] = i.name.decode()
    text = next(s for s in pe.sections if s.Name.startswith(b".text"))
    code = text.get_data()
    tva = base + text.VirtualAddress
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
    # call dword ptr [iat] = FF 15 imm32 ; mov reg, [iat] forms are listed too (A1 / 8B xx)
    sites = []
    for va, name in sorted(iat.items()):
        if args and name not in args:
            continue
        pat = b"\xff\x15" + va.to_bytes(4, "little")
        pos = code.find(pat)
        while pos >= 0:
            sites.append((tva + pos, name))
            pos = code.find(pat, pos + 1)
        ref = va.to_bytes(4, "little")
        pos = code.find(ref)
        while pos >= 0:
            if code[pos - 2:pos] != b"\xff\x15":
                sites.append((tva + pos, name + " (address taken / loaded)"))
            pos = code.find(ref, pos + 1)
    for va, name in sorted(sites):
        print(f"=== {name} at {va:08x}")
        start = va - 40
        # resynchronise: disassemble from a few candidate starts and keep one that hits the site exactly
        best = None
        for s in range(start, start + 16):
            ins = list(md.disasm(code[s - tva:va - tva], s))
            if ins and ins[-1].address + ins[-1].size == va:
                best = ins; break
        for i in (best or [])[-8:]:
            print(f"    {i.address:08x}  {i.mnemonic} {i.op_str}")
        for n, i in enumerate(md.disasm(code[va - tva:va - tva + 200], va)):
            if n > ctx:
                break
            mark = "  <--" if n == 0 else ""
            print(f"  > {i.address:08x}  {i.mnemonic} {i.op_str}{mark}")
            if i.mnemonic in ("ret", "int3"):
                break
    print(f"{len(sites)} sites")
    return 0


if __name__ == "__main__":
    sys.exit(main())
