#!/usr/bin/env python3
"""T-006 follow-up: how much of .text did Ghidra's recursive descent NOT reach, and what is it?

Input:  re/ghidra_export/blocks.csv (start,end,func,flow), jumptables.csv, the exe's .text bytes
Output: re/coverage_gaps.json (+ summary on stdout)

Every byte of .text is put in exactly one class:
  code_in_function   inside a Ghidra basic block that belongs to a function
  code_no_function   inside a basic block with no containing function
  padding            0xCC / 0x90 / 0x00 filler between covered ranges
  pointer_table      gap made of dwords pointing into .text (jump tables / vtables in .text)
  likely_code        gap that follows padding or a block end, decodes cleanly to a ret/jmp
                     (candidate functions only reachable through pointers -> lifter must seed them)
  unknown            anything else (inline data, constants, undecoded)
Run:  .venv/bin/python tools/coverage_gaps.py
"""
import csv
import json
import struct
import sys
from pathlib import Path

import capstone
import pefile

ROOT = Path(__file__).resolve().parent.parent
EXE = ROOT / "reference" / "Call of Duty Black Ops" / "BlackOps.exe"
EXP = ROOT / "re" / "ghidra_export"
OUT = ROOT / "re" / "coverage_gaps.json"
PAD = {0xCC, 0x90, 0x00}


def main() -> int:
    pe = pefile.PE(str(EXE), fast_load=True)
    base = pe.OPTIONAL_HEADER.ImageBase
    text = next(s for s in pe.sections if s.Name.rstrip(b"\0") == b".text")
    code = text.get_data()[: text.Misc_VirtualSize]
    va0, size = base + text.VirtualAddress, len(code)
    va1 = va0 + size

    cls = bytearray(size)  # 0 = gap, 1 = code in function, 2 = code without function
    with (EXP / "blocks.csv").open() as f:
        for row in csv.DictReader(f):
            s, e = int(row["start"], 16), int(row["end"], 16)
            if s < va0 or e >= va1:
                continue
            mark = 1 if row["func"] else 2
            cls[s - va0: e - va0 + 1] = bytes([mark]) * (e - s + 1)

    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
    counts = {"code_in_function": cls.count(1), "code_no_function": cls.count(2), "padding": 0,
              "pointer_table": 0, "likely_code": 0, "unknown": 0}
    likely, unknown = [], []
    i = 0
    while i < size:
        if cls[i]:
            i += 1
            continue
        j = i
        while j < size and not cls[j]:
            j += 1
        gap = code[i:j]
        k = 0
        while k < len(gap):
            b = gap[k]
            if b in PAD:
                m = k
                while m < len(gap) and gap[m] in PAD:
                    m += 1
                # a lone 0x00/0x90 inside real bytes is not padding; require run or boundary
                counts["padding"] += m - k
                k = m
                continue
            # pointer table: run of dwords that all point into .text
            m = k
            while m + 4 <= len(gap) and va0 <= struct.unpack_from("<I", gap, m)[0] < va1:
                m += 4
            if m - k >= 8:
                counts["pointer_table"] += m - k
                k = m
                continue
            # try to decode a clean instruction run that ends in ret/jmp
            end, n, ok = k, 0, False
            for addr, sz, mn, ops in md.disasm_lite(gap[k:], va0 + i + k):
                end = addr - (va0 + i) + sz
                n += 1
                if mn in ("int3",):
                    end -= sz
                    break
                if mn in ("ret", "retn", "jmp", "retf"):
                    ok = mn != "retf"
                    break
            if ok and n >= 2 and end > k:
                counts["likely_code"] += end - k
                likely.append((va0 + i + k, end - k))
                k = end
            else:
                # give up on this stretch up to the next padding byte run
                m = k + 1
                while m < len(gap) and not (gap[m] in PAD and m + 1 < len(gap) and gap[m + 1] in PAD):
                    m += 1
                counts["unknown"] += m - k
                unknown.append((va0 + i + k, m - k))
                k = m
        i = j

    # merge adjacent likely-code runs into candidate regions
    regions = []
    for va, n in likely:
        if regions and regions[-1][0] + regions[-1][1] == va:
            regions[-1][1] += n
        else:
            regions.append([va, n])
    result = {
        "text_va": f"0x{va0:08x}", "text_size": size,
        "bytes": counts,
        "share": {k: round(v / size, 4) for k, v in counts.items()},
        "likely_code_regions": len(regions),
        "likely_code_largest": [[f"0x{a:08x}", n] for a, n in sorted(regions, key=lambda r: -r[1])[:25]],
        "unknown_largest": [[f"0x{a:08x}", n] for a, n in sorted(unknown, key=lambda r: -r[1])[:25]],
        "likely_code_region_starts": [f"0x{a:08x}" for a, n in regions if n >= 8],
    }
    OUT.write_text(json.dumps(result, indent=1) + "\n")
    brief = dict(result)
    brief["likely_code_region_starts"] = f"{len(result['likely_code_region_starts'])} entries (see file)"
    print(json.dumps(brief, indent=1))
    return 0


if __name__ == "__main__":
    sys.exit(main())
