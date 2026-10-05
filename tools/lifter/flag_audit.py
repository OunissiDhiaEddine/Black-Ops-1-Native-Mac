#!/usr/bin/env python3
"""Audit of lifter assumption A1 (docs/LIFTER.md): status flags are dead across call/ret.

  .venv/bin/python tools/lifter/flag_audit.py --out re/flag_audit.json

Per function, a backward liveness analysis over CF/PF/AF/ZF/SF/OF (capstone's per-instruction
EFLAGS read/write sets) reports
  - entry_live:  flags read on some path from the entry before being written
                 -> the function takes a flag as INPUT from its caller;
  - after_call:  call sites where a flag is read after the call returns before being written
                 -> the caller takes a flag as OUTPUT of the callee.
Lifted functions keep flags in a local variable, so every hit is a place where the generated code
would be wrong unless the lifter passes flags explicitly.
"""
import argparse
import json
import sys
from collections import Counter
from pathlib import Path

import capstone.x86 as X

sys.path.insert(0, str(Path(__file__).resolve().parent))
import lift  # noqa: E402

FLAGS = ["CF", "PF", "AF", "ZF", "SF", "OF"]
BIT = {f: 1 << k for k, f in enumerate(FLAGS)}


def masks():
    use, define = {}, {}
    for f in FLAGS:
        use[getattr(X, f"X86_EFLAGS_TEST_{f}")] = BIT[f]
        for kind in ("MODIFY", "RESET", "SET", "UNDEFINED"):
            v = getattr(X, f"X86_EFLAGS_{kind}_{f}", None)
            if v is not None:
                define[v] = BIT[f]
    return use, define


USE, DEFINE = masks()


WRITES_ALL = {"test", "cmp", "and", "or", "xor", "add", "sub", "neg", "adc", "sbb", "cmpsb", "cmpsw", "cmpsd",
              "scasb", "scasw", "scasd", "xadd", "cmpxchg"}


def use_def(i):
    """(flags read, flags written) by one instruction."""
    m = i.mnemonic.split()[-1]
    if 0xD8 <= i.opcode[0] <= 0xDF:                      # x87: capstone's eflags field is the FPU flag set here
        if m in ("fcomi", "fcomip", "fcompi", "fucomi", "fucomip", "fucompi"):
            return 0, 0x3F
        if m.startswith("fcmov"):
            return {"b": BIT["CF"], "e": BIT["ZF"], "be": BIT["CF"] | BIT["ZF"], "u": BIT["PF"]}[m[5:].lstrip("n")], 0
        return 0, 0
    if m in ("comiss", "ucomiss", "comisd", "ucomisd"):
        return 0, 0x3F
    e = i.eflags
    u = d = 0
    for bit, f in USE.items():
        if e & bit:
            u |= f
    for bit, f in DEFINE.items():
        if e & bit:
            d |= f
    if m in WRITES_ALL:                                  # capstone's table misses some encodings (e.g. test r/m8, r8)
        d = 0x3F
    if m in ("adc", "sbb"):
        u |= BIT["CF"]
    if m == "sahf":
        d |= BIT["CF"] | BIT["PF"] | BIT["AF"] | BIT["ZF"] | BIT["SF"]
    if m == "popfd":
        d = 0x3F
    if m in ("lahf",):
        u |= BIT["CF"] | BIT["PF"] | BIT["AF"] | BIT["ZF"] | BIT["SF"]
    if m == "pushfd":
        u = 0x3F
    return u, d


def names(mask):
    return [f for f in FLAGS if mask & BIT[f]]


def audit(L, fn):
    blocks = sorted(set(L.blocks.get(fn, [])))
    if not blocks:
        return None
    by_start = {}
    for s, e in blocks:
        got = list(L.md.disasm(L.image[s - L.base: e + 1 - L.base], s))
        if not got or got[-1].address + got[-1].size != e + 1:
            return None
        by_start[s] = got
    try:
        L.extend(fn, by_start)
    except lift.Unsupported:
        return None
    succ = {}
    for s, got in by_start.items():
        last = got[-1]
        m = last.mnemonic.split()[-1]
        out = []
        if (m == "jmp" or m == "jecxz" or (m[0] == "j" and m[1:] in lift.CC)) and last.operands[0].type == X.X86_OP_IMM:
            out.append(last.operands[0].imm & 0xFFFFFFFF)
        if last.address in L.tables:
            out.extend(L.tables[last.address])
        if m not in L.TERMINATORS and not m.startswith("ret"):
            out.append(last.address + last.size)
        succ[s] = [t for t in out if t in by_start]
    live_in = {s: 0 for s in by_start}
    changed = True
    while changed:
        changed = False
        for s in sorted(by_start, reverse=True):
            live = 0
            for t in succ[s]:
                live |= live_in[t]
            for i in reversed(by_start[s]):
                u, d = use_def(i)
                if i.mnemonic == "call":
                    d = 0                               # for the entry analysis a call neither reads nor writes flags
                live = (live & ~d) | u
            if live != live_in[s]:
                live_in[s] = live
                changed = True
    entry = live_in.get(fn, 0)
    after_call = []
    for s, got in by_start.items():
        live = 0
        for t in succ[s]:
            live |= live_in[t]
        for i in reversed(got):
            if i.mnemonic == "call" and live:
                tgt = i.operands[0].imm & 0xFFFFFFFF if i.operands[0].type == X.X86_OP_IMM else None
                after_call.append((i.address, tgt, live))
            u, d = use_def(i)
            if i.mnemonic == "call":
                d = 0
            live = (live & ~d) | u
    return entry, after_call


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default=str(lift.ROOT / "re" / "flag_audit.json"))
    args = ap.parse_args()
    L = lift.Lifter()
    entry_live, sites, callee_count = {}, [], Counter()
    analysed = 0
    for fn in sorted(L.funcs):
        r = audit(L, fn)
        if r is None:
            continue
        analysed += 1
        entry, after = r
        if entry:
            entry_live[f"0x{fn:08x}"] = names(entry)
        for addr, tgt, live in after:
            sites.append({"call": f"0x{addr:08x}", "in": f"0x{fn:08x}", "target": f"0x{tgt:08x}" if tgt else "indirect",
                          "flags": names(live)})
            callee_count[f"0x{tgt:08x}" if tgt else "indirect"] += 1
    rep = {"functions_analysed": analysed,
           "functions_reading_flags_at_entry": len(entry_live),
           "call_sites_reading_flags_after_return": len(sites),
           "callees_whose_flags_are_read": dict(callee_count.most_common()),
           "entry_live": entry_live, "after_call": sites}
    Path(args.out).write_text(json.dumps(rep, indent=1) + "\n")
    print(f"functions analysed {analysed}; reading flags at entry: {len(entry_live)}; "
          f"call sites reading flags after return: {len(sites)} (distinct callees {len(callee_count)})")
    for t, c in callee_count.most_common(12):
        print(f"  {c:5d}  {t}  {L.funcs.get(int(t, 16), '') if t != 'indirect' else ''}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
