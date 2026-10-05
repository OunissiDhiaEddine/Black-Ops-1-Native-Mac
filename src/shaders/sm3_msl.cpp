// SM2/SM3 bytecode -> MSL. One pass to decode, one to emit. See sm3.h for scope.
//
// Translation model: every register is a float4 local; every source operand is evaluated as a
// float4 (swizzle + modifier applied), every operation produces a float4, and the destination
// write mask picks components. That mirrors the D3D virtual machine exactly and leaves the
// simplification to the Metal compiler.
#include "sm3.h"

#include <array>
#include <cstdarg>
#include <cstring>
#include <map>
#include <set>

namespace bo1::sm3 {

namespace {

enum RegType : uint8_t {
    kTemp = 0, kInput = 1, kConst = 2, kAddrOrTex = 3, kRastOut = 4, kAttrOut = 5, kOutput = 6,
    kConstInt = 7, kColorOut = 8, kDepthOut = 9, kSampler = 10, kConst2 = 11, kConst3 = 12,
    kConst4 = 13, kConstBool = 14, kLoop = 15, kTempFloat16 = 16, kMisc = 17, kLabel = 18, kPredicate = 19,
};

enum Op : uint16_t {
    NOP = 0, MOV, ADD, SUB, MAD, MUL, RCP, RSQ, DP3, DP4, MIN, MAX, SLT, SGE, EXP, LOG, LIT, DST, LRP,
    FRC, M4x4, M4x3, M3x4, M3x3, M3x2, CALL, CALLNZ, LOOP, RET, ENDLOOP, LABEL, DCL, POW, CRS, SGN,
    ABS, NRM, SINCOS, REP, ENDREP, IF, IFC, ELSE, ENDIF, BREAK, BREAKC, MOVA, DEFB, DEFI,
    TEXCOORD = 64, TEXKILL = 65, TEX = 66, CND = 80, DEF = 81, CMP = 88, BEM = 89, DP2ADD = 90,
    DSX = 91, DSY = 92, TEXLDD = 93, SETP = 94, TEXLDL = 95, BREAKP = 96,
    PHASE = 0xFFFD, COMMENT = 0xFFFE, END = 0xFFFF,
};

const char *op_name(unsigned op) {
    static const std::map<unsigned, const char *> names = {
        {NOP, "nop"}, {MOV, "mov"}, {ADD, "add"}, {SUB, "sub"}, {MAD, "mad"}, {MUL, "mul"}, {RCP, "rcp"},
        {RSQ, "rsq"}, {DP3, "dp3"}, {DP4, "dp4"}, {MIN, "min"}, {MAX, "max"}, {SLT, "slt"}, {SGE, "sge"},
        {EXP, "exp"}, {LOG, "log"}, {LIT, "lit"}, {DST, "dst"}, {LRP, "lrp"}, {FRC, "frc"}, {M4x4, "m4x4"},
        {M4x3, "m4x3"}, {M3x4, "m3x4"}, {M3x3, "m3x3"}, {M3x2, "m3x2"}, {CALL, "call"}, {CALLNZ, "callnz"},
        {LOOP, "loop"}, {RET, "ret"}, {ENDLOOP, "endloop"}, {LABEL, "label"}, {DCL, "dcl"}, {POW, "pow"},
        {CRS, "crs"}, {SGN, "sgn"}, {ABS, "abs"}, {NRM, "nrm"}, {SINCOS, "sincos"}, {REP, "rep"},
        {ENDREP, "endrep"}, {IF, "if"}, {IFC, "ifc"}, {ELSE, "else"}, {ENDIF, "endif"}, {BREAK, "break"},
        {BREAKC, "breakc"}, {MOVA, "mova"}, {DEFB, "defb"}, {DEFI, "defi"}, {TEXCOORD, "texcoord"},
        {TEXKILL, "texkill"}, {TEX, "texld"}, {CND, "cnd"}, {DEF, "def"}, {CMP, "cmp"}, {BEM, "bem"},
        {DP2ADD, "dp2add"}, {DSX, "dsx"}, {DSY, "dsy"}, {TEXLDD, "texldd"}, {SETP, "setp"},
        {TEXLDL, "texldl"}, {BREAKP, "breakp"},
    };
    auto it = names.find(op);
    return it == names.end() ? "?" : it->second;
}

struct Param {
    uint8_t type = 0;
    uint16_t num = 0;
    uint8_t swz[4] = {0, 1, 2, 3}; // source swizzle
    uint8_t mask = 0xF;            // destination write mask
    uint8_t mod = 0;               // source modifier or destination result modifier
    bool rel = false;              // relative addressing: index += rel register component
    uint8_t rel_type = 0, rel_comp = 0;
};

struct Inst {
    uint16_t op = 0;
    uint8_t ctrl = 0; // opcode-specific controls (comparison, texld flags)
    bool has_dst = false;
    Param dst;
    std::array<Param, 4> src;
    uint8_t nsrc = 0;
};

uint8_t reg_type(uint32_t t) { return (uint8_t)(((t >> 28) & 7) | ((t >> 8) & 0x18)); }

Param decode_src(uint32_t t) {
    Param p;
    p.type = reg_type(t);
    p.num = t & 0x7FF;
    for (int i = 0; i < 4; i++) p.swz[i] = (t >> (16 + 2 * i)) & 3;
    p.mod = (t >> 24) & 0xF;
    p.rel = (t & 0x2000) != 0;
    return p;
}
Param decode_dst(uint32_t t) {
    Param p;
    p.type = reg_type(t);
    p.num = t & 0x7FF;
    p.mask = (t >> 16) & 0xF;
    p.mod = (t >> 20) & 0xF;
    p.rel = (t & 0x2000) != 0;
    return p;
}

std::string fmt(const char *f, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, f);
    vsnprintf(buf, sizeof(buf), f, ap);
    va_end(ap);
    return buf;
}

const char kXYZW[] = "xyzw";
std::string mask_str(uint8_t m) {
    std::string s;
    for (int i = 0; i < 4; i++)
        if (m & (1 << i)) s += kXYZW[i];
    return s;
}
int popcount4(uint8_t m) { return (m & 1) + ((m >> 1) & 1) + ((m >> 2) & 1) + ((m >> 3) & 1); }

struct Translator {
    std::span<const uint32_t> code;
    const Options &opts;
    Result res;
    bool vs = true;
    std::vector<Inst> insts;
    std::map<unsigned, std::array<uint32_t, 4>> def_f, def_i;
    std::map<unsigned, bool> def_b;
    std::set<unsigned> temps;
    std::map<unsigned, Semantic> in_sem, out_sem; // by register number
    std::map<unsigned, uint8_t> sampler_type;
    bool uses_a0 = false, uses_aL = false;
    std::string body;
    int indent = 1;

    explicit Translator(std::span<const uint32_t> c, const Options &o) : code(c), opts(o) {}

    bool fail(const std::string &why) {
        if (res.error.empty()) res.error = why;
        return false;
    }
    void line(const std::string &s) {
        body.append((size_t)indent * 4, ' ');
        body += s;
        body += '\n';
    }

    // ---- pass 1: decode ----------------------------------------------------------------
    bool decode() {
        if (code.size() < 2) return fail("too short");
        uint32_t ver = code[0];
        if ((ver >> 16) == 0xFFFE) vs = true;
        else if ((ver >> 16) == 0xFFFF) vs = false;
        else return fail("not a shader version token");
        res.stage = vs ? Stage::Vertex : Stage::Pixel;
        res.major = (ver >> 8) & 0xFF;
        res.minor = ver & 0xFF;
        if (res.major < 2 || res.major > 3) return fail(fmt("unsupported shader model %u.%u", res.major, res.minor));
        size_t i = 1;
        while (i < code.size()) {
            uint32_t t = code[i];
            unsigned op = t & 0xFFFF;
            if (op == END) return true;
            if (op == COMMENT) {
                size_t n = (t >> 16) & 0x7FFF;
                if (i + 1 + n > code.size()) return fail("comment overruns shader");
                if (n >= 3 && code[i + 1] == 0x42415443) { // 'CTAB'
                    const char *base = reinterpret_cast<const char *>(&code[i + 2]);
                    size_t bytes = (n - 1) * 4;
                    uint32_t creator = code[i + 3]; // D3DXSHADER_CONSTANTTABLE.Creator offset
                    if (creator < bytes) res.creator.assign(base + creator, strnlen(base + creator, bytes - creator));
                }
                i += 1 + n;
                continue;
            }
            size_t len = (t >> 24) & 0xF;
            if (i + 1 + len > code.size()) return fail("instruction overruns shader");
            const uint32_t *p = &code[i + 1];
            if (t & 0x10000000) return fail("predicated instruction");
            if (op == DCL) {
                if (len != 2) return fail("bad dcl");
                uint32_t usage = p[0];
                Param d = decode_dst(p[1]);
                if (d.type == kSampler) {
                    sampler_type[d.num] = (usage >> 27) & 0xF;
                } else if (d.type == kInput || (!vs && d.type == kAddrOrTex)) {
                    Semantic s;
                    s.reg = d.num;
                    s.mask = d.mask;
                    if (vs || res.major >= 3) {
                        s.usage = usage & 0x1F;
                        s.index = (usage >> 16) & 0xF;
                    } else { // ps_2_x: v# = COLOR#, t# = TEXCOORD#
                        s.usage = d.type == kInput ? (uint8_t)kColor : (uint8_t)kTexCoord;
                        s.index = (uint8_t)d.num;
                        if (d.type == kAddrOrTex) s.reg = 16 + d.num; // keep t# apart from v#
                    }
                    in_sem[s.reg] = s;
                } else if (d.type == kOutput && vs) {
                    Semantic s;
                    s.reg = d.num;
                    s.mask = d.mask;
                    s.usage = usage & 0x1F;
                    s.index = (usage >> 16) & 0xF;
                    out_sem[d.num] = s;
                } else if (d.type == kMisc) {
                    // vPos / vFace need no declaration data
                } else
                    return fail(fmt("dcl of register type %u", d.type));
            } else if (op == DEF) {
                if (len != 5) return fail("bad def");
                def_f[decode_dst(p[0]).num] = {p[1], p[2], p[3], p[4]};
            } else if (op == DEFI) {
                if (len != 5) return fail("bad defi");
                def_i[decode_dst(p[0]).num] = {p[1], p[2], p[3], p[4]};
            } else if (op == DEFB) {
                if (len != 2) return fail("bad defb");
                def_b[decode_dst(p[0]).num] = p[1] != 0;
            } else if (op != NOP) {
                Inst in;
                in.op = (uint16_t)op;
                in.ctrl = (t >> 16) & 0xFF;
                size_t k = 0;
                bool dst = !(op == REP || op == ENDREP || op == IF || op == IFC || op == ELSE || op == ENDIF ||
                             op == BREAK || op == BREAKC || op == LOOP || op == ENDLOOP || op == RET ||
                             op == LABEL || op == CALL || op == CALLNZ);
                auto take_rel = [&](Param &q) -> bool {
                    if (!q.rel) return true;
                    if (k >= len) return false;
                    Param r = decode_src(p[k++]);
                    q.rel_type = r.type;
                    q.rel_comp = r.swz[0];
                    return true;
                };
                if (dst && k < len) {
                    in.has_dst = true;
                    in.dst = decode_dst(p[k++]);
                    if (!take_rel(in.dst)) return fail("truncated relative destination");
                    if (in.dst.rel) return fail("relative destination addressing");
                }
                while (k < len) {
                    if (in.nsrc >= 4) return fail(fmt("%s: too many operands", op_name(op)));
                    Param s = decode_src(p[k++]);
                    if (!take_rel(s)) return fail("truncated relative source");
                    in.src[in.nsrc++] = s;
                }
                insts.push_back(in);
            }
            i += 1 + len;
        }
        return fail("missing END token");
    }

    // ---- operand rendering ---------------------------------------------------------------
    std::string const_ref(const Param &p) {
        if (p.rel) {
            res.uses_relative_consts = true;
            std::string idx;
            if (p.rel_type == kLoop) {
                uses_aL = true;
                idx = fmt("%u + aL", p.num);
            } else {
                uses_a0 = true;
                idx = fmt("%u + a0.%c", p.num, kXYZW[p.rel_comp]);
            }
            return fmt("u.c[clamp(%s, 0, %u)]", idx.c_str(), kMaxFloatConsts - 1);
        }
        if (def_f.count(p.num)) return fmt("c%u_def", p.num);
        res.float_consts_used = std::max<uint32_t>(res.float_consts_used, p.num + 1u);
        if (p.num >= kMaxFloatConsts) fail(fmt("constant register c%u out of range", p.num));
        return fmt("u.c[%u]", p.num);
    }

    bool src_base(const Param &p, std::string &out) {
        switch (p.type) {
        case kTemp:
            temps.insert(p.num);
            out = fmt("r%u", p.num);
            return true;
        case kInput:
            out = fmt("v%u", p.num);
            if (!in_sem.count(p.num)) return fail(fmt("input v%u used without dcl", p.num));
            return true;
        case kConst:
            out = const_ref(p);
            return res.error.empty();
        case kConstInt:
            out = def_i.count(p.num) ? fmt("float4(i%u_def)", p.num) : fmt("float4(u.i[%u])", p.num & 15);
            return true;
        case kAddrOrTex:
            if (vs) {
                uses_a0 = true;
                out = "float4(a0)";
            } else {
                out = fmt("v%u", 16 + p.num); // ps_2_x texture coordinate register t#
                if (!in_sem.count(16 + p.num)) return fail(fmt("t%u used without dcl", p.num));
            }
            return true;
        case kLoop:
            uses_aL = true;
            out = "float4(aL)";
            return true;
        case kMisc:
            if (p.num == 0) {
                res.uses_vpos = true;
                out = "vPos";
            } else if (p.num == 1) {
                res.uses_vface = true;
                out = "vFace";
            } else
                return fail("unknown misc register");
            return true;
        default:
            return fail(fmt("source register type %u", p.type));
        }
    }

    // float4-valued expression for a source operand.
    bool src(const Param &p, std::string &out) {
        std::string e;
        if (!src_base(p, e)) return false;
        if (!(p.swz[0] == 0 && p.swz[1] == 1 && p.swz[2] == 2 && p.swz[3] == 3)) {
            e += '.';
            for (int i = 0; i < 4; i++) e += kXYZW[p.swz[i]];
        }
        switch (p.mod) {
        case 0: break;
        case 1: e = "(-" + e + ")"; break;
        case 11: e = "abs(" + e + ")"; break;
        case 12: e = "(-abs(" + e + "))"; break;
        default: return fail(fmt("source modifier %u", p.mod));
        }
        out = e;
        return true;
    }

    bool dst_name(const Param &d, std::string &out) {
        switch (d.type) {
        case kTemp:
            temps.insert(d.num);
            out = fmt("r%u", d.num);
            return true;
        case kOutput: // vs_3_0 o#; vs_2_x oT#
            if (!vs) return fail("output register in a pixel shader");
            if (res.major < 3 && !out_sem.count(d.num)) {
                Semantic s;
                s.reg = d.num, s.usage = kTexCoord, s.index = (uint8_t)d.num;
                out_sem[d.num] = s;
            }
            if (!out_sem.count(d.num)) return fail(fmt("output o%u written without dcl", d.num));
            out = fmt("o%u", d.num);
            return true;
        case kRastOut: { // vs_2_x: 0 position, 1 fog, 2 point size
            if (!vs) return fail("rastout in a pixel shader");
            unsigned reg = 32 + d.num;
            if (!out_sem.count(reg)) {
                Semantic s;
                s.reg = (uint16_t)reg;
                s.usage = d.num == 0 ? kPosition : d.num == 1 ? kFog : kPSize;
                out_sem[reg] = s;
            }
            out = fmt("o%u", reg);
            return true;
        }
        case kAttrOut: { // vs_2_x oD#
            unsigned reg = 40 + d.num;
            if (!out_sem.count(reg)) {
                Semantic s;
                s.reg = (uint16_t)reg, s.usage = kColor, s.index = (uint8_t)d.num;
                out_sem[reg] = s;
            }
            out = fmt("o%u", reg);
            return true;
        }
        case kColorOut:
            if (d.num != 0) return fail("multiple render targets");
            out = "oC0";
            return true;
        case kDepthOut:
            res.writes_depth = true;
            out = "oDepth";
            return true;
        case kAddrOrTex:
            if (vs) {
                uses_a0 = true;
                out = "a0";
                return true;
            }
            return fail("write to a texture coordinate register");
        default:
            return fail(fmt("destination register type %u", d.type));
        }
    }

    // dst.mask = expr (expr is float4-valued).
    bool assign(const Inst &in, std::string expr) {
        std::string name;
        if (!dst_name(in.dst, name)) return false;
        const Param &d = in.dst;
        if (d.mod & 1) expr = "saturate(" + expr + ")";
        if (d.mod & ~7u) return fail("destination shift modifier");
        if (d.type == kDepthOut) {
            line(fmt("oDepth = (%s).x;", expr.c_str()));
            return true;
        }
        if (d.type == kAddrOrTex) { // mova: round to nearest
            std::string m = mask_str(d.mask);
            line(fmt("a0.%s = int4(floor(%s + 0.5)).%s;", m.c_str(), expr.c_str(), m.c_str()));
            return true;
        }
        if (d.mask == 0xF) line(fmt("%s = %s;", name.c_str(), expr.c_str()));
        else {
            std::string m = mask_str(d.mask);
            line(fmt("%s.%s = (%s).%s;", name.c_str(), m.c_str(), expr.c_str(), m.c_str()));
        }
        return true;
    }

    static const char *cmp_op(unsigned c) {
        switch (c & 7) {
        case 1: return ">";
        case 2: return "==";
        case 3: return ">=";
        case 4: return "<";
        case 5: return "!=";
        case 6: return "<=";
        default: return nullptr;
        }
    }

    bool tex(const Inst &in) {
        if (in.nsrc < 2 || in.src[1].type != kSampler) return fail("texture op without sampler operand");
        unsigned s = in.src[1].num;
        if (!sampler_type.count(s)) return fail(fmt("sampler s%u used without dcl", s));
        unsigned type = sampler_type[s];
        std::string c, dx, dy;
        if (!src(in.src[0], c)) return false;
        bool shadow = (opts.shadow_samplers >> s) & 1;
        bool project = in.op == TEX && (in.ctrl & 1), bias = in.op == TEX && (in.ctrl & 2);
        std::string coord, e;
        if (type == 2) coord = project ? "((" + c + ").xy / (" + c + ").w)" : "(" + c + ").xy";
        else if (type == 3 || type == 4) coord = project ? "((" + c + ").xyz / (" + c + ").w)" : "(" + c + ").xyz";
        else return fail(fmt("sampler type %u", type));
        std::string extra;
        if (in.op == TEXLDL) extra = ", level((" + c + ").w)";
        else if (bias) extra = ", bias((" + c + ").w)";
        else if (in.op == TEXLDD) {
            if (in.nsrc < 4 || !src(in.src[2], dx) || !src(in.src[3], dy)) return fail("bad texldd");
            const char *g = type == 2 ? "gradient2d" : type == 3 ? "gradientcube" : "gradient3d";
            const char *sw = type == 2 ? "xy" : "xyz";
            extra = fmt(", %s((%s).%s, (%s).%s)", g, dx.c_str(), sw, dy.c_str(), sw);
        }
        if (shadow) {
            if (type != 2) return fail("shadow sampling on a non-2D sampler");
            std::string z = project ? "((" + c + ").z / (" + c + ").w)" : "(" + c + ").z";
            e = fmt("float4(t%u.sample_compare(s%u, %s, %s))", s, s, coord.c_str(), z.c_str());
        } else
            e = fmt("t%u.sample(s%u, %s%s)", s, s, coord.c_str(), extra.c_str());
        const Param &sp = in.src[1]; // the sampler operand's swizzle applies to the fetched colour
        if (!(sp.swz[0] == 0 && sp.swz[1] == 1 && sp.swz[2] == 2 && sp.swz[3] == 3)) {
            e += '.';
            for (int i = 0; i < 4; i++) e += kXYZW[sp.swz[i]];
        }
        return assign(in, e);
    }

    // ---- pass 2: emit one instruction -----------------------------------------------------
    bool emit(const Inst &in) {
        std::string a, b, c;
        auto S = [&](int n, std::string &o) { return n < in.nsrc ? src(in.src[n], o) : fail(fmt("%s: missing operand", op_name(in.op))); };
        res.instruction_count++;
        switch (in.op) {
        case MOV: return S(0, a) && assign(in, a);
        case MOVA: return S(0, a) && assign(in, a);
        case ADD: return S(0, a) && S(1, b) && assign(in, "(" + a + " + " + b + ")");
        case SUB: return S(0, a) && S(1, b) && assign(in, "(" + a + " - " + b + ")");
        case MUL: return S(0, a) && S(1, b) && assign(in, "(" + a + " * " + b + ")");
        case MAD: return S(0, a) && S(1, b) && S(2, c) && assign(in, "(" + a + " * " + b + " + " + c + ")");
        case RCP: return S(0, a) && assign(in, "float4(1.0 / (" + a + ").x)");
        case RSQ: return S(0, a) && assign(in, "float4(rsqrt(abs((" + a + ").x)))");
        case DP3: return S(0, a) && S(1, b) && assign(in, "float4(dot((" + a + ").xyz, (" + b + ").xyz))");
        case DP4: return S(0, a) && S(1, b) && assign(in, "float4(dot(" + a + ", " + b + "))");
        case MIN: return S(0, a) && S(1, b) && assign(in, "min(" + a + ", " + b + ")");
        case MAX: return S(0, a) && S(1, b) && assign(in, "max(" + a + ", " + b + ")");
        case SLT: return S(0, a) && S(1, b) && assign(in, "select(float4(0.0), float4(1.0), " + a + " < " + b + ")");
        case SGE: return S(0, a) && S(1, b) && assign(in, "select(float4(0.0), float4(1.0), " + a + " >= " + b + ")");
        case EXP: return S(0, a) && assign(in, "exp2(" + a + ")");
        case LOG: return S(0, a) && assign(in, "log2(abs(" + a + "))");
        case FRC: return S(0, a) && assign(in, "fract(" + a + ")");
        case ABS: return S(0, a) && assign(in, "abs(" + a + ")");
        case LRP: return S(0, a) && S(1, b) && S(2, c) && assign(in, "mix(" + c + ", " + b + ", " + a + ")");
        case POW: return S(0, a) && S(1, b) && assign(in, "float4(pow(abs((" + a + ").x), (" + b + ").x))");
        case CRS: return S(0, a) && S(1, b) && assign(in, "float4(cross((" + a + ").xyz, (" + b + ").xyz), 0.0)");
        case NRM: return S(0, a) && assign(in, "(" + a + " * rsqrt(dot((" + a + ").xyz, (" + a + ").xyz)))");
        case SINCOS: return S(0, a) && assign(in, "float4(cos((" + a + ").x), sin((" + a + ").x), 0.0, 0.0)");
        case CMP: return S(0, a) && S(1, b) && S(2, c) && assign(in, "select(" + c + ", " + b + ", " + a + " >= float4(0.0))");
        case DP2ADD: return S(0, a) && S(1, b) && S(2, c) && assign(in, "float4(dot((" + a + ").xy, (" + b + ").xy) + (" + c + ").x)");
        case DSX: return S(0, a) && assign(in, "dfdx(" + a + ")");
        case DSY: return S(0, a) && assign(in, "dfdy(" + a + ")");
        case TEX: case TEXLDL: case TEXLDD: return tex(in);
        case TEXKILL: {
            std::string name;
            if (!dst_name(in.dst, name)) return false;
            res.uses_discard = true;
            std::string m = mask_str(in.dst.mask);
            if (popcount4(in.dst.mask) == 1) line(fmt("if (%s.%s < 0.0) discard_fragment();", name.c_str(), m.c_str()));
            else line(fmt("if (any(%s.%s < 0.0)) discard_fragment();", name.c_str(), m.c_str()));
            return true;
        }
        case IF: {
            if (in.nsrc < 1 || in.src[0].type != kConstBool) return fail("if without a bool register");
            unsigned n = in.src[0].num;
            std::string cond = def_b.count(n) ? (def_b[n] ? "true" : "false") : fmt("(u.b[%u] != 0u)", n & 15);
            if (in.src[0].mod == 13) cond = "!" + cond;
            line("if (" + cond + ") {");
            indent++;
            return true;
        }
        case IFC: {
            const char *o = cmp_op(in.ctrl);
            if (!o) return fail("ifc comparison");
            if (!S(0, a) || !S(1, b)) return false;
            line(fmt("if ((%s).x %s (%s).x) {", a.c_str(), o, b.c_str()));
            indent++;
            return true;
        }
        case ELSE:
            if (indent < 2) return fail("else without if");
            indent--;
            line("} else {");
            indent++;
            return true;
        case ENDIF: case ENDREP:
            if (indent < 2) return fail("unbalanced block end");
            indent--;
            line("}");
            return true;
        case REP: {
            if (in.nsrc < 1 || in.src[0].type != kConstInt) return fail("rep without an int register");
            unsigned n = in.src[0].num;
            std::string count = def_i.count(n) ? fmt("i%u_def.x", n) : fmt("u.i[%u].x", n & 15);
            line(fmt("for (int rep%d = 0; rep%d < min(%s, 255); ++rep%d) {", indent, indent, count.c_str(), indent));
            indent++;
            return true;
        }
        case BREAK:
            line("break;");
            return true;
        case BREAKC: {
            const char *o = cmp_op(in.ctrl);
            if (!o) return fail("breakc comparison");
            if (!S(0, a) || !S(1, b)) return false;
            line(fmt("if ((%s).x %s (%s).x) break;", a.c_str(), o, b.c_str()));
            return true;
        }
        default:
            return fail(fmt("unsupported instruction %s", op_name(in.op)));
        }
    }

    static std::string varying_name(const Semantic &s) { return fmt("%s%u", usage_name(s.usage), s.index); }
    static bool is_varying(const Semantic &s) {
        return (s.usage == kTexCoord && s.index < kTexCoordVaryings) || (s.usage == kColor && s.index < kColorVaryings);
    }

    // ---- assemble the MSL translation unit --------------------------------------------------
    bool build() {
        if (!decode()) return false;
        for (const Inst &in : insts)
            if (!emit(in)) return false;
        if (indent != 1) return fail("unterminated block");

        std::string m = "#include <metal_stdlib>\nusing namespace metal;\n\n";
        m += fmt("// %s_%u_%u, %u instructions. Generated by bo1 sm3 translator.\n", vs ? "vs" : "ps", res.major, res.minor, res.instruction_count);
        m += fmt("struct Consts {\n    float4 c[%u];\n    int4 i[16];\n    uint b[16];\n    float4 extra; // vs: half-pixel NDC offset (x, y); ps: alpha ref, alpha func\n};\n\n", kMaxFloatConsts);

        std::string decls;
        for (auto &[n, v] : def_f) decls += fmt("    const float4 c%u_def = as_type<float4>(uint4(0x%08xu, 0x%08xu, 0x%08xu, 0x%08xu));\n", n, v[0], v[1], v[2], v[3]);
        for (auto &[n, v] : def_i) decls += fmt("    const int4 i%u_def = int4(%d, %d, %d, %d);\n", n, (int)v[0], (int)v[1], (int)v[2], (int)v[3]);
        for (unsigned t : temps) decls += fmt("    float4 r%u = float4(0.0);\n", t);
        if (uses_a0) decls += "    int4 a0 = int4(0);\n";
        if (uses_aL) return fail("loop register aL");

        if (vs) {
            bool has_pos = false;
            m += "struct VSIn {\n";
            for (auto &[reg, s] : in_sem) {
                if (reg > 15) return fail("vertex input register out of range");
                m += fmt("    float4 v%u [[attribute(%u)]]; // %s%u\n", reg, reg, usage_name(s.usage), s.index);
                res.inputs.push_back(s);
            }
            m += "};\nstruct VSOut {\n    float4 position [[position]];\n";
            for (unsigned i = 0; i < kTexCoordVaryings; i++) m += fmt("    float4 TEXCOORD%u [[user(TEXCOORD%u)]];\n", i, i);
            for (unsigned i = 0; i < kColorVaryings; i++) m += fmt("    float4 COLOR%u [[user(COLOR%u)]];\n", i, i);
            m += "};\n\n";
            m += fmt("vertex VSOut %s(%sconstant Consts &u [[buffer(%u)]]) {\n", opts.entry.c_str(),
                     in_sem.empty() ? "" : "VSIn in [[stage_in]], ", kConstBufferIndex);
            for (auto &[reg, s] : in_sem) decls += fmt("    float4 v%u = in.v%u;\n", reg, reg);
            for (auto &[reg, s] : out_sem) decls += fmt("    float4 o%u = float4(0.0);\n", reg);
            m += decls + body;
            m += "    VSOut out = {};\n";
            for (auto &[reg, s] : out_sem) {
                res.outputs.push_back(s);
                if (s.usage == kPosition && s.index == 0) {
                    m += fmt("    out.position = o%u;\n", reg);
                    has_pos = true;
                } else if (is_varying(s))
                    m += fmt("    out.%s = o%u;\n", varying_name(s).c_str(), reg);
                else if (s.usage == kFog || s.usage == kPSize) {
                    // no fog or point-sprite state is ever set by the game: dropped
                } else
                    return fail("vertex output semantic " + varying_name(s));
            }
            if (!has_pos) return fail("vertex shader does not write POSITION");
            // D3D9 pixel centres are at integer coordinates, Metal's at +0.5.
            m += "    out.position.xy += u.extra.xy * out.position.w;\n    return out;\n}\n";
        } else {
            m += "struct PSIn {\n    float4 position [[position]];\n";
            std::set<std::string> seen;
            for (auto &[reg, s] : in_sem) {
                if (!is_varying(s)) return fail("pixel input semantic " + varying_name(s));
                res.inputs.push_back(s);
                std::string n = varying_name(s);
                if (seen.insert(n).second) m += fmt("    float4 %s [[user(%s)]];\n", n.c_str(), n.c_str());
                decls += fmt("    float4 v%u = in.%s;\n", reg, n.c_str());
            }
            if (res.uses_vface) m += "    bool front [[front_facing]];\n";
            m += "};\nstruct PSOut {\n    float4 c0 [[color(0)]];\n";
            if (res.writes_depth) m += "    float depth [[depth(any)]];\n";
            m += "};\n\n";
            m += fmt("fragment PSOut %s(PSIn in [[stage_in]], constant Consts &u [[buffer(%u)]]", opts.entry.c_str(), kConstBufferIndex);
            for (auto &[s, type] : sampler_type) {
                if (s > 15) return fail("sampler register out of range");
                bool shadow = (opts.shadow_samplers >> s) & 1;
                const char *tt = shadow ? "depth2d<float>" : type == 2 ? "texture2d<float>" : type == 3 ? "texturecube<float>" : type == 4 ? "texture3d<float>" : nullptr;
                if (!tt) return fail(fmt("sampler type %u", type));
                m += fmt(",\n        %s t%u [[texture(%u)]], sampler s%u [[sampler(%u)]]", tt, s, s, s, s);
                res.samplers.push_back({(uint8_t)s, (uint8_t)type});
            }
            m += ") {\n";
            if (res.uses_vpos) decls += "    float4 vPos = float4(in.position.xy - 0.5, 0.0, 0.0);\n";
            if (res.uses_vface) decls += "    float4 vFace = float4(in.front ? 1.0 : -1.0);\n";
            decls += "    float4 oC0 = float4(0.0);\n";
            if (res.writes_depth) decls += "    float oDepth = 0.0;\n";
            m += decls + body;
            // D3DRS_ALPHATESTENABLE / ALPHAFUNC / ALPHAREF: extra.y = 0 (off) or D3DCMPFUNC 1..8.
            m += "    if (u.extra.y != 0.0) {\n"
                 "        float a = oC0.w, ref = u.extra.x;\n"
                 "        int f = int(u.extra.y);\n"
                 "        bool pass = f == 8 || (f == 2 && a < ref) || (f == 3 && a == ref) || (f == 4 && a <= ref) ||\n"
                 "                    (f == 5 && a > ref) || (f == 6 && a != ref) || (f == 7 && a >= ref);\n"
                 "        if (!pass) discard_fragment();\n"
                 "    }\n";
            m += "    PSOut out;\n    out.c0 = oC0;\n";
            if (res.writes_depth) m += "    out.depth = oDepth;\n";
            m += "    return out;\n}\n";
        }
        res.msl = std::move(m);
        res.ok = true;
        return true;
    }
};

} // namespace

const char *usage_name(uint8_t usage) {
    static const char *const names[] = {"POSITION", "BLENDWEIGHT", "BLENDINDICES", "NORMAL", "PSIZE", "TEXCOORD",
                                        "TANGENT", "BINORMAL", "TESSFACTOR", "POSITIONT", "COLOR", "FOG",
                                        "DEPTH", "SAMPLE"};
    return usage < sizeof(names) / sizeof(names[0]) ? names[usage] : "USAGE?";
}

Result translate(std::span<const uint32_t> code, const Options &opts) {
    Translator t(code, opts);
    t.build();
    if (!t.res.ok) t.res.msl.clear();
    return std::move(t.res);
}

} // namespace bo1::sm3
