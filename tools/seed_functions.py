#!/usr/bin/env python3
"""T-009a: find function starts Ghidra's recursive descent did not reach.

Evidence classes (strongest first), written to re/seeds.csv as addr,reason,refs:
  data-pointer            a dword in .rdata/.data points at it (callback/vtable/handler table)
  aligned-after-padding   16-byte aligned, preceded by 0xCC padding or a ret, in uncovered bytes
Every seed must also decode cleanly (capstone) up to a ret/jmp with no privileged or
obviously-bogus opcode. Seeds are fed to Ghidra by tools/ghidra/SeedFunctions.java.

Run:  .venv/bin/python tools/seed_functions.py      (needs re/ghidra_export/blocks.csv)
Iterate with `scripts/ghidra_analyze.sh --seed` until "0 new seeds"; re/seeds.csv accumulates.
"""
import csv
import struct
import sys
from collections import Counter, defaultdict
from pathlib import Path

import capstone
import pefile

ROOT = Path(__file__).resolve().parent.parent
EXE = ROOT / "reference" / "Call of Duty Black Ops" / "BlackOps.exe"
EXP = ROOT / "re" / "ghidra_export"
OUT = ROOT / "re" / "seeds.csv"
BOGUS = {"in", "out", "insb", "insd", "outsb", "outsd", "hlt", "cli", "sti", "iretd", "retf", "lcall",
         "ljmp", "int1", "into", "salc", "aaa", "aas", "aam", "aad", "daa", "das", "arpl", "bound",
         "les", "lds", "int", "syscall", "sysenter", "rdmsr", "wrmsr", "xlatb", "enter", "loopne",
         "loope", "pushal", "popal", "lahf", "wait"}
PRE_OK = {0xCC, 0xC3, 0x90}


def main() -> int:
    raw = EXE.read_bytes()
    pe = pefile.PE(data=raw, fast_load=True)
    base = pe.OPTIONAL_HEADER.ImageBase
    secs = {s.Name.rstrip(b"\0").decode(): s for s in pe.sections}
    text = secs[".text"]
    code = text.get_data()[: text.Misc_VirtualSize]
    va0, size = base + text.VirtualAddress, len(code)
    va1 = va0 + size

    covered = bytearray(size)
    with (EXP / "blocks.csv").open() as f:
        for row in csv.DictReader(f):
            s, e = int(row["start"], 16), int(row["end"], 16)
            if va0 <= s and e < va1:
                covered[s - va0: e - va0 + 1] = b"\x01" * (e - s + 1)
    with (EXP / "functions.csv").open() as f:
        known = {int(r["addr"], 16) for r in csv.DictReader(f)}

    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)

    def decodes(va: int) -> bool:
        off, n = va - va0, 0
        for addr, sz, mn, ops in md.disasm_lite(code[off: off + 4096], va):
            core = mn.split()[-1]
            if core in BOGUS or (core == "int3" and n == 0):
                return False
            n += 1
            if core in ("ret", "retn"):
                return n >= 1
            if core == "jmp":
                return n >= 1
            if core == "int3":
                return n >= 3          # fell into padding after a noreturn call
            if n >= 400:
                return True
        return False

    def prologue_ok(va: int) -> bool:
        off = va - va0
        if off == 0:
            return True
        if code[off - 1] in PRE_OK:
            return True
        return off >= 3 and code[off - 3] == 0xC2      # ret imm16

    refs = defaultdict(int)
    for name in (".rdata", ".data"):
        s = secs[name]
        data = s.get_data()[: min(s.SizeOfRawData, s.Misc_VirtualSize)]
        for (v,) in struct.iter_unpack("<I", data[: len(data) & ~3]):
            if va0 <= v < va1:
                refs[v] += 1

    seeds = {}
    stats = Counter()
    for v, n in refs.items():
        off = v - va0
        if v in known:
            stats["data ptr -> known function"] += 1
            continue
        if covered[off]:
            stats["data ptr -> inside covered code (jump-table case label etc.)"] += 1
            continue
        if not prologue_ok(v):
            stats["data ptr -> uncovered, no padding/ret before"] += 1
            continue
        if not decodes(v):
            stats["data ptr -> uncovered, does not decode"] += 1
            continue
        seeds[v] = ("data-pointer", n)
        stats["seed: data-pointer" + ("" if v % 16 == 0 else " (unaligned)")] += 1

    # aligned-after-padding: first uncovered byte after a 0xCC run or a ret, on a 16-byte boundary
    for off in range(16, size, 16):
        v = va0 + off
        if covered[off] or v in known or v in seeds:
            continue
        if code[off] in (0xCC, 0x00) or code[off - 1] not in (0xCC, 0xC3):
            continue
        if not decodes(v):
            stats["aligned candidate, does not decode"] += 1
            continue
        seeds[v] = ("aligned-after-padding", 0)
        stats["seed: aligned-after-padding"] += 1

    # re/seeds.csv is cumulative: seeds accepted in earlier rounds are functions now (so they
    # are "known" above) and must stay in the file for a from-scratch rebuild.
    new = len(seeds)
    if OUT.exists():
        with OUT.open() as f:
            for r in csv.DictReader(f):
                seeds.setdefault(int(r["addr"], 16), (r["reason"], int(r["refs"])))
    with OUT.open("w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["addr", "reason", "refs"])
        for v in sorted(seeds):
            w.writerow([f"0x{v:08x}", seeds[v][0], seeds[v][1]])
    print(f"{new} new seeds this round")
    for k, n in sorted(stats.items()):
        print(f"{n:7d}  {k}")
    print(f"{len(seeds)} seeds -> {OUT.relative_to(ROOT)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
