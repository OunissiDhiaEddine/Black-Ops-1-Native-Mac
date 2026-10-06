#!/usr/bin/env python3
"""The game's Bink code, for reading the BINK / BINKFRAMEBUFFERS fields it touches (T-054, ADR-008).

  .venv/bin/python tools/bink_sites.py [--range LO HI]      -> listing on stdout (do not commit it)
  .venv/bin/python tools/bink_sites.py --sites              -> only the import call sites, one line each

Disassembles the game functions between LO and HI (default 0x006D9000–0x006DA2C0, the block that holds
every binkw32.dll call site; function starts from re/ghidra_export/functions.csv) and names every
import call and every known symbol from re/symbols.csv. Memory operands of the form [reg+disp] are
what the struct offsets in docs/RE_NOTES.md "Bink as the game uses it" were read from.
"""
import csv
import sys
from pathlib import Path

import capstone
import pefile

ROOT = Path(__file__).resolve().parent.parent
EXE = ROOT / "reference" / "Call of Duty Black Ops" / "BlackOps.exe"


def main() -> int:
    args = sys.argv[1:]
    lo, hi = 0x006D9000, 0x006DA2C0
    only_sites = False
    while args:
        if args[0] == "--range":
            lo, hi = int(args[1], 16), int(args[2], 16); args = args[3:]
        elif args[0] == "--sites":
            only_sites = True; args = args[1:]
        else:
            print(__doc__); return 2
    pe = pefile.PE(str(EXE), fast_load=True)
    pe.parse_data_directories(directories=[pefile.DIRECTORY_ENTRY["IMAGE_DIRECTORY_ENTRY_IMPORT"]])
    base = pe.OPTIONAL_HEADER.ImageBase
    iat = {}
    for d in pe.DIRECTORY_ENTRY_IMPORT:
        dll = d.dll.decode().lower().split(".")[0]
        for i in d.imports:
            iat[i.address] = f"{dll}!{i.name.decode() if i.name else '#%d' % i.ordinal}"
    names = {}
    with open(ROOT / "re" / "symbols.csv", newline="") as f:
        for row in csv.reader(f):
            if row and row[0].startswith("0x"):
                names[int(row[0], 16)] = row[1]
    funcs = []
    with open(ROOT / "re" / "ghidra_export" / "functions.csv", newline="") as f:
        for row in csv.DictReader(f):
            a = int(row["addr"], 16)
            if lo <= a < hi:
                funcs.append((a, int(row["size"])))
    text = next(s for s in pe.sections if s.Name.startswith(b".text"))
    code = text.get_data()
    tva = base + text.VirtualAddress
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
    md.detail = True
    for fa, size in sorted(funcs):
        if not only_sites:
            print(f"\n=== f_{fa:08x} {names.get(fa, '')} ({size} bytes)")
        for ins in md.disasm(code[fa - tva:fa - tva + size], fa):
            note = ""
            for op in ins.operands:
                if op.type == capstone.x86.X86_OP_MEM and op.mem.base == 0 and op.mem.index == 0:
                    t = op.mem.disp & 0xFFFFFFFF
                    note = iat.get(t) or names.get(t) or note
                elif op.type == capstone.x86.X86_OP_IMM:
                    t = op.imm & 0xFFFFFFFF
                    note = names.get(t) or note
            if only_sites:
                if "binkw32!" in note:
                    print(f"{ins.address:08x}  f_{fa:08x}  {ins.mnemonic} {ins.op_str}  ; {note}")
                continue
            print(f"{ins.address:08x}  {ins.mnemonic:8s} {ins.op_str}" + (f"   ; {note}" if note else ""))
    return 0


if __name__ == "__main__":
    sys.exit(main())
