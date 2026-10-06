// kernel32 / shlwapi / psapi functions that only the emulated steam_api.dll (built with a newer C
// runtime than the game) asks for (T-024). Added as its start-up reaches them.
#include <sys/stat.h>
#include <time.h>

#include <cstdlib>

#include "image_info.h"
#include "win32_util.h"

namespace bo1 {

uint32_t tick_ms();

static std::string narrow(const std::u16string &w) {
    std::string s;
    for (char16_t c : w) s.push_back(c < 256 ? (char)c : '?');
    return s;
}
static void put_wstr(uint32_t dst, uint32_t cap_chars, const std::string &s) {
    if (!dst || !cap_chars) return;
    uint32_t n = (uint32_t)s.size() < cap_chars - 1 ? (uint32_t)s.size() : cap_chars - 1;
    for (uint32_t i = 0; i < n; i++) wr16(dst + 2 * i, (uint8_t)s[i]);
    wr16(dst + 2 * n, 0);
}

SHIM(kernel32, InitializeCriticalSectionEx, 3) {
    memset(g_mem + a[0], 0, 24);
    wr32(a[0] + 4, 0xFFFFFFFFu); // LockCount = -1: free (same layout as kernel32_sync.cpp)
    wr32(a[0] + 20, a[1]);
    return 1;
}
SHIM(kernel32, GetTickCount64, 0) {
    cpu->edx = 0;
    return tick_ms();
}
SHIM(kernel32, IsProcessorFeaturePresent, 1) {
    switch (a[0]) {
    case 2: case 3: case 6: case 8: case 10: return 1; // CMPXCHG8B, MMX, SSE, RDTSC, SSE2 — what rt_cpuid reports
    default: return 0;
    }
}
SHIM(kernel32, FlushInstructionCache, 3) { return 1; }
SHIM(kernel32, DisableThreadLibraryCalls, 1) { return 1; }
SHIM(kernel32, FlushProcessWriteBuffers, 0) { return 0; }
SHIM(kernel32, SetThreadStackGuarantee, 1) { return 1; }
SHIM(kernel32, HeapDestroy, 1) { return 1; }
SHIM(kernel32, AreFileApisANSI, 0) { return 1; }
SHIM(kernel32, GetCurrentProcessorNumber, 0) { return 0; }
SHIM(kernel32, GetUserDefaultLCID, 0) { return 0x0409; } // en-US
SHIM(kernel32, IsValidLocale, 2) { return 1; }
SHIM(kernel32, GetCurrentPackageId, 2) { return 15700; } // APPMODEL_ERROR_NO_PACKAGE: not a packaged app
SHIM(kernel32, OutputDebugStringW, 1) {
    if (g_verbose) log("debug: %s", narrow(gwstr(a[0])).c_str());
    return 0;
}
SHIM(kernel32, GetModuleFileNameW, 3) {
    std::string path = module_path(a[0]);
    if (path.empty()) {
        last_error(cpu) = ERROR_MOD_NOT_FOUND;
        return 0;
    }
    put_wstr(a[1], a[2], path);
    return (uint32_t)path.size() < a[2] ? (uint32_t)path.size() : a[2];
}
SHIM(kernel32, GetModuleHandleExW, 3) {
    // (dwFlags, lpModuleName, phModule): only "the module containing this address" / NULL are answered
    uint32_t h = 0;
    if (!a[1]) h = image_info().base;
    else if (a[0] & 4) { // GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS
        uint32_t base, size;
        if (vm_query(a[1], &base, &size)) h = base;
    }
    if (a[2]) wr32(a[2], h);
    if (!h) last_error(cpu) = ERROR_MOD_NOT_FOUND;
    return h != 0;
}
SHIM(kernel32, SetFilePointerEx, 5) {
    // (hFile, liDistanceToMove (2 slots), lpNewFilePointer, dwMoveMethod) -> forwarded to SetFilePointer's logic
    uint32_t hi_va = heap_alloc(4);
    wr32(hi_va, a[2]);
    uint32_t args[4] = {a[0], a[1], hi_va, a[4]};
    uint32_t t = import_lookup("kernel32.dll", "SetFilePointer");
    uint32_t saved = cpu->esp;
    uint32_t lo = guest_call(cpu, t, {args[0], args[1], args[2], args[3]});
    cpu->esp = saved;
    uint32_t hi = rd32(hi_va);
    heap_free(hi_va);
    if (lo == 0xFFFFFFFFu && last_error(cpu) != 0) return 0;
    if (a[3]) wr32(a[3], lo), wr32(a[3] + 4, hi);
    return 1;
}

SHIM(kernel32, GetStartupInfoW, 1) {
    memset(g_mem + a[0], 0, 68);
    wr32(a[0], 68);
    return 0;
}
static uint32_t g_cmdline_w = 0;
SHIM(kernel32, GetCommandLineW, 0) {
    if (!g_cmdline_w) {
        std::string c = std::string("\"") + kGuestGameDir + "\\BlackOps.exe\"";
        g_cmdline_w = heap_alloc((uint32_t)c.size() * 2 + 2);
        put_wstr(g_cmdline_w, (uint32_t)c.size() + 1, c);
    }
    return g_cmdline_w;
}
SHIM(kernel32, LCMapStringEx, 9) {
    // (lpLocaleName, dwMapFlags, lpSrcStr, cchSrc, lpDestStr, cchDest, lpVersionInformation, lpReserved, sortHandle)
    uint32_t t = import_lookup("kernel32.dll", "LCMapStringW");
    uint32_t saved = cpu->esp;
    uint32_t r = guest_call(cpu, t, {0x0409, a[1], a[2], a[3], a[4], a[5]});
    cpu->esp = saved;
    return r;
}

// ---- locale: a fixed en-US locale (design choice; the baseline prefix is en-US as well — LIKELY) --------------
static const char *locale_string(uint32_t type) {
    static const char *const days[] = {"Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday", "Sunday"};
    static const char *const adays[] = {"Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun"};
    static const char *const months[] = {"January", "February", "March", "April", "May", "June", "July", "August", "September", "October", "November", "December", ""};
    static const char *const amonths[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec", ""};
    if (type >= 0x2A && type <= 0x30) return days[type - 0x2A];
    if (type >= 0x31 && type <= 0x37) return adays[type - 0x31];
    if (type >= 0x38 && type <= 0x43) return months[type - 0x38];
    if (type >= 0x44 && type <= 0x4F) return amonths[type - 0x44];
    switch (type) {
    case 0x01: return "0409";           // LOCALE_ILANGUAGE
    case 0x02: return "English (United States)";
    case 0x03: return "ENU";
    case 0x05: return "1";              // LOCALE_ICOUNTRY
    case 0x06: return "United States";
    case 0x07: return "USA";
    case 0x0B: return "437";            // LOCALE_IDEFAULTCODEPAGE
    case 0x1004: return "1252";         // LOCALE_IDEFAULTANSICODEPAGE
    case 0x0C: return ",";              // LOCALE_SLIST
    case 0x0D: return "1";              // LOCALE_IMEASURE
    case 0x0E: return ".";              // LOCALE_SDECIMAL
    case 0x0F: return ",";              // LOCALE_STHOUSAND
    case 0x10: return "3;0";            // LOCALE_SGROUPING
    case 0x11: return "2";              // LOCALE_IDIGITS
    case 0x12: return "1";              // LOCALE_ILZERO
    case 0x13: return "0123456789";
    case 0x14: return "$";              // LOCALE_SCURRENCY
    case 0x15: return "USD";
    case 0x16: return ".";
    case 0x17: return ",";
    case 0x18: return "3;0";
    case 0x19: case 0x1A: return "2";
    case 0x1B: return "0";
    case 0x1C: return "0";
    case 0x1D: return "/";              // LOCALE_SDATE
    case 0x1E: return ":";              // LOCALE_STIME
    case 0x1F: return "M/d/yyyy";       // LOCALE_SSHORTDATE
    case 0x20: return "dddd, MMMM d, yyyy";
    case 0x21: return "0";              // LOCALE_IDATE
    case 0x22: return "0";              // LOCALE_ILDATE
    case 0x23: return "0";              // LOCALE_ITIME
    case 0x24: return "1";              // LOCALE_ICENTURY
    case 0x25: case 0x26: case 0x27: return "0"; // leading zeros in time / day / month
    case 0x28: return "AM";
    case 0x29: return "PM";
    case 0x50: return "+";
    case 0x51: return "-";
    case 0x59: return "en";             // LOCALE_SISO639LANGNAME
    case 0x5A: return "US";             // LOCALE_SISO3166CTRYNAME
    case 0x5C: return "en-US";          // LOCALE_SNAME
    case 0x1001: return "English";      // LOCALE_SENGLANGUAGE
    case 0x1002: return "United States";
    case 0x1003: return "h:mm:ss tt";   // LOCALE_STIMEFORMAT
    case 0x1009: return "1";            // LOCALE_ICALENDARTYPE
    case 0x100C: return "6";            // LOCALE_IFIRSTDAYOFWEEK
    case 0x100D: return "0";
    default: return nullptr;
    }
}
// Returns the character count including the terminator (0 on failure), like GetLocaleInfoW.
static uint32_t locale_info(Cpu *cpu, uint32_t type, uint32_t dst, uint32_t cch, bool wide) {
    bool number = type & 0x20000000u; // LOCALE_RETURN_NUMBER
    const char *v = locale_string(type & 0x0000FFFFu);
    if (!v) {
        log("GetLocaleInfo(type %#x) is not in the fixed en-US table", type);
        last_error(cpu) = ERROR_INVALID_PARAMETER;
        return 0;
    }
    if (number) {
        if (dst && cch * (wide ? 2 : 1) >= 4) wr32(dst, (uint32_t)strtoul(v, nullptr, (type & 0xFFFF) == 1 ? 16 : 10));
        return wide ? 2 : 4;
    }
    uint32_t n = (uint32_t)strlen(v) + 1;
    if (!cch) return n;
    if (cch < n) {
        last_error(cpu) = ERROR_INSUFFICIENT_BUFFER;
        return 0;
    }
    if (wide) put_wstr(dst, cch, v);
    else put_str(dst, cch, v);
    return n;
}
SHIM(kernel32, GetLocaleInfoEx, 4) { return locale_info(cpu, a[1], a[2], a[3], true); }
SHIM(kernel32, GetLocaleInfoW, 4) { return locale_info(cpu, a[1], a[2], a[3], true); }
SHIM(kernel32, GetLocaleInfoA, 4) { return locale_info(cpu, a[1], a[2], a[3], false); }
SHIM(kernel32, IsValidLocaleName, 1) { return 1; }
SHIM(kernel32, GetUserDefaultLocaleName, 2) {
    if (a[1] < 6) return 0;
    put_wstr(a[0], a[1], "en-US");
    return 6;
}

// ---- wide-character file API: converted and forwarded to the ANSI shims ------------------------------------
static uint32_t call_a(Cpu *cpu, const char *name, std::initializer_list<uint32_t> args) {
    uint32_t t = import_lookup("kernel32.dll", name);
    uint32_t saved = cpu->esp;
    uint32_t r = guest_call(cpu, t, args);
    cpu->esp = saved;
    return r;
}
struct TempStr { // a guest copy of a narrowed string, freed on scope exit
    uint32_t va;
    explicit TempStr(uint32_t wide_va) : va(wide_va ? heap_strdup(narrow(gwstr(wide_va))) : 0) {}
    ~TempStr() { heap_free(va); }
};
SHIM(kernel32, GetCurrentDirectoryW, 2) {
    uint32_t n = (uint32_t)strlen(kGuestGameDir);
    if (a[0] <= n) return n + 1;
    put_wstr(a[1], a[0], kGuestGameDir);
    return n;
}
SHIM(kernel32, CreateFileW, 7) {
    TempStr name(a[0]);
    return call_a(cpu, "CreateFileA", {name.va, a[1], a[2], a[3], a[4], a[5], a[6]});
}
SHIM(kernel32, GetFileAttributesW, 1) {
    TempStr name(a[0]);
    return call_a(cpu, "GetFileAttributesA", {name.va});
}
SHIM(kernel32, CreateDirectoryW, 2) {
    TempStr name(a[0]);
    return call_a(cpu, "CreateDirectoryA", {name.va, a[1]});
}
SHIM(kernel32, DeleteFileW, 1) {
    TempStr name(a[0]);
    return call_a(cpu, "DeleteFileA", {name.va});
}
SHIM(kernel32, GetFullPathNameW, 4) {
    std::string full = guest_full_path(narrow(gwstr(a[0])));
    if (a[1] <= full.size()) return (uint32_t)full.size() + 1;
    put_wstr(a[2], a[1], full);
    if (a[3]) {
        size_t p = full.find_last_of('\\');
        wr32(a[3], p + 1 < full.size() ? a[2] + 2 * ((uint32_t)p + 1) : 0);
    }
    return (uint32_t)full.size();
}
static void widen_find_data(uint32_t dst_w, uint32_t src_a) {
    memcpy(g_mem + dst_w, g_mem + src_a, 44); // attributes, times, sizes, reserved
    put_wstr(dst_w + 44, 260, gstr(src_a + 44));
    wr16(dst_w + 44 + 520, 0); // cAlternateFileName
}
static uint32_t find_first_w(Cpu *cpu, uint32_t name_w, uint32_t data_w) {
    TempStr name(name_w);
    uint32_t tmp = heap_alloc(320);
    uint32_t h = call_a(cpu, "FindFirstFileA", {name.va, tmp});
    if (h != INVALID_HANDLE) widen_find_data(data_w, tmp);
    heap_free(tmp);
    return h;
}
SHIM(kernel32, FindFirstFileW, 2) { return find_first_w(cpu, a[0], a[1]); }
SHIM(kernel32, FindFirstFileExW, 6) { return find_first_w(cpu, a[0], a[2]); } // (name, infoLevel, data, searchOp, filter, flags)
SHIM(kernel32, FindNextFileW, 2) {
    uint32_t tmp = heap_alloc(320);
    uint32_t ok = call_a(cpu, "FindNextFileA", {a[0], tmp});
    if (ok) widen_find_data(a[1], tmp);
    heap_free(tmp);
    return ok;
}
SHIM(kernel32, GetDriveTypeW, 1) { return 3; } // DRIVE_FIXED
SHIM(kernel32, GetDriveTypeA, 1) { return 3; }
SHIM(kernel32, GetFileAttributesExW, 3) {
    // (name, GetFileExInfoStandard, WIN32_FILE_ATTRIBUTE_DATA*): attributes, 3 FILETIMEs, size high/low
    std::string host = host_path(narrow(gwstr(a[0])));
    struct stat st;
    if (stat(host.c_str(), &st) != 0) {
        last_error(cpu) = ERROR_FILE_NOT_FOUND;
        return 0;
    }
    wr32(a[2], S_ISDIR(st.st_mode) ? 0x10u : 0x20u);
    wr64(a[2] + 4, unix_to_filetime(st.st_birthtimespec.tv_sec, st.st_birthtimespec.tv_nsec));
    wr64(a[2] + 12, unix_to_filetime(st.st_atimespec.tv_sec, st.st_atimespec.tv_nsec));
    wr64(a[2] + 20, unix_to_filetime(st.st_mtimespec.tv_sec, st.st_mtimespec.tv_nsec));
    wr32(a[2] + 28, (uint32_t)((uint64_t)st.st_size >> 32));
    wr32(a[2] + 32, (uint32_t)st.st_size);
    return 1;
}
SHIM(kernel32, LoadLibraryExW, 3) {
    TempStr name(a[0]);
    return call_a(cpu, "LoadLibraryA", {name.va});
}
SHIM(kernel32, LoadLibraryW, 1) {
    TempStr name(a[0]);
    return call_a(cpu, "LoadLibraryA", {name.va});
}

// ---- SYSTEMTIME conversions ---------------------------------------------------------------------------------
static time_t systemtime_to_unix(uint32_t st, uint32_t *ms) { // SYSTEMTIME is UTC here
    struct tm tm = {};
    tm.tm_year = rd16(st) - 1900;
    tm.tm_mon = rd16(st + 2) - 1;
    tm.tm_mday = rd16(st + 6);
    tm.tm_hour = rd16(st + 8);
    tm.tm_min = rd16(st + 10);
    tm.tm_sec = rd16(st + 12);
    *ms = rd16(st + 14);
    return timegm(&tm);
}
static void unix_to_systemtime(const struct tm &tm, uint32_t ms, uint32_t dst) {
    wr16(dst, (uint16_t)(tm.tm_year + 1900));
    wr16(dst + 2, (uint16_t)(tm.tm_mon + 1));
    wr16(dst + 4, (uint16_t)tm.tm_wday);
    wr16(dst + 6, (uint16_t)tm.tm_mday);
    wr16(dst + 8, (uint16_t)tm.tm_hour);
    wr16(dst + 10, (uint16_t)tm.tm_min);
    wr16(dst + 12, (uint16_t)tm.tm_sec);
    wr16(dst + 14, (uint16_t)ms);
}
SHIM(kernel32, SystemTimeToTzSpecificLocalTime, 3) { // (lpTimeZone (NULL = current), lpUniversalTime, lpLocalTime)
    uint32_t ms;
    time_t t = systemtime_to_unix(a[1], &ms);
    struct tm tm;
    localtime_r(&t, &tm);
    unix_to_systemtime(tm, ms, a[2]);
    return 1;
}
SHIM(kernel32, SystemTimeToFileTime, 2) {
    uint32_t ms;
    time_t t = systemtime_to_unix(a[0], &ms);
    wr64(a[1], unix_to_filetime(t, (int64_t)ms * 1000000));
    return 1;
}
SHIM(kernel32, GetLocalTime, 1) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    struct tm tm;
    localtime_r(&ts.tv_sec, &tm);
    unix_to_systemtime(tm, (uint32_t)(ts.tv_nsec / 1000000), a[0]);
    return 0;
}

// ---- memory status: a 4 GiB machine with a 2 GiB user address space (design choice) --------------------------
SHIM(kernel32, GlobalMemoryStatusEx, 1) {
    if (rd32(a[0]) < 64) return 0;
    wr32(a[0] + 4, 25);                    // dwMemoryLoad (%)
    wr64(a[0] + 8, 4096ull << 20);         // ullTotalPhys
    wr64(a[0] + 16, 3072ull << 20);        // ullAvailPhys
    wr64(a[0] + 24, 8192ull << 20);        // ullTotalPageFile
    wr64(a[0] + 32, 7168ull << 20);        // ullAvailPageFile
    wr64(a[0] + 40, 0x7FFE0000ull);        // ullTotalVirtual
    wr64(a[0] + 48, 0x70000000ull);        // ullAvailVirtual
    wr64(a[0] + 56, 0);
    return 1;
}
SHIM(kernel32, GlobalMemoryStatus, 1) {
    wr32(a[0], 32);
    wr32(a[0] + 4, 25);
    wr32(a[0] + 8, 0x7FFFFFFFu);  // dwTotalPhys (saturated, as Windows does for > 2 GiB)
    wr32(a[0] + 12, 0x7FFFFFFFu); // dwAvailPhys
    wr32(a[0] + 16, 0x7FFFFFFFu);
    wr32(a[0] + 20, 0x7FFFFFFFu);
    wr32(a[0] + 24, 0x7FFE0000u); // dwTotalVirtual
    wr32(a[0] + 28, 0x70000000u); // dwAvailVirtual
    return 0;
}

// ---- shlwapi / psapi --------------------------------------------------------------------------------------
SHIM(shlwapi, PathFindFileNameA, 1) {
    std::string s = gstr(a[0]);
    size_t p = s.find_last_of("\\/:");
    return a[0] + (p == std::string::npos || p + 1 >= s.size() ? 0 : (uint32_t)p + 1);
}
SHIM(shlwapi, PathFindFileNameW, 1) {
    std::u16string s = gwstr(a[0]);
    size_t p = s.find_last_of(u"\\/:");
    return a[0] + 2 * (p == std::u16string::npos || p + 1 >= s.size() ? 0 : (uint32_t)p + 1);
}
SHIM(shlwapi, PathRemoveFileSpecA, 1) {
    std::string s = gstr(a[0]);
    size_t p = s.find_last_of("\\/");
    if (p == std::string::npos) return 0;
    g_mem[a[0] + (p == 2 && s[1] == ':' ? 3 : p)] = 0; // keep the root "C:\"
    return 1;
}
SHIM(psapi, GetModuleFileNameExA, 4) {
    std::string path = module_path(a[1]);
    if (path.empty()) {
        last_error(cpu) = ERROR_INVALID_HANDLE;
        return 0;
    }
    uint32_t n = put_str(a[2], a[3], path);
    return n < a[3] ? n : a[3];
}

} // namespace bo1
