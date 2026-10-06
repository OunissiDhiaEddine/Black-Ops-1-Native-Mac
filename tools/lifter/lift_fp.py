"""x87 / SSE / SSE2 / MMX and integer leftovers for the lifter (T-042). See docs/LIFTER.md §5.

lift(L, i, addr) returns a list of C statements or None when the instruction is not handled here.
x87 is decoded from the opcode bytes (D8..DF + ModRM), not from capstone's operand list, whose
conventions for the register forms are irregular; capstone's mnemonic is used as a cross-check
(MISMATCH counts what disagrees). Helpers used by the generated code: src/runtime/lifted_fp.h.
"""
import re
from collections import Counter

import capstone.x86 as X

XMM = {X.X86_REG_XMM0 + k: k for k in range(8)}
MM = {X.X86_REG_MM0 + k: k for k in range(8)}
MISMATCH = Counter()


def is_xmm(o):
    return o.type == X.X86_OP_REG and o.reg in XMM


def is_mm(o):
    return o.type == X.X86_OP_REG and o.reg in MM


def is_mem(o):
    return o.type == X.X86_OP_MEM


def xr(L, o):
    """128-bit read."""
    if is_xmm(o):
        return f"cpu->xmm[{XMM[o.reg]}]"
    if is_mem(o):
        return f"ldx({L.ea(o)})"
    raise L.Unsupported("xmm operand")


def mr(L, o):
    """64-bit read widened to Xmm (MMX)."""
    if is_mm(o):
        return f"x_from64(cpu->mm[{MM[o.reg]}])"
    if is_mem(o):
        return f"x_from64(M64({L.ea(o)}))"
    raise L.Unsupported("mmx operand")


def fr(L, o, w):
    """Scalar float read: w = 32 or 64."""
    if is_xmm(o):
        return f"cpu->xmm[{XMM[o.reg]}].f{w}[0]"
    if is_mem(o):
        return f"MF{w}({L.ea(o)})"
    raise L.Unsupported("scalar operand")


# dst = helper(dst, src) on 128-bit values; MMX forms use the low 64 bits unless listed in MMX_ONLY
PACKED = {m: "x_" + m for m in """
addps subps mulps divps minps maxps sqrtps rcpps rsqrtps addpd subpd mulpd divpd minpd maxpd sqrtpd
paddb paddw paddd paddq psubb psubw psubd psubq paddsb paddsw paddusb paddusw psubsb psubsw psubusb psubusw
pmullw pmulhw pmulhuw pmaddwd pmuludq pcmpeqb pcmpeqw pcmpeqd pcmpgtb pcmpgtw pcmpgtd pavgb pavgw
pmaxsw pminsw pmaxub pminub psadbw cvtdq2ps cvttps2dq cvtps2pd cvtpd2ps cvtdq2pd
punpcklbw punpckhbw punpcklwd punpckhwd punpckldq punpckhdq punpcklqdq punpckhqdq packsswb packuswb packssdw
unpcklps unpckhps unpcklpd unpckhpd movlhps movhlps
""".split()}
PACKED.update({"andps": "x_and", "andpd": "x_and", "pand": "x_and", "andnps": "x_andn", "andnpd": "x_andn",
               "pandn": "x_andn", "orps": "x_or", "orpd": "x_or", "por": "x_or", "xorps": "x_xor",
               "xorpd": "x_xor", "pxor": "x_xor"})
MMX_ONLY = {m: "m_" + m for m in "punpcklbw punpckhbw punpcklwd punpckhwd punpckldq punpckhdq packsswb packuswb packssdw".split()}
SHIFTS = {"psllw", "pslld", "psllq", "psrlw", "psrld", "psrlq", "psraw", "psrad"}
MOV128 = {"movaps", "movups", "movapd", "movupd", "movdqa", "movdqu", "lddqu", "movntps", "movntpd", "movntdq"}
SCALAR = {"add", "sub", "mul", "div", "sqrt", "min", "max"}
PRED = {"eq": 0, "lt": 1, "le": 2, "unord": 3, "neq": 4, "nlt": 5, "nle": 6, "ord": 7}
CMP_RE = re.compile(r"^cmp(eq|lt|le|unord|neq|nlt|nle|ord)?(ss|sd|ps|pd)$")
NOPS = {"emms", "pause", "wait", "fwait", "sfence", "lfence", "mfence", "prefetch", "prefetchw", "prefetchnta",
        "prefetcht0", "prefetcht1", "prefetcht2", "fnop"}


def lift_sse(L, i, m, ops):
    n = len(ops)
    if m in PACKED and n == 2:
        d, s = ops
        if is_xmm(d):
            k = XMM[d.reg]
            return [f"cpu->xmm[{k}] = {PACKED[m]}(cpu->xmm[{k}], {xr(L, s)});"]
        if is_mm(d):
            k = MM[d.reg]
            return [f"cpu->mm[{k}] = {MMX_ONLY.get(m, PACKED[m])}(x_from64(cpu->mm[{k}]), {mr(L, s)}).u64[0];"]
        return None
    if m in SHIFTS and n == 2:
        d, s = ops
        if s.type == X.X86_OP_IMM:
            c = f"{s.imm & 0xFF}u"
        elif is_xmm(s):
            c = f"cpu->xmm[{XMM[s.reg]}].u64[0]"
        elif is_mm(s):
            c = f"cpu->mm[{MM[s.reg]}]"
        else:
            c = f"M64({L.ea(s)})"
        if is_xmm(d):
            return [f"cpu->xmm[{XMM[d.reg]}] = x_{m}(cpu->xmm[{XMM[d.reg]}], {c});"]
        if is_mm(d):
            return [f"cpu->mm[{MM[d.reg]}] = x_{m}(x_from64(cpu->mm[{MM[d.reg]}]), {c}).u64[0];"]
        return None
    if m in ("pslldq", "psrldq") and is_xmm(ops[0]):
        return [f"cpu->xmm[{XMM[ops[0].reg]}] = x_{m}(cpu->xmm[{XMM[ops[0].reg]}], {ops[1].imm & 0xFF}u);"]
    if m in ("shufps", "shufpd") and is_xmm(ops[0]):
        k = XMM[ops[0].reg]
        return [f"cpu->xmm[{k}] = x_{m}(cpu->xmm[{k}], {xr(L, ops[1])}, {ops[2].imm & 0xFF});"]
    if m in ("pshufd", "pshuflw", "pshufhw") and is_xmm(ops[0]):
        return [f"cpu->xmm[{XMM[ops[0].reg]}] = x_{m}({xr(L, ops[1])}, {ops[2].imm & 0xFF});"]
    if m == "pshufw" and is_mm(ops[0]):
        return [f"cpu->mm[{MM[ops[0].reg]}] = x_pshuflw({mr(L, ops[1])}, {ops[2].imm & 0xFF}).u64[0];"]

    # ---- moves -------------------------------------------------------------------------------
    if m in MOV128 and n == 2:
        d, s = ops
        if is_xmm(d):
            return [f"cpu->xmm[{XMM[d.reg]}] = {xr(L, s)};"]
        if is_mem(d) and is_xmm(s):
            return [f"stx({L.ea(d)}, cpu->xmm[{XMM[s.reg]}]);"]
        return None
    if m in ("movss", "movsd") and n == 2 and (is_xmm(ops[0]) or is_xmm(ops[1])):
        d, s = ops
        u = "u32" if m == "movss" else "u64"
        mem = "M32" if m == "movss" else "M64"
        if is_xmm(d) and is_xmm(s):
            return [f"cpu->xmm[{XMM[d.reg]}].{u}[0] = cpu->xmm[{XMM[s.reg]}].{u}[0];"]
        if is_xmm(d) and is_mem(s):
            k = XMM[d.reg]
            return [f"{{ uint64_t v_ = {mem}({L.ea(s)}); cpu->xmm[{k}].u64[0] = v_; cpu->xmm[{k}].u64[1] = 0; }}"]
        if is_mem(d) and is_xmm(s):
            return [f"{mem}({L.ea(d)}) = cpu->xmm[{XMM[s.reg]}].{u}[0];"]
        return None
    if m == "movd" and n == 2:
        d, s = ops
        if is_xmm(d):
            return [f"{{ uint32_t v_ = {L.rd(s, 4)}; cpu->xmm[{XMM[d.reg]}] = x_from64(v_); }}"]
        if is_mm(d):
            return [f"cpu->mm[{MM[d.reg]}] = (uint32_t){L.rd(s, 4)};"]
        if is_xmm(s):
            return [L.wr(d, f"cpu->xmm[{XMM[s.reg]}].u32[0]", 4)]
        if is_mm(s):
            return [L.wr(d, f"(uint32_t)cpu->mm[{MM[s.reg]}]", 4)]
        return None
    if m == "movq" and n == 2:
        d, s = ops
        if is_xmm(d):
            src = f"cpu->xmm[{XMM[s.reg]}].u64[0]" if is_xmm(s) else f"M64({L.ea(s)})"
            return [f"cpu->xmm[{XMM[d.reg]}] = x_from64({src});"]
        if is_mm(d):
            src = f"cpu->mm[{MM[s.reg]}]" if is_mm(s) else f"M64({L.ea(s)})"
            return [f"cpu->mm[{MM[d.reg]}] = {src};"]
        if is_mem(d) and is_xmm(s):
            return [f"M64({L.ea(d)}) = cpu->xmm[{XMM[s.reg]}].u64[0];"]
        if is_mem(d) and is_mm(s):
            return [f"M64({L.ea(d)}) = cpu->mm[{MM[s.reg]}];"]
        return None
    if m == "movntq" and is_mem(ops[0]) and is_mm(ops[1]):
        return [f"M64({L.ea(ops[0])}) = cpu->mm[{MM[ops[1].reg]}];"]
    if m in ("movlps", "movlpd", "movhps", "movhpd") and n == 2:
        h = 0 if m[3] == "l" else 1
        d, s = ops
        if is_xmm(d) and is_mem(s):
            return [f"cpu->xmm[{XMM[d.reg]}].u64[{h}] = M64({L.ea(s)});"]
        if is_mem(d) and is_xmm(s):
            return [f"M64({L.ea(d)}) = cpu->xmm[{XMM[s.reg]}].u64[{h}];"]
        return None
    if m == "movdq2q" and is_mm(ops[0]) and is_xmm(ops[1]):
        return [f"cpu->mm[{MM[ops[0].reg]}] = cpu->xmm[{XMM[ops[1].reg]}].u64[0];"]
    if m == "movq2dq" and is_xmm(ops[0]) and is_mm(ops[1]):
        return [f"cpu->xmm[{XMM[ops[0].reg]}] = x_from64(cpu->mm[{MM[ops[1].reg]}]);"]
    if m in ("movmskps", "movmskpd") and is_xmm(ops[1]):
        return [L.wr(ops[0], f"x_{m}(cpu->xmm[{XMM[ops[1].reg]}])", 4)]
    if m == "pmovmskb":
        if is_xmm(ops[1]):
            return [L.wr(ops[0], f"x_pmovmskb(cpu->xmm[{XMM[ops[1].reg]}], 16)", 4)]
        if is_mm(ops[1]):
            return [L.wr(ops[0], f"x_pmovmskb(x_from64(cpu->mm[{MM[ops[1].reg]}]), 8)", 4)]
    if m == "pextrw" and n == 3 and ops[0].type == X.X86_OP_REG:
        if is_xmm(ops[1]):
            return [L.wr(ops[0], f"cpu->xmm[{XMM[ops[1].reg]}].u16[{ops[2].imm & 7}]", 4)]
        if is_mm(ops[1]):
            return [L.wr(ops[0], f"(uint16_t)(cpu->mm[{MM[ops[1].reg]}] >> {16 * (ops[2].imm & 3)})", 4)]
    if m == "pinsrw" and n == 3:
        v = L.rd(ops[1], 2) if is_mem(ops[1]) else f"(uint16_t){L.rd(ops[1], 4)}"
        if is_xmm(ops[0]):
            return [f"cpu->xmm[{XMM[ops[0].reg]}].u16[{ops[2].imm & 7}] = {v};"]
        if is_mm(ops[0]):
            sh = 16 * (ops[2].imm & 3)
            k = MM[ops[0].reg]
            return [f"cpu->mm[{k}] = (cpu->mm[{k}] & ~(0xFFFFull << {sh})) | ((uint64_t){v} << {sh});"]

    # ---- scalar arithmetic, compares, conversions -------------------------------------------------
    if m[-2:] in ("ss", "sd") and m[:-2] in SCALAR and n == 2 and is_xmm(ops[0]):
        w = 32 if m[-1] == "s" else 64
        k = XMM[ops[0].reg]
        return [f"cpu->xmm[{k}].f{w}[0] = {m}(cpu->xmm[{k}].f{w}[0], {fr(L, ops[1], w)});"]
    if m in ("rcpss", "rsqrtss") and is_xmm(ops[0]):
        k = XMM[ops[0].reg]
        return [f"cpu->xmm[{k}].f32[0] = {m}(cpu->xmm[{k}].f32[0], {fr(L, ops[1], 32)});"]
    if m in ("comiss", "ucomiss", "comisd", "ucomisd") and is_xmm(ops[0]):
        w = 32 if m[-1] == "s" else 64
        return [f"fp_comi(&fl, cpu->xmm[{XMM[ops[0].reg]}].f{w}[0], {fr(L, ops[1], w)});"]
    c = CMP_RE.match(m)
    if c and n >= 2 and is_xmm(ops[0]):
        if c.group(1):
            pred = PRED[c.group(1)]
        elif n == 3 and ops[2].type == X.X86_OP_IMM:
            pred = ops[2].imm & 7
        else:
            return None
        k = XMM[ops[0].reg]
        kind = c.group(2)
        if kind == "ss":
            return [f"cpu->xmm[{k}].u32[0] = fp_cmp({pred}, cpu->xmm[{k}].f32[0], {fr(L, ops[1], 32)}) ? 0xFFFFFFFFu : 0;"]
        if kind == "sd":
            return [f"cpu->xmm[{k}].u64[0] = fp_cmp({pred}, cpu->xmm[{k}].f64[0], {fr(L, ops[1], 64)}) ? ~0ull : 0;"]
        return [f"cpu->xmm[{k}] = x_cmp{kind}(cpu->xmm[{k}], {xr(L, ops[1])}, {pred});"]
    if m in ("cvtsi2ss", "cvtsi2sd") and is_xmm(ops[0]):
        w, t = (32, "float") if m[-1] == "s" else (64, "double")
        return [f"cpu->xmm[{XMM[ops[0].reg]}].f{w}[0] = ({t})(int32_t){L.rd(ops[1], 4)};"]
    if m in ("cvttss2si", "cvttsd2si", "cvtss2si", "cvtsd2si") and ops[0].type == X.X86_OP_REG:
        w = 32 if m[-4] == "s" else 64
        v = fr(L, ops[1], w)
        return [L.wr(ops[0], f"(uint32_t)cvtt2si({v})" if m[3] == "t" else f"(uint32_t)cvt2si(cpu, {v})", 4)]
    if m == "cvtss2sd" and is_xmm(ops[0]):
        return [f"cpu->xmm[{XMM[ops[0].reg]}].f64[0] = (double){fr(L, ops[1], 32)};"]
    if m == "cvtsd2ss" and is_xmm(ops[0]):
        return [f"cpu->xmm[{XMM[ops[0].reg]}].f32[0] = cvtsd2ss({fr(L, ops[1], 64)});"]
    if m == "cvtps2dq" and is_xmm(ops[0]):
        return [f"cpu->xmm[{XMM[ops[0].reg]}] = x_cvtps2dq(cpu, {xr(L, ops[1])});"]
    if m == "cvtpi2ps" and is_xmm(ops[0]):
        k = XMM[ops[0].reg]
        return [f"{{ Xmm s_ = {mr(L, ops[1])}; cpu->xmm[{k}].f32[0] = (float)s_.i32[0]; cpu->xmm[{k}].f32[1] = (float)s_.i32[1]; }}"]
    if m in ("cvtps2pi", "cvttps2pi") and is_mm(ops[0]):
        cv = "cvtt2si(%s)" if m[3] == "t" else "cvt2si(cpu, %s)"
        return [f"{{ Xmm s_ = {xr(L, ops[1])}; Xmm r_ = x_zero(); r_.i32[0] = {cv % 's_.f32[0]'}; r_.i32[1] = {cv % 's_.f32[1]'}; "
                f"cpu->mm[{MM[ops[0].reg]}] = r_.u64[0]; }}"]
    if m == "stmxcsr":
        return [f"M32({L.ea(ops[0])}) = cpu->mxcsr;"]
    if m == "ldmxcsr":
        return [f"cpu->mxcsr = M32({L.ea(ops[0])});"]
    return None


# ---- x87 ----------------------------------------------------------------------------------------------
ARITH = ["ST(0) = x87_add(cpu, ST(0), {v});", "ST(0) = x87_mul(cpu, ST(0), {v});", "x87_com(cpu, ST(0), {v});",
         "x87_com(cpu, ST(0), {v}); x87_pop(cpu);", "ST(0) = x87_sub(cpu, ST(0), {v});", "ST(0) = x87_sub(cpu, {v}, ST(0));",
         "ST(0) = x87_div(cpu, ST(0), {v});", "ST(0) = x87_div(cpu, {v}, ST(0));"]
ARITH_NAMES = ["add", "mul", "com", "comp", "sub", "subr", "div", "divr"]
CONSTS = {0xE8: ("fld1", "1.0"), 0xE9: ("fldl2t", "3.3219280948873623"), 0xEA: ("fldl2e", "1.4426950408889634"),
          0xEB: ("fldpi", "3.14159265358979323846"), 0xEC: ("fldlg2", "0.30102999566398120"),
          0xED: ("fldln2", "0.69314718055994531"), 0xEE: ("fldz", "0.0")}
D9_MISC = {
    0xD0: ("fnop", ""),
    0xE0: ("fchs", "ST(0) = -ST(0);"),
    0xE1: ("fabs", "ST(0) = fabs(ST(0));"),
    0xE4: ("ftst", "x87_com(cpu, ST(0), 0.0);"),
    0xE5: ("fxam", "x87_fxam(cpu);"),
    0xF0: ("f2xm1", "ST(0) = expm1(ST(0) * 0.69314718055994531);"),
    0xF1: ("fyl2x", "{ double a_ = ST(1), b_ = ST(0); double r_ = a_ * log2(b_); NANFIX64(r_, a_, b_); x87_pop(cpu); ST(0) = r_; }"),
    0xF2: ("fptan", "x87_fptan(cpu);"),
    0xF3: ("fpatan", "{ double r_ = atan2(ST(1), ST(0)); x87_pop(cpu); ST(0) = r_; }"),
    0xF5: ("fprem1", "x87_prem(cpu, 1);"),
    0xF6: ("fdecstp", "cpu->ftop = (cpu->ftop - 1) & 7;"),
    0xF7: ("fincstp", "cpu->ftop = (cpu->ftop + 1) & 7;"),
    0xF8: ("fprem", "x87_prem(cpu, 0);"),
    0xF9: ("fyl2xp1", "{ double r_ = ST(1) * (log1p(ST(0)) * 1.4426950408889634); x87_pop(cpu); ST(0) = r_; }"),
    0xFA: ("fsqrt", "ST(0) = x87_sqrt(cpu, ST(0));"),
    0xFB: ("fsincos", "x87_fsincos(cpu);"),
    0xFC: ("frndint", "ST(0) = x87_rnd(cpu, ST(0));"),
    0xFD: ("fscale", "x87_fscale(cpu);"),
    0xFE: ("fsin", "x87_fsin(cpu);"),
    0xFF: ("fcos", "x87_fcos(cpu);"),
}
FCMOV = {(0xDA, 0): ("fcmovb", "CC_B"), (0xDA, 1): ("fcmove", "CC_E"), (0xDA, 2): ("fcmovbe", "CC_BE"),
         (0xDA, 3): ("fcmovu", "CC_P"), (0xDB, 0): ("fcmovnb", "CC_AE"), (0xDB, 1): ("fcmovne", "CC_NE"),
         (0xDB, 2): ("fcmovnbe", "CC_A"), (0xDB, 3): ("fcmovnu", "CC_NP")}
# capstone spellings that differ from the names used in the tables
ALIASES = {"fucompi": "fucomip", "fcompi": "fcomip", "fstsw": "fnstsw", "fstcw": "fnstcw", "fclex": "fnclex",
           "finit": "fninit", "fstenv": "fnstenv", "fsave": "fnsave"}


def x87(L, i):
    """Returns (name, C statement) or raises Unsupported. Decoded from the opcode bytes."""
    op = i.opcode[0]
    modrm = i.modrm
    reg, rm = (modrm >> 3) & 7, modrm & 7
    if modrm < 0xC0:                                    # ---- memory forms
        mem = next((o for o in i.operands if is_mem(o)), None)
        if mem is None:
            raise L.Unsupported("x87 memory form without operand")
        ea = L.ea(mem)
        if op in (0xD8, 0xDC, 0xDA, 0xDE):
            v = {0xD8: f"(double)MF32({ea})", 0xDC: f"MF64({ea})", 0xDA: f"(double)(int32_t)M32({ea})",
                 0xDE: f"(double)(int16_t)M16({ea})"}[op]
            name = ("fi" if op in (0xDA, 0xDE) else "f") + ARITH_NAMES[reg]
            return name, "{ double v_ = " + v + "; " + ARITH[reg].format(v="v_") + " }"
        table = {
            (0xD9, 0): ("fld", f"{{ double v_ = (double)MF32({ea}); x87_push(cpu, v_); }}"),
            (0xD9, 2): ("fst", f"MF32({ea}) = (float)ST(0);"),
            (0xD9, 3): ("fstp", f"MF32({ea}) = (float)ST(0); x87_pop(cpu);"),
            (0xD9, 4): ("fldenv", f"x87_fldenv(cpu, {ea});"),
            (0xD9, 5): ("fldcw", f"cpu->fcw = M16({ea});"),
            (0xD9, 6): ("fnstenv", f"x87_fnstenv(cpu, {ea});"),
            (0xDD, 4): ("frstor", f"x87_frstor(cpu, {ea});"),
            (0xDD, 6): ("fnsave", f"x87_fnsave(cpu, {ea});"),
            (0xD9, 7): ("fnstcw", f"M16({ea}) = cpu->fcw;"),
            (0xDB, 0): ("fild", f"{{ double v_ = (double)(int32_t)M32({ea}); x87_push(cpu, v_); }}"),
            (0xDB, 1): ("fisttp", f"M32({ea}) = (uint32_t)fp_to_i32(trunc(ST(0))); x87_pop(cpu);"),
            (0xDB, 2): ("fist", f"M32({ea}) = (uint32_t)fp_to_i32(x87_rnd(cpu, ST(0)));"),
            (0xDB, 3): ("fistp", f"M32({ea}) = (uint32_t)fp_to_i32(x87_rnd(cpu, ST(0))); x87_pop(cpu);"),
            (0xDB, 5): ("fld", f"{{ double v_ = x87_ld80({ea}); x87_push(cpu, v_); }}"),
            (0xDB, 7): ("fstp", f"x87_st80({ea}, ST(0)); x87_pop(cpu);"),
            (0xDD, 0): ("fld", f"{{ double v_ = MF64({ea}); x87_push(cpu, v_); }}"),
            (0xDD, 1): ("fisttp", f"M64({ea}) = (uint64_t)fp_to_i64(trunc(ST(0))); x87_pop(cpu);"),
            (0xDD, 2): ("fst", f"MF64({ea}) = ST(0);"),
            (0xDD, 3): ("fstp", f"MF64({ea}) = ST(0); x87_pop(cpu);"),
            (0xDD, 7): ("fnstsw", f"M16({ea}) = x87_status(cpu);"),
            (0xDF, 0): ("fild", f"{{ double v_ = (double)(int16_t)M16({ea}); x87_push(cpu, v_); }}"),
            (0xDF, 1): ("fisttp", f"M16({ea}) = (uint16_t)fp_to_i16(trunc(ST(0))); x87_pop(cpu);"),
            (0xDF, 2): ("fist", f"M16({ea}) = (uint16_t)fp_to_i16(x87_rnd(cpu, ST(0)));"),
            (0xDF, 3): ("fistp", f"M16({ea}) = (uint16_t)fp_to_i16(x87_rnd(cpu, ST(0))); x87_pop(cpu);"),
            (0xDF, 5): ("fild", f"{{ double v_ = (double)(int64_t)M64({ea}); x87_push(cpu, v_); }}"),
            (0xDF, 7): ("fistp", f"M64({ea}) = (uint64_t)fp_to_i64(x87_rnd(cpu, ST(0))); x87_pop(cpu);"),
        }
        if (op, reg) in table:
            return table[(op, reg)]
        raise L.Unsupported(f"x87 {op:02x}/{reg} m ({i.mnemonic})")
    s = f"ST({rm})"                                       # ---- register forms
    if op == 0xD8:
        return "f" + ARITH_NAMES[reg], "{ double v_ = " + s + "; " + ARITH[reg].format(v="v_") + " }"
    if op == 0xD9:
        if reg == 0:
            return "fld", f"{{ double v_ = {s}; x87_push(cpu, v_); }}"
        if reg == 1:
            return "fxch", f"{{ double t_ = ST(0); ST(0) = {s}; {s} = t_; }}"
        if modrm in CONSTS:
            return CONSTS[modrm][0], f"x87_push(cpu, {CONSTS[modrm][1]});"
        if modrm in D9_MISC:
            return D9_MISC[modrm]
    if op in (0xDA, 0xDB) and (op, reg) in FCMOV:
        name, cc = FCMOV[(op, reg)]
        return name, f"if ({cc}) ST(0) = {s};"
    if op == 0xDA and modrm == 0xE9:
        return "fucompp", "x87_com(cpu, ST(0), ST(1)); x87_pop(cpu); x87_pop(cpu);"
    if op == 0xDB:
        if modrm == 0xE2:
            return "fnclex", "cpu->fsw &= 0x4700;"
        if modrm == 0xE3:
            return "fninit", "cpu->fcw = 0x37F; cpu->fsw = 0; cpu->ftop = 0;"
        if reg == 5:
            return "fucomi", f"x87_comi(&fl, ST(0), {s});"
        if reg == 6:
            return "fcomi", f"x87_comi(&fl, ST(0), {s});"
    if op == 0xDC and reg in (0, 1, 4, 5, 6, 7):
        expr = {0: f"x87_add(cpu, {s}, ST(0))", 1: f"x87_mul(cpu, {s}, ST(0))", 4: f"x87_sub(cpu, ST(0), {s})",
                5: f"x87_sub(cpu, {s}, ST(0))", 6: f"x87_div(cpu, ST(0), {s})", 7: f"x87_div(cpu, {s}, ST(0))"}[reg]
        return {0: "fadd", 1: "fmul", 4: "fsubr", 5: "fsub", 6: "fdivr", 7: "fdiv"}[reg], f"{s} = {expr};"
    if op == 0xDD:
        if reg == 0:
            return "ffree", ""
        if reg == 2:
            return "fst", f"{s} = ST(0);"
        if reg == 3:
            return "fstp", f"{s} = ST(0); x87_pop(cpu);"
        if reg == 4:
            return "fucom", f"x87_com(cpu, ST(0), {s});"
        if reg == 5:
            return "fucomp", f"x87_com(cpu, ST(0), {s}); x87_pop(cpu);"
    if op == 0xDE:
        if modrm == 0xD9:
            return "fcompp", "x87_com(cpu, ST(0), ST(1)); x87_pop(cpu); x87_pop(cpu);"
        if reg in (0, 1, 4, 5, 6, 7):
            expr = {0: f"x87_add(cpu, {s}, ST(0))", 1: f"x87_mul(cpu, {s}, ST(0))", 4: f"x87_sub(cpu, ST(0), {s})",
                    5: f"x87_sub(cpu, {s}, ST(0))", 6: f"x87_div(cpu, ST(0), {s})", 7: f"x87_div(cpu, {s}, ST(0))"}[reg]
            return ({0: "faddp", 1: "fmulp", 4: "fsubrp", 5: "fsubp", 6: "fdivrp", 7: "fdivp"}[reg],
                    f"{s} = {expr}; x87_pop(cpu);")
    if op == 0xDF:
        if modrm == 0xE0:
            return "fnstsw", "cpu->eax = (cpu->eax & 0xFFFF0000u) | x87_status(cpu);"
        if reg == 5:
            return "fucomip", f"x87_comi(&fl, ST(0), {s}); x87_pop(cpu);"
        if reg == 6:
            return "fcomip", f"x87_comi(&fl, ST(0), {s}); x87_pop(cpu);"
    raise L.Unsupported(f"x87 {op:02x} {modrm:02x} ({i.mnemonic})")


# ---- integer leftovers ----------------------------------------------------------------------------------
T = {1: "uint8_t", 2: "uint16_t", 4: "uint32_t"}
ACC = {1: X.X86_REG_AL, 2: X.X86_REG_AX, 4: X.X86_REG_EAX}


def lift_int(L, i, m, ops):
    n = len(ops)
    sz = ops[0].size if n else 4
    a = i.address
    if m == "xadd" and n == 2:
        if is_mem(ops[0]) and sz == 4:
            return [f"{{ uint32_t b_ = {L.rd(ops[1])}; uint32_t a_ = lk_xadd32({L.ea(ops[0])}, b_); "
                    f"(void)add32(&fl, a_, b_); {L.wr(ops[1], 'a_')} }}"]
        return [f"{{ {T[sz]} a_ = {L.rd(ops[0])}; {T[sz]} b_ = {L.rd(ops[1])}; {T[sz]} r_ = add{8 * sz}(&fl, a_, b_); "
                f"{L.wr(ops[1], 'a_')} {L.wr(ops[0], 'r_')} }}"]
    if m == "cmpxchg" and n == 2:
        if is_mem(ops[0]) and sz == 4:
            return [f"{{ uint32_t e_ = cpu->eax, old_ = e_; int ok_ = lk_cas32({L.ea(ops[0])}, &old_, {L.rd(ops[1])}); "
                    f"(void)sub32(&fl, e_, old_); if (!ok_) cpu->eax = old_; }}"]
        acc = L.rd_reg(ACC[sz])
        return [f"{{ {T[sz]} d_ = {L.rd(ops[0])}; (void)sub{8 * sz}(&fl, {acc}, d_); "
                f"if (fl.zf) {{ {L.wr(ops[0], L.rd(ops[1]))} }} else {{ {L.wr_reg(ACC[sz], 'd_')} }} }}"]
    if m == "cmpxchg8b" and n == 1 and is_mem(ops[0]):
        return [f"{{ uint64_t old_ = ((uint64_t)cpu->edx << 32) | cpu->eax; uint64_t new_ = ((uint64_t)cpu->ecx << 32) | cpu->ebx; "
                f"if (lk_cas64({L.ea(ops[0])}, &old_, new_)) fl.zf = 1; "
                f"else {{ fl.zf = 0; cpu->eax = (uint32_t)old_; cpu->edx = (uint32_t)(old_ >> 32); }} }}"]
    if m in ("bt", "bts", "btr", "btc") and n == 2 and sz == 4:
        upd = {"bt": "", "bts": "v_ | m_", "btr": "v_ & ~m_", "btc": "v_ ^ m_"}[m]
        if ops[0].type == X.X86_OP_REG or ops[1].type == X.X86_OP_IMM:
            w = L.wr(ops[0], upd) if upd else ""
            return [f"{{ uint32_t v_ = {L.rd(ops[0])}; uint32_t m_ = 1u << ({L.rd(ops[1], 1)} & 31); fl.cf = (v_ & m_) != 0; {w} }}"]
        # memory bit string indexed by a register: the bit offset is signed and unbounded
        w = f"M32(p_) = {upd};" if upd else ""
        return [f"{{ int32_t i_ = (int32_t){L.rd(ops[1])}; uint32_t p_ = {L.ea(ops[0])} + (uint32_t)((i_ >> 5) * 4); uint32_t v_ = M32(p_); "
                f"uint32_t m_ = 1u << (i_ & 31); fl.cf = (v_ & m_) != 0; {w} }}"]
    if m in ("rcl", "rcr") and sz == 4:
        c = L.rd(ops[1], 1) if n == 2 else "1"
        return [L.wr(ops[0], f"{m}32(&fl, {L.rd(ops[0])}, {c})")]
    if m == "pushfd":
        return ["PUSH32(flags_to_eflags(&fl, cpu));"]
    if m == "popfd":
        return ["eflags_to_flags(&fl, cpu, POP32());"]
    if m == "pushal":
        return ["{ uint32_t s_ = cpu->esp; PUSH32(cpu->eax); PUSH32(cpu->ecx); PUSH32(cpu->edx); PUSH32(cpu->ebx); PUSH32(s_); "
                "PUSH32(cpu->ebp); PUSH32(cpu->esi); PUSH32(cpu->edi); }"]
    if m == "popal":
        return ["cpu->edi = POP32(); cpu->esi = POP32(); cpu->ebp = POP32(); cpu->esp += 4; cpu->ebx = POP32(); "
                "cpu->edx = POP32(); cpu->ecx = POP32(); cpu->eax = POP32();"]
    if m == "pushaw":   # 16-bit PUSHA: ax, cx, dx, bx, sp (before), bp, si, di
        regs = ["eax", "ecx", "edx", "ebx", None, "ebp", "esi", "edi"]
        body = "".join(f"cpu->esp -= 2; M16(cpu->esp) = (uint16_t){'s_' if r is None else 'cpu->' + r}; " for r in regs)
        return ["{ uint32_t s_ = cpu->esp; " + body + "}"]
    if m == "popaw":
        regs = ["edi", "esi", "ebp", None, "ebx", "edx", "ecx", "eax"]
        body = "".join("cpu->esp += 2; " if r is None else f"cpu->{r} = (cpu->{r} & 0xFFFF0000u) | M16(cpu->esp); cpu->esp += 2; " for r in regs)
        return [body]
    if m == "xlatb":
        return ["cpu->eax = (cpu->eax & 0xFFFFFF00u) | M8(cpu->ebx + (uint8_t)cpu->eax);"]
    if m == "mul" and n == 1 and sz == 1:
        return [f"{{ uint32_t w_ = (uint32_t)(uint8_t)cpu->eax * {L.rd(ops[0])}; cpu->eax = (cpu->eax & 0xFFFF0000u) | (uint16_t)w_; "
                f"fl.cf = fl.of = (w_ >> 8) != 0; szp8(&fl, (uint8_t)w_); fl.af = 0; }}"]
    if m == "mul" and n == 1 and sz == 2:
        return [f"{{ uint32_t w_ = (uint32_t)(uint16_t)cpu->eax * {L.rd(ops[0])}; cpu->eax = (cpu->eax & 0xFFFF0000u) | (uint16_t)w_; "
                f"cpu->edx = (cpu->edx & 0xFFFF0000u) | (w_ >> 16); fl.cf = fl.of = (w_ >> 16) != 0; szp16(&fl, (uint16_t)w_); fl.af = 0; }}"]
    if m == "imul" and n == 1 and sz == 1:
        return [f"{{ int32_t w_ = (int32_t)(int8_t)cpu->eax * (int8_t){L.rd(ops[0])}; cpu->eax = (cpu->eax & 0xFFFF0000u) | (uint16_t)w_; "
                f"fl.cf = fl.of = w_ != (int8_t)w_; szp8(&fl, (uint8_t)w_); fl.af = 0; }}"]
    if m == "imul" and n == 1 and sz == 2:
        return [f"{{ int32_t w_ = (int32_t)(int16_t)cpu->eax * (int16_t){L.rd(ops[0])}; cpu->eax = (cpu->eax & 0xFFFF0000u) | (uint16_t)w_; "
                f"cpu->edx = (cpu->edx & 0xFFFF0000u) | (uint16_t)(w_ >> 16); fl.cf = fl.of = w_ != (int16_t)w_; szp16(&fl, (uint16_t)w_); fl.af = 0; }}"]
    if m == "cpuid":
        return ["rt_cpuid(cpu);"]
    if m == "rdtsc":
        return ["rt_rdtsc(cpu);"]
    return None


def lift(L, i, m):
    """m = mnemonic without lock/rep prefixes. Returns C statements or None."""
    ops = i.operands
    if m in NOPS:
        return []
    op0 = i.opcode[0]
    if 0xD8 <= op0 <= 0xDF and not any(is_xmm(o) or is_mm(o) for o in ops) and m[0] == "f":
        name, code = x87(L, i)
        if ALIASES.get(m, m) != name:
            MISMATCH[f"{m} decoded as {name}"] += 1
            raise L.Unsupported(f"x87 decode mismatch {m}/{name}")
        return [code] if code else []
    r = lift_sse(L, i, m, ops)
    if r is not None:
        return r
    return lift_int(L, i, m, ops)
