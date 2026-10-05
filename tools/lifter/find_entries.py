#!/usr/bin/env python3
"""Find address-taken code that is not a known function entry (T-043/T-045).

  .venv/bin/python tools/lifter/find_entries.py --out re/extra_entries.csv

Indirect calls and jumps go through rt_dispatch(), which only knows function entries. Code whose
address is stored somewhere (a callback passed by address, a label written into a run-time jump
table, a function Ghidra did not recognise) is found here by scanning
  - every 32-bit immediate of every instruction in the known functions, and
  - every aligned dword of the non-executable sections
for values that point into the code section but are not a function entry. A candidate is kept when
it is an instruction boundary of already decoded code, or when undefined bytes there decode cleanly
up to a terminator. The lifter turns each kept address into an additional entry (a function of its
own, decoded by recursive descent; code shared with the enclosing function is duplicated).
"""
import argparse
import csv
import sys
from collections import Counter
from pathlib import Path

import capstone.x86 as X

sys.path.insert(0, str(Path(__file__).resolve().parent))
import lift  # noqa: E402


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default=str(lift.ROOT / "re" / "extra_entries.csv"))
    args = ap.parse_args()
    L = lift.Lifter(extra_entries=False)
    insn_starts, block_starts = {}, set()
    imm_refs = {}                                   # value -> first referencing instruction
    for fn, blocks in L.blocks.items():
        for s, e in set(blocks):
            block_starts.add(s)
            for i in L.md.disasm(L.image[s - L.base: e + 1 - L.base], s):
                insn_starts[i.address] = fn
                if i.mnemonic in ("call", "jmp") or i.mnemonic[0] == "j":
                    continue                        # direct branch targets are handled by the lifter itself
                for o in i.operands:
                    if o.type == X.X86_OP_IMM:
                        v = o.imm & 0xFFFFFFFF
                        if L.is_code(v) and v not in L.funcs:
                            imm_refs.setdefault(v, i.address)
    data_refs = {}
    for sec in L.pe.sections:
        if sec.Characteristics & 0x20000000:
            continue
        lo = L.base + sec.VirtualAddress
        data = L.image[lo - L.base: lo - L.base + min(sec.Misc_VirtualSize, sec.SizeOfRawData)]
        for off in range(0, len(data) - 3, 4):
            v = int.from_bytes(data[off: off + 4], "little")
            if L.is_code(v) and v not in L.funcs:
                data_refs.setdefault(v, lo + off)

    # jump tables embedded in the code section (hand-written CRT routines): aligned dwords in the gaps
    # between decoded instructions that point at an instruction boundary of decoded code
    resolved = {t for ts in L.tables.values() for t in ts}      # case labels of switch tables Ghidra resolved
    covered = bytearray(len(L.image))
    for fn, blocks in L.blocks.items():
        for s, e in set(blocks):
            covered[s - L.base: e + 1 - L.base] = b"\x01" * (e + 1 - s)
    for lo, hi in L.code:
        for a in range((lo + 3) & ~3, hi - 3, 4):
            if covered[a - L.base] or covered[a - L.base + 3]:
                continue
            v = int.from_bytes(L.image[a - L.base: a - L.base + 4], "little")
            if v in insn_starts and v not in L.funcs and v not in resolved:
                data_refs.setdefault(v, a)

    rows, kinds = [], Counter()
    for v in sorted(set(imm_refs) | set(data_refs)):
        src = f"imm@0x{imm_refs[v]:08x}" if v in imm_refs else f"data@0x{data_refs[v]:08x}"
        if v in insn_starts:
            kind = "block" if v in block_starts else "instruction"
            owner = insn_starts[v]
        else:
            # undefined bytes: accept only if they decode to a terminator without running into junk
            owner, a, ok = 0, v, False
            first = L.image[v - L.base]
            if first in (0x00, 0xCC):
                continue
            for _ in range(400):
                i = next(L.md.disasm(L.image[a - L.base: a - L.base + 16], a), None)
                if i is None or i.mnemonic in ("int3", "hlt", "in", "out", "insb", "insd", "outsb", "outsd", "iretd", "ljmp", "lcall", "retf"):
                    break
                a += i.size
                if i.mnemonic == "jmp" or i.mnemonic.startswith("ret"):
                    ok = True
                    break
            if not ok:
                continue
            kind = "undefined"
        # a data word pointing into the middle of a block is most likely a coincidence, unless it sits in the
        # code section itself (a jump table)
        if kind == "instruction" and v not in imm_refs and not L.is_code(data_refs[v]):
            continue
        kinds[kind + (" (imm)" if v in imm_refs else " (data)")] += 1
        rows.append({"addr": f"0x{v:08x}", "kind": kind, "inside_function": f"0x{owner:08x}" if owner else "", "first_reference": src})
    with open(args.out, "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=["addr", "kind", "inside_function", "first_reference"])
        w.writeheader()
        w.writerows(rows)
    print(f"extra entries: {len(rows)}  " + ", ".join(f"{k}: {v}" for k, v in kinds.most_common()))
    return 0


if __name__ == "__main__":
    sys.exit(main())
