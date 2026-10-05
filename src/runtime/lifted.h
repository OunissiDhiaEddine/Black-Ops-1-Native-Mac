// Support header for GENERATED lifted code (tools/lifter). C11, included by every lifted
// translation unit and by the runtime. Semantics follow the Intel SDM; anything "undefined" there
// is given a fixed value so that the lifted code is deterministic.
#pragma once
#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef union Xmm {
    uint8_t u8[16];
    int8_t i8[16];
    uint16_t u16[8];
    int16_t i16[8];
    uint32_t u32[4];
    int32_t i32[4];
    uint64_t u64[2];
    int64_t i64[2];
    float f32[4];
    double f64[2];
} Xmm;

// Status flags are function-local in lifted code: compiled Win32 code does not pass them across
// calls. The few places that do (re/flag_audit.json) go through Cpu.xfl explicitly.
typedef struct Flags {
    uint8_t cf, pf, af, zf, sf, of;
} Flags;

typedef struct Cpu {
    uint32_t eax, ecx, edx, ebx, esp, ebp, esi, edi; // capstone/x86 encoding order
    uint32_t teb;                                    // guest address of the thread's TEB (fs base)
    uint8_t df;                                      // direction flag: the only flag that survives calls
    Flags xfl;                                       // flags handed across a call boundary (audited sites only)
    uint8_t ftop;                                    // x87 top-of-stack index (0 = empty stack at reset)
    uint16_t fcw, fsw;                               // x87 control word; status word (C0-C3 only, TOP kept in ftop)
    uint32_t mxcsr;
    double st[8];                                    // x87 registers as doubles (physical order; ST(i) macro)
    uint64_t mm[8];                                  // MMX registers, modelled separately from st[] (LIFTER.md A3)
    Xmm xmm[8];
    // set by the runtime when lifted code stops abnormally
    uint32_t trap_addr, trap_code;
} Cpu;


// ---- guest memory -----------------------------------------------------------------------
extern uint8_t *g_mem; // base of the 4 GiB guest arena

typedef uint8_t u8a;
typedef uint16_t __attribute__((aligned(1), may_alias)) u16a;
typedef uint32_t __attribute__((aligned(1), may_alias)) u32a;
typedef uint64_t __attribute__((aligned(1), may_alias)) u64a;
#define M8(a) (*(u8a *)(g_mem + (uint32_t)(a)))
#define M16(a) (*(u16a *)(g_mem + (uint32_t)(a)))
#define M32(a) (*(u32a *)(g_mem + (uint32_t)(a)))
#define M64(a) (*(u64a *)(g_mem + (uint32_t)(a)))

#define PUSH32(v) do { cpu->esp -= 4; M32(cpu->esp) = (uint32_t)(v); } while (0)
#define POP32() (cpu->esp += 4, M32(cpu->esp - 4))

// ---- runtime entry points used by lifted code ----------------------------------------------
typedef void (*LiftedFn)(Cpu *);
void rt_dispatch(Cpu *cpu, uint32_t target);                      // indirect call / jump
void rt_trap(Cpu *cpu, uint32_t addr, uint32_t code) __attribute__((noreturn));
void rt_cpuid(Cpu *cpu);                                          // fixed feature set, see runtime.c
void rt_rdtsc(Cpu *cpu);
enum { TRAP_INT3 = 1, TRAP_BAD_JUMP = 2, TRAP_DIV = 3, TRAP_UNSUPPORTED = 4, TRAP_NO_TARGET = 5, TRAP_TIMEOUT = 6, TRAP_FALLTHROUGH = 7 };

// ---- flag helpers ----------------------------------------------------------------------------
#define LIFT_INLINE static inline __attribute__((always_inline))

LIFT_INLINE uint8_t parity8(uint32_t v) { return (uint8_t)(!(__builtin_popcount(v & 0xFF) & 1)); }

#define DEF_ARITH(N, T, S)                                                                         \
    LIFT_INLINE void szp##N(Flags *f, T r) {                                                       \
        f->zf = r == 0;                                                                            \
        f->sf = (uint8_t)(r >> (N - 1));                                                           \
        f->pf = parity8(r);                                                                        \
    }                                                                                              \
    LIFT_INLINE T add##N(Flags *f, T a, T b) {                                                     \
        T r = (T)(a + b);                                                                          \
        f->cf = r < a;                                                                             \
        f->of = (uint8_t)(((a ^ r) & (b ^ r)) >> (N - 1));                                         \
        f->af = (uint8_t)(((a ^ b ^ r) >> 4) & 1);                                                 \
        szp##N(f, r);                                                                              \
        return r;                                                                                  \
    }                                                                                              \
    LIFT_INLINE T adc##N(Flags *f, T a, T b) {                                                     \
        uint64_t w = (uint64_t)a + b + f->cf;                                                      \
        T r = (T)w;                                                                                \
        f->cf = (uint8_t)((w >> N) & 1);                                                           \
        f->of = (uint8_t)(((a ^ r) & (b ^ r)) >> (N - 1));                                         \
        f->af = (uint8_t)(((a ^ b ^ r) >> 4) & 1);                                                 \
        szp##N(f, r);                                                                              \
        return r;                                                                                  \
    }                                                                                              \
    LIFT_INLINE T sub##N(Flags *f, T a, T b) {                                                     \
        T r = (T)(a - b);                                                                          \
        f->cf = a < b;                                                                             \
        f->of = (uint8_t)(((a ^ b) & (a ^ r)) >> (N - 1));                                         \
        f->af = (uint8_t)(((a ^ b ^ r) >> 4) & 1);                                                 \
        szp##N(f, r);                                                                              \
        return r;                                                                                  \
    }                                                                                              \
    LIFT_INLINE T sbb##N(Flags *f, T a, T b) {                                                     \
        uint64_t w = (uint64_t)a - b - f->cf;                                                      \
        T r = (T)w;                                                                                \
        f->cf = (uint8_t)((w >> N) & 1);                                                           \
        f->of = (uint8_t)(((a ^ b) & (a ^ r)) >> (N - 1));                                         \
        f->af = (uint8_t)(((a ^ b ^ r) >> 4) & 1);                                                 \
        szp##N(f, r);                                                                              \
        return r;                                                                                  \
    }                                                                                              \
    LIFT_INLINE T inc##N(Flags *f, T a) { /* CF unchanged */                                       \
        T r = (T)(a + 1);                                                                          \
        f->of = r == (T)((T)1 << (N - 1));                                                         \
        f->af = (r & 0xF) == 0;                                                                    \
        szp##N(f, r);                                                                              \
        return r;                                                                                  \
    }                                                                                              \
    LIFT_INLINE T dec##N(Flags *f, T a) {                                                          \
        T r = (T)(a - 1);                                                                          \
        f->of = a == (T)((T)1 << (N - 1));                                                         \
        f->af = (r & 0xF) == 0xF;                                                                  \
        szp##N(f, r);                                                                              \
        return r;                                                                                  \
    }                                                                                              \
    LIFT_INLINE T neg##N(Flags *f, T a) {                                                          \
        T r = (T)(0 - a);                                                                          \
        f->cf = a != 0;                                                                            \
        f->of = a == (T)((T)1 << (N - 1));                                                         \
        f->af = (uint8_t)(((a ^ r) >> 4) & 1);                                                     \
        szp##N(f, r);                                                                              \
        return r;                                                                                  \
    }                                                                                              \
    LIFT_INLINE T logic##N(Flags *f, T r) { /* and/or/xor/test */                                  \
        f->cf = 0;                                                                                 \
        f->of = 0;                                                                                 \
        f->af = 0;                                                                                 \
        szp##N(f, r);                                                                              \
        return r;                                                                                  \
    }                                                                                              \
    LIFT_INLINE T shl##N(Flags *f, T a, uint32_t c) {                                              \
        c &= 31;                                                                                   \
        if (!c) return a;                                                                          \
        T r = c < N ? (T)(a << c) : 0;                                                             \
        f->cf = c <= N ? (uint8_t)((a >> (N - c)) & 1) : 0;                                        \
        f->of = (uint8_t)((r >> (N - 1)) ^ f->cf);                                                 \
        f->af = 0;                                                                                 \
        szp##N(f, r);                                                                              \
        return r;                                                                                  \
    }                                                                                              \
    LIFT_INLINE T shr##N(Flags *f, T a, uint32_t c) {                                              \
        c &= 31;                                                                                   \
        if (!c) return a;                                                                          \
        T r = c < N ? (T)(a >> c) : 0;                                                             \
        f->cf = c <= N ? (uint8_t)((a >> (c - 1)) & 1) : 0;                                        \
        f->of = (uint8_t)(a >> (N - 1));                                                           \
        f->af = 0;                                                                                 \
        szp##N(f, r);                                                                              \
        return r;                                                                                  \
    }                                                                                              \
    LIFT_INLINE T sar##N(Flags *f, T a, uint32_t c) {                                              \
        c &= 31;                                                                                   \
        if (!c) return a;                                                                          \
        uint32_t k = c < N ? c : N - 1;                                                            \
        T r = (T)((S)a >> k);                                                                      \
        f->cf = (uint8_t)(((S)a >> (c <= N ? c - 1 : N - 1)) & 1);                                 \
        f->of = 0;                                                                                 \
        f->af = 0;                                                                                 \
        szp##N(f, r);                                                                              \
        return r;                                                                                  \
    }                                                                                              \
    LIFT_INLINE T rol##N(Flags *f, T a, uint32_t c) {                                              \
        c &= 31;                                                                                   \
        if (!c) return a;                                                                          \
        uint32_t k = c % N;                                                                        \
        T r = k ? (T)((a << k) | (a >> (N - k))) : a;                                              \
        f->cf = (uint8_t)(r & 1);                                                                  \
        f->of = (uint8_t)((r >> (N - 1)) ^ f->cf);                                                 \
        return r;                                                                                  \
    }                                                                                              \
    LIFT_INLINE T ror##N(Flags *f, T a, uint32_t c) {                                              \
        c &= 31;                                                                                   \
        if (!c) return a;                                                                          \
        uint32_t k = c % N;                                                                        \
        T r = k ? (T)((a >> k) | (a << (N - k))) : a;                                              \
        f->cf = (uint8_t)(r >> (N - 1));                                                           \
        f->of = (uint8_t)(((r >> (N - 1)) ^ (r >> (N - 2))) & 1);                                  \
        return r;                                                                                  \
    }

DEF_ARITH(8, uint8_t, int8_t)
DEF_ARITH(16, uint16_t, int16_t)
DEF_ARITH(32, uint32_t, int32_t)

// Double-precision shifts (32-bit forms only are generated).
LIFT_INLINE uint32_t shld32(Flags *f, uint32_t a, uint32_t b, uint32_t c) {
    c &= 31;
    if (!c) return a;
    uint32_t r = (a << c) | (b >> (32 - c));
    f->cf = (a >> (32 - c)) & 1;
    f->of = (uint8_t)(((r ^ a) >> 31) & 1);
    f->af = 0;
    szp32(f, r);
    return r;
}
LIFT_INLINE uint32_t shrd32(Flags *f, uint32_t a, uint32_t b, uint32_t c) {
    c &= 31;
    if (!c) return a;
    uint32_t r = (a >> c) | (b << (32 - c));
    f->cf = (a >> (c - 1)) & 1;
    f->of = (uint8_t)(((r ^ a) >> 31) & 1);
    f->af = 0;
    szp32(f, r);
    return r;
}

// Rotate through carry (33-bit rotate of CF:value), 32-bit forms.
LIFT_INLINE uint32_t rcl32(Flags *f, uint32_t a, uint32_t c) {
    c &= 31;
    if (!c) return a;
    uint64_t w = ((uint64_t)f->cf << 32) | a;
    uint64_t r = ((w << c) | (w >> (33 - c))) & 0x1FFFFFFFFull;
    f->cf = (uint8_t)(r >> 32);
    f->of = (uint8_t)((((uint32_t)r) >> 31) ^ f->cf);
    return (uint32_t)r;
}
LIFT_INLINE uint32_t rcr32(Flags *f, uint32_t a, uint32_t c) {
    c &= 31;
    if (!c) return a;
    uint64_t w = ((uint64_t)f->cf << 32) | a;
    uint64_t r = ((w >> c) | (w << (33 - c))) & 0x1FFFFFFFFull;
    f->cf = (uint8_t)(r >> 32);
    f->of = (uint8_t)((((uint32_t)r) >> 31) ^ ((((uint32_t)r) >> 30) & 1));
    return (uint32_t)r;
}

// Conditions (Jcc / SETcc / CMOVcc), indexed by the x86 condition code 0..15.
#define CC_O (fl.of)
#define CC_NO (!fl.of)
#define CC_B (fl.cf)
#define CC_AE (!fl.cf)
#define CC_E (fl.zf)
#define CC_NE (!fl.zf)
#define CC_BE (fl.cf || fl.zf)
#define CC_A (!fl.cf && !fl.zf)
#define CC_S (fl.sf)
#define CC_NS (!fl.sf)
#define CC_P (fl.pf)
#define CC_NP (!fl.pf)
#define CC_L (fl.sf != fl.of)
#define CC_GE (fl.sf == fl.of)
#define CC_LE (fl.zf || fl.sf != fl.of)
#define CC_G (!fl.zf && fl.sf == fl.of)

LIFT_INLINE uint8_t flags_to_ah(const Flags *f) { // LAHF layout: SF ZF 0 AF 0 PF 1 CF
    return (uint8_t)((f->sf << 7) | (f->zf << 6) | (f->af << 4) | (f->pf << 2) | 2 | f->cf);
}
LIFT_INLINE void ah_to_flags(Flags *f, uint8_t ah) {
    f->sf = (ah >> 7) & 1, f->zf = (ah >> 6) & 1, f->af = (ah >> 4) & 1, f->pf = (ah >> 2) & 1, f->cf = ah & 1;
}

// EFLAGS image for PUSHFD/POPFD (IF always set; bit 1 always set).
LIFT_INLINE uint32_t flags_to_eflags(const Flags *f, const Cpu *cpu) {
    return (uint32_t)(f->cf | 2 | (f->pf << 2) | (f->af << 4) | (f->zf << 6) | (f->sf << 7) | 0x200 | (cpu->df << 10) | (f->of << 11));
}
LIFT_INLINE void eflags_to_flags(Flags *f, Cpu *cpu, uint32_t v) {
    f->cf = v & 1, f->pf = (v >> 2) & 1, f->af = (v >> 4) & 1, f->zf = (v >> 6) & 1, f->sf = (v >> 7) & 1, f->of = (v >> 11) & 1;
    cpu->df = (v >> 10) & 1;
}
// LOCKed read-modify-write on guest memory. Atomic when the operand is naturally aligned (always the
// case for compiler-generated code); a misaligned operand (legal on x86, a fault for arm64 exclusives)
// falls back to a plain sequence.
LIFT_INLINE int lk_cas32(uint32_t a, uint32_t *expected, uint32_t desired) {
    if (__builtin_expect((a & 3) == 0, 1))
        return __atomic_compare_exchange_n((uint32_t *)(g_mem + a), expected, desired, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    uint32_t cur = M32(a);
    if (cur == *expected) { M32(a) = desired; return 1; }
    *expected = cur;
    return 0;
}
LIFT_INLINE int lk_cas64(uint32_t a, uint64_t *expected, uint64_t desired) {
    if (__builtin_expect((a & 7) == 0, 1))
        return __atomic_compare_exchange_n((uint64_t *)(g_mem + a), expected, desired, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    uint64_t cur = M64(a);
    if (cur == *expected) { M64(a) = desired; return 1; }
    *expected = cur;
    return 0;
}
LIFT_INLINE uint32_t lk_xadd32(uint32_t a, uint32_t v) {
    if (__builtin_expect((a & 3) == 0, 1)) return __atomic_fetch_add((uint32_t *)(g_mem + a), v, __ATOMIC_SEQ_CST);
    uint32_t cur = M32(a);
    M32(a) = cur + v;
    return cur;
}

#include "lifted_fp.h"

#ifdef __cplusplus
}
#endif
