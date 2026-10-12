// The game's Windows-only debug console window, replaced by the host's log (P8 / T-076).
// The fatal-error routine (0x004C9B60) opens that console and writes the message into it before its message
// box; building the window needs GDI fonts and edit controls the port has no use for.
#include <stdio.h>

#include "native.h"

// VA 0x00586100 — show the console window (creating it through 0x00634C60 on first use). cdecl, no arguments.
// CONFIRMED by its body: GetModuleHandleA + 0x00634C60 when [0x0276FF10] is 0, then ShowWindow and an
// EM_SETSEL to the edit control at [0x0276FF14]. Here: no window. Callers use no result.
NATIVE(00586100) {
    RET();
}

// VA 0x00477CE0 — append text to the console window's edit control. cdecl (const char *text).
// CONFIRMED by its body: copies up to 0x3FFF characters, converts line ends (0x008682C0) and sends
// EM_REPLACESEL to [0x0276FF14]. Here: the text goes to the host's log.
NATIVE(00477ce0) {
    const char *text = (const char *)(g_mem + ARG(0));
    size_t n = strnlen(text, 0x4000);
    while (n && (text[n - 1] == '\n' || text[n - 1] == '\r')) n--;
    if (n) fprintf(stderr, "[bo1] game console: %.*s\n", (int)n, text);
    RET();
}
