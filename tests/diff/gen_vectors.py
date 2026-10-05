#!/usr/bin/env python3
"""Differential-test vectors (T-041): run original functions under Unicorn on randomised state.

  .venv/bin/python tests/diff/gen_vectors.py --out artifacts/lifted/vectors.bin [--max-funcs 400]
                                             [--per-func 3] [--seed 1] [--funcs 0x...,0x...]

Picks lifted LEAF functions (no calls or tail calls, no indirect transfers, no fs:) from
artifacts/lifted/lifted_meta.json, executes the ORIGINAL bytes of each with Unicorn from a
random register/stack/heap state until it returns, and records inputs + resulting registers +
every byte written. tests/diff/difftest.c replays the same inputs through the lifted code.
Unmapped memory reads as zero on both sides (Unicorn maps zero pages on demand; the native arena
is zero-fill), so wild pointers are deterministic rather than fatal.
"""
import argparse
import json
import random
import struct
import sys
from collections import Counter
from pathlib import Path

import capstone
import pefile
from unicorn import (Uc, UcError, UC_ARCH_X86, UC_MODE_32, UC_HOOK_MEM_WRITE, UC_HOOK_MEM_UNMAPPED, UC_HOOK_BLOCK, UC_PROT_ALL,
                     UC_MEM_FETCH_UNMAPPED)
from unicorn.x86_const import (UC_X86_REG_EAX, UC_X86_REG_ECX, UC_X86_REG_EDX, UC_X86_REG_EBX, UC_X86_REG_ESP,
                               UC_X86_REG_EBP, UC_X86_REG_ESI, UC_X86_REG_EDI, UC_X86_REG_EFLAGS, UC_X86_REG_EIP)

ROOT = Path(__file__).resolve().parent.parent.parent
EXE = ROOT / "reference" / "Call of Duty Black Ops" / "BlackOps.exe"
REGS = [UC_X86_REG_EAX, UC_X86_REG_ECX, UC_X86_REG_EDX, UC_X86_REG_EBX, UC_X86_REG_ESP, UC_X86_REG_EBP,
        UC_X86_REG_ESI, UC_X86_REG_EDI]
SENTINEL = 0xFFFF0000
STACK_TOP = 0x7FF00000
STACK_SIZE = 0x100000
HEAP = 0x60000000
HEAP_SIZE = 0x4000
ESP0 = STACK_TOP - 0x10000
MAX_INSNS = 200000
MAX_OUT = 1 << 16
DEBUG = False


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default=str(ROOT / "artifacts" / "lifted" / "vectors.bin"))
    ap.add_argument("--meta", default=str(ROOT / "artifacts" / "lifted" / "lifted_meta.json"))
    ap.add_argument("--max-funcs", type=int, default=400)
    ap.add_argument("--per-func", type=int, default=3)
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--funcs")
    args = ap.parse_args()
    rng = random.Random(args.seed)
    meta = json.loads(Path(args.meta).read_text())
    if args.funcs:
        cands = [int(a, 16) for a in args.funcs.split(",")]
    else:
        cands = [int(a, 16) for a, m in meta.items() if not m["xfer"] and not m["indirect"] and not m["fs"] and m["insns"] >= 3]
        rng.shuffle(cands)
        cands = sorted(cands[: args.max_funcs])

    pe = pefile.PE(str(EXE), fast_load=True)
    base = pe.OPTIONAL_HEADER.ImageBase
    image = bytes(pe.get_memory_mapped_image())
    img_size = (pe.OPTIONAL_HEADER.SizeOfImage + 0xFFF) & ~0xFFF
    image = image.ljust(img_size, b"\0")

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

    def on_block(u, addr, size, _):                           # per block, not per instruction: 50x cheaper
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

    out = bytearray(b"BO1DIFF1" + struct.pack("<I", 0))
    count, skipped, funcs_ok = 0, Counter(), 0
    for fn in cands:
        got = 0
        for _ in range(args.per_func):
            regs = [rand_value() for _ in range(8)]
            regs[4] = ESP0
            stack = struct.pack("<I", SENTINEL) + b"".join(struct.pack("<I", rand_value()) for _ in range(31))
            heap = rng.randbytes(HEAP_SIZE)
            uc.context_restore(ctx0)
            uc.mem_write(ESP0, stack)
            uc.mem_write(HEAP, heap)
            for r, v in zip(REGS, regs):
                uc.reg_write(r, v)
            uc.reg_write(UC_X86_REG_EFLAGS, 0x202)
            written.clear()
            run_pcs.clear()
            try:
                uc.emu_start(fn, SENTINEL, timeout=5_000_000, count=MAX_INSNS)
                ok = uc.reg_read(UC_X86_REG_ESP) > ESP0 and True
                finished = True
            except UcError as e:
                finished, ok = False, False
                skipped[str(e)] += 1
                if DEBUG: print(f'{fn:08x} {e} eip={uc.reg_read(UC_X86_REG_EIP):08x} esp={uc.reg_read(UC_X86_REG_ESP):08x}')
            if finished:
                # emu_start returns normally both at the sentinel and at the instruction limit
                if uc.reg_read(UC_X86_REG_EIP) != SENTINEL:
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
                out += struct.pack("<I8II", fn, *regs, 0)
                out += struct.pack("<I", 2)
                out += struct.pack("<II", ESP0, len(stack)) + stack
                out += struct.pack("<II", HEAP, len(heap)) + heap
                out += struct.pack("<8I", *regs_out)
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
    covered = {a for addr, size in covered if base <= addr < base + img_size
               for a, _, _, _ in md.disasm_lite(image[addr - base: addr - base + size], addr)}
    total_insns = sum(meta[f"0x{f:08x}"]["insns"] for f in cands if f"0x{f:08x}" in meta)
    print(f"functions tried {len(cands)}, with at least one vector {funcs_ok}; vectors {count}; "
          f"instructions executed {len(covered)} of {total_insns} ({100.0 * len(covered) / max(total_insns, 1):.1f}%); "
          f"file {len(out) / 1048576:.1f} MiB")
    for k, v in skipped.most_common(6):
        print(f"  skipped {v}: {k}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
