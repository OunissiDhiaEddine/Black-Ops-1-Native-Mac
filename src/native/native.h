// Hand-written 64-bit replacements for lifted game functions (P6, strangler fig; docs/LIFTER.md §8).
//
//   NATIVE(00967760) { ... }      defines f_00967760(Cpu *cpu), the function every lifted caller and
//                                 rt_dispatch() reach; tools/lifter/lift.py finds the marker and emits the
//                                 lifted body of that address as f_00967760_lifted instead.
//
// A replacement sees the guest state exactly as the lifted function would at its entry: the return address
// is on the guest stack (M32(cpu->esp)), stack arguments follow it, register arguments are in the Cpu. It must
// leave esp as the original `ret` does and keep the registers the original keeps. What it does NOT reproduce
// (caller-saved registers, scratch below esp, flags) is stated at each function with the evidence that no
// caller reads it.
//
// BO1_NATIVE=off runs every lifted original instead (A/B measurements, bisecting, the differential tests).
#pragma once
#include "lifted.h"

#ifdef __cplusplus
extern "C" {
#endif

extern int g_native_off;

#define NATIVE(va)                                                                                  \
    void f_##va##_lifted(Cpu *cpu);                                                                 \
    static void native_##va(Cpu *cpu);                                                              \
    void f_##va(Cpu *cpu);                                                                          \
    void f_##va(Cpu *cpu) {                                                                         \
        if (__builtin_expect(g_native_off, 0)) {                                                    \
            f_##va##_lifted(cpu);                                                                   \
            return;                                                                                 \
        }                                                                                           \
        native_##va(cpu);                                                                           \
    }                                                                                               \
    static void native_##va(Cpu *cpu)

// stack argument n (0 = first) of a function that has just been entered
#define ARG(n) M32(cpu->esp + 4 + 4 * (n))
// plain `ret` of a cdecl / caller-cleans function
#define RET() (cpu->esp += 4)

#ifdef __cplusplus
}
#endif
