// kernel32: process, modules, TLS, heaps, virtual memory, time, system information (T-045).
// Behaviour notes carry a confidence tag where Windows semantics are approximated.
#include <mach/mach_time.h>
#include <sched.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include <atomic>
#include <cstdlib>
#include <mutex>
#include <vector>

#include "image_info.h"
#include "win32_util.h"

namespace bo1 {

const char *const kGuestGameDir = "C:\\bo1";
const char *const kGuestHomeDir = "C:\\users\\player";

// ---- modules ---------------------------------------------------------------------------------------
// DLLs the runtime stands in for. Module "handles" are distinct addresses in an unmapped part of the
// system area; nothing may read through them (LIKELY fine: the game only passes them to GetProcAddress).
static const char *const kModules[] = {"kernel32.dll", "user32.dll",  "gdi32.dll",   "advapi32.dll", "shell32.dll",   "ole32.dll", "shlwapi.dll",
                                       "winmm.dll",    "wsock32.dll", "ws2_32.dll",  "d3d9.dll",     "d3dx9_43.dll",  "dsound.dll",
                                       "xinput1_3.dll", "binkw32.dll", "steam_api.dll", "psapi.dll", "ntdll.dll"};
static const uint32_t kModuleBase = 0x7FD00000u;

static std::string base_name_lower(std::string s) {
    size_t p = s.find_last_of("\\/");
    if (p != std::string::npos) s = s.substr(p + 1);
    for (char &c : s) c = (char)tolower((unsigned char)c);
    if (s.find('.') == std::string::npos) s += ".dll";
    return s;
}
static uint32_t module_find(const std::string &name) {
    std::string n = base_name_lower(name);
    if (n == "blackops.exe") return image_info().base;
    for (size_t i = 0; i < sizeof(kModules) / sizeof(kModules[0]); i++)
        if (n == kModules[i]) return kModuleBase + (uint32_t)i * 0x1000;
    return 0;
}
uint32_t module_list(uint32_t *out, uint32_t max) {
    uint32_t n = 0;
    auto add = [&](uint32_t h) {
        if (n < max) out[n] = h;
        n++;
    };
    add(image_info().base);
    for (size_t i = 0; i < sizeof(kModules) / sizeof(kModules[0]); i++) add(kModuleBase + (uint32_t)i * 0x1000);
    return n;
}
std::string module_path(uint32_t h) {
    if (h == 0 || h == image_info().base) return std::string(kGuestGameDir) + "\\BlackOps.exe";
    uint32_t i = (h - kModuleBase) / 0x1000;
    if (h >= kModuleBase && i < sizeof(kModules) / sizeof(kModules[0])) return std::string("C:\\windows\\system32\\") + kModules[i];
    return "";
}
static const char *module_name(uint32_t h) {
    uint32_t i = (h - kModuleBase) / 0x1000;
    return h >= kModuleBase && i < sizeof(kModules) / sizeof(kModules[0]) ? kModules[i] : nullptr;
}
static std::string narrow(const std::u16string &w) {
    std::string s;
    for (char16_t c : w) s.push_back(c < 256 ? (char)c : '?');
    return s;
}

SHIM(kernel32, GetModuleHandleA, 1) {
    if (!a[0]) return image_info().base;
    uint32_t h = module_find(gstr(a[0]));
    if (!h) {
        last_error(cpu) = ERROR_MOD_NOT_FOUND;
        if (g_verbose) log("GetModuleHandleA(\"%s\") -> not provided", gstr(a[0]).c_str());
    }
    return h;
}
SHIM(kernel32, GetModuleHandleW, 1) {
    if (!a[0]) return image_info().base;
    uint32_t h = module_find(narrow(gwstr(a[0])));
    if (!h) {
        last_error(cpu) = ERROR_MOD_NOT_FOUND;
        if (g_verbose) log("GetModuleHandleW(\"%s\") -> not provided", narrow(gwstr(a[0])).c_str());
    }
    return h;
}
SHIM(kernel32, LoadLibraryA, 1) {
    uint32_t h = module_find(gstr(a[0]));
    if (!h) {
        last_error(cpu) = ERROR_MOD_NOT_FOUND;
        log("LoadLibraryA(\"%s\") -> not provided", gstr(a[0]).c_str());
    }
    return h;
}
SHIM(kernel32, FreeLibrary, 1) { return 1; }
SHIM(kernel32, GetProcAddress, 2) {
    const char *dll = module_name(a[0]);
    std::string name = a[1] > 0xFFFF ? gstr(a[1]) : "#" + std::to_string(a[1]);
    // The game probes for optional APIs and copes with NULL. An emulated module (its loader stub resolves
    // its whole import table this way) must not get NULL: it gets a trampoline that names the function
    // if it is ever called.
    uint32_t caller = rd32(cpu->esp);
    bool from_game = caller >= image_info().text_lo && caller < image_info().text_hi;
    uint32_t t = !dll ? 0 : from_game ? import_lookup(dll, name) : import_lookup_or_stub(dll, name);
    if (!t) {
        last_error(cpu) = ERROR_PROC_NOT_FOUND;
        if (g_verbose) log("GetProcAddress(%s, \"%s\") -> not provided", dll ? dll : "?", name.c_str());
    }
    return t;
}
SHIM(kernel32, GetModuleFileNameA, 3) {
    std::string path = module_path(a[0]);
    if (path.empty()) {
        last_error(cpu) = ERROR_MOD_NOT_FOUND;
        return 0;
    }
    uint32_t n = put_str(a[1], a[2], path);
    return n < a[2] ? n : a[2];
}

// ---- process / environment ---------------------------------------------------------------------------
static uint32_t g_cmdline = 0, g_env_w = 0;
SHIM(kernel32, GetCommandLineA, 0) {
    if (!g_cmdline) {
        std::string c = std::string("\"") + kGuestGameDir + "\\BlackOps.exe\"";
        if (const char *extra = getenv("BO1_ARGS")) c += std::string(" ") + extra;
        g_cmdline = heap_strdup(c);
    }
    return g_cmdline;
}
SHIM(kernel32, GetEnvironmentStringsW, 0) {
    if (!g_env_w) {
        static const char *const vars[] = {"SystemRoot=C:\\windows", "TEMP=C:\\users\\player\\Temp", "TMP=C:\\users\\player\\Temp",
                                           "USERNAME=player", "USERPROFILE=C:\\users\\player"};
        std::u16string block;
        for (const char *v : vars) {
            for (const char *p = v; *p; p++) block.push_back((char16_t)(unsigned char)*p);
            block.push_back(0);
        }
        block.push_back(0);
        g_env_w = heap_alloc((uint32_t)block.size() * 2);
        memcpy(g_mem + g_env_w, block.data(), block.size() * 2);
    }
    return g_env_w;
}
SHIM(kernel32, FreeEnvironmentStringsW, 1) { return 1; }
SHIM(kernel32, GetStartupInfoA, 1) {
    memset(g_mem + a[0], 0, 68);
    wr32(a[0], 68); // cb; no STARTF_USESTDHANDLES, no window placement
    return 0;
}
SHIM(kernel32, GetVersionExA, 1) {
    // Windows 7 SP1 (design choice; the Wine baseline prefix also reports Windows 7 — LIKELY, to confirm)
    uint32_t size = rd32(a[0]);
    wr32(a[0] + 4, 6);
    wr32(a[0] + 8, 1);
    wr32(a[0] + 12, 7601);
    wr32(a[0] + 16, 2); // VER_PLATFORM_WIN32_NT
    put_str(a[0] + 20, 128, "Service Pack 1");
    if (size >= 156) {
        wr16(a[0] + 148, 1); // wServicePackMajor
        wr16(a[0] + 150, 0);
        wr16(a[0] + 152, 0x100); // VER_SUITE_SINGLEUSERTS
        g_mem[a[0] + 154] = 1;   // VER_NT_WORKSTATION
        g_mem[a[0] + 155] = 0;
    }
    return 1;
}
static uint32_t cpu_count() {
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    if (const char *v = getenv("BO1_CPUS")) n = atol(v);
    return (uint32_t)(n < 1 ? 1 : n > 32 ? 32 : n);
}
SHIM(kernel32, GetSystemInfo, 1) {
    memset(g_mem + a[0], 0, 36);
    wr32(a[0] + 4, 0x1000);            // dwPageSize
    wr32(a[0] + 8, 0x00010000);        // lpMinimumApplicationAddress
    wr32(a[0] + 12, 0x7FFEFFFF);       // lpMaximumApplicationAddress
    wr32(a[0] + 16, (uint32_t)((1ull << cpu_count()) - 1));
    wr32(a[0] + 20, cpu_count());
    wr32(a[0] + 24, 586);              // PROCESSOR_INTEL_PENTIUM
    wr32(a[0] + 28, 0x10000);          // dwAllocationGranularity
    wr16(a[0] + 32, 6);                // wProcessorLevel
    wr16(a[0] + 34, 0x0F0B);           // wProcessorRevision (model 15 stepping 11, matches rt_cpuid)
    return 0;
}
SHIM(kernel32, GetProcessAffinityMask, 3) {
    uint32_t mask = (uint32_t)((1ull << cpu_count()) - 1);
    wr32(a[1], mask);
    wr32(a[2], mask);
    return 1;
}
SHIM(kernel32, GetCurrentProcess, 0) { return 0xFFFFFFFFu; }
SHIM(kernel32, GetCurrentThread, 0) { return 0xFFFFFFFEu; }
SHIM(kernel32, GetCurrentProcessId, 0) { return rd32(cpu->teb + TEB_PID); }
SHIM(kernel32, GetCurrentThreadId, 0) { return rd32(cpu->teb + TEB_TID); }
SHIM(kernel32, GetLastError, 0) { return last_error(cpu); }
SHIM(kernel32, SetLastError, 1) {
    last_error(cpu) = a[0];
    return 0;
}
SHIM(kernel32, SetErrorMode, 1) { return 0; }
SHIM(kernel32, SetHandleCount, 1) { return a[0]; }
static std::atomic<uint32_t> g_unhandled_filter{0};
SHIM(kernel32, SetUnhandledExceptionFilter, 1) { return g_unhandled_filter.exchange(a[0]); }
SHIM(kernel32, IsDebuggerPresent, 0) { return 0; }
SHIM(kernel32, SetThreadExecutionState, 1) { return 0x80000000u; }
SHIM(kernel32, OutputDebugStringA, 1) {
    if (g_verbose) {
        std::string s = gstr(a[0]);
        while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
        log("debug: %s", s.c_str());
    }
    return 0;
}
SHIM(kernel32, ExitProcess, 1) {
    log("ExitProcess(%u)", a[0]);
    process_exit(a[0]);
    return 0;
}
SHIM(kernel32, TerminateProcess, 2) {
    log("TerminateProcess(%u)", a[1]);
    process_exit(a[1]);
    return 0;
}
SHIM(kernel32, RaiseException, 4) {
    if (a[0] == 0x406D1388u) return 0; // MSVC "set thread name" convention: a debugger-only notification
    log("RaiseException(code %08x, flags %x, %u args) — no handler support yet", a[0], a[1], a[2]);
    rt_trap(cpu, a[0], TRAP_UNSUPPORTED);
}

// ---- TLS (slots live in the TEB like on Windows, so both the API and inline TEB reads agree) ---------
static std::mutex g_tls_mu;
static uint64_t g_tls_used = 0;
SHIM(kernel32, TlsAlloc, 0) {
    std::lock_guard<std::mutex> lk(g_tls_mu);
    for (uint32_t i = 0; i < 64; i++)
        if (!(g_tls_used >> i & 1)) {
            g_tls_used |= 1ull << i;
            return i; // note: other threads' slot values are not cleared (slots are never reused after TlsFree here)
        }
    return 0xFFFFFFFFu;
}
SHIM(kernel32, TlsFree, 1) { return 1; }
SHIM(kernel32, TlsGetValue, 1) {
    if (a[0] >= 64) {
        last_error(cpu) = ERROR_INVALID_PARAMETER;
        return 0;
    }
    last_error(cpu) = 0; // documented: TlsGetValue clears the last error on success
    return rd32(cpu->teb + TEB_TLS_SLOTS + 4 * a[0]);
}
SHIM(kernel32, TlsSetValue, 2) {
    if (a[0] >= 64) {
        last_error(cpu) = ERROR_INVALID_PARAMETER;
        return 0;
    }
    wr32(cpu->teb + TEB_TLS_SLOTS + 4 * a[0], a[1]);
    return 1;
}

// Fiber-local storage without fibers is thread-local storage (callbacks at thread exit are not run).
SHIM(kernel32, FlsAlloc, 1) {
    std::lock_guard<std::mutex> lk(g_tls_mu);
    for (uint32_t i = 0; i < 64; i++)
        if (!(g_tls_used >> i & 1)) {
            g_tls_used |= 1ull << i;
            return i;
        }
    return 0xFFFFFFFFu;
}
SHIM(kernel32, FlsFree, 1) { return 1; }
SHIM(kernel32, FlsGetValue, 1) {
    if (a[0] >= 64) return 0;
    last_error(cpu) = 0;
    return rd32(cpu->teb + TEB_TLS_SLOTS + 4 * a[0]);
}
SHIM(kernel32, FlsSetValue, 2) {
    if (a[0] >= 64) return 0;
    wr32(cpu->teb + TEB_TLS_SLOTS + 4 * a[0], a[1]);
    return 1;
}
SHIM(kernel32, EncodePointer, 1) { return a[0]; }
SHIM(kernel32, DecodePointer, 1) { return a[0]; }

// ---- heaps: every heap handle is served by the one process allocator ---------------------------------
static std::atomic<uint32_t> g_next_heap{0x00DE0000u};
SHIM(kernel32, HeapCreate, 3) { return g_next_heap.fetch_add(0x10000); }
SHIM(kernel32, GetProcessHeap, 0) { return 0x00DD0000u; }
SHIM(kernel32, HeapAlloc, 3) {
    uint32_t p = heap_alloc(a[2], (a[1] & 8) != 0); // HEAP_ZERO_MEMORY
    if (!p) last_error(cpu) = ERROR_NOT_ENOUGH_MEMORY;
    return p;
}
SHIM(kernel32, HeapFree, 3) {
    heap_free(a[2]);
    return 1;
}
SHIM(kernel32, HeapReAlloc, 4) {
    uint32_t p = heap_realloc(a[2], a[3], (a[1] & 8) != 0);
    if (!p) last_error(cpu) = ERROR_NOT_ENOUGH_MEMORY;
    return p;
}
SHIM(kernel32, HeapSize, 3) { return heap_size(a[2]); }

// ---- virtual memory ---------------------------------------------------------------------------------------
SHIM(kernel32, VirtualAlloc, 4) {
    uint32_t p = vm_alloc(a[0], a[1], false);
    if (!p) {
        last_error(cpu) = ERROR_NOT_ENOUGH_MEMORY;
        log("VirtualAlloc(%08x, %u bytes, type %x) failed", a[0], a[1], a[2]);
    }
    return p;
}
SHIM(kernel32, VirtualFree, 3) {
    if (a[2] & 0x8000) return vm_free(a[0]) ? 1 : 0; // MEM_RELEASE
    if (a[2] & 0x4000) vm_zero(a[0], a[1]);           // MEM_DECOMMIT
    return 1;
}
SHIM(kernel32, VirtualProtect, 4) {
    // Page protections are not modelled (the arena is readable and writable everywhere); report the
    // previous protection as PAGE_EXECUTE_READWRITE so callers that restore it are consistent.
    if (a[3]) wr32(a[3], 0x40);
    return 1;
}
SHIM(kernel32, VirtualQuery, 3) {
    if (a[2] < 28) return 0;
    uint32_t base, size;
    uint32_t page = a[0] & ~0xFFFu;
    memset(g_mem + a[1], 0, 28);
    wr32(a[1], page);
    if (vm_query(a[0], &base, &size)) {
        const ImageInfo &im = image_info();
        wr32(a[1] + 4, base);                  // AllocationBase
        wr32(a[1] + 8, 4);                     // AllocationProtect = PAGE_READWRITE
        wr32(a[1] + 12, base + size - page);   // RegionSize
        wr32(a[1] + 16, 0x1000);               // MEM_COMMIT
        wr32(a[1] + 20, 4);                    // PAGE_READWRITE (protections are not modelled)
        wr32(a[1] + 24, base == im.base ? 0x1000000u : 0x20000u); // MEM_IMAGE / MEM_PRIVATE
    } else {
        wr32(a[1] + 12, 0x10000 - (page & 0xFFFF));
        wr32(a[1] + 16, 0x10000); // MEM_FREE
        wr32(a[1] + 20, 1);       // PAGE_NOACCESS
    }
    return 28;
}

// ---- time ---------------------------------------------------------------------------------------------------
static uint64_t mono_ns() {
    static mach_timebase_info_data_t tb;
    static uint64_t t0;
    if (!tb.denom) {
        mach_timebase_info(&tb);
        t0 = mach_absolute_time();
    }
    return (mach_absolute_time() - t0) * tb.numer / tb.denom;
}
uint32_t tick_ms() { return (uint32_t)(mono_ns() / 1000000ull) + 600000u; } // pretend 10 minutes of uptime
SHIM(kernel32, QueryPerformanceCounter, 1) {
    wr64(a[0], mono_ns() / 100 + 6000000000ull); // 10 MHz, same origin as the tick count
    return 1;
}
SHIM(kernel32, QueryPerformanceFrequency, 1) {
    wr64(a[0], 10000000ull);
    return 1;
}
SHIM(kernel32, GetTickCount, 0) { return tick_ms(); }
SHIM(kernel32, GetSystemTimeAsFileTime, 1) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    wr64(a[0], unix_to_filetime(ts.tv_sec, ts.tv_nsec));
    return 0;
}
static void put_systemtime(uint32_t dst, const struct tm &tm, uint32_t ms) {
    wr16(dst, (uint16_t)(tm.tm_year + 1900));
    wr16(dst + 2, (uint16_t)(tm.tm_mon + 1));
    wr16(dst + 4, (uint16_t)tm.tm_wday);
    wr16(dst + 6, (uint16_t)tm.tm_mday);
    wr16(dst + 8, (uint16_t)tm.tm_hour);
    wr16(dst + 10, (uint16_t)tm.tm_min);
    wr16(dst + 12, (uint16_t)tm.tm_sec);
    wr16(dst + 14, (uint16_t)ms);
}
SHIM(kernel32, FileTimeToSystemTime, 2) {
    uint64_t ft;
    memcpy(&ft, g_mem + a[0], 8);
    time_t sec = (time_t)(ft / 10000000ull) - 11644473600ll;
    struct tm tm;
    gmtime_r(&sec, &tm);
    put_systemtime(a[1], tm, (uint32_t)(ft / 10000 % 1000));
    return 1;
}
SHIM(kernel32, FileTimeToLocalFileTime, 2) {
    uint64_t ft;
    memcpy(&ft, g_mem + a[0], 8);
    time_t sec = (time_t)(ft / 10000000ull) - 11644473600ll;
    struct tm tm;
    localtime_r(&sec, &tm);
    wr64(a[1], ft + (uint64_t)((int64_t)tm.tm_gmtoff * 10000000ll));
    return 1;
}
SHIM(kernel32, GetSystemTime, 1) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    struct tm tm;
    gmtime_r(&ts.tv_sec, &tm);
    put_systemtime(a[0], tm, (uint32_t)(ts.tv_nsec / 1000000));
    return 0;
}
SHIM(kernel32, GetTimeZoneInformation, 1) {
    time_t now = time(nullptr);
    struct tm tm;
    localtime_r(&now, &tm);
    memset(g_mem + a[0], 0, 172);
    wr32(a[0], (uint32_t)(-(tm.tm_gmtoff - (tm.tm_isdst > 0 ? 3600 : 0)) / 60)); // Bias in minutes, standard time
    wr32(a[0] + 168, (uint32_t)-60);                                              // DaylightBias
    return tm.tm_isdst > 0 ? 2 : 1;                                               // TIME_ZONE_ID_DAYLIGHT / STANDARD
}

} // namespace bo1
