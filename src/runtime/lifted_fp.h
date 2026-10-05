// Floating-point / SIMD support for GENERATED lifted code: x87 stack, SSE/SSE2 scalar and packed,
// MMX. Included at the end of lifted.h. Model and its known deviations: docs/LIFTER.md §5.
#pragma once
#include <math.h>

// LIFT_EXACT_NAN=1 reproduces x86 NaN results bit for bit (default NaN is NEGATIVE on x86,
// positive on arm64; operand priority differs). Costs one predictable branch per FP operation.
#ifndef LIFT_EXACT_NAN
#define LIFT_EXACT_NAN 1
#endif

typedef float __attribute__((aligned(1), may_alias)) f32a;
typedef double __attribute__((aligned(1), may_alias)) f64a;
#define MF32(a) (*(f32a *)(g_mem + (uint32_t)(a)))
#define MF64(a) (*(f64a *)(g_mem + (uint32_t)(a)))

LIFT_INLINE Xmm ldx(uint32_t a) { Xmm x; memcpy(&x, g_mem + a, 16); return x; }
LIFT_INLINE void stx(uint32_t a, Xmm x) { memcpy(g_mem + a, &x, 16); }
LIFT_INLINE Xmm x_zero(void) { Xmm x; x.u64[0] = 0; x.u64[1] = 0; return x; }
LIFT_INLINE Xmm x_from64(uint64_t v) { Xmm x; x.u64[0] = v; x.u64[1] = 0; return x; }

LIFT_INLINE uint32_t f32_bits(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }
LIFT_INLINE float bits_f32(uint32_t u) { float f; memcpy(&f, &u, 4); return f; }
LIFT_INLINE uint64_t f64_bits(double f) { uint64_t u; memcpy(&u, &f, 8); return u; }
LIFT_INLINE double bits_f64(uint64_t u) { double f; memcpy(&f, &u, 8); return f; }

// ---- NaN fix-up (x86 rule: first NaN operand, quieted; otherwise the negative default NaN) --------
static inline float fix_nan32(float a, float b) {
    if (a != a) return bits_f32(f32_bits(a) | 0x00400000u);
    if (b != b) return bits_f32(f32_bits(b) | 0x00400000u);
    return bits_f32(0xFFC00000u);
}
static inline double fix_nan64(double a, double b) {
    if (a != a) return bits_f64(f64_bits(a) | 0x0008000000000000ull);
    if (b != b) return bits_f64(f64_bits(b) | 0x0008000000000000ull);
    return bits_f64(0xFFF8000000000000ull);
}
#if LIFT_EXACT_NAN
#define NANFIX32(r, a, b) do { if (__builtin_expect((r) != (r), 0)) (r) = fix_nan32((a), (b)); } while (0)
#define NANFIX64(r, a, b) do { if (__builtin_expect((r) != (r), 0)) (r) = fix_nan64((a), (b)); } while (0)
#else
#define NANFIX32(r, a, b) ((void)0)
#define NANFIX64(r, a, b) ((void)0)
#endif

#define DEF_FOP(sfx, T, FIX, SQRT)                                                                  \
    LIFT_INLINE T add##sfx(T a, T b) { T r = a + b; FIX(r, a, b); return r; }                       \
    LIFT_INLINE T sub##sfx(T a, T b) { T r = a - b; FIX(r, a, b); return r; }                       \
    LIFT_INLINE T mul##sfx(T a, T b) { T r = a * b; FIX(r, a, b); return r; }                       \
    LIFT_INLINE T div##sfx(T a, T b) { T r = a / b; FIX(r, a, b); return r; }                       \
    LIFT_INLINE T sqrt##sfx(T a, T b) { (void)a; T r = SQRT(b); FIX(r, b, b); return r; }           \
    /* MINSS/MAXSS: the SECOND operand is returned when either is NaN or both are zero */           \
    LIFT_INLINE T min##sfx(T a, T b) { return a < b ? a : b; }                                      \
    LIFT_INLINE T max##sfx(T a, T b) { return a > b ? a : b; }
DEF_FOP(ss, float, NANFIX32, sqrtf)
DEF_FOP(sd, double, NANFIX64, sqrt)
// RCPSS / RSQRTSS are table-driven 12-bit approximations on x86; exact values are used here.
LIFT_INLINE float rcpss(float a, float b) { (void)a; return 1.0f / b; }
LIFT_INLINE float rsqrtss(float a, float b) { (void)a; return 1.0f / sqrtf(b); }

// COMISS/UCOMISS/COMISD/UCOMISD and FCOMI/FUCOMI: unordered -> ZF=PF=CF=1.
LIFT_INLINE void fp_comi(Flags *f, double a, double b) {
    f->of = f->sf = f->af = 0;
    if (a != a || b != b) f->zf = f->pf = f->cf = 1;
    else f->zf = a == b, f->pf = 0, f->cf = a < b;
}
// CMPSS/CMPSD/CMPPS/CMPPD predicates 0..7 -> boolean
LIFT_INLINE int fp_cmp(int pred, double a, double b) {
    switch (pred & 7) {
    case 0: return a == b;
    case 1: return a < b;
    case 2: return a <= b;
    case 3: return a != a || b != b;
    case 4: return !(a == b);
    case 5: return !(a < b);
    case 6: return !(a <= b);
    default: return !(a != a || b != b);
    }
}

// ---- float -> integer, x86 semantics (out of range / NaN -> "integer indefinite") -----------------
LIFT_INLINE double mx_round(const Cpu *cpu, double v) { // MXCSR.RC
    switch ((cpu->mxcsr >> 13) & 3) {
    case 0: return rint(v); // host stays in round-to-nearest-even
    case 1: return floor(v);
    case 2: return ceil(v);
    default: return trunc(v);
    }
}
LIFT_INLINE int32_t fp_to_i32(double r) { return (r >= -2147483648.0 && r < 2147483648.0) ? (int32_t)r : INT32_MIN; }
LIFT_INLINE int64_t fp_to_i64(double r) {
    return (r >= -9223372036854775808.0 && r < 9223372036854775808.0) ? (int64_t)r : INT64_MIN;
}
LIFT_INLINE int16_t fp_to_i16(double r) { return (r >= -32768.0 && r < 32768.0) ? (int16_t)r : INT16_MIN; }
#define cvtt2si(v) fp_to_i32(trunc((double)(v)))
#define cvt2si(cpu, v) fp_to_i32(mx_round((cpu), (double)(v)))
LIFT_INLINE float cvtsd2ss(double v) { float r = (float)v; return r; }

// ---- packed helpers ------------------------------------------------------------------------------------
#define PK2(name, field, n, expr)                                                                   \
    LIFT_INLINE Xmm name(Xmm a, Xmm b) {                                                            \
        Xmm r = a;                                                                                  \
        for (int i = 0; i < n; i++) r.field[i] = (expr);                                            \
        return r;                                                                                   \
    }
PK2(x_addps, f32, 4, addss(a.f32[i], b.f32[i]))
PK2(x_subps, f32, 4, subss(a.f32[i], b.f32[i]))
PK2(x_mulps, f32, 4, mulss(a.f32[i], b.f32[i]))
PK2(x_divps, f32, 4, divss(a.f32[i], b.f32[i]))
PK2(x_minps, f32, 4, minss(a.f32[i], b.f32[i]))
PK2(x_maxps, f32, 4, maxss(a.f32[i], b.f32[i]))
PK2(x_sqrtps, f32, 4, sqrtss(a.f32[i], b.f32[i]))
PK2(x_rcpps, f32, 4, rcpss(a.f32[i], b.f32[i]))
PK2(x_rsqrtps, f32, 4, rsqrtss(a.f32[i], b.f32[i]))
PK2(x_addpd, f64, 2, addsd(a.f64[i], b.f64[i]))
PK2(x_subpd, f64, 2, subsd(a.f64[i], b.f64[i]))
PK2(x_mulpd, f64, 2, mulsd(a.f64[i], b.f64[i]))
PK2(x_divpd, f64, 2, divsd(a.f64[i], b.f64[i]))
PK2(x_minpd, f64, 2, minsd(a.f64[i], b.f64[i]))
PK2(x_maxpd, f64, 2, maxsd(a.f64[i], b.f64[i]))
PK2(x_sqrtpd, f64, 2, sqrtsd(a.f64[i], b.f64[i]))
PK2(x_and, u64, 2, a.u64[i] & b.u64[i])
PK2(x_andn, u64, 2, ~a.u64[i] & b.u64[i])
PK2(x_or, u64, 2, a.u64[i] | b.u64[i])
PK2(x_xor, u64, 2, a.u64[i] ^ b.u64[i])
PK2(x_paddb, u8, 16, (uint8_t)(a.u8[i] + b.u8[i]))
PK2(x_paddw, u16, 8, (uint16_t)(a.u16[i] + b.u16[i]))
PK2(x_paddd, u32, 4, a.u32[i] + b.u32[i])
PK2(x_paddq, u64, 2, a.u64[i] + b.u64[i])
PK2(x_psubb, u8, 16, (uint8_t)(a.u8[i] - b.u8[i]))
PK2(x_psubw, u16, 8, (uint16_t)(a.u16[i] - b.u16[i]))
PK2(x_psubd, u32, 4, a.u32[i] - b.u32[i])
PK2(x_psubq, u64, 2, a.u64[i] - b.u64[i])
LIFT_INLINE int32_t clampi(int32_t v, int32_t lo, int32_t hi) { return v < lo ? lo : v > hi ? hi : v; }
PK2(x_paddsb, i8, 16, (int8_t)clampi(a.i8[i] + b.i8[i], -128, 127))
PK2(x_paddsw, i16, 8, (int16_t)clampi(a.i16[i] + b.i16[i], -32768, 32767))
PK2(x_paddusb, u8, 16, (uint8_t)clampi(a.u8[i] + b.u8[i], 0, 255))
PK2(x_paddusw, u16, 8, (uint16_t)clampi(a.u16[i] + b.u16[i], 0, 65535))
PK2(x_psubsb, i8, 16, (int8_t)clampi(a.i8[i] - b.i8[i], -128, 127))
PK2(x_psubsw, i16, 8, (int16_t)clampi(a.i16[i] - b.i16[i], -32768, 32767))
PK2(x_psubusb, u8, 16, (uint8_t)clampi(a.u8[i] - b.u8[i], 0, 255))
PK2(x_psubusw, u16, 8, (uint16_t)clampi(a.u16[i] - b.u16[i], 0, 65535))
PK2(x_pmullw, u16, 8, (uint16_t)(a.i16[i] * b.i16[i]))
PK2(x_pmulhw, u16, 8, (uint16_t)((a.i16[i] * b.i16[i]) >> 16))
PK2(x_pmulhuw, u16, 8, (uint16_t)(((uint32_t)a.u16[i] * b.u16[i]) >> 16))
PK2(x_pmaddwd, i32, 4, a.i16[2 * i] * b.i16[2 * i] + a.i16[2 * i + 1] * b.i16[2 * i + 1])
PK2(x_pmuludq, u64, 2, (uint64_t)a.u32[2 * i] * b.u32[2 * i])
PK2(x_pcmpeqb, u8, 16, a.u8[i] == b.u8[i] ? 0xFF : 0)
PK2(x_pcmpeqw, u16, 8, a.u16[i] == b.u16[i] ? 0xFFFF : 0)
PK2(x_pcmpeqd, u32, 4, a.u32[i] == b.u32[i] ? 0xFFFFFFFFu : 0)
PK2(x_pcmpgtb, u8, 16, a.i8[i] > b.i8[i] ? 0xFF : 0)
PK2(x_pcmpgtw, u16, 8, a.i16[i] > b.i16[i] ? 0xFFFF : 0)
PK2(x_pcmpgtd, u32, 4, a.i32[i] > b.i32[i] ? 0xFFFFFFFFu : 0)
PK2(x_pavgb, u8, 16, (uint8_t)((a.u8[i] + b.u8[i] + 1) >> 1))
PK2(x_pavgw, u16, 8, (uint16_t)((a.u16[i] + b.u16[i] + 1) >> 1))
PK2(x_pmaxsw, i16, 8, a.i16[i] > b.i16[i] ? a.i16[i] : b.i16[i])
PK2(x_pminsw, i16, 8, a.i16[i] < b.i16[i] ? a.i16[i] : b.i16[i])
PK2(x_pmaxub, u8, 16, a.u8[i] > b.u8[i] ? a.u8[i] : b.u8[i])
PK2(x_pminub, u8, 16, a.u8[i] < b.u8[i] ? a.u8[i] : b.u8[i])
PK2(x_cvtdq2ps, f32, 4, (float)b.i32[i])
PK2(x_cvttps2dq, i32, 4, cvtt2si(b.f32[i]))

LIFT_INLINE Xmm x_cmpps(Xmm a, Xmm b, int pred) {
    Xmm r;
    for (int i = 0; i < 4; i++) r.u32[i] = fp_cmp(pred, a.f32[i], b.f32[i]) ? 0xFFFFFFFFu : 0;
    return r;
}
LIFT_INLINE Xmm x_cmppd(Xmm a, Xmm b, int pred) {
    Xmm r;
    for (int i = 0; i < 2; i++) r.u64[i] = fp_cmp(pred, a.f64[i], b.f64[i]) ? ~0ull : 0;
    return r;
}
LIFT_INLINE Xmm x_cvtps2dq(const Cpu *cpu, Xmm b) {
    Xmm r;
    for (int i = 0; i < 4; i++) r.i32[i] = cvt2si(cpu, b.f32[i]);
    return r;
}
LIFT_INLINE Xmm x_cvtps2pd(Xmm a, Xmm b) { (void)a; Xmm r; r.f64[0] = b.f32[0]; r.f64[1] = b.f32[1]; return r; }
LIFT_INLINE Xmm x_cvtpd2ps(Xmm a, Xmm b) { (void)a; Xmm r = x_zero(); r.f32[0] = (float)b.f64[0]; r.f32[1] = (float)b.f64[1]; return r; }
LIFT_INLINE Xmm x_cvtdq2pd(Xmm a, Xmm b) { (void)a; Xmm r; r.f64[0] = b.i32[0]; r.f64[1] = b.i32[1]; return r; }
LIFT_INLINE Xmm x_psadbw(Xmm a, Xmm b) {
    Xmm r = x_zero();
    for (int h = 0; h < 2; h++) {
        uint32_t s = 0;
        for (int i = 0; i < 8; i++) { int d = a.u8[8 * h + i] - b.u8[8 * h + i]; s += (uint32_t)(d < 0 ? -d : d); }
        r.u64[h] = s;
    }
    return r;
}
// unpack / pack. The *_mm variants work on the low 64 bits only (MMX register semantics).
#define UNPCK(name, field, n, half)                                                                 \
    LIFT_INLINE Xmm name(Xmm a, Xmm b) {                                                            \
        Xmm r;                                                                                      \
        for (int i = 0; i < (n) / 2; i++) r.field[2 * i] = a.field[(half) + i], r.field[2 * i + 1] = b.field[(half) + i]; \
        return r;                                                                                   \
    }
UNPCK(x_punpcklbw, u8, 16, 0)
UNPCK(x_punpckhbw, u8, 16, 8)
UNPCK(x_punpcklwd, u16, 8, 0)
UNPCK(x_punpckhwd, u16, 8, 4)
UNPCK(x_punpckldq, u32, 4, 0)
UNPCK(x_punpckhdq, u32, 4, 2)
UNPCK(x_punpcklqdq, u64, 2, 0)
UNPCK(x_punpckhqdq, u64, 2, 1)
#define UNPCK_MM(name, field, n, half)                                                              \
    LIFT_INLINE Xmm name(Xmm a, Xmm b) {                                                            \
        Xmm r = x_zero();                                                                           \
        for (int i = 0; i < (n) / 2; i++) r.field[2 * i] = a.field[(half) + i], r.field[2 * i + 1] = b.field[(half) + i]; \
        return r;                                                                                   \
    }
UNPCK_MM(m_punpcklbw, u8, 8, 0)
UNPCK_MM(m_punpckhbw, u8, 8, 4)
UNPCK_MM(m_punpcklwd, u16, 4, 0)
UNPCK_MM(m_punpckhwd, u16, 4, 2)
UNPCK_MM(m_punpckldq, u32, 2, 0)
UNPCK_MM(m_punpckhdq, u32, 2, 1)
#define PACK(name, src, dst, n, lo, hi, zero_hi)                                                    \
    LIFT_INLINE Xmm name(Xmm a, Xmm b) {                                                            \
        Xmm r = x_zero();                                                                           \
        for (int i = 0; i < (n); i++) {                                                             \
            r.dst[i] = (__typeof__(r.dst[0]))clampi(a.src[i], lo, hi);                              \
            r.dst[(n) + i] = (__typeof__(r.dst[0]))clampi(b.src[i], lo, hi);                        \
        }                                                                                           \
        return r;                                                                                   \
    }
PACK(x_packsswb, i16, i8, 8, -128, 127, 0)
PACK(x_packuswb, i16, u8, 8, 0, 255, 0)
PACK(x_packssdw, i32, i16, 4, -32768, 32767, 0)
PACK(m_packsswb, i16, i8, 4, -128, 127, 1)
PACK(m_packuswb, i16, u8, 4, 0, 255, 1)
PACK(m_packssdw, i32, i16, 2, -32768, 32767, 1)

// shifts: count >= element width gives 0 (or all sign bits for arithmetic shifts)
#define SHIFT(name, field, n, bits, expr_in, expr_out)                                              \
    LIFT_INLINE Xmm name(Xmm a, uint64_t c) {                                                       \
        Xmm r = a;                                                                                  \
        for (int i = 0; i < n; i++) r.field[i] = c < bits ? (expr_in) : (expr_out);                 \
        return r;                                                                                   \
    }
SHIFT(x_psllw, u16, 8, 16, (uint16_t)(a.u16[i] << c), 0)
SHIFT(x_pslld, u32, 4, 32, a.u32[i] << c, 0)
SHIFT(x_psllq, u64, 2, 64, a.u64[i] << c, 0)
SHIFT(x_psrlw, u16, 8, 16, (uint16_t)(a.u16[i] >> c), 0)
SHIFT(x_psrld, u32, 4, 32, a.u32[i] >> c, 0)
SHIFT(x_psrlq, u64, 2, 64, a.u64[i] >> c, 0)
SHIFT(x_psraw, i16, 8, 16, (int16_t)(a.i16[i] >> c), (int16_t)(a.i16[i] >> 15))
SHIFT(x_psrad, i32, 4, 32, a.i32[i] >> c, a.i32[i] >> 31)
LIFT_INLINE Xmm x_pslldq(Xmm a, uint64_t c) {
    Xmm r = x_zero();
    for (int i = 0; i < 16; i++) if (i >= (int)c && c < 16) r.u8[i] = a.u8[i - c];
    return r;
}
LIFT_INLINE Xmm x_psrldq(Xmm a, uint64_t c) {
    Xmm r = x_zero();
    for (int i = 0; i < 16; i++) if (i + c < 16) r.u8[i] = a.u8[i + c];
    return r;
}
LIFT_INLINE Xmm x_shufps(Xmm a, Xmm b, int k) {
    Xmm r;
    r.u32[0] = a.u32[k & 3], r.u32[1] = a.u32[(k >> 2) & 3], r.u32[2] = b.u32[(k >> 4) & 3], r.u32[3] = b.u32[(k >> 6) & 3];
    return r;
}
LIFT_INLINE Xmm x_shufpd(Xmm a, Xmm b, int k) { Xmm r; r.u64[0] = a.u64[k & 1], r.u64[1] = b.u64[(k >> 1) & 1]; return r; }
LIFT_INLINE Xmm x_pshufd(Xmm b, int k) {
    Xmm r;
    for (int i = 0; i < 4; i++) r.u32[i] = b.u32[(k >> (2 * i)) & 3];
    return r;
}
LIFT_INLINE Xmm x_pshuflw(Xmm b, int k) {
    Xmm r = b;
    for (int i = 0; i < 4; i++) r.u16[i] = b.u16[(k >> (2 * i)) & 3];
    return r;
}
LIFT_INLINE Xmm x_pshufhw(Xmm b, int k) {
    Xmm r = b;
    for (int i = 0; i < 4; i++) r.u16[4 + i] = b.u16[4 + ((k >> (2 * i)) & 3)];
    return r;
}
LIFT_INLINE Xmm x_unpcklps(Xmm a, Xmm b) { return x_punpckldq(a, b); }
LIFT_INLINE Xmm x_unpckhps(Xmm a, Xmm b) { return x_punpckhdq(a, b); }
LIFT_INLINE Xmm x_unpcklpd(Xmm a, Xmm b) { return x_punpcklqdq(a, b); }
LIFT_INLINE Xmm x_unpckhpd(Xmm a, Xmm b) { return x_punpckhqdq(a, b); }
LIFT_INLINE Xmm x_movlhps(Xmm a, Xmm b) { a.u64[1] = b.u64[0]; return a; }
LIFT_INLINE Xmm x_movhlps(Xmm a, Xmm b) { a.u64[0] = b.u64[1]; return a; }
LIFT_INLINE uint32_t x_movmskps(Xmm a) { return (a.u32[0] >> 31) | ((a.u32[1] >> 31) << 1) | ((a.u32[2] >> 31) << 2) | ((a.u32[3] >> 31) << 3); }
LIFT_INLINE uint32_t x_movmskpd(Xmm a) { return (uint32_t)(a.u64[0] >> 63) | ((uint32_t)(a.u64[1] >> 63) << 1); }
LIFT_INLINE uint32_t x_pmovmskb(Xmm a, int n) {
    uint32_t m = 0;
    for (int i = 0; i < n; i++) m |= (uint32_t)(a.u8[i] >> 7) << i;
    return m;
}

// ---- x87 ------------------------------------------------------------------------------------------------
// Register stack of doubles (not 80-bit): see docs/LIFTER.md for what that changes.
#define ST(i) (cpu->st[(cpu->ftop + (i)) & 7])
LIFT_INLINE void x87_push(Cpu *cpu, double v) { cpu->ftop = (cpu->ftop - 1) & 7; cpu->st[cpu->ftop] = v; }
LIFT_INLINE void x87_pop(Cpu *cpu) { cpu->ftop = (cpu->ftop + 1) & 7; }
// precision control (FCW bits 8-9): 00 = 24-bit significand -> results rounded to float
LIFT_INLINE double x87_pc(const Cpu *cpu, double r) { return (cpu->fcw & 0x300) ? r : (double)(float)r; }
LIFT_INLINE double x87_add(const Cpu *cpu, double a, double b) { double r = a + b; NANFIX64(r, a, b); return x87_pc(cpu, r); }
LIFT_INLINE double x87_sub(const Cpu *cpu, double a, double b) { double r = a - b; NANFIX64(r, a, b); return x87_pc(cpu, r); }
LIFT_INLINE double x87_mul(const Cpu *cpu, double a, double b) { double r = a * b; NANFIX64(r, a, b); return x87_pc(cpu, r); }
LIFT_INLINE double x87_div(const Cpu *cpu, double a, double b) { double r = a / b; NANFIX64(r, a, b); return x87_pc(cpu, r); }
LIFT_INLINE double x87_sqrt(const Cpu *cpu, double a) { double r = sqrt(a); NANFIX64(r, a, a); return x87_pc(cpu, r); }
// FCOM family: C3 (bit 14), C2 (bit 10), C0 (bit 8)
LIFT_INLINE void x87_com(Cpu *cpu, double a, double b) {
    uint16_t c = (a != a || b != b) ? 0x4500 : a == b ? 0x4000 : a < b ? 0x0100 : 0;
    cpu->fsw = (uint16_t)((cpu->fsw & ~0x4700) | c);
}
LIFT_INLINE uint16_t x87_status(const Cpu *cpu) { return (uint16_t)((cpu->fsw & 0x47FF) | (cpu->ftop << 11)); }
LIFT_INLINE double x87_rnd(const Cpu *cpu, double v) { // FCW.RC
    switch ((cpu->fcw >> 10) & 3) {
    case 0: return rint(v);
    case 1: return floor(v);
    case 2: return ceil(v);
    default: return trunc(v);
    }
}
LIFT_INLINE void x87_fxam(Cpu *cpu) {
    double v = ST(0);
    uint16_t c = v != v ? 0x0100 : isinf(v) ? 0x0500 : v == 0 ? 0x4000 : fpclassify(v) == FP_SUBNORMAL ? 0x4400 : 0x0400;
    if (signbit(v)) c |= 0x0200;
    cpu->fsw = (uint16_t)((cpu->fsw & ~0x4700) | c);
}
static inline void x87_prem(Cpu *cpu, int ieee) { // FPREM (truncating) / FPREM1 (round to nearest)
    double a = ST(0), b = ST(1);
    double r = ieee ? remainder(a, b) : fmod(a, b);
    NANFIX64(r, a, b);
    double qd = (a - r) / b;
    uint64_t q = (qd == qd && fabs(qd) < 9.0e18) ? (uint64_t)(int64_t)fabs(qd) : 0;
    uint16_t c = (uint16_t)(((q & 4) ? 0x0100 : 0) | ((q & 2) ? 0x4000 : 0) | ((q & 1) ? 0x0200 : 0));
    cpu->fsw = (uint16_t)((cpu->fsw & ~0x4700) | c); // C2 = 0: reduction complete
    ST(0) = r;
}
static inline void x87_fscale(Cpu *cpu) {
    double n = trunc(ST(1));
    ST(0) = (n == n && fabs(n) < 100000.0) ? scalbn(ST(0), (int)n) : ST(0) * pow(2.0, n);
}
// 80-bit extended <-> double (FLD/FSTP m80). Values that do not fit a double are rounded/saturated.
static inline double x87_ld80(uint32_t a) {
    uint64_t mant = M64(a);
    uint16_t se = M16(a + 8);
    int e = se & 0x7FFF;
    double v;
    if (e == 0x7FFF) v = (mant << 1) ? bits_f64(0x7FF8000000000000ull | (mant >> 11)) : INFINITY;
    else v = ldexp((double)mant, (e ? e : 1) - 16383 - 63);
    return (se & 0x8000) ? -v : v;
}
static inline void x87_st80(uint32_t a, double v) {
    uint64_t mant = 0;
    uint16_t e = 0;
    if (v != v) e = 0x7FFF, mant = 0xC000000000000000ull | ((f64_bits(v) & 0x0007FFFFFFFFFFFFull) << 11);
    else if (isinf(v)) e = 0x7FFF, mant = 0x8000000000000000ull;
    else if (v != 0) {
        int x;
        double m = frexp(fabs(v), &x); // m in [0.5, 1)
        mant = (uint64_t)ldexp(m, 64);
        e = (uint16_t)(x + 16382);
    }
    M64(a) = mant;
    M16(a + 8) = (uint16_t)(e | (signbit(v) ? 0x8000 : 0));
}
// FSIN/FCOS/FSINCOS/FPTAN: operands with |x| >= 2^63 are left untouched and C2 is set ("reduction
// incomplete"); the stack does not change in that case.
LIFT_INLINE int x87_trig_ok(Cpu *cpu, double v) {
    if (fabs(v) >= 9223372036854775808.0 && !isinf(v)) {
        cpu->fsw |= 0x0400;
        return 0;
    }
    cpu->fsw &= ~0x0400;
    return 1;
}
static inline void x87_fsin(Cpu *cpu) { double v = ST(0); if (x87_trig_ok(cpu, v)) { double r = sin(v); NANFIX64(r, v, v); ST(0) = r; } }
static inline void x87_fcos(Cpu *cpu) { double v = ST(0); if (x87_trig_ok(cpu, v)) { double r = cos(v); NANFIX64(r, v, v); ST(0) = r; } }
static inline void x87_fsincos(Cpu *cpu) {
    double v = ST(0);
    if (!x87_trig_ok(cpu, v)) return;
    double s = sin(v), c = cos(v);
    NANFIX64(s, v, v);
    NANFIX64(c, v, v);
    ST(0) = s;
    x87_push(cpu, c);
}
static inline void x87_fptan(Cpu *cpu) {
    double v = ST(0);
    if (!x87_trig_ok(cpu, v)) return;
    double r = tan(v);
    NANFIX64(r, v, v);
    ST(0) = r;
    x87_push(cpu, 1.0);
}
