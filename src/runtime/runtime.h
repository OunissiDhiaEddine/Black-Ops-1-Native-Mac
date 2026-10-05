// Minimal runtime for lifted code (P4, first cut): guest arena, PE image mapping, address-table
// dispatch, traps. Threads, TEB/TLS and import shims come with T-045.
#pragma once
#include <setjmp.h>
#include <stddef.h>

#include "lifted.h"

#ifdef __cplusplus
extern "C" {
#endif

// Reserve (or reset to zero) the 4 GiB guest arena. Returns 0 on success.
int rt_arena_reset(void);
// Map a PE32 image's sections at its preferred base inside the arena. Returns 0 on success.
int rt_load_image(const void *file, size_t size);
// Look up a lifted function by guest address (NULL if none).
LiftedFn rt_lookup(uint32_t va);

// Embedder hooks (NULL in the differential tests):
//   g_rt_host_call   — rt_dispatch() target at or above RT_TRAMP_BASE (import trampolines, docs/HOST_ABI.md §4)
//   g_rt_trap_handler — called by rt_trap() instead of longjmp(g_trap_jmp); must not return
#define RT_TRAMP_BASE 0xFFF00000u
extern void (*g_rt_host_call)(Cpu *cpu, uint32_t target);
extern void (*g_rt_trap_handler)(Cpu *cpu, uint32_t addr, uint32_t code);

// rt_trap() longjmps here with the trap code; the embedder sets it up before entering guest code.
extern jmp_buf g_trap_jmp;

#ifdef __cplusplus
}
#endif
