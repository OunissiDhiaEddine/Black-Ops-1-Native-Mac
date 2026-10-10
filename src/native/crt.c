// C runtime routines of the game image, replaced by the host's (P6 / T-064).
#include <stdlib.h>
#include <string.h>

#include "native.h"

int g_native_off;

__attribute__((constructor)) static void native_init(void) {
    const char *e = getenv("BO1_NATIVE");
    g_native_off = e && strcmp(e, "off") == 0;
}

// VA 0x00967760  _memcpy (MSVC CRT; cdecl: dst, src, n; returns dst in eax).
// CONFIRMED by its body: it copies downwards when dst lies inside [src, src + n), so it is memmove.
// Not reproduced: ecx / edx at return (caller-saved in cdecl; the original leaves copy counters there).
NATIVE(00967760) {
    uint32_t dst = ARG(0), src = ARG(1), n = ARG(2);
    memmove(g_mem + dst, g_mem + src, n);
    cpu->eax = dst;
    RET();
}
