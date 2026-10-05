// Helpers shared by the Win32 shims.
#pragma once
#include <cstring>
#include <string>

#include "guest_os.h"

namespace bo1 {

// Win32 error codes used by the shims
enum : uint32_t {
    ERROR_SUCCESS = 0, ERROR_FILE_NOT_FOUND = 2, ERROR_PATH_NOT_FOUND = 3, ERROR_ACCESS_DENIED = 5, ERROR_INVALID_HANDLE = 6,
    ERROR_NOT_ENOUGH_MEMORY = 8, ERROR_NO_MORE_FILES = 18, ERROR_FILE_EXISTS = 80, ERROR_INVALID_PARAMETER = 87,
    ERROR_INSUFFICIENT_BUFFER = 122, ERROR_MOD_NOT_FOUND = 126, ERROR_PROC_NOT_FOUND = 127, ERROR_ALREADY_EXISTS = 183,
    WAIT_OBJECT_0 = 0, WAIT_IO_COMPLETION = 0xC0, WAIT_TIMEOUT = 258, WAIT_FAILED = 0xFFFFFFFFu, INFINITE_MS = 0xFFFFFFFFu,
    INVALID_HANDLE = 0xFFFFFFFFu,
};

inline uint32_t rd32(uint32_t a) { uint32_t v; memcpy(&v, g_mem + a, 4); return v; }
inline uint16_t rd16(uint32_t a) { uint16_t v; memcpy(&v, g_mem + a, 2); return v; }
inline void wr32(uint32_t a, uint32_t v) { memcpy(g_mem + a, &v, 4); }
inline void wr16(uint32_t a, uint16_t v) { memcpy(g_mem + a, &v, 2); }
inline void wr64(uint32_t a, uint64_t v) { memcpy(g_mem + a, &v, 8); }

// Copies s into a guest buffer of `cap` bytes (always NUL-terminated when cap > 0). Returns strlen(s).
inline uint32_t put_str(uint32_t dst, uint32_t cap, const std::string &s) {
    if (dst && cap) {
        uint32_t n = (uint32_t)s.size() < cap - 1 ? (uint32_t)s.size() : cap - 1;
        memcpy(g_mem + dst, s.data(), n);
        g_mem[dst + n] = 0;
    }
    return (uint32_t)s.size();
}

// The guest sees the game directory as C:\bo1 and a per-user area as C:\users\player.
extern const char *const kGuestGameDir; // "C:\\bo1"
extern const char *const kGuestHomeDir; // "C:\\users\\player"
std::string host_path(const std::string &win_path); // Windows path (absolute or relative to C:\bo1) -> host path
std::string guest_full_path(const std::string &win_path);

// FILETIME (100 ns units since 1601-01-01) <-> Unix time
inline uint64_t unix_to_filetime(int64_t sec, int64_t nsec) { return (uint64_t)(sec + 11644473600ll) * 10000000ull + (uint64_t)(nsec / 100); }

// kernel objects living in kernel32_sync.cpp that other shims wait on / create
uint32_t wait_one(Cpu *cpu, uint32_t handle, uint32_t ms, bool alertable);
bool run_apcs(Cpu *cpu);                                                   // returns true if at least one ran
void queue_apc(uint32_t routine, uint32_t a0, uint32_t a1, uint32_t a2);   // to the calling thread

// from guest_os.cpp
bool thread_resume(HObject *o);
bool thread_wait(HObject *o, uint32_t ms, bool *is_thread);
bool thread_exit_code(HObject *o, uint32_t *code);

} // namespace bo1
