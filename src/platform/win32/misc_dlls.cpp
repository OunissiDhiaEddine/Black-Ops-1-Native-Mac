// Small DLLs for the boot runtime (T-045): winmm, advapi32, shell32, ole32 (non-COM parts), psapi,
// xinput1_3, dsound. Devices that do not exist in the headless runtime report "not present" with the
// error code the real API uses, so the game takes its own fallback paths.
#include <sys/stat.h>

#include <cstdlib>

#include "win32_util.h"

namespace bo1 {

uint32_t tick_ms(); // kernel32_core.cpp

// ---- winmm ---------------------------------------------------------------------------------------------
SHIM(winmm, timeGetTime, 0) { return tick_ms(); }
SHIM(winmm, timeBeginPeriod, 1) { return 0; }
SHIM(winmm, timeEndPeriod, 1) { return 0; }
SHIM(winmm, mixerGetNumDevs, 0) { return 0; }
SHIM(winmm, waveInGetNumDevs, 0) { return 0; }
SHIM(winmm, mixerOpen, 5) { return 2; }           // MMSYSERR_BADDEVICEID
SHIM(winmm, mixerClose, 1) { return 5; }          // MMSYSERR_INVALHANDLE
SHIM(winmm, mixerGetLineInfoA, 3) { return 2; }
SHIM(winmm, mixerGetLineControlsA, 3) { return 2; }
SHIM(winmm, mixerGetControlDetailsA, 3) { return 2; }
SHIM(winmm, mixerSetControlDetails, 3) { return 2; }

// ---- advapi32 ------------------------------------------------------------------------------------------
SHIM(advapi32, CryptAcquireContextA, 5) {
    wr32(a[0], 0x00C0FFEE); // dummy provider handle
    return 1;
}
SHIM(advapi32, CryptGenRandom, 3) {
    arc4random_buf(g_mem + a[2], a[1]);
    return 1;
}
SHIM(advapi32, CryptReleaseContext, 2) { return 1; }
SHIM(advapi32, RegOpenKeyExW, 5) { return ERROR_FILE_NOT_FOUND; } // there is no registry: every key is absent
SHIM(advapi32, RegQueryValueExW, 6) { return ERROR_FILE_NOT_FOUND; }
SHIM(advapi32, RegCloseKey, 1) { return 0; }

// ---- shell32 -------------------------------------------------------------------------------------------
SHIM(shell32, SHGetFolderPathA, 5) {
    // (hwnd, csidl, hToken, dwFlags, pszPath[MAX_PATH])
    std::string sub, root = kGuestHomeDir;
    switch (a[1] & 0xFF) {
    case 0x1C: sub = "\\AppData\\Local"; break;   // CSIDL_LOCAL_APPDATA
    case 0x1A: sub = "\\AppData\\Roaming"; break; // CSIDL_APPDATA
    case 0x05: sub = "\\Documents"; break;        // CSIDL_PERSONAL
    case 0x00: sub = "\\Desktop"; break;
    case 0x2E: root = "C:\\users\\Public", sub = "\\Documents"; break; // CSIDL_COMMON_DOCUMENTS
    case 0x23: root = "C:\\ProgramData"; break;                          // CSIDL_COMMON_APPDATA
    default:
        log("SHGetFolderPathA(csidl %#x) not mapped", a[1]);
        return 0x80070057u; // E_INVALIDARG
    }
    std::string path = root + sub;
    if (g_verbose) log("SHGetFolderPathA(csidl %#x) -> %s", a[1], path.c_str());
    std::string host = host_path(path);
    for (size_t i = 1; i <= host.size(); i++)
        if (i == host.size() || host[i] == '/') mkdir(host.substr(0, i).c_str(), 0755);
    put_str(a[4], 260, path);
    return 0; // S_OK
}

// ---- ole32 (memory + GUID helpers; object creation lives with the COM shims) ---------------------------------
SHIM(ole32, CoInitializeEx, 2) { return 0; }
SHIM(ole32, CoUninitialize, 0) { return 0; }
SHIM(ole32, CoTaskMemAlloc, 1) { return heap_alloc(a[0]); }
SHIM(ole32, CoTaskMemFree, 1) {
    heap_free(a[0]);
    return 0;
}
static int hexval(uint16_t c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; }
SHIM(ole32, CLSIDFromString, 2) {
    // "{xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx}" (UTF-16) -> GUID
    std::u16string s = gwstr(a[0]);
    uint8_t nib[32];
    int n = 0;
    for (char16_t c : s) {
        int v = hexval(c);
        if (v >= 0 && n < 32) nib[n++] = (uint8_t)v;
    }
    if (n != 32 || s.size() != 38) return 0x800401F3u; // CO_E_CLASSSTRING
    auto byte = [&](int i) { return (uint8_t)(nib[2 * i] << 4 | nib[2 * i + 1]); };
    uint8_t *g = g_mem + a[1];
    g[0] = byte(3), g[1] = byte(2), g[2] = byte(1), g[3] = byte(0); // Data1 (little endian)
    g[4] = byte(5), g[5] = byte(4);                                 // Data2
    g[6] = byte(7), g[7] = byte(6);                                 // Data3
    for (int i = 8; i < 16; i++) g[i] = byte(i);                    // Data4
    return 0;
}

// ---- shlwapi / winsock start-up (imports of the emulated steam_api.dll) -----------------------------------
SHIM(shlwapi, PathFileExistsW, 1) {
    std::string narrow;
    for (char16_t c : gwstr(a[0])) narrow.push_back(c < 256 ? (char)c : '?');
    struct stat st;
    bool ok = stat(host_path(narrow).c_str(), &st) == 0;
    if (g_verbose) log("PathFileExistsW(\"%s\") -> %d", narrow.c_str(), ok);
    if (!ok) last_error(cpu) = ERROR_FILE_NOT_FOUND;
    return ok;
}
static uint32_t wsa_startup(uint32_t version, uint32_t data) {
    memset(g_mem + data, 0, 400); // WSADATA
    wr16(data, (uint16_t)version);
    wr16(data + 2, 0x0202);
    put_str(data + 4, 257, "WinSock 2.0 (bo1 runtime)");
    return 0;
}
SHIM(ws2_32, WSAStartup, 2) { return wsa_startup(a[0], a[1]); }
SHIM(wsock32, WSAStartup, 2) { return wsa_startup(a[0], a[1]); }
SHIM(psapi, EnumProcessModules, 4) {
    // (hProcess, lphModule, cb, lpcbNeeded): executable first, then every loaded module
    uint32_t list[64];
    uint32_t n = module_list(list, 64);
    for (uint32_t i = 0; i < n && i < 64 && (i + 1) * 4 <= a[2]; i++) wr32(a[1] + 4 * i, list[i]);
    if (a[3]) wr32(a[3], n * 4);
    return 1;
}

// ---- psapi / xinput / dsound ------------------------------------------------------------------------------
SHIM(psapi, GetProcessMemoryInfo, 3) {
    memset(g_mem + a[1], 0, a[2] < 40 ? a[2] : 40);
    if (a[2] >= 4) wr32(a[1], 40);
    return 1;
}
SHIM(xinput1_3, XInputGetState, 2) { return 1167; }        // ERROR_DEVICE_NOT_CONNECTED
SHIM(xinput1_3, XInputSetState, 2) { return 1167; }
SHIM(xinput1_3, XInputGetCapabilities, 3) { return 1167; }
SHIM(dsound, DirectSoundCreate8, 3) { return 0x88780078u; }        // DSERR_NODRIVER
SHIM(dsound, DirectSoundCaptureCreate, 3) { return 0x88780078u; }

} // namespace bo1

namespace bo1 {

// binkw32: see bink_ffmpeg.cpp (ADR-008).

// ---- ole32: object creation ---------------------------------------------------------------------------------
SHIM(ole32, CoCreateInstance, 5) {
    // (rclsid, pUnkOuter, dwClsContext, riid, ppv). XAudio2 2.7 is the only class the game creates
    // (re/com_steam_usage.json); the FAudio-backed implementation is P5 work (ADR-009).
    const uint8_t *g = g_mem + a[0];
    log("CoCreateInstance({%02x%02x%02x%02x-%02x%02x-%02x%02x-...}) -> REGDB_E_CLASSNOTREG (no audio in the headless runtime)", g[3], g[2], g[1], g[0],
        g[5], g[4], g[7], g[6]);
    if (a[4]) wr32(a[4], 0);
    return 0x80040154u;
}

} // namespace bo1
