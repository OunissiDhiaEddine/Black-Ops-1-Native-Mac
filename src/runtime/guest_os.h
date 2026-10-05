// Guest "operating system" services shared by the Win32 shims (T-045): address-space manager,
// heap, handle table, guest threads / TEB / TLS, import trampolines. See docs/HOST_ABI.md.
#pragma once
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include "runtime.h"

namespace bo1 {

// ---- guest address space layout (design choice; the image itself sits at 0x00400000-0x04677000) ----
constexpr uint32_t kNullGuard = 0x00010000u; // [0, kNullGuard) is inaccessible in the boot runtime
constexpr uint32_t kVmLow = 0x05000000u;     // VirtualAlloc / heaps grow upwards from here
constexpr uint32_t kVmHigh = 0x70000000u;
constexpr uint32_t kSysLow = 0x70000000u;    // stacks, TEBs, PEB, strings owned by the runtime
constexpr uint32_t kSysHigh = 0x7FFE0000u;   // nothing at or above 2 GiB: the image is not LARGEADDRESSAWARE
constexpr uint32_t kTrampBase = 0xFFF00000u; // host entry points: never mapped, only ever reached through rt_dispatch
constexpr uint32_t kTrampStep = 16;

// guest <-> host pointers. 0 maps to nullptr and back.
template <class T = uint8_t> inline T *g2h(uint32_t a) { return a ? reinterpret_cast<T *>(g_mem + a) : nullptr; }
inline uint32_t h2g(const void *p) { return p ? (uint32_t)(static_cast<const uint8_t *>(p) - g_mem) : 0; }

// ---- address space ---------------------------------------------------------------------------------
// Regions are page-granular in size and 64 KiB-granular in placement, like VirtualAlloc. Memory is
// zero on allocation. addr = 0 lets the manager choose.
uint32_t vm_alloc(uint32_t addr, uint32_t size, bool system_area = false);
bool vm_free(uint32_t base);
bool vm_query(uint32_t addr, uint32_t *base, uint32_t *size); // region containing addr
void vm_zero(uint32_t addr, uint32_t size);                   // decommit semantics: pages read as zero again
void vm_reserve_fixed(uint32_t base, uint32_t size);          // mark a range as taken (image, guards)

// ---- heap (one process-wide allocator serves every Win32 heap) ------------------------------------------
uint32_t heap_alloc(uint32_t size, bool zero = false);
void heap_free(uint32_t p);
uint32_t heap_size(uint32_t p); // 0xFFFFFFFF if p is not a live block
uint32_t heap_realloc(uint32_t p, uint32_t size, bool zero = false);
uint32_t heap_strdup(const std::string &s);

// ---- handles -----------------------------------------------------------------------------------------------
struct HObject {
    virtual ~HObject() = default;
    virtual const char *kind() const = 0;
};
uint32_t handle_new(std::shared_ptr<HObject> obj);
std::shared_ptr<HObject> handle_get(uint32_t h);
template <class T> std::shared_ptr<T> handle_as(uint32_t h) { return std::dynamic_pointer_cast<T>(handle_get(h)); }
bool handle_close(uint32_t h);

// ---- threads -------------------------------------------------------------------------------------------------
struct GuestThread;
GuestThread *thread_current();
Cpu *thread_cpu(GuestThread *t);
uint32_t thread_id(GuestThread *t);
uint32_t thread_teb(GuestThread *t);
// Creates a guest thread (stack, TEB, static TLS copy) backed by a host pthread. The thread calls
// `entry(param)` in the guest. Returns a handle (HObject of kind "thread") or 0.
uint32_t thread_create(uint32_t entry, uint32_t param, uint32_t stack_size, bool suspended, uint32_t *tid_out);
// Runs `body` as the first guest thread on a host thread with a large stack and waits for it.
int thread_run_main(uint32_t stack_size, const std::function<void(Cpu *)> &body);
[[noreturn]] void thread_exit_current(uint32_t code);
void process_exit(uint32_t code);

// TEB field offsets (x86 NT layout)
enum : uint32_t {
    TEB_SEH = 0x00, TEB_STACK_BASE = 0x04, TEB_STACK_LIMIT = 0x08, TEB_SELF = 0x18, TEB_PID = 0x20, TEB_TID = 0x24,
    TEB_TLS_PTR = 0x2C, TEB_PEB = 0x30, TEB_LAST_ERROR = 0x34, TEB_TLS_SLOTS = 0xE10, TEB_SIZE = 0x1000,
};
inline uint32_t &last_error(Cpu *cpu) { return *reinterpret_cast<uint32_t *>(g_mem + cpu->teb + TEB_LAST_ERROR); }

// Host -> guest call with stdcall arguments (callee pops). Returns eax.
uint32_t guest_call(Cpu *cpu, uint32_t fn, std::initializer_list<uint32_t> args);

// ---- imports ---------------------------------------------------------------------------------------------------
using ShimFn = uint32_t (*)(Cpu *cpu, const uint32_t *a);
struct ShimDef {
    const char *dll;  // lower case, with extension: "kernel32.dll"
    const char *name;
    int nargs;        // 32-bit stack slots; stdcall pops 4*nargs. -1 = cdecl (caller pops)
    ShimFn fn;
    ShimDef *next;
};
void shim_register(ShimDef *d);
// Writes trampoline addresses into the IAT slots of the loaded image (table generated from
// re/import_abi.csv) and checks every shim's argument count against it. Returns the number of
// imports that have no shim yet.
int imports_bind();
// Trampoline for a function resolved by name at run time (GetProcAddress); 0 if we provide no such DLL.
uint32_t import_lookup(const std::string &dll, const std::string &name);
const char *tramp_name(uint32_t target);
void imports_report(); // call statistics (BO1_IMPORT_STATS=1)

#define SHIM(dll_, name_, nargs_)                                                                  \
    static uint32_t shim_##dll_##_##name_(Cpu *cpu, const uint32_t *a);                            \
    static ::bo1::ShimDef def_##dll_##_##name_ = {#dll_ ".dll", #name_, nargs_, shim_##dll_##_##name_, nullptr}; \
    static const struct Reg_##dll_##_##name_ {                                                     \
        Reg_##dll_##_##name_() { ::bo1::shim_register(&def_##dll_##_##name_); }                    \
    } reg_##dll_##_##name_;                                                                        \
    static uint32_t shim_##dll_##_##name_([[maybe_unused]] Cpu *cpu, [[maybe_unused]] const uint32_t *a)

// ---- misc ------------------------------------------------------------------------------------------------------
std::string gstr(uint32_t a);                 // NUL-terminated guest string ("" for 0)
std::u16string gwstr(uint32_t a);
void log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
extern int g_verbose; // BO1_VERBOSE: 1 = unimplemented/odd things, 2 = every import call

} // namespace bo1
