#!/usr/bin/env python3
"""x86-32 -> C lifter for BlackOps.exe (ADR-013). See docs/LIFTER.md.

Front end: capstone (detail mode) over the function / basic-block / jump-table facts exported
from Ghidra (re/ghidra_export). Back end: one C function per guest function, using
src/runtime/lifted.h.

  .venv/bin/python tools/lifter/lift.py --out artifacts/lifted [--only 0x401000,...] [--stats re/lift_coverage.json]

Generated code is derived from the game binary: it goes under artifacts/ and is never committed.
"""
import argparse
import csv
import json
import re
import sys
from collections import Counter, defaultdict
from pathlib import Path

import capstone
import capstone.x86 as X
import pefile

ROOT = Path(__file__).resolve().parent.parent.parent
EXE = ROOT / "reference" / "Call of Duty Black Ops" / "BlackOps.exe"
EXP = ROOT / "re" / "ghidra_export"

REG32 = {X.X86_REG_EAX: "eax", X.X86_REG_ECX: "ecx", X.X86_REG_EDX: "edx", X.X86_REG_EBX: "ebx",
         X.X86_REG_ESP: "esp", X.X86_REG_EBP: "ebp", X.X86_REG_ESI: "esi", X.X86_REG_EDI: "edi"}
REG16 = {X.X86_REG_AX: "eax", X.X86_REG_CX: "ecx", X.X86_REG_DX: "edx", X.X86_REG_BX: "ebx",
         X.X86_REG_SP: "esp", X.X86_REG_BP: "ebp", X.X86_REG_SI: "esi", X.X86_REG_DI: "edi"}
REG8L = {X.X86_REG_AL: "eax", X.X86_REG_CL: "ecx", X.X86_REG_DL: "edx", X.X86_REG_BL: "ebx"}
REG8H = {X.X86_REG_AH: "eax", X.X86_REG_CH: "ecx", X.X86_REG_DH: "edx", X.X86_REG_BH: "ebx"}
CC = {"o": "CC_O", "no": "CC_NO", "b": "CC_B", "ae": "CC_AE", "e": "CC_E", "ne": "CC_NE", "be": "CC_BE",
      "a": "CC_A", "s": "CC_S", "ns": "CC_NS", "p": "CC_P", "np": "CC_NP", "l": "CC_L", "ge": "CC_GE",
      "le": "CC_LE", "g": "CC_G"}
T = {1: "uint8_t", 2: "uint16_t", 4: "uint32_t"}
ST = {1: "int8_t", 2: "int16_t", 4: "int32_t"}


class Unsupported(Exception):
    pass


class Lifter:
    def __init__(self):
        raw = EXE.read_bytes()
        self.pe = pefile.PE(data=raw, fast_load=True)
        self.base = self.pe.OPTIONAL_HEADER.ImageBase
        self.image = self.pe.get_memory_mapped_image()
        self.md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
        self.md.detail = True
        self.funcs = {}                       # addr -> name
        self.blocks = defaultdict(list)       # func -> [(start, end)]
        with (EXP / "functions.csv").open() as f:
            for r in csv.DictReader(f):
                self.funcs[int(r["addr"], 16)] = r["name"]
        with (EXP / "blocks.csv").open() as f:
            for r in csv.DictReader(f):
                if r["func"]:
                    self.blocks[int(r["func"], 16)].append((int(r["start"], 16), int(r["end"], 16)))
        self.tables = {}                      # insn addr -> [targets]
        with (EXP / "jumptables.csv").open() as f:
            for r in csv.DictReader(f):
                if r["kind"] == "jump" and r["targets"]:
                    self.tables[int(r["insn"], 16)] = [int(t, 16) for t in r["targets"].split(";")]

    # ---- operand helpers -----------------------------------------------------------------
    def rd_reg(self, reg):
        if reg in REG32:
            return f"cpu->{REG32[reg]}"
        if reg in REG16:
            return f"(uint16_t)cpu->{REG16[reg]}"
        if reg in REG8L:
            return f"(uint8_t)cpu->{REG8L[reg]}"
        if reg in REG8H:
            return f"(uint8_t)(cpu->{REG8H[reg]} >> 8)"
        raise Unsupported(f"register {self.md.reg_name(reg)}")

    def wr_reg(self, reg, val):
        if reg in REG32:
            return f"cpu->{REG32[reg]} = (uint32_t)({val});"
        if reg in REG16:
            r = REG16[reg]
            return f"cpu->{r} = (cpu->{r} & 0xFFFF0000u) | (uint16_t)({val});"
        if reg in REG8L:
            r = REG8L[reg]
            return f"cpu->{r} = (cpu->{r} & 0xFFFFFF00u) | (uint8_t)({val});"
        if reg in REG8H:
            r = REG8H[reg]
            return f"cpu->{r} = (cpu->{r} & 0xFFFF00FFu) | ((uint32_t)(uint8_t)({val}) << 8);"
        raise Unsupported(f"register {self.md.reg_name(reg)}")

    def ea(self, op):
        m = op.mem
        parts = []
        if m.segment == X.X86_REG_FS:
            parts.append("cpu->teb")
        elif m.segment not in (0, X.X86_REG_DS, X.X86_REG_SS, X.X86_REG_ES, X.X86_REG_CS):
            raise Unsupported("segment override")
        if m.base:
            if m.base not in REG32:
                raise Unsupported("16-bit addressing")
            parts.append(f"cpu->{REG32[m.base]}")
        if m.index:
            if m.index not in REG32:
                raise Unsupported("16-bit addressing")
            parts.append(f"cpu->{REG32[m.index]} * {m.scale}u" if m.scale != 1 else f"cpu->{REG32[m.index]}")
        if m.disp or not parts:
            parts.append(f"0x{m.disp & 0xFFFFFFFF:x}u")
        return "(uint32_t)(" + " + ".join(parts) + ")"

    def rd(self, op, size=None):
        size = size or op.size
        if op.type == X.X86_OP_REG:
            return self.rd_reg(op.reg)
        if op.type == X.X86_OP_IMM:
            return f"({T[size]})0x{op.imm & ((1 << (8 * size)) - 1):x}u"
        if op.type == X.X86_OP_MEM:
            return f"M{8 * size}({self.ea(op)})"
        raise Unsupported("operand type")

    def wr(self, op, val, size=None):
        size = size or op.size
        if op.type == X.X86_OP_REG:
            return self.wr_reg(op.reg, val)
        if op.type == X.X86_OP_MEM:
            return f"M{8 * size}({self.ea(op)}) = ({T[size]})({val});"
        raise Unsupported("write to immediate")

    # ---- one instruction -> list of C lines --------------------------------------------------
    def lift_insn(self, i, fn_addr, in_func):
        m = i.mnemonic
        ops = i.operands
        n = len(ops)
        out = []
        sz = ops[0].size if n else 4
        if any(p in i.prefix for p in (0xF0,)) and m.split()[0] == "lock":
            m = m.split(" ", 1)[1]            # lock prefix: atomicity handled in T-044 (single-threaded tests)
        parts = m.split()
        rep = None
        if parts[0] in ("rep", "repe", "repne"):
            rep, m = parts[0], parts[1]

        def goto(target):
            if target in in_func:
                return f"goto L_{target:08x};"
            if target in self.funcs:
                return f"{{ f_{target:08x}(cpu); return; }}"      # tail call
            return f"{{ rt_dispatch(cpu, 0x{target:08x}u); return; }}"

        def binop(name, write=True, carry_only=False):
            a, b = self.rd(ops[0]), self.rd(ops[1], sz)
            if ops[1].type == X.X86_OP_IMM:
                b = f"({T[sz]})0x{ops[1].imm & ((1 << (8 * sz)) - 1):x}u"
            expr = f"{name}{8 * sz}(&fl, {a}, {b})"
            return [f"{{ {T[sz]} r_ = {expr}; {self.wr(ops[0], 'r_')} }}" if write else f"(void){expr};"]

        if m in ("add", "sub", "adc", "sbb"):
            return binop(m)
        if m == "cmp":
            return binop("sub", write=False)
        if m in ("and", "or", "xor", "test"):
            c_op = {"and": "&", "or": "|", "xor": "^", "test": "&"}[m]
            a, b = self.rd(ops[0]), self.rd(ops[1], sz)
            expr = f"logic{8 * sz}(&fl, ({T[sz]})({a} {c_op} {b}))"
            return [f"(void){expr};"] if m == "test" else [f"{{ {T[sz]} r_ = {expr}; {self.wr(ops[0], 'r_')} }}"]
        if m in ("inc", "dec", "neg"):
            return [f"{{ {T[sz]} r_ = {m}{8 * sz}(&fl, {self.rd(ops[0])}); {self.wr(ops[0], 'r_')} }}"]
        if m == "not":
            return [f"{{ {T[sz]} r_ = ({T[sz]})~{self.rd(ops[0])}; {self.wr(ops[0], 'r_')} }}"]
        if m in ("shl", "sal", "shr", "sar", "rol", "ror"):
            name = "shl" if m == "sal" else m
            cnt = self.rd(ops[1], 1) if n > 1 else "1"
            return [f"{{ {T[sz]} r_ = {name}{8 * sz}(&fl, {self.rd(ops[0])}, {cnt}); {self.wr(ops[0], 'r_')} }}"]
        if m in ("shld", "shrd") and sz == 4:
            return [f"{{ uint32_t r_ = {m}32(&fl, {self.rd(ops[0])}, {self.rd(ops[1])}, {self.rd(ops[2], 1)}); {self.wr(ops[0], 'r_')} }}"]
        if m == "mov":
            return [self.wr(ops[0], self.rd(ops[1], sz))]
        if m == "movzx":
            return [self.wr(ops[0], f"({T[ops[1].size]}){self.rd(ops[1])}")]
        if m == "movsx":
            return [self.wr(ops[0], f"({ST[sz]})({ST[ops[1].size]}){self.rd(ops[1])}")]
        if m == "lea":
            if ops[1].mem.segment == X.X86_REG_FS:
                raise Unsupported("lea with fs")
            return [self.wr(ops[0], self.ea(ops[1]))]
        if m == "xchg":
            return [f"{{ {T[sz]} a_ = {self.rd(ops[0])}, b_ = {self.rd(ops[1])}; {self.wr(ops[0], 'b_')} {self.wr(ops[1], 'a_')} }}"]
        if m == "push":
            if sz != 4:
                raise Unsupported("push 16-bit")
            return [f"{{ uint32_t v_ = {self.rd(ops[0])}; PUSH32(v_); }}"]
        if m == "pop":
            if sz != 4:
                raise Unsupported("pop 16-bit")
            return [f"{{ uint32_t v_ = POP32(); {self.wr(ops[0], 'v_')} }}"]
        if m == "leave":
            return ["cpu->esp = cpu->ebp; cpu->ebp = POP32();"]
        if m == "nop" or m == "fnop" and False:
            return []
        if m == "cdq":
            return ["cpu->edx = (uint32_t)((int32_t)cpu->eax >> 31);"]
        if m == "cwde":
            return ["cpu->eax = (uint32_t)(int32_t)(int16_t)cpu->eax;"]
        if m == "cbw":
            return ["cpu->eax = (cpu->eax & 0xFFFF0000u) | (uint16_t)(int16_t)(int8_t)cpu->eax;"]
        if m == "cwd":
            return ["cpu->edx = (cpu->edx & 0xFFFF0000u) | (uint16_t)((int16_t)cpu->eax >> 15);"]
        if m == "imul":
            if n == 1:
                if sz != 4:
                    raise Unsupported("imul 8/16-bit one-operand")
                return [f"{{ int64_t w_ = (int64_t)(int32_t)cpu->eax * (int32_t){self.rd(ops[0])}; cpu->eax = (uint32_t)w_; "
                        f"cpu->edx = (uint32_t)(w_ >> 32); fl.cf = fl.of = w_ != (int32_t)w_; szp32(&fl, cpu->eax); fl.af = 0; }}"]
            a = self.rd(ops[1] if n >= 2 else ops[0], sz)
            b = self.rd(ops[2], sz) if n == 3 else self.rd(ops[0])
            if n == 3 and ops[2].type == X.X86_OP_IMM:
                b = f"({ST[sz]})0x{ops[2].imm & ((1 << (8 * sz)) - 1):x}u"
            return [f"{{ int64_t w_ = (int64_t)({ST[sz]}){a} * ({ST[sz]}){b}; {T[sz]} r_ = ({T[sz]})w_; "
                    f"fl.cf = fl.of = w_ != ({ST[sz]})w_; szp{8 * sz}(&fl, r_); fl.af = 0; {self.wr(ops[0], 'r_')} }}"]
        if m == "mul":
            if sz != 4:
                raise Unsupported("mul 8/16-bit")
            return [f"{{ uint64_t w_ = (uint64_t)cpu->eax * {self.rd(ops[0])}; cpu->eax = (uint32_t)w_; cpu->edx = (uint32_t)(w_ >> 32); "
                    f"fl.cf = fl.of = cpu->edx != 0; szp32(&fl, cpu->eax); fl.af = 0; }}"]
        if m in ("div", "idiv"):
            if sz != 4:
                raise Unsupported("div 8/16-bit")
            d = self.rd(ops[0])
            if m == "div":
                return [f"{{ uint32_t d_ = {d}; uint64_t n_ = ((uint64_t)cpu->edx << 32) | cpu->eax; "
                        f"if (!d_ || n_ / d_ > 0xFFFFFFFFull) rt_trap(cpu, 0x{i.address:08x}u, TRAP_DIV); "
                        f"cpu->eax = (uint32_t)(n_ / d_); cpu->edx = (uint32_t)(n_ % d_); }}"]
            return [f"{{ int32_t d_ = (int32_t){d}; int64_t n_ = (int64_t)(((uint64_t)cpu->edx << 32) | cpu->eax); "
                    f"if (!d_ || (n_ == INT64_MIN && d_ == -1) || n_ / d_ > INT32_MAX || n_ / d_ < INT32_MIN) rt_trap(cpu, 0x{i.address:08x}u, TRAP_DIV); "
                    f"cpu->eax = (uint32_t)(n_ / d_); cpu->edx = (uint32_t)(n_ % d_); }}"]
        if m.startswith("set") and m[3:] in CC:
            return [self.wr(ops[0], f"({CC[m[3:]]}) ? 1 : 0")]
        if m.startswith("cmov") and m[4:] in CC:
            return [f"if ({CC[m[4:]]}) {{ {self.wr(ops[0], self.rd(ops[1], sz))} }}"]
        if m == "bswap":
            return [self.wr(ops[0], f"__builtin_bswap32({self.rd(ops[0])})")]
        if m in ("bsf", "bsr"):
            fnc = "__builtin_ctz" if m == "bsf" else "31 - __builtin_clz"
            return [f"{{ uint32_t s_ = {self.rd(ops[1], sz)}; fl.zf = s_ == 0; if (s_) {{ {self.wr(ops[0], f'{fnc}(s_)')} }} }}"]
        if m == "bt" and sz == 4 and ops[0].type == X.X86_OP_REG:
            return [f"fl.cf = ({self.rd(ops[0])} >> ({self.rd(ops[1], 1)} & 31)) & 1;"]
        if m == "cld":
            return ["cpu->df = 0;"]
        if m == "std":
            return ["cpu->df = 1;"]
        if m == "sahf":
            return ["ah_to_flags(&fl, (uint8_t)(cpu->eax >> 8));"]
        if m == "lahf":
            return ["cpu->eax = (cpu->eax & 0xFFFF00FFu) | ((uint32_t)flags_to_ah(&fl) << 8);"]
        if m in ("stc", "clc", "cmc"):
            return [{"stc": "fl.cf = 1;", "clc": "fl.cf = 0;", "cmc": "fl.cf = !fl.cf;"}[m]]
        if m == "int3":
            return [f"rt_trap(cpu, 0x{i.address:08x}u, TRAP_INT3);"]

        # ---- string operations ------------------------------------------------------------
        if m[:4] in ("movs", "stos", "lods", "scas", "cmps") and m[4:] in ("b", "w", "d") and n <= 2 and \
                (n == 0 or all(o.type == X.X86_OP_MEM for o in ops) or m[:4] in ("stos", "lods", "scas")):
            w = {"b": 1, "w": 2, "d": 4}[m[4]]
            step = f"(cpu->df ? -{w} : {w})"
            kind = m[:4]
            acc = {1: "(uint8_t)cpu->eax", 2: "(uint16_t)cpu->eax", 4: "cpu->eax"}[w]
            body = {
                "movs": f"M{8 * w}(cpu->edi) = M{8 * w}(cpu->esi); cpu->esi += {step}; cpu->edi += {step};",
                "stos": f"M{8 * w}(cpu->edi) = {acc}; cpu->edi += {step};",
                "lods": self.wr_reg({1: X.X86_REG_AL, 2: X.X86_REG_AX, 4: X.X86_REG_EAX}[w], f"M{8 * w}(cpu->esi)") + f" cpu->esi += {step};",
                "scas": f"(void)sub{8 * w}(&fl, {acc}, M{8 * w}(cpu->edi)); cpu->edi += {step};",
                "cmps": f"(void)sub{8 * w}(&fl, M{8 * w}(cpu->esi), M{8 * w}(cpu->edi)); cpu->esi += {step}; cpu->edi += {step};",
            }[kind]
            if not rep:
                return [body]
            if kind in ("scas", "cmps"):
                stop = "!fl.zf" if rep in ("repe", "rep") else "fl.zf"
                return [f"while (cpu->ecx) {{ {body} cpu->ecx--; if ({stop}) break; }}"]
            return [f"while (cpu->ecx) {{ {body} cpu->ecx--; }}"]

        # ---- control flow ------------------------------------------------------------------
        if m == "jmp":
            if ops[0].type == X.X86_OP_IMM:
                return [goto(ops[0].imm & 0xFFFFFFFF)]
            if i.address in self.tables:
                cases = "".join(f" case 0x{t:08x}u: {goto(t)}" for t in sorted(set(self.tables[i.address])))
                return [f"switch ({self.rd(ops[0])}) {{{cases} default: rt_trap(cpu, 0x{i.address:08x}u, TRAP_BAD_JUMP); }}"]
            return [f"{{ uint32_t t_ = {self.rd(ops[0])}; rt_dispatch(cpu, t_); return; }}"]      # indirect tail call
        if m[0] == "j" and m[1:] in CC:
            return [f"if ({CC[m[1:]]}) {goto(ops[0].imm & 0xFFFFFFFF)}"]
        if m == "jecxz":
            return [f"if (!cpu->ecx) {goto(ops[0].imm & 0xFFFFFFFF)}"]
        if m == "call":
            ret = i.address + i.size
            if ops[0].type == X.X86_OP_IMM:
                t = ops[0].imm & 0xFFFFFFFF
                if t in self.funcs:
                    return [f"PUSH32(0x{ret:08x}u); f_{t:08x}(cpu);"]
                return [f"PUSH32(0x{ret:08x}u); rt_dispatch(cpu, 0x{t:08x}u);"]
            return [f"{{ uint32_t t_ = {self.rd(ops[0])}; PUSH32(0x{ret:08x}u); rt_dispatch(cpu, t_); }}"]
        if m in ("ret", "retn"):
            extra = ops[0].imm if n else 0
            return [f"cpu->esp += {4 + extra}; return;"]
        raise Unsupported(m.split()[0] if not rep else m)

    # ---- one function ---------------------------------------------------------------------------
    def lift_function(self, fn):
        blocks = sorted(set(self.blocks.get(fn, [])))
        if not blocks:
            raise Unsupported("no blocks")
        starts = {s for s, _ in blocks}
        insns = []
        for s, e in blocks:
            data = self.image[s - self.base: e + 1 - self.base]
            got = list(self.md.disasm(data, s))
            if not got or got[-1].address + got[-1].size != e + 1:
                raise Unsupported("block does not decode cleanly")
            insns.append((s, got))
        # every direct branch target inside the function must start a block we emit
        lines = [f"void f_{fn:08x}(Cpu *cpu) {{", "    Flags fl = {0, 0, 0, 0, 0, 0}; (void)fl;"]
        if blocks[0][0] != fn:
            lines.append(f"    goto L_{fn:08x};")
        info = {"calls": 0, "indirect": 0, "fs": False, "insns": 0}
        bad = []
        for idx, (s, got) in enumerate(insns):
            lines.append(f"L_{s:08x}: ;")
            for i in got:
                info["insns"] += 1
                for o in i.operands:
                    if o.type == X.X86_OP_MEM and o.mem.segment == X.X86_REG_FS:
                        info["fs"] = True
                try:
                    c = self.lift_insn(i, fn, starts)
                except Unsupported as e:                       # keep going: census of ALL blockers
                    bad.append(str(e))
                    continue
                if i.mnemonic == "call":
                    info["calls"] += 1
                if "rt_dispatch" in " ".join(c):
                    info["indirect"] += 1
                lines.append(f"    /* {i.address:08x} {i.mnemonic} {i.op_str} */ " + " ".join(c))
            last = got[-1]
            nxt = last.address + last.size
            falls = last.mnemonic not in ("jmp", "ret", "retn", "int3") and not last.mnemonic.startswith("ret")
            next_block = insns[idx + 1][0] if idx + 1 < len(insns) else None
            if falls and nxt != next_block:
                if nxt in starts:
                    lines.append(f"    goto L_{nxt:08x};")
                elif nxt in self.funcs:
                    lines.append(f"    f_{nxt:08x}(cpu); return;")   # falls into the next function
                else:
                    bad.append("fallthrough leaves the function")
        if bad:
            e = Unsupported(bad[0])
            e.all = bad
            raise e
        lines.append("}")
        text = "\n".join(lines)
        info["xfer"] = len(re.findall(r"\bf_[0-9a-f]{8}\(cpu\)", text))   # calls + tail calls + fallthroughs
        # labels that are never the target of a goto would only produce warnings; keep them (';' after label)
        return text, info


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default=str(ROOT / "artifacts" / "lifted"))
    ap.add_argument("--only", help="comma-separated function addresses")
    ap.add_argument("--stats", help="write coverage statistics JSON here")
    ap.add_argument("--chunk", type=int, default=400, help="functions per generated .c file")
    args = ap.parse_args()
    L = Lifter()
    todo = sorted(int(a, 16) for a in args.only.split(",")) if args.only else sorted(L.funcs)
    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    for old in out.glob("lifted_*.c"):
        old.unlink()
    ok, reasons, meta = [], Counter(), {}
    all_reasons, func_reasons = Counter(), Counter()   # per instruction / per function containing it
    texts = []
    for fn in todo:
        try:
            text, info = L.lift_function(fn)
            ok.append(fn)
            meta[fn] = info
            texts.append(text)
        except Unsupported as e:
            reasons[str(e)] += 1
            every = getattr(e, "all", [str(e)])
            all_reasons.update(every)
            for r in set(every):
                func_reasons[r] += 1
        except Exception as e:  # decoding surprises must not stop the batch
            reasons[f"internal: {type(e).__name__}"] += 1
    protos = "\n".join(f"void f_{a:08x}(Cpu *cpu);" for a in sorted(L.funcs))
    (out / "lifted_protos.h").write_text("// GENERATED by tools/lifter/lift.py — do not edit\n#include \"lifted.h\"\n" + protos + "\n")
    for k in range(0, len(texts), args.chunk):
        (out / f"lifted_{k // args.chunk:04d}.c").write_text(
            "// GENERATED by tools/lifter/lift.py — do not edit\n#include \"lifted_protos.h\"\n\n" + "\n\n".join(texts[k:k + args.chunk]) + "\n")
    # address table for rt_dispatch + stubs for functions that are not lifted yet
    okset = set(ok)
    tab = ["// GENERATED by tools/lifter/lift.py — do not edit", "#include \"lifted_protos.h\"", ""]
    for a in sorted(L.funcs):
        if a not in okset:
            tab.append(f"void f_{a:08x}(Cpu *cpu) {{ rt_trap(cpu, 0x{a:08x}u, TRAP_UNSUPPORTED); }}")
    tab.append("const struct LiftedEntry { uint32_t va; LiftedFn fn; } g_lifted_table[] = {")
    tab += [f"    {{0x{a:08x}u, f_{a:08x}}}," for a in sorted(L.funcs)]
    tab.append("};")
    tab.append(f"const unsigned g_lifted_count = {len(L.funcs)};")
    (out / "lifted_table.c").write_text("\n".join(tab) + "\n")
    (out / "lifted_meta.json").write_text(json.dumps({f"0x{a:08x}": meta[a] for a in ok}))
    total = len(todo)
    print(f"functions {total}: lifted {len(ok)} ({100.0 * len(ok) / max(total, 1):.1f}%), "
          f"instructions lifted {sum(m['insns'] for m in meta.values()):,}")
    for r, c in reasons.most_common(40):
        print(f"  {c:6d}  {r}")
    if args.stats:
        Path(args.stats).write_text(json.dumps({
            "functions": total, "lifted": len(ok), "instructions_lifted": sum(m["insns"] for m in meta.values()),
            "leaf_lifted": sum(1 for m in meta.values() if not m["xfer"] and not m["indirect"]),
            "blockers": dict(reasons.most_common()),
            "blockers_all_instructions": dict(all_reasons.most_common()),
            "blockers_functions_containing": dict(func_reasons.most_common())}, indent=1) + "\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
