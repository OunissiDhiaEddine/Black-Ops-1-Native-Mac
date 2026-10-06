// Emulated x86 modules (ADR-010, docs/HOST_ABI.md §7a; task T-024).
// A DLL that is not lifted — the shipped steam_api.dll — is mapped UNMODIFIED into the guest arena and
// executed by an embedded x86 interpreter (Unicorn) that shares the arena and the calling thread's
// Cpu. Nothing here looks inside the module: it is loaded the way the Windows loader would load it
// (sections, relocations, imports, DllMain) and then simply runs.
//   game -> module : rt_dispatch() finds no lifted function for the target -> emu_call()
//   module -> host : the module's imports are trampoline addresses; reaching one leaves the
//                    interpreter and runs the same Win32 shim the lifted game would get
//   module -> game : game code reached from the module is interpreted too (it is x86 in the arena)
// Limits (first cut): x87/SSE registers are not carried across the boundary; one interpreter
// instance per guest thread, so code generated on one thread after another thread has executed the
// same pages would not be re-translated there.
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <unicorn/unicorn.h>

#include <cstdlib>
#include <map>
#include <mutex>
#include <vector>

#include "image_info.h"
#include "win32_util.h"

namespace bo1 {

struct Module {
    std::string name; // lower case, e.g. "steam_api.dll"
    uint32_t base = 0, size = 0, entry = 0;
    std::map<std::string, uint32_t> exports;
};
static std::vector<Module> g_modules;
static std::mutex g_mod_mu;

static uint16_t f16(const uint8_t *p) { uint16_t v; memcpy(&v, p, 2); return v; }
static uint32_t f32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }

// ---- loader ---------------------------------------------------------------------------------------------
// Returns the load address or 0. `host_file` is read-only input; the image in the arena is the only copy
// that is ever written (by the module itself, through the interpreter).
uint32_t emu_module_load(const std::string &host_file, const std::string &dll_name) {
    int fd = open(host_file.c_str(), O_RDONLY);
    struct stat st;
    if (fd < 0 || fstat(fd, &st) != 0) return 0;
    std::vector<uint8_t> file((size_t)st.st_size);
    ssize_t got = read(fd, file.data(), file.size());
    close(fd);
    if (got != (ssize_t)file.size() || file.size() < 0x200 || f16(&file[0]) != 0x5A4D) return 0;
    const uint8_t *pe = &file[f32(&file[0x3C])];
    if (f32(pe) != 0x00004550 || f16(pe + 24) != 0x10B) return 0;
    const uint8_t *opt = pe + 24;
    uint16_t nsec = f16(pe + 6), opt_size = f16(pe + 20);
    uint32_t pref = f32(opt + 28), size = f32(opt + 56), headers = f32(opt + 60);
    uint32_t base = vm_alloc(pref, size);
    if (base != pref) {
        if (base) vm_free(base);
        base = vm_alloc(0, size);
    }
    if (!base) return 0;
    memcpy(g_mem + base, file.data(), headers < file.size() ? headers : file.size());
    const uint8_t *sec = opt + opt_size;
    for (unsigned i = 0; i < nsec; i++, sec += 40) {
        uint32_t vsize = f32(sec + 8), va = f32(sec + 12), raw_size = f32(sec + 16), raw = f32(sec + 20);
        uint32_t n = raw_size < vsize || !vsize ? raw_size : vsize;
        if ((uint64_t)raw + n <= file.size() && (uint64_t)va + n <= size) memcpy(g_mem + base + va, &file[raw], n);
    }
    auto dir = [&](int idx, uint32_t *rva, uint32_t *len) {
        *rva = f32(opt + 96 + 8 * idx);
        *len = f32(opt + 100 + 8 * idx);
    };
    uint32_t rva, len;
    // base relocations (only when the preferred base was taken)
    if (base != pref) {
        dir(5, &rva, &len);
        uint32_t delta = base - pref;
        for (uint32_t off = 0; rva && off + 8 <= len;) {
            uint32_t page = rd32(base + rva + off), block = rd32(base + rva + off + 4);
            if (block < 8) break;
            for (uint32_t k = 8; k + 2 <= block; k += 2) {
                uint16_t e = rd16(base + rva + off + k);
                if ((e >> 12) == 3) wr32(base + page + (e & 0xFFF), rd32(base + page + (e & 0xFFF)) + delta); // IMAGE_REL_BASED_HIGHLOW
            }
            off += block;
        }
    }
    Module m;
    m.name = dll_name;
    m.base = base;
    m.size = size;
    m.entry = f32(opt + 16) ? base + f32(opt + 16) : 0;
    // imports -> trampolines of the Win32 shims (a missing shim stops the guest with the function's name)
    dir(1, &rva, &len);
    for (uint32_t d = base + rva; rva && rd32(d + 12); d += 20) {
        std::string dll = gstr(base + rd32(d + 12));
        for (char &c : dll) c = (char)tolower((unsigned char)c);
        uint32_t names = rd32(d) ? rd32(d) : rd32(d + 16), iat = rd32(d + 16);
        for (uint32_t k = 0;; k += 4) {
            uint32_t thunk = rd32(base + names + k);
            if (!thunk) break;
            std::string fn = (thunk & 0x80000000u) ? "#" + std::to_string(thunk & 0xFFFF) : gstr(base + thunk + 2);
            wr32(base + iat + k, import_lookup_or_stub(dll, fn));
            if (g_verbose) log("%s imports %s!%s", dll_name.c_str(), dll.c_str(), fn.c_str());
        }
    }
    // exports
    dir(0, &rva, &len);
    if (rva) {
        uint32_t e = base + rva, n = rd32(e + 24), funcs = base + rd32(e + 28), names = base + rd32(e + 32), ords = base + rd32(e + 36);
        for (uint32_t i = 0; i < n; i++) m.exports[gstr(base + rd32(names + 4 * i))] = base + rd32(funcs + 4 * rd16(ords + 2 * i));
    }
    log("loaded %s at %08x-%08x (%zu exports, entry %08x) for interpretation", dll_name.c_str(), base, base + size, m.exports.size(), m.entry);
    std::lock_guard<std::mutex> lk(g_mod_mu);
    g_modules.push_back(m);
    return base;
}

uint32_t emu_module_export(const std::string &dll, const std::string &name) {
    std::lock_guard<std::mutex> lk(g_mod_mu);
    for (const Module &m : g_modules)
        if (m.name == dll) {
            auto it = m.exports.find(name);
            return it == m.exports.end() ? 0 : it->second;
        }
    return 0;
}
uint32_t emu_module_base(const std::string &dll) {
    std::lock_guard<std::mutex> lk(g_mod_mu);
    for (const Module &m : g_modules)
        if (m.name == dll) return m.base;
    return 0;
}

uint32_t emu_module_by_index(unsigned i, std::string *name) {
    std::lock_guard<std::mutex> lk(g_mod_mu);
    if (i >= g_modules.size()) return 0;
    if (name) *name = g_modules[i].name;
    return g_modules[i].base;
}

// ---- interpreter ------------------------------------------------------------------------------------------
struct Emu {
    uc_engine *uc = nullptr;
    uint32_t teb = 0, gdt = 0;
};
// Flat ring-0 segments plus FS based at the thread's TEB (GDT descriptor layout: Intel SDM vol. 3, 3.4.5).
static uint64_t descriptor(uint32_t base, uint32_t limit, uint32_t access, uint32_t flags) {
    return (uint64_t)(limit & 0xFFFF) | ((uint64_t)(base & 0xFFFFFF) << 16) | ((uint64_t)access << 40) | ((uint64_t)((limit >> 16) & 0xF) << 48) |
           ((uint64_t)(flags & 0xF) << 52) | ((uint64_t)(base >> 24) << 56);
}
static thread_local Emu t_emu;
static const int kRegs[8] = {UC_X86_REG_EAX, UC_X86_REG_ECX, UC_X86_REG_EDX, UC_X86_REG_EBX, UC_X86_REG_ESP, UC_X86_REG_EBP, UC_X86_REG_ESI, UC_X86_REG_EDI};

static Emu &emu_get(Cpu *cpu) {
    Emu &e = t_emu;
    if (!e.uc) {
        if (uc_open(UC_ARCH_X86, UC_MODE_32, &e.uc) != UC_ERR_OK) {
            log("cannot create the x86 interpreter");
            rt_trap(cpu, 0, TRAP_UNSUPPORTED);
        }
        // The interpreter addresses the guest arena directly. The null guard and the trampoline zone stay
        // unmapped: fetching from the latter is how module code "calls the host".
        uc_err err = uc_mem_map_ptr(e.uc, kNullGuard, kTrampBase - kNullGuard, UC_PROT_ALL, g_mem + kNullGuard);
        if (err != UC_ERR_OK) {
            log("interpreter: cannot map the arena: %s", uc_strerror(err));
            rt_trap(cpu, 0, TRAP_UNSUPPORTED);
        }
        uint32_t fcw = 0x27F, mxcsr = 0x1F80;
        uc_reg_write(e.uc, UC_X86_REG_FPCW, &fcw);
        uc_reg_write(e.uc, UC_X86_REG_MXCSR, &mxcsr);
    }
    if (e.teb != cpu->teb) { // fs:[x] must reach this thread's TEB
        if (!e.gdt) e.gdt = heap_alloc(64, true);
        wr64(e.gdt + 8, descriptor(0, 0xFFFFF, 0x9B, 0xC));          // 0x08: code
        wr64(e.gdt + 16, descriptor(0, 0xFFFFF, 0x93, 0xC));         // 0x10: data / stack
        wr64(e.gdt + 24, descriptor(cpu->teb, 0xFFFFF, 0x93, 0xC));  // 0x18: FS
        uc_x86_mmr gdtr = {};
        gdtr.base = e.gdt;
        gdtr.limit = 4 * 8 - 1;
        uc_reg_write(e.uc, UC_X86_REG_GDTR, &gdtr);
        static const struct { int reg; uint32_t sel; } segs[] = {{UC_X86_REG_SS, 0x10}, {UC_X86_REG_CS, 0x08}, {UC_X86_REG_DS, 0x10},
                                                                 {UC_X86_REG_ES, 0x10}, {UC_X86_REG_FS, 0x18}, {UC_X86_REG_GS, 0x10}};
        for (const auto &sg : segs) {
            uc_err err = uc_reg_write(e.uc, sg.reg, &sg.sel);
            if (err != UC_ERR_OK) log("interpreter: segment setup failed: %s", uc_strerror(err));
        }
        e.teb = cpu->teb;
    }
    return e;
}
static void sync_in(Emu &e, const Cpu *cpu) {
    const uint32_t *r = &cpu->eax;
    for (int i = 0; i < 8; i++) uc_reg_write(e.uc, kRegs[i], &r[i]);
    uint32_t efl = 0x202u | ((uint32_t)cpu->df << 10);
    uc_reg_write(e.uc, UC_X86_REG_EFLAGS, &efl);
}
static void sync_out(Emu &e, Cpu *cpu) {
    uint32_t *r = &cpu->eax;
    for (int i = 0; i < 8; i++) uc_reg_read(e.uc, kRegs[i], &r[i]);
    uint32_t efl = 0;
    uc_reg_read(e.uc, UC_X86_REG_EFLAGS, &efl);
    cpu->df = (efl >> 10) & 1;
}

// Runs x86 code at `target` as a function called by the guest: the return address is already on the
// guest stack. Returns when that call returns (the callee has popped the return address, and its
// arguments if it is stdcall).
static void emu_call(Cpu *cpu, uint32_t target) {
    Emu &e = emu_get(cpu);
    const uint32_t entry_esp = cpu->esp, ret_addr = rd32(cpu->esp);
    uint32_t pc = target;
    for (;;) {
        sync_in(e, cpu);
        uc_err err = uc_emu_start(e.uc, pc, ret_addr, 0, 0);
        sync_out(e, cpu);
        uint32_t eip = 0;
        uc_reg_read(e.uc, UC_X86_REG_EIP, &eip);
        if (err == UC_ERR_OK && eip == ret_addr) {
            if (cpu->esp > entry_esp) return;
            // the same address was reached at a deeper nesting level: step over it and keep going
            sync_in(e, cpu);
            err = uc_emu_start(e.uc, eip, 0, 0, 1);
            sync_out(e, cpu);
            uc_reg_read(e.uc, UC_X86_REG_EIP, &eip);
            if (err == UC_ERR_OK) {
                pc = eip;
                continue;
            }
        }
        if (eip >= kTrampBase) { // module code calls (or jumps to) a host function
            uint32_t back = rd32(cpu->esp), esp_at_call = cpu->esp;
            rt_dispatch(cpu, eip);
            if (back == ret_addr && esp_at_call == entry_esp) return; // tail jump into an import: the call is over
            pc = back;
            continue;
        }
        log("interpreter stopped: %s at eip %08x (entered at %08x)", uc_strerror(err), eip, target);
        rt_trap(cpu, eip, TRAP_UNSUPPORTED);
    }
}

// rt_dispatch() hook: a target that is not lifted. Code of the game image itself must be lifted (a miss
// there is a CFG-recovery gap to fix, not something to paper over), everything else is interpreted.
static int unknown_target(Cpu *cpu, uint32_t target) {
    const ImageInfo &im = image_info();
    if (target < kNullGuard || (target >= im.text_lo && target < im.text_hi)) return 0;
    uint32_t base, size;
    if (!vm_query(target, &base, &size)) return 0; // not in memory anybody allocated
    emu_call(cpu, target);
    return 1;
}

// Loads the module, points the game's imports of it at its exports and runs its DllMain.
bool emu_module_attach(Cpu *cpu, const std::string &guest_path, const std::string &dll_name) {
    g_rt_unknown_target = unknown_target;
    uint32_t base = emu_module_load(host_path(guest_path), dll_name);
    if (!base) {
        log("cannot load %s", guest_path.c_str());
        return false;
    }
    int bound = 0, missing = 0;
    imports_redirect(dll_name, [&](const std::string &fn) {
        uint32_t va = emu_module_export(dll_name, fn);
        va ? bound++ : missing++;
        if (!va) log("%s does not export %s", dll_name.c_str(), fn.c_str());
        return va;
    });
    log("%s: %d of the game's imports bound to its exports (%d missing)", dll_name.c_str(), bound, missing);
    uint32_t entry = 0;
    {
        std::lock_guard<std::mutex> lk(g_mod_mu);
        entry = g_modules.back().entry;
    }
    if (entry) {
        uint32_t ok = guest_call(cpu, entry, {base, 1 /* DLL_PROCESS_ATTACH */, 0});
        log("%s: DllMain(DLL_PROCESS_ATTACH) returned %u", dll_name.c_str(), ok);
        if (!ok) return false;
    }
    return true;
}

} // namespace bo1
