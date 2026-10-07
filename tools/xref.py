#!/usr/bin/env python3
"""References to an address or a string in BlackOps.exe, with the function each one sits in (T-061).

  .venv/bin/python tools/xref.py 0x009abdd0 0x00a2887c     -> every place whose bytes hold that VA
  .venv/bin/python tools/xref.py -s "savegame request"      -> the same for every string that contains the text
  .venv/bin/python tools/xref.py -c 0x0049B930              -> direct calls / jumps (E8 / E9) to that function

Method: byte search for the little-endian VA over all sections (code operands and data tables, e.g. the
command table that pairs a name with its handler), and a rel32 scan of .text for -c. Functions come from
re/ghidra_export/functions.csv, names from re/symbols.csv. A hit inside .text is shown disassembled.
"""
import bisect
import csv
import struct
import sys
from pathlib import Path

import capstone
import pefile

ROOT = Path(__file__).resolve().parent.parent
EXE = ROOT / "reference" / "Call of Duty Black Ops" / "BlackOps.exe"


def main() -> int:
    args = sys.argv[1:]
    if not args:
        print(__doc__)
        return 1
    pe = pefile.PE(str(EXE), fast_load=True)
    base = pe.OPTIONAL_HEADER.ImageBase
    secs = [(s.Name.rstrip(b"\0").decode(), base + s.VirtualAddress, s.get_data()) for s in pe.sections]
    text = next(s for s in secs if s[0] == ".text")
    funcs = sorted(int(r["addr"], 16) for r in csv.DictReader(open(ROOT / "re/ghidra_export/functions.csv")))
    names = {int(r["addr"], 16): r["name"] for r in csv.DictReader(open(ROOT / "re/symbols.csv", newline=""))}
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)

    def func_of(va):
        i = bisect.bisect_right(funcs, va) - 1
        return funcs[i] if i >= 0 else 0

    def where(va):
        f = func_of(va)
        return f"f_{f:08x}" + (f" {names[f]}" if f in names else "")

    def insn_at(va):
        # decode from the function start so the hit lands on an instruction boundary
        f = func_of(va)
        off = f - text[1]
        for ins in md.disasm(text[2][off : off + (va - f) + 16], f):
            if ins.address <= va < ins.address + ins.size:
                return f"{ins.address:08x}  {ins.mnemonic} {ins.op_str}"
        return "(not on an instruction)"

    targets, mode = [], "ref"
    it = iter(args)
    for a in it:
        if a == "-s":
            needle = next(it).encode()
            for name, va, data in secs:
                pos = data.find(needle)
                while pos >= 0:
                    start = data.rfind(b"\0", 0, pos) + 1
                    end = data.find(b"\0", pos)
                    targets.append((va + start, data[start:end].decode("latin-1")))
                    pos = data.find(needle, end)
        elif a == "-c":
            mode = "call"
        else:
            targets.append((int(a, 16), names.get(int(a, 16), "")))

    for va, label in targets:
        print(f"== {va:#010x} {label!r}")
        if mode == "call":
            data, tva = text[2], text[1]
            for op in (0xE8, 0xE9):
                pos = data.find(bytes([op]))
                while pos >= 0 and pos + 5 <= len(data):
                    if tva + pos + 5 + struct.unpack_from("<i", data, pos + 1)[0] == va:
                        print(f"   {'call' if op == 0xE8 else 'jmp '} at {tva + pos:08x} in {where(tva + pos)}")
                    pos = data.find(bytes([op]), pos + 1)
            continue
        pat = struct.pack("<I", va)
        for name, sva, data in secs:
            pos = data.find(pat)
            while pos >= 0:
                at = sva + pos
                if name == ".text":
                    print(f"   .text {insn_at(at)}   in {where(at)}")
                else:
                    around = struct.unpack_from("<4I", data, max(0, pos - 4) & ~3) if pos >= 4 else ()
                    print(f"   {name} {at:08x}  words around: " + " ".join(f"{w:08x}" for w in around))
                pos = data.find(pat, pos + 1)
    return 0


if __name__ == "__main__":
    sys.exit(main())
