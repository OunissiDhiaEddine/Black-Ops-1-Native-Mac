// kernel32: code pages and character classification (T-045).
// Code page 1252 is treated as ISO-8859-1 (identical except 0x80-0x9F): LIKELY sufficient — the game's
// own text handling is ASCII/UTF-8; the CRT only builds its ctype tables from these calls.
#include "win32_util.h"

namespace bo1 {

SHIM(kernel32, GetACP, 0) { return 1252; }
SHIM(kernel32, GetOEMCP, 0) { return 437; }
SHIM(kernel32, IsValidCodePage, 1) { return a[0] == 1252 || a[0] == 437 || a[0] == 850 || a[0] == 65001 || a[0] == 20127; }
SHIM(kernel32, GetCPInfo, 2) {
    memset(g_mem + a[1], 0, 20);
    wr32(a[1], a[0] == 65001 ? 4 : 1); // MaxCharSize
    g_mem[a[1] + 4] = '?';             // DefaultChar; LeadByte[] stays empty (single-byte code page)
    return 1;
}

SHIM(kernel32, MultiByteToWideChar, 6) {
    // (CodePage, dwFlags, lpMultiByteStr, cbMultiByte, lpWideCharStr, cchWideChar)
    const uint8_t *src = g_mem + a[2];
    uint32_t n = (int32_t)a[3] < 0 ? (uint32_t)strlen((const char *)src) + 1 : a[3];
    if (a[5] == 0) return n;
    if (a[5] < n) {
        last_error(cpu) = ERROR_INSUFFICIENT_BUFFER;
        return 0;
    }
    for (uint32_t i = 0; i < n; i++) wr16(a[4] + 2 * i, src[i]);
    return n;
}
SHIM(kernel32, WideCharToMultiByte, 8) {
    // (CodePage, dwFlags, lpWideCharStr, cchWideChar, lpMultiByteStr, cbMultiByte, lpDefaultChar, lpUsedDefaultChar)
    uint32_t n = a[3];
    if ((int32_t)n < 0) {
        n = 0;
        while (rd16(a[2] + 2 * n)) n++;
        n++;
    }
    if (a[5] == 0) return n;
    if (a[5] < n) {
        last_error(cpu) = ERROR_INSUFFICIENT_BUFFER;
        return 0;
    }
    bool used = false;
    for (uint32_t i = 0; i < n; i++) {
        uint16_t c = rd16(a[2] + 2 * i);
        if (c > 255) used = true;
        g_mem[a[4] + i] = c > 255 ? (a[6] ? g_mem[a[6]] : '?') : (uint8_t)c;
    }
    if (a[7]) wr32(a[7], used);
    return n;
}

static bool is_upper(uint16_t c) { return (c >= 'A' && c <= 'Z') || (c >= 0xC0 && c <= 0xDE && c != 0xD7); }
static bool is_lower(uint16_t c) { return (c >= 'a' && c <= 'z') || (c >= 0xDF && c <= 0xFF && c != 0xF7); }
SHIM(kernel32, GetStringTypeW, 4) {
    // (dwInfoType, lpSrcStr, cchSrc, lpCharType)
    if (a[0] != 1) { // only CT_CTYPE1 is implemented
        log("GetStringTypeW(type %u) not implemented", a[0]);
        last_error(cpu) = ERROR_INVALID_PARAMETER;
        return 0;
    }
    uint32_t n = a[2];
    if ((int32_t)n < 0) {
        n = 0;
        while (rd16(a[1] + 2 * n)) n++;
        n++;
    }
    for (uint32_t i = 0; i < n; i++) {
        uint16_t c = rd16(a[1] + 2 * i), t = 0;
        if (is_upper(c)) t |= 0x001 | 0x100;                                                            // C1_UPPER | C1_ALPHA
        if (is_lower(c)) t |= 0x002 | 0x100;                                                            // C1_LOWER | C1_ALPHA
        if (c >= '0' && c <= '9') t |= 0x004 | 0x080;                                                   // C1_DIGIT | C1_XDIGIT
        if ((c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F')) t |= 0x080;                               // C1_XDIGIT
        if (c == ' ' || (c >= 9 && c <= 13) || c == 0xA0) t |= 0x008;                                   // C1_SPACE
        if (c == ' ' || c == '\t' || c == 0xA0) t |= 0x040;                                             // C1_BLANK
        if ((c >= 33 && c <= 47) || (c >= 58 && c <= 64) || (c >= 91 && c <= 96) || (c >= 123 && c <= 126) || (c >= 0xA1 && c <= 0xBF) || c == 0xD7 || c == 0xF7)
            t |= 0x010;                                                                                 // C1_PUNCT
        if (c < 32 || (c >= 0x7F && c <= 0x9F)) t |= 0x020;                                             // C1_CNTRL
        if (c < 0x100) t |= 0x200;                                                                      // C1_DEFINED
        wr16(a[3] + 2 * i, t);
    }
    return 1;
}
SHIM(kernel32, LCMapStringW, 6) {
    // (Locale, dwMapFlags, lpSrcStr, cchSrc, lpDestStr, cchDest): upper/lower case mapping only
    if (a[1] & ~(0x100u | 0x200u)) {
        log("LCMapStringW(flags %x) not implemented", a[1]);
        last_error(cpu) = ERROR_INVALID_PARAMETER;
        return 0;
    }
    uint32_t n = a[3];
    if ((int32_t)n < 0) {
        n = 0;
        while (rd16(a[2] + 2 * n)) n++;
        n++;
    }
    if (a[5] == 0) return n;
    if (a[5] < n) {
        last_error(cpu) = ERROR_INSUFFICIENT_BUFFER;
        return 0;
    }
    for (uint32_t i = 0; i < n; i++) {
        uint16_t c = rd16(a[2] + 2 * i);
        if ((a[1] & 0x100) && is_upper(c)) c += 32;                    // LCMAP_LOWERCASE
        else if ((a[1] & 0x200) && is_lower(c) && c != 0xDF && c != 0xFF) c -= 32; // LCMAP_UPPERCASE
        wr16(a[4] + 2 * i, c);
    }
    return n;
}

} // namespace bo1
