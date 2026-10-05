#!/usr/bin/env python3
"""Differential-test vectors (T-041): run original functions under Unicorn on randomised state.

  .venv/bin/python tests/diff/gen_vectors.py --out artifacts/lifted/vectors.bin [--max-funcs 400]
                                             [--per-func 3] [--seed 1] [--funcs 0x...,0x...]

Picks lifted LEAF functions (no calls or tail calls, no indirect transfers, no fs:) from
artifacts/lifted/lifted_meta.json, executes the ORIGINAL bytes of each with Unicorn from a
random register/stack/heap/XMM state until it returns, and records inputs + resulting registers,
x87 stack, XMM (or MMX) registers and every byte written. tests/diff/difftest.c replays the same inputs through the lifted code.
Unmapped memory reads as zero on both sides (Unicorn maps zero pages on demand; the native arena
is zero-fill), so wild pointers are deterministic rather than fatal.
"""
import argparse
import json
import random
import math
import struct
import sys
from collections import Counter
from pathlib import Path

import capstone
import pefile
from unicorn import (Uc, UcError, UC_ARCH_X86, UC_MODE_32, UC_HOOK_MEM_WRITE, UC_HOOK_MEM_UNMAPPED, UC_HOOK_BLOCK, UC_PROT_ALL,
                     UC_MEM_FETCH_UNMAPPED)
from unicorn.x86_const import (UC_X86_REG_EAX, UC_X86_REG_ECX, UC_X86_REG_EDX, UC_X86_REG_EBX, UC_X86_REG_ESP,
                               UC_X86_REG_EBP, UC_X86_REG_ESI, UC_X86_REG_EDI, UC_X86_REG_EFLAGS, UC_X86_REG_EIP,
                               UC_X86_REG_FPCW, UC_X86_REG_FPSW, UC_X86_REG_MXCSR, UC_X86_REG_XMM0, UC_X86_REG_FP0)

ROOT = Path(__file__).resolve().parent.parent.parent
EXE = ROOT / "reference" / "Call of Duty Black Ops" / "BlackOps.exe"
REGS = [UC_X86_REG_EAX, UC_X86_REG_ECX, UC_X86_REG_EDX, UC_X86_REG_EBX, UC_X86_REG_ESP, UC_X86_REG_EBP,
        UC_X86_REG_ESI, UC_X86_REG_EDI]
SENTINEL = 0xFFFF0000
STACK_TOP = 0x7FF00000
STACK_SIZE = 0x100000
HEAP = 0x60000000        # must match difftest.c
HEAP_SIZE = 0x4000
POISON = 0xFE000000       # unmapped: import slots point here
ESP0 = STACK_TOP - 0x10000
MAX_INSNS = 100000
MAX_OUT = 1 << 16
DEBUG = False
FLAG_MMX = 1          # vector flag: the function uses MMX -> compare mm0-7 instead of the x87 stack


def f80_to_double(mant, se):
    """80-bit extended (64-bit significand, sign+exponent) -> nearest double."""
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
    ap.add_argument("--out", default=str(ROOT / "artifacts" / "lifted" / "vectors.bin"))
    ap.add_argument("--meta", default=str(ROOT / "artifacts" / "lifted" / "lifted_meta.json"))
    ap.add_argument("--max-funcs", type=int, default=400)
    ap.add_argument("--per-func", type=int, default=3)
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--funcs")
    ap.add_argument("--select", choices=("leaf", "all"), default="leaf",
                    help="leaf: no calls/indirect transfers/fs:. all: every lifted function (call trees run on both sides; "
                         "whatever Unicorn cannot finish - imports, wild pointers - is skipped)")
    ap.add_argument("--shard", default="0/1", help="I/N: take every N-th candidate starting at I (parallel generation)")
    args = ap.parse_args()
    shard_i, shard_n = (int(x) for x in args.shard.split("/"))
    rng = random.Random(args.seed)
    meta = json.loads(Path(args.meta).read_text())
    if args.funcs:
        cands = [int(a, 16) for a in args.funcs.split(",")]
    else:
        cands = [int(a, 16) for a, m in meta.items()
                 if m["insns"] >= 3 and (args.select == "all" or (not m["xfer"] and not m["indirect"] and not m["fs"]))]
        cands.sort()
        rng.shuffle(cands)
        cands = sorted(cands[: args.max_funcs])[shard_i::shard_n]

    uses_mmx = {int(a, 16) for a, m in meta.items() if m.get("mmx")}

    pe = pefile.PE(str(EXE), fast_load=True)
    base = pe.OPTIONAL_HEADER.ImageBase
    image = bytes(pe.get_memory_mapped_image())
    img_size = (pe.OPTIONAL_HEADER.SizeOfImage + 0xFFF) & ~0xFFF
    image = bytearray(image.ljust(img_size, b"\0"))
    # Import slots hold hint/name RVAs in the file; as jump targets those would land in random code.
    # Poison them (same values on the lifted side, via the file header) so that any call into an
    # import faults under Unicorn and the vector is dropped.
    pe.parse_data_directories(directories=[pefile.DIRECTORY_ENTRY["IMAGE_DIRECTORY_ENTRY_IMPORT"]])
    patches = []
    for entry in getattr(pe, "DIRECTORY_ENTRY_IMPORT", []):
        for imp in entry.imports:
            patches.append((imp.address, POISON + 16 * len(patches)))
    for addr, val in patches:
        struct.pack_into("<I", image, addr - base, val)
    image = bytes(image)

    lazy = set()
    written = []

    def on_unmapped(u, access, addr, size, value, _):
        if access == UC_MEM_FETCH_UNMAPPED:                   # jumped into nowhere: let it fail
            return False
        try:
            for page in {addr & ~0xFFF, (addr + size - 1) & ~0xFFF}:
                if page not in lazy:
                    u.mem_map(page, 0x1000, UC_PROT_ALL)
                    lazy.add(page)
        except UcError:
            return False
        return True

    def on_write(u, access, addr, size, value, _):
        written.append((addr, size))

    run_pcs, covered = set(), set()

    code = [(base + sec.VirtualAddress, base + sec.VirtualAddress + sec.Misc_VirtualSize)
            for sec in pe.sections if sec.Characteristics & 0x20000000]
    wild = [False]

    def on_block(u, addr, size, _):                           # per block, not per instruction: 50x cheaper
        if not any(a <= addr < b for a, b in code) and addr != SENTINEL:
            wild[0] = True                                    # executing data / lazily mapped zero pages
            u.emu_stop()
            return
        run_pcs.add((addr, size))

    def make_uc():
        """Fresh emulator. Rebuilt after every failed run: a faulting run can leave Unicorn with
        stale translated code / CPU state that silently breaks the runs after it."""
        u = Uc(UC_ARCH_X86, UC_MODE_32)
        u.mem_map(base, img_size, UC_PROT_ALL)
        u.mem_write(base, image)
        u.mem_map(STACK_TOP - STACK_SIZE, STACK_SIZE, UC_PROT_ALL)
        u.mem_map(HEAP, HEAP_SIZE, UC_PROT_ALL)
        u.mem_map(SENTINEL, 0x1000, UC_PROT_ALL)             # return lands here; emu_start stops at it
        u.hook_add(UC_HOOK_MEM_UNMAPPED, on_unmapped)
        u.hook_add(UC_HOOK_MEM_WRITE, on_write)
        u.hook_add(UC_HOOK_BLOCK, on_block)
        u.reg_write(UC_X86_REG_FPCW, 0x27F)
        u.reg_write(UC_X86_REG_MXCSR, 0x1F80)
        lazy.clear()
        return u, u.context_save()

    uc, ctx0 = make_uc()

    def rand_value():
        k = rng.random()
        if k < 0.4:
            return HEAP + (rng.randrange(0, HEAP_SIZE - 0x800) & ~3)
        if k < 0.7:
            return rng.randrange(0, 65)
        return rng.getrandbits(32)

    def nice_float():
        k = rng.random()
        if k < 0.5:
            return rng.uniform(-1000.0, 1000.0)
        if k < 0.7:
            return float(rng.randrange(-16, 17))
        if k < 0.8:
            return rng.choice((0.0, 1.0, -1.0, 0.5, 2.0, -0.0))
        if k < 0.9:
            return rng.uniform(-1.0, 1.0) * 1e-6
        return rng.uniform(-1.0, 1.0) * 1e7

    def rand_word():
        """One 32-bit word for the heap / XMM lanes: floats dominate so FP code sees usable numbers."""
        k = rng.random()
        if k < 0.45:
            return struct.unpack("<I", struct.pack("<f", nice_float()))[0]
        if k < 0.70:
            return rng.getrandbits(32)
        if k < 0.85:
            return rng.randrange(0, 65)
        return HEAP + (rng.randrange(0, HEAP_SIZE - 0x800) & ~3)

    def make_heap():
        h = bytearray(struct.pack(f"<{HEAP_SIZE // 4}I", *(rand_word() for _ in range(HEAP_SIZE // 4))))
        for _ in range(HEAP_SIZE // 64):                       # sprinkle doubles
            struct.pack_into("<d", h, rng.randrange(0, HEAP_SIZE - 8) & ~7, nice_float())
        return bytes(h)

    rng.seed(args.seed * 7919 + 1)
    heaps = [make_heap() for _ in range(48)]          # identical in every shard
    rng.seed(args.seed * 7919 + 2 + shard_i)

    out = bytearray(b"BO1DIFF3" + struct.pack("<III", 0, len(heaps), HEAP_SIZE) + b"".join(heaps))
    out += struct.pack("<I", len(patches)) + b"".join(struct.pack("<II", a, v) for a, v in patches)
    count, skipped, funcs_ok = 0, Counter(), 0
    for fn in cands:
        got = 0
        for _ in range(args.per_func):
            regs = [rand_value() for _ in range(8)]
            regs[4] = ESP0
            stack = struct.pack("<I", SENTINEL) + b"".join(struct.pack("<I", rand_value()) for _ in range(31))
            heap_idx = rng.randrange(len(heaps))
            heap = heaps[heap_idx]
            fcw = 0x27F if rng.random() < 0.7 else 0x07F        # precision control: 53-bit (Windows default) or 24-bit (D3D9)
            xmm = [struct.pack("<4I", *(rand_word() for _ in range(4))) for _ in range(8)]
            flags = FLAG_MMX if fn in uses_mmx else 0
            uc.context_restore(ctx0)
            uc.reg_write(UC_X86_REG_FPCW, fcw)
            for k in range(8):
                uc.reg_write(UC_X86_REG_XMM0 + k, int.from_bytes(xmm[k], "little"))
            uc.mem_write(ESP0, stack)
            uc.mem_write(HEAP, heap)
            for r, v in zip(REGS, regs):
                uc.reg_write(r, v)
            uc.reg_write(UC_X86_REG_EFLAGS, 0x202)
            written.clear()
            run_pcs.clear()
            wild[0] = False
            try:
                uc.emu_start(fn, SENTINEL, timeout=5_000_000, count=MAX_INSNS)
                ok = uc.reg_read(UC_X86_REG_ESP) > ESP0 and True
                finished = True
            except UcError as e:
                finished, ok = False, False
                skipped[str(e)] += 1
                if DEBUG: print(f'{fn:08x} {e} eip={uc.reg_read(UC_X86_REG_EIP):08x} esp={uc.reg_read(UC_X86_REG_ESP):08x}')
            if finished:
                # emu_start returns normally at the sentinel, at the instruction limit and after emu_stop()
                if wild[0]:
                    ok = False
                    skipped["executed outside the code sections"] += 1
                elif uc.reg_read(UC_X86_REG_EIP) != SENTINEL:
                    ok = False
                    skipped["did not return within the instruction limit"] += 1
            pages = {}
            if ok:
                for addr, size in written:
                    for a in range(addr, addr + size):
                        a &= 0xFFFFFFFF
                        pages.setdefault(a & ~0xFFF, set()).add(a)
                total = sum(len(s) for s in pages.values())
                if total > MAX_OUT:
                    ok = False
                    skipped["wrote too much memory"] += 1
            if ok:
                regs_out = [uc.reg_read(r) for r in REGS]
                addrs = sorted(a for s in pages.values() for a in s)
                ranges = []
                for a in addrs:
                    if ranges and ranges[-1][1] == a:
                        ranges[-1][1] = a + 1
                    else:
                        ranges.append([a, a + 1])
                top = (uc.reg_read(UC_X86_REG_FPSW) >> 11) & 7
                phys = [uc.reg_read(UC_X86_REG_FP0 + k) for k in range(8)]
                st_out = [f80_to_double(*phys[(top + k) & 7]) for k in range(8)]
                mm_out = [phys[k][0] for k in range(8)]
                xmm_out = [uc.reg_read(UC_X86_REG_XMM0 + k).to_bytes(16, "little") for k in range(8)]
                out += struct.pack("<I8IIIIII", fn, *regs, 0, fcw, 0x1F80, flags, heap_idx) + b"".join(xmm)
                out += struct.pack("<I", 1)
                out += struct.pack("<II", ESP0, len(stack)) + stack
                out += struct.pack("<8II", *regs_out, top) + struct.pack("<8d", *st_out) + struct.pack("<8Q", *mm_out)
                out += b"".join(xmm_out)
                out += struct.pack("<I", len(ranges))
                for a, b in ranges:
                    out += struct.pack("<II", a, b - a) + bytes(uc.mem_read(a, b - a))
                count += 1
                got += 1
                covered |= run_pcs
            if not finished:
                uc, ctx0 = make_uc()
                continue
            # restore: image bytes, zero everything else that was written
            touched = {(a + k) & 0xFFFFF000 for a, sz in written for k in (0, sz - 1)}
            try:
                for pg in touched:
                    if base <= pg < base + img_size:
                        uc.mem_write(pg, image[pg - base: pg - base + 0x1000])
                    else:
                        uc.mem_write(pg, b"\0" * 0x1000)
            except UcError:                                   # write hook saw a page that never got mapped
                uc, ctx0 = make_uc()
        funcs_ok += got > 0
    struct.pack_into("<I", out, 8, count)
    Path(args.out).write_bytes(out)
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
    covered = sorted({a for addr, size in covered if base <= addr < base + img_size
                      for a, _, _, _ in md.disasm_lite(image[addr - base: addr - base + size], addr)})
    # sidecars for the report: statistics and the set of executed instruction addresses
    Path(args.out + ".cov").write_bytes(struct.pack(f"<{len(covered)}I", *covered))
    stats = {"functions_tried": len(cands), "functions_with_vectors": funcs_ok, "vectors": count,
             "insns_in_tried_functions": sum(meta[f"0x{f:08x}"]["insns"] for f in cands if f"0x{f:08x}" in meta),
             "skipped": dict(skipped)}
    Path(args.out + ".json").write_text(json.dumps(stats))
    print(f"shard {args.shard}: functions tried {len(cands)}, with at least one vector {funcs_ok}; vectors {count}; "
          f"instructions executed {len(covered)}; file {len(out) / 1048576:.1f} MiB")
    for k, v in skipped.most_common(6):
        print(f"  skipped {v}: {k}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
