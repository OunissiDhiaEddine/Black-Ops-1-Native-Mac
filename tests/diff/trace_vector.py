#!/usr/bin/env python3
"""Debug aid: replay one recorded vector of a function under Unicorn, instruction by instruction,
printing every register/XMM/x87 change. Use it to see where the lifted code and the reference part.

  .venv/bin/python tests/diff/trace_vector.py 0x00633e40 [--nth 0] [--vectors 'artifacts/lifted/vectors_*.bin']
"""
import argparse
import glob
import math
import struct
import sys
from pathlib import Path

import capstone
import pefile
from unicorn import Uc, UcError, UC_ARCH_X86, UC_MODE_32, UC_HOOK_CODE, UC_HOOK_MEM_UNMAPPED, UC_PROT_ALL, UC_MEM_FETCH_UNMAPPED
from unicorn.x86_const import *  # noqa: F403

ROOT = Path(__file__).resolve().parent.parent.parent
EXE = ROOT / "reference" / "Call of Duty Black Ops" / "BlackOps.exe"
REGS = [UC_X86_REG_EAX, UC_X86_REG_ECX, UC_X86_REG_EDX, UC_X86_REG_EBX, UC_X86_REG_ESP, UC_X86_REG_EBP, UC_X86_REG_ESI, UC_X86_REG_EDI]
NAMES = ["eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi"]
SENTINEL, HEAP = 0xFFFF0000, 0x60000000


def vectors(path):
    b = Path(path).read_bytes()
    assert b[:8] == b"BO1DIFF3"
    count, nheaps, hsize = struct.unpack_from("<III", b, 8)
    p = 20
    heaps = [b[p + k * hsize: p + (k + 1) * hsize] for k in range(nheaps)]
    p += nheaps * hsize
    npatch, = struct.unpack_from("<I", b, p)
    patches = [struct.unpack_from("<II", b, p + 4 + 8 * k) for k in range(npatch)]
    p += 4 + 8 * npatch
    for _ in range(count):
        fn, *regs = struct.unpack_from("<9I", b, p)
        df, fcw, mxcsr, flags, heap_idx = struct.unpack_from("<5I", b, p + 36)
        p += 56
        xmm = [b[p + 16 * k: p + 16 * k + 16] for k in range(8)]
        p += 128
        nblk, = struct.unpack_from("<I", b, p)
        p += 4
        blocks = []
        for _ in range(nblk):
            a, n = struct.unpack_from("<II", b, p)
            blocks.append((a, b[p + 8: p + 8 + n]))
            p += 8 + n
        p += 32 + 4 + 64 + 64 + 128
        nout, = struct.unpack_from("<I", b, p)
        p += 4
        for _ in range(nout):
            a, n = struct.unpack_from("<II", b, p)
            p += 8 + n
        yield fn, regs, fcw, xmm, heaps[heap_idx], blocks, patches


def f80_to_double(mant, se):
    e = se & 0x7FFF
    if e == 0x7FFF:
        v = math.inf if (mant << 1) & 0xFFFFFFFFFFFFFFFF == 0 else math.nan
    else:
        try:
            v = math.ldexp(float(mant), (e or 1) - 16383 - 63)
        except OverflowError:
            v = math.inf
    return -v if se & 0x8000 else v


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("func")
    ap.add_argument("--plain", action="store_true", help="address + changes only, without flags (for diffing against the lifted trace)")
    ap.add_argument("--nth", type=int, default=0)
    ap.add_argument("--vectors", default=str(ROOT / "artifacts" / "lifted" / "vectors_*.bin"))
    args = ap.parse_args()
    want = int(args.func, 16)
    found = [v for f in sorted(glob.glob(args.vectors)) for v in vectors(f) if v[0] == want]
    if args.nth >= len(found):
        print(f"{len(found)} vector(s) for {want:08x}")
        return 1
    fn, regs, fcw, xmm, heap, blocks, patches = found[args.nth]
    pe = pefile.PE(str(EXE), fast_load=True)
    base = pe.OPTIONAL_HEADER.ImageBase
    image = bytearray(pe.get_memory_mapped_image())
    size = (len(image) + 0xFFF) & ~0xFFF
    for a, v in patches:
        struct.pack_into("<I", image, a - base, v)
    uc = Uc(UC_ARCH_X86, UC_MODE_32)
    uc.mem_map(base, size, UC_PROT_ALL)
    uc.mem_write(base, bytes(image))
    uc.mem_map(0x7FE00000, 0x100000, UC_PROT_ALL)
    uc.mem_map(HEAP, len(heap), UC_PROT_ALL)
    uc.mem_map(SENTINEL, 0x1000, UC_PROT_ALL)
    uc.mem_write(HEAP, heap)
    for a, data in blocks:
        uc.mem_write(a, data)
    for r, v in zip(REGS, regs):
        uc.reg_write(r, v)
    uc.reg_write(UC_X86_REG_FPCW, fcw)
    uc.reg_write(UC_X86_REG_MXCSR, 0x1F80)
    uc.reg_write(UC_X86_REG_EFLAGS, 0x202)
    for k in range(8):
        uc.reg_write(UC_X86_REG_XMM0 + k, int.from_bytes(xmm[k], "little"))

    def unmapped(u, access, addr, sz, value, _):
        if access == UC_MEM_FETCH_UNMAPPED:
            return False
        for page in {addr & ~0xFFF, (addr + sz - 1) & ~0xFFF}:
            try:
                u.mem_map(page, 0x1000, UC_PROT_ALL)
            except UcError:
                pass
        return True

    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)

    def state(u):
        s = {n: u.reg_read(r) for n, r in zip(NAMES, REGS)}
        for k in range(8):
            s[f"xmm{k}"] = u.reg_read(UC_X86_REG_XMM0 + k)
        fpsw = u.reg_read(UC_X86_REG_FPSW)
        s["top"] = (fpsw >> 11) & 7
        s["fsw"] = fpsw & 0x4700
        for k in range(8):
            s[f"fp{k}"] = u.reg_read(UC_X86_REG_FP0 + k)
        if args.plain:
            del s["fsw"]
        else:
            s["efl"] = u.reg_read(UC_X86_REG_EFLAGS) & 0x8D5
        return s

    prev = [state(uc), None]

    def show_changes(u):
        cur = state(u)
        ch = []
        for k, v in cur.items():
            if prev[0][k] != v:
                ch.append(f"{k}={'%.17g' % f80_to_double(*v)}" if isinstance(v, tuple) else f"{k}={v:x}")
        if prev[1] is not None:
            print((prev[1][:8] + "".join(" " + c for c in ch)) if args.plain else f"{prev[1]:<58s} {' '.join(ch)}")
        prev[0] = cur

    def code(u, addr, sz, _):
        show_changes(u)
        i = next(md.disasm(bytes(u.mem_read(addr, 15)), addr))
        prev[1] = f"{addr:08x} {i.mnemonic} {i.op_str}"

    uc.hook_add(UC_HOOK_MEM_UNMAPPED, unmapped)
    uc.hook_add(UC_HOOK_CODE, code)
    if not args.plain:
        print(f"vector {args.nth} of {len(found)} for {fn:08x}; fcw={fcw:03x}; " + " ".join(f"{n}={v:08x}" for n, v in zip(NAMES, regs)))
    try:
        uc.emu_start(fn, SENTINEL, count=100000)
    except UcError as e:
        print("stopped:", e)
    show_changes(uc)
    return 0


if __name__ == "__main__":
    sys.exit(main())
