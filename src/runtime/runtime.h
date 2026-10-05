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

// rt_trap() longjmps here with the trap code; the embedder sets it up before entering guest code.
extern jmp_buf g_trap_jmp;

#ifdef __cplusplus
}
#endif
