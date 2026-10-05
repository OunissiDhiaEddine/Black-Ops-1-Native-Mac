// Generic per-slot thunks (i386, GNU as, preprocessed).
// Each hooked vtable entry points at one of these. On entry the stack is exactly what the
// game pushed for the COM call (ret addr, this, args...), and eax/ecx/edx are scratch
// (stdcall entry). The thunk reports (slot, pointer-to-args) to the C side, then tail-jumps
// to the original method, so the original sees an untouched frame.
#include "d3d9_methods.h"

    .text

// void __stdcall trace_<iface>(unsigned slot, void **args)   args[0] = return address,
//                                                            args[1] = this, args[2..] = params
.macro THUNK iface, slot
    .globl _thunk_\iface\()_\slot
_thunk_\iface\()_\slot:
    pushl   %esp                    // args pointer (== esp before this push)
    pushl   $\slot
    call    _trace_\iface\()@8
    jmp     *(_orig_\iface + 4*\slot)
    .p2align 4
.endm

.macro ENTRY iface, slot
    .long   _thunk_\iface\()_\slot
.endm

#define X_D3D9(i, name)   THUNK d3d9, i ;
#define X_DEVICE(i, name) THUNK device, i ;
    .p2align 4
IDIRECT3D9_METHODS(X_D3D9)
IDIRECT3DDEVICE9_METHODS(X_DEVICE)

    .section .rdata, "dr"
    .p2align 2
#define E_D3D9(i, name)   ENTRY d3d9, i ;
#define E_DEVICE(i, name) ENTRY device, i ;
    .globl _thunks_d3d9
_thunks_d3d9:
IDIRECT3D9_METHODS(E_D3D9)
    .globl _thunks_device
_thunks_device:
IDIRECT3DDEVICE9_METHODS(E_DEVICE)
