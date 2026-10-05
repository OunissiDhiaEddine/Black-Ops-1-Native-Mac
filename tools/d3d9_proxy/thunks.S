// Generic per-slot thunks (i386, GNU as, preprocessed).
// Each hooked vtable entry points at one of these. On entry the stack is exactly what the
// caller pushed for the COM call (ret addr, this, args...), and eax/ecx/edx are scratch
// (stdcall entry). The thunk reports (iface, slot, pointer-to-frame) to trace_pre, then
// tail-jumps to the original method, so the original sees an untouched frame.
//
// If trace_pre returns non-zero it wants to see the result as well: it has saved the real
// return address in a per-thread shadow stack and returns the address of post_tramp, which
// the thunk writes over the return address. When the original method returns (stdcall: it
// has popped its own arguments) control lands in post_tramp with the result in eax;
// trace_post records it and hands back the real return address.
#include "d3d9_methods.h"

    .text

// void *__stdcall trace_pre(unsigned iface, unsigned slot, void **frame)
//      frame[0] = return address, frame[1] = this, frame[2..] = parameters
.macro THUNK sym, id, slot
    .globl _thunk_\sym\()_\slot
_thunk_\sym\()_\slot:
    pushl   %esp                    // frame pointer (== esp before this push)
    pushl   $\slot
    pushl   $\id
    call    _trace_pre@12
    testl   %eax, %eax
    jz      1f
    movl    %eax, (%esp)            // return into post_tramp instead of the caller
1:  jmp     *(_orig_\sym + 4*\slot)
    .p2align 4
.endm

.macro ENTRY sym, slot
    .long   _thunk_\sym\()_\slot
.endm

    .p2align 4
    .globl _post_tramp
_post_tramp:
    pushl   %eax                    // keep the method's result
    pushl   %eax
    call    _trace_post@4           // void *__stdcall trace_post(unsigned result) -> real return address
    movl    %eax, %ecx
    popl    %eax
    jmp     *%ecx

    .p2align 4
#define T_d3d9(i, name)      THUNK d3d9, 0, i ;
#define T_device(i, name)    THUNK device, 1, i ;
#define T_swapchain(i, name) THUNK swapchain, 2, i ;
#define T_texture(i, name)   THUNK texture, 3, i ;
#define T_cube(i, name)      THUNK cube, 4, i ;
#define T_volume(i, name)    THUNK volume, 5, i ;
#define T_vb(i, name)        THUNK vb, 6, i ;
#define T_ib(i, name)        THUNK ib, 7, i ;
#define T_surface(i, name)   THUNK surface, 8, i ;
#define T_query(i, name)     THUNK query, 9, i ;
#define GEN_THUNKS(id, sym, com, METHODS, count) METHODS(T_##sym)
FOR_EACH_IFACE(GEN_THUNKS)

    .section .rdata, "dr"
    .p2align 2
#define E_d3d9(i, name)      ENTRY d3d9, i ;
#define E_device(i, name)    ENTRY device, i ;
#define E_swapchain(i, name) ENTRY swapchain, i ;
#define E_texture(i, name)   ENTRY texture, i ;
#define E_cube(i, name)      ENTRY cube, i ;
#define E_volume(i, name)    ENTRY volume, i ;
#define E_vb(i, name)        ENTRY vb, i ;
#define E_ib(i, name)        ENTRY ib, i ;
#define E_surface(i, name)   ENTRY surface, i ;
#define E_query(i, name)     ENTRY query, i ;
#define GEN_TABLE(id, sym, com, METHODS, count) \
    .globl _thunks_##sym ; _thunks_##sym: ; METHODS(E_##sym)
FOR_EACH_IFACE(GEN_TABLE)
