#!/usr/bin/env python3
"""T-005: float-model + lifter-feasibility census of BlackOps.exe (.text).

Method: capstone LINEAR SWEEP over the whole .text section (resync +1 byte on a decode
failure). Linear sweep mis-decodes inline data (jump tables, padding, constants), so the
numbers are a census, not ground truth — good to a few percent. T-006 (Ghidra function
boundaries) can re-run this per function with `--ranges`.

Output: re/float_census.json (+ summary on stdout).
Run:    .venv/bin/python tools/float_census.py
"""
import json
import re
import sys
from collections import Counter
from pathlib import Path

import capstone
import pefile

ROOT = Path(__file__).resolve().parent.parent
EXE = ROOT / "reference" / "Call of Duty Black Ops" / "BlackOps.exe"
OUT = ROOT / "re" / "float_census.json"

X87_NONFP = {"fxsave", "fxrstor"}
MMX = {"emms", "movq2dq", "movdq2q", "pshufw", "maskmovq", "movntq"}
SSE3 = {"addsubps", "addsubpd", "haddps", "haddpd", "hsubps", "hsubpd", "movddup", "movshdup",
        "movsldup", "lddqu", "fisttp"}
SSSE3 = {"pshufb", "phaddw", "phaddd", "phaddsw", "phsubw", "phsubd", "phsubsw", "pmaddubsw",
         "pmulhrsw", "psignb", "psignw", "psignd", "pabsb", "pabsw", "pabsd", "palignr"}
SSE4 = {"blendps", "blendpd", "blendvps", "blendvpd", "dpps", "dppd", "extractps", "insertps",
        "roundps", "roundpd", "roundss", "roundsd", "pblendw", "pblendvb", "pcmpeqq", "pcmpgtq",
        "pextrb", "pextrd", "pinsrb", "pinsrd", "pmaxsb", "pmaxsd", "pmaxud", "pmaxuw", "pminsb",
        "pminsd", "pminud", "pminuw", "pmovsxbw", "pmovsxbd", "pmovsxwd", "pmovzxbw", "pmovzxbd",
        "pmovzxwd", "pmuldq", "pmulld", "ptest", "packusdw", "popcnt", "crc32", "pcmpestri",
        "pcmpistri", "movntdqa", "mpsadbw", "phminposuw"}
SSE_SCALAR_SS = re.compile(r"^(add|sub|mul|div|sqrt|min|max|rcp|rsqrt|cmp|comi|ucomi|mov)ss$|^cvt(t)?ss2|^cvtsi2ss$")
SSE_SCALAR_SD = re.compile(r"^(add|sub|mul|div|sqrt|min|max|cmp|comi|ucomi)sd$|^cvt(t)?sd2|^cvtsi2sd$")
SSE_PACKED_PS = re.compile(r"ps$|^movmskps$|^shufps$|^cvtdq2ps$|^cvt(t)?ps2")
SSE_PACKED_PD = re.compile(r"pd$|^cvtdq2pd$|^cvt(t)?pd2")
SSE_INT = re.compile(r"^(p[a-z]+|movdqa|movdqu|movd|movq|movntdq|movnti|maskmovdqu)$")
SYSTEM = {"cpuid", "rdtsc", "rdtscp", "int", "int3", "int1", "into", "sysenter", "syscall", "hlt",
          "in", "out", "cli", "sti", "rdmsr", "wrmsr", "xgetbv", "pause", "prefetchnta",
          "prefetcht0", "prefetcht1", "prefetcht2", "lfence", "mfence", "sfence", "ldmxcsr",
          "stmxcsr", "fldcw", "fnstcw", "fnstsw", "fnclex", "fninit", "fnsave", "frstor", "pushfd",
          "popfd", "lahf", "sahf", "xchg", "cmpxchg", "cmpxchg8b", "xadd", "bswap", "bsf", "bsr",
          "bt", "bts", "btr", "btc", "enter", "leave", "lcall", "ljmp", "retf", "iretd", "salc",
          "aaa", "aas", "aam", "aad", "daa", "das", "xlatb", "loop", "loope", "loopne", "jecxz",
          "movbe", "lzcnt", "tzcnt"}


def classify(mn: str) -> str:
    if mn in SSE3:
        return "sse3"
    if mn in SSSE3:
        return "ssse3"
    if mn in SSE4:
        return "sse4"
    if mn in MMX:
        return "mmx"
    if mn.startswith("f") and mn not in X87_NONFP and mn not in ("femms",):
        return "x87"
    if mn == "movsd":            # ambiguous with the string op; resolved by caller via operands
        return "sse_scalar_double"
    if SSE_SCALAR_SS.search(mn):
        return "sse_scalar_single"
    if SSE_SCALAR_SD.search(mn):
        return "sse_scalar_double"
    if mn in ("movaps", "movups", "movlps", "movhps", "movlhps", "movhlps", "movntps"):
        return "sse_packed_single"
    if mn in ("movapd", "movupd", "movlpd", "movhpd", "movntpd"):
        return "sse_packed_double"
    if SSE_PACKED_PS.search(mn):
        return "sse_packed_single"
    if SSE_PACKED_PD.search(mn):
        return "sse_packed_double"
    if SSE_INT.match(mn) and mn not in ("push", "pop", "pushal", "popal", "pushfd", "popfd", "pause",
                                         "prefetchnta", "prefetcht0", "prefetcht1", "prefetcht2",
                                         "pushaw", "popaw", "pushfw", "popfw"):
        return "simd_int"
    return "int"


def main() -> int:
    pe = pefile.PE(str(EXE), fast_load=True)
    base = pe.OPTIONAL_HEADER.ImageBase
    text = next(s for s in pe.sections if s.Name.rstrip(b"\0") == b".text")
    code = text.get_data()[: text.Misc_VirtualSize]
    va0 = base + text.VirtualAddress

    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
    cls = Counter()
    mnem = Counter()
    feat = Counter()
    x87_mnem = Counter()
    off, n, bad = 0, 0, 0
    size = len(code)
    while off < size:
        progressed = False
        for addr, sz, mn, ops in md.disasm_lite(code[off:], va0 + off):
            progressed = True
            off = addr - va0 + sz
            n += 1
            # strip prefixes capstone folds into the mnemonic
            parts = mn.split()
            core = parts[-1]
            if len(parts) > 1:
                feat["prefix_" + parts[0]] += 1
            c = classify(core)
            if core == "movsd" and "xmm" not in ops:
                c = "int"
                feat["string_op"] += 1
            if core == "movq" and "xmm" not in ops:
                c = "mmx"
            if c == "simd_int" and "xmm" not in ops and "mm" in ops:
                c = "mmx"
            cls[c] += 1
            mnem[core] += 1
            if c == "x87":
                x87_mnem[core] += 1
            if core in SYSTEM:
                feat["op_" + core] += 1
            if core == "call":
                if ops.startswith("0x"):
                    feat["call_direct"] += 1
                elif "[" in ops:
                    feat["call_indirect_mem"] += 1
                    if ops.startswith("dword ptr [0x"):
                        feat["call_indirect_mem_absolute(IAT/global fnptr)"] += 1
                else:
                    feat["call_indirect_reg"] += 1
            elif core == "jmp":
                if ops.startswith("0x"):
                    feat["jmp_direct"] += 1
                elif "[" in ops:
                    feat["jmp_indirect_mem"] += 1
                    if "*4" in ops:
                        feat["jmp_indirect_table(*4)"] += 1
                else:
                    feat["jmp_indirect_reg"] += 1
            elif core in ("ret", "retn"):
                feat["ret" if not ops else "ret_imm(stdcall/thiscall callee-pop)"] += 1
            if "fs:" in ops:
                feat["fs_segment_access"] += 1
                if "fs:[0]" in ops or "fs:[0x0]" in ops:
                    feat["fs:[0] (SEH frame chain)"] += 1
                if "fs:[0x2c]" in ops:
                    feat["fs:[0x2c] (TLS array)"] += 1
                if "fs:[0x18]" in ops or "fs:[0x30]" in ops:
                    feat["fs:[0x18/0x30] (TEB/PEB)"] += 1
            elif "gs:" in ops or "es:" in ops and core not in ("movsb", "movsd", "movsw", "stosb", "stosd",
                                                               "stosw", "scasb", "scasd", "scasw", "cmpsb",
                                                               "cmpsd", "cmpsw", "insb", "insd", "insw"):
                feat["other_segment_override"] += 1
            if core in ("movsb", "movsw", "stosb", "stosd", "stosw", "scasb", "scasd", "scasw", "cmpsb",
                        "cmpsd", "cmpsw", "lodsb", "lodsd", "lodsw"):
                feat["string_op"] += 1
            if "xmmword" in ops and core in ("movaps", "movapd", "movdqa"):
                feat["aligned_xmm_mem_access"] += 1
            if "ymm" in ops:
                feat["avx_ymm"] += 1
        if off < size and not progressed:
            bad += 1
            off += 1
        elif off < size:
            # disasm_lite stopped on an undecodable byte
            bad += 1
            off += 1

    fp_total = sum(v for k, v in cls.items() if k in ("x87", "sse_scalar_single", "sse_scalar_double",
                                                     "sse_packed_single", "sse_packed_double"))
    result = {
        "file": "BlackOps.exe",
        "method": "capstone %s linear sweep of .text, resync +1" % capstone.__version__,
        "text_va": f"0x{va0:08x}", "text_size": size,
        "instructions": n, "undecodable_bytes": bad,
        "classes": dict(cls.most_common()),
        "fp_share": {k: round(cls[k] / fp_total, 4) for k in
                     ("x87", "sse_scalar_single", "sse_scalar_double", "sse_packed_single",
                      "sse_packed_double")},
        "x87_top": dict(x87_mnem.most_common(40)),
        "features": dict(sorted(feat.items(), key=lambda kv: -kv[1])),
        "mnemonics_top": dict(mnem.most_common(60)),
        "distinct_mnemonics": len(mnem),
        "all_mnemonics": dict(mnem.most_common()),
    }
    OUT.write_text(json.dumps(result, indent=1) + "\n")
    brief = {k: v for k, v in result.items() if k not in ("all_mnemonics",)}
    print(json.dumps(brief, indent=1))
    return 0


if __name__ == "__main__":
    sys.exit(main())
