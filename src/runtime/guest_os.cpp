// Guest OS services for the boot runtime (T-045). See guest_os.h and docs/HOST_ABI.md.
#include "guest_os.h"

#include <pthread.h>
#include <sys/mman.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <csetjmp>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "image_info.h"

namespace bo1 {

int g_verbose = 0;

void log(const char *fmt, ...) {
    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    GuestThread *t = thread_current();
    fprintf(stderr, "[bo1 t%u] %s\n", t ? thread_id(t) : 0, buf);
}

std::string gstr(uint32_t a) { return a ? std::string(reinterpret_cast<const char *>(g_mem + a)) : std::string(); }
std::u16string gwstr(uint32_t a) {
    std::u16string s;
    if (!a) return s;
    for (;; a += 2) {
        uint16_t c;
        memcpy(&c, g_mem + a, 2);
        if (!c) break;
        s.push_back(c);
    }
    return s;
}

// ---- image facts -----------------------------------------------------------------------------------
static ImageInfo g_image;
const ImageInfo &image_info() { return g_image; }

bool image_parse(const void *file, size_t size) {
    const uint8_t *f = static_cast<const uint8_t *>(file);
    auto rd32 = [&](size_t off) { uint32_t v = 0; if (off + 4 <= size) memcpy(&v, f + off, 4); return v; };
    auto rd16 = [&](size_t off) { uint16_t v = 0; if (off + 2 <= size) memcpy(&v, f + off, 2); return v; };
    if (size < 0x100 || rd16(0) != 0x5A4D) return false;
    size_t pe = rd32(0x3C);
    if (rd32(pe) != 0x00004550 || rd16(pe + 24) != 0x10B) return false; // "PE\0\0", PE32 optional header
    size_t opt = pe + 24;
    uint16_t nsec = rd16(pe + 6), opt_size = rd16(pe + 20);
    g_image.base = rd32(opt + 28);
    g_image.entry = g_image.base + rd32(opt + 16);
    g_image.size = rd32(opt + 56);
    g_image.stack_reserve = rd32(opt + 72);
    size_t sec = opt + opt_size;
    auto rva_to_off = [&](uint32_t rva) -> size_t {
        for (unsigned i = 0; i < nsec; i++) {
            size_t s = sec + 40 * i;
            uint32_t va = rd32(s + 12), vsz = rd32(s + 8), raw = rd32(s + 20), rsz = rd32(s + 16);
            if (rva >= va && rva < va + (vsz > rsz ? vsz : rsz)) return raw + (rva - va);
        }
        return 0;
    };
    for (unsigned i = 0; i < nsec; i++) {
        size_t s = sec + 40 * i;
        if (rd32(s + 36) & 0x20000000u) { // IMAGE_SCN_MEM_EXECUTE
            g_image.text_lo = g_image.base + rd32(s + 12);
            g_image.text_hi = g_image.text_lo + rd32(s + 8);
            break;
        }
    }
    uint32_t tls_rva = rd32(opt + 96 + 8 * 9); // data directory 9 = TLS
    if (tls_rva) {
        size_t t = rva_to_off(tls_rva);
        g_image.tls_start = rd32(t);
        g_image.tls_end = rd32(t + 4);
        g_image.tls_index_va = rd32(t + 8);
        g_image.tls_zero_fill = rd32(t + 16);
    }
    return true;
}

// ---- address space ---------------------------------------------------------------------------------
static std::mutex g_vm_mu;
static std::map<uint32_t, uint32_t> g_regions; // base -> size

static uint32_t page_up(uint64_t v) { return (uint32_t)((v + 0xFFF) & ~0xFFFull); }

static bool range_free(uint32_t base, uint32_t size) {
    auto it = g_regions.upper_bound(base);
    if (it != g_regions.end() && it->first < (uint64_t)base + size) return false;
    if (it != g_regions.begin()) {
        --it;
        if ((uint64_t)it->first + it->second > base) return false;
    }
    return true;
}

void vm_reserve_fixed(uint32_t base, uint32_t size) {
    std::lock_guard<std::mutex> lk(g_vm_mu);
    g_regions[base] = page_up(size);
}

uint32_t vm_alloc(uint32_t addr, uint32_t size, bool system_area) {
    if (!size) return 0;
    std::lock_guard<std::mutex> lk(g_vm_mu);
    if (addr) {
        uint32_t base = addr & ~0xFFFFu;
        uint32_t len = page_up((uint64_t)addr + size - base);
        if (!range_free(base, len)) {
            // commit inside an existing reservation is a no-op (memory is demand-zero already)
            auto it = g_regions.upper_bound(addr);
            if (it != g_regions.begin()) {
                --it;
                if (addr >= it->first && (uint64_t)addr + size <= (uint64_t)it->first + it->second) return addr & ~0xFFFu;
            }
            return 0;
        }
        g_regions[base] = len;
        return base;
    }
    uint32_t len = page_up(size);
    uint32_t lo = system_area ? kSysLow : kVmLow, hi = system_area ? kSysHigh : kVmHigh;
    uint64_t cand = lo;
    for (auto it = g_regions.lower_bound(lo); cand + len <= hi; ++it) {
        if (it == g_regions.end() || it->first >= cand + len) {
            g_regions[(uint32_t)cand] = len;
            return (uint32_t)cand;
        }
        cand = std::max<uint64_t>(cand, ((uint64_t)it->first + it->second + 0xFFFF) & ~0xFFFFull);
    }
    return 0;
}

void vm_zero(uint32_t addr, uint32_t size) {
    uint32_t a = addr & ~0xFFFu, len = page_up((uint64_t)addr + size - a);
    // a fresh anonymous mapping over the range: reads as zero again and gives the pages back to the host
    mmap(g_mem + a, len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0);
}

bool vm_free(uint32_t base) {
    uint32_t size;
    {
        std::lock_guard<std::mutex> lk(g_vm_mu);
        auto it = g_regions.find(base);
        if (it == g_regions.end()) return false;
        size = it->second;
        g_regions.erase(it);
    }
    vm_zero(base, size);
    return true;
}

bool vm_query(uint32_t addr, uint32_t *base, uint32_t *size) {
    std::lock_guard<std::mutex> lk(g_vm_mu);
    auto it = g_regions.upper_bound(addr);
    if (it == g_regions.begin()) return false;
    --it;
    if ((uint64_t)it->first + it->second <= addr) return false;
    *base = it->first;
    *size = it->second;
    return true;
}

// ---- heap ------------------------------------------------------------------------------------------------
// Size-class free lists carved from 1 MiB chunks; blocks above 32 KiB are regions of their own.
// Block sizes are kept host-side, so guest overruns cannot corrupt allocator metadata.
static std::mutex g_heap_mu;
static std::unordered_map<uint32_t, uint32_t> g_live; // address -> requested size
static const uint32_t kClasses[] = {16, 32, 48, 64, 96, 128, 192, 256, 384, 512, 768, 1024, 1536, 2048, 3072, 4096, 6144, 8192, 12288, 16384, 24576, 32768};
static std::vector<uint32_t> g_free[sizeof(kClasses) / sizeof(kClasses[0])];
static uint32_t g_chunk = 0, g_chunk_left = 0;

static int class_of(uint32_t size) {
    for (size_t i = 0; i < sizeof(kClasses) / sizeof(kClasses[0]); i++)
        if (size <= kClasses[i]) return (int)i;
    return -1;
}

uint32_t heap_alloc(uint32_t size, bool zero) {
    uint32_t want = size ? size : 1;
    int c = class_of(want);
    uint32_t p;
    if (c < 0) {
        p = vm_alloc(0, want);
        if (!p) return 0;
        std::lock_guard<std::mutex> lk(g_heap_mu);
        g_live[p] = size;
        return p; // fresh region: already zero
    }
    {
        std::lock_guard<std::mutex> lk(g_heap_mu);
        if (!g_free[c].empty()) {
            p = g_free[c].back();
            g_free[c].pop_back();
        } else {
            if (g_chunk_left < kClasses[c]) {
                g_chunk = vm_alloc(0, 1u << 20);
                if (!g_chunk) return 0;
                g_chunk_left = 1u << 20;
            }
            p = g_chunk;
            g_chunk += kClasses[c];
            g_chunk_left -= kClasses[c];
        }
        g_live[p] = size;
    }
    if (zero) memset(g_mem + p, 0, kClasses[c]);
    return p;
}

uint32_t heap_size(uint32_t p) {
    std::lock_guard<std::mutex> lk(g_heap_mu);
    auto it = g_live.find(p);
    return it == g_live.end() ? 0xFFFFFFFFu : it->second;
}

void heap_free(uint32_t p) {
    if (!p) return;
    uint32_t size;
    {
        std::lock_guard<std::mutex> lk(g_heap_mu);
        auto it = g_live.find(p);
        if (it == g_live.end()) {
            log("heap_free: %08x is not a live block (ignored)", p);
            return;
        }
        size = it->second;
        g_live.erase(it);
        int c = class_of(size ? size : 1);
        if (c >= 0) {
            g_free[c].push_back(p);
            return;
        }
    }
    vm_free(p);
}

uint32_t heap_realloc(uint32_t p, uint32_t size, bool zero) {
    if (!p) return heap_alloc(size, zero);
    uint32_t old = heap_size(p);
    if (old == 0xFFFFFFFFu) return 0;
    int co = class_of(old ? old : 1), cn = class_of(size ? size : 1);
    if (co >= 0 && co == cn) {
        if (zero && size > old) memset(g_mem + p + old, 0, size - old);
        std::lock_guard<std::mutex> lk(g_heap_mu);
        g_live[p] = size;
        return p;
    }
    uint32_t q = heap_alloc(size, zero);
    if (!q) return 0;
    memcpy(g_mem + q, g_mem + p, std::min(old, size));
    heap_free(p);
    return q;
}

uint32_t heap_strdup(const std::string &s) {
    uint32_t p = heap_alloc((uint32_t)s.size() + 1);
    memcpy(g_mem + p, s.c_str(), s.size() + 1);
    return p;
}

// ---- handles ---------------------------------------------------------------------------------------------
static std::mutex g_handle_mu;
static std::unordered_map<uint32_t, std::shared_ptr<HObject>> g_handles;
static uint32_t g_next_handle = 0x100;

uint32_t handle_new(std::shared_ptr<HObject> obj) {
    std::lock_guard<std::mutex> lk(g_handle_mu);
    uint32_t h = g_next_handle;
    g_next_handle += 4;
    g_handles[h] = std::move(obj);
    return h;
}
std::shared_ptr<HObject> handle_get(uint32_t h) {
    std::lock_guard<std::mutex> lk(g_handle_mu);
    auto it = g_handles.find(h);
    return it == g_handles.end() ? nullptr : it->second;
}
bool handle_close(uint32_t h) {
    std::lock_guard<std::mutex> lk(g_handle_mu);
    return g_handles.erase(h) != 0;
}

// ---- threads ---------------------------------------------------------------------------------------------
struct GuestThread {
    Cpu cpu{};
    uint32_t tid = 0, teb = 0, stack_lo = 0, stack_hi = 0;
    uint32_t entry = 0, param = 0;
    pthread_t host{};
    std::mutex mu;
    std::condition_variable cv;
    bool suspended = false, finished = false;
    uint32_t exit_code = 0;
    jmp_buf exit_jmp;
    std::function<void(Cpu *)> body; // main thread only
};
static thread_local GuestThread *t_current = nullptr;
static std::atomic<uint32_t> g_next_tid{0x1000};
static const uint32_t kPid = 0x0BB0;
static uint32_t g_peb = 0;
static const uint32_t kRetMagic = 0xFFEF0000u; // return address pushed for host->guest calls (never executed)

GuestThread *thread_current() { return t_current; }
Cpu *thread_cpu(GuestThread *t) { return &t->cpu; }
uint32_t thread_id(GuestThread *t) { return t->tid; }
uint32_t thread_teb(GuestThread *t) { return t->teb; }

static bool thread_setup(GuestThread *t, uint32_t stack_size) {
    const ImageInfo &im = image_info();
    if (!g_peb) g_peb = vm_alloc(0, 0x1000, true);
    stack_size = std::max<uint32_t>(page_up(stack_size ? stack_size : im.stack_reserve), 0x10000);
    t->stack_lo = vm_alloc(0, stack_size, true);
    t->teb = vm_alloc(0, TEB_SIZE + 0x1000, true);
    if (!t->stack_lo || !t->teb) return false;
    t->stack_hi = t->stack_lo + stack_size;
    t->tid = g_next_tid.fetch_add(4);
    auto put = [&](uint32_t off, uint32_t v) { memcpy(g_mem + t->teb + off, &v, 4); };
    put(TEB_SEH, 0xFFFFFFFFu);
    put(TEB_STACK_BASE, t->stack_hi);
    put(TEB_STACK_LIMIT, t->stack_lo);
    put(TEB_SELF, t->teb);
    put(TEB_PID, kPid);
    put(TEB_TID, t->tid);
    put(TEB_PEB, g_peb);
    // static TLS (__declspec(thread)): TEB->ThreadLocalStoragePointer[_tls_index] -> per-thread copy of the template
    if (im.tls_start) {
        uint32_t len = im.tls_end - im.tls_start;
        uint32_t block = heap_alloc(len + im.tls_zero_fill + 16, true);
        memcpy(g_mem + block, g_mem + im.tls_start, len);
        uint32_t array = heap_alloc(4 * 64, true);
        uint32_t index = 0;
        memcpy(g_mem + im.tls_index_va, &index, 4);
        memcpy(g_mem + array, &block, 4);
        put(TEB_TLS_PTR, array);
    }
    memset(&t->cpu, 0, sizeof(t->cpu));
    t->cpu.esp = t->stack_hi - 0x40;
    t->cpu.teb = t->teb;
    t->cpu.fcw = 0x027F;
    t->cpu.mxcsr = 0x1F80;
    return true;
}

uint32_t guest_call(Cpu *cpu, uint32_t fn, std::initializer_list<uint32_t> args) {
    uint32_t saved = cpu->esp;
    for (auto it = std::rbegin(args); it != std::rend(args); ++it) PUSH32(*it);
    PUSH32(kRetMagic);
    rt_dispatch(cpu, fn);
    cpu->esp = saved; // stdcall callees already restored it; cdecl ones leave the arguments
    return cpu->eax;
}

struct ThreadObject : HObject {
    GuestThread *t;
    explicit ThreadObject(GuestThread *t_) : t(t_) {}
    const char *kind() const override { return "thread"; }
};

static void *thread_host_main(void *arg) {
    GuestThread *t = static_cast<GuestThread *>(arg);
    t_current = t;
    {
        std::unique_lock<std::mutex> lk(t->mu);
        t->cv.wait(lk, [&] { return !t->suspended; });
    }
    if (setjmp(t->exit_jmp) == 0) {
        if (t->body) t->body(&t->cpu);
        else t->exit_code = guest_call(&t->cpu, t->entry, {t->param});
    }
    {
        std::lock_guard<std::mutex> lk(t->mu);
        t->finished = true;
    }
    t->cv.notify_all();
    return nullptr;
}

// Host stack for a guest thread: lifted frames are larger than the x86 frames they replace and the
// host stack additionally nests one frame per guest call, so give it 16x the guest reservation.
static bool thread_start(GuestThread *t, uint32_t guest_stack) {
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, std::max<size_t>((size_t)guest_stack * 16, 16u << 20));
    int rc = pthread_create(&t->host, &attr, thread_host_main, t);
    pthread_attr_destroy(&attr);
    return rc == 0;
}

uint32_t thread_create(uint32_t entry, uint32_t param, uint32_t stack_size, bool suspended, uint32_t *tid_out) {
    auto *t = new GuestThread();
    if (!thread_setup(t, stack_size)) {
        delete t;
        return 0;
    }
    t->entry = entry;
    t->param = param;
    t->suspended = suspended;
    if (tid_out) *tid_out = t->tid;
    uint32_t h = handle_new(std::make_shared<ThreadObject>(t));
    if (!thread_start(t, t->stack_hi - t->stack_lo)) return 0;
    return h;
}

int thread_run_main(uint32_t stack_size, const std::function<void(Cpu *)> &body) {
    auto *t = new GuestThread();
    if (!thread_setup(t, stack_size)) return -1;
    t->body = body;
    if (!thread_start(t, std::max<uint32_t>(t->stack_hi - t->stack_lo, 4u << 20))) return -1;
    pthread_join(t->host, nullptr);
    return (int)t->exit_code;
}

void thread_exit_current(uint32_t code) {
    GuestThread *t = t_current;
    t->exit_code = code;
    longjmp(t->exit_jmp, 1);
}

void process_exit(uint32_t code) {
    imports_report();
    fflush(nullptr);
    _exit((int)code);
}

// thread object accessors for the kernel32 shims
bool thread_resume(HObject *o) {
    auto *to = dynamic_cast<ThreadObject *>(o);
    if (!to) return false;
    {
        std::lock_guard<std::mutex> lk(to->t->mu);
        to->t->suspended = false;
    }
    to->t->cv.notify_all();
    return true;
}
bool thread_wait(HObject *o, uint32_t ms, bool *is_thread) {
    auto *to = dynamic_cast<ThreadObject *>(o);
    *is_thread = to != nullptr;
    if (!to) return false;
    std::unique_lock<std::mutex> lk(to->t->mu);
    if (ms == 0xFFFFFFFFu) {
        to->t->cv.wait(lk, [&] { return to->t->finished; });
        return true;
    }
    return to->t->cv.wait_for(lk, std::chrono::milliseconds(ms), [&] { return to->t->finished; });
}
bool thread_exit_code(HObject *o, uint32_t *code) {
    auto *to = dynamic_cast<ThreadObject *>(o);
    if (!to) return false;
    std::lock_guard<std::mutex> lk(to->t->mu);
    *code = to->t->finished ? to->t->exit_code : 259 /* STILL_ACTIVE */;
    return true;
}

// ---- imports ---------------------------------------------------------------------------------------------
struct ImportAbi {
    uint32_t iat;
    const char *dll, *name;
    int nargs; // -1 = cdecl
};
static const ImportAbi kImportAbi[] = {
#include "import_abi.inc"
};

struct Tramp {
    std::string dll, name;
    ShimFn fn;
    int nargs;
    std::atomic<uint64_t> calls{0};
    Tramp(std::string d, std::string n, ShimFn f, int a) : dll(std::move(d)), name(std::move(n)), fn(f), nargs(a) {}
};
static ShimDef *g_shims = nullptr;
static std::vector<std::unique_ptr<Tramp>> g_tramps;
static std::mutex g_tramp_mu;

void shim_register(ShimDef *d) {
    d->next = g_shims;
    g_shims = d;
}
static ShimDef *shim_find(const std::string &dll, const std::string &name) {
    for (ShimDef *d = g_shims; d; d = d->next)
        if (name == d->name && strcasecmp(dll.c_str(), d->dll) == 0) return d;
    return nullptr;
}
static uint32_t tramp_add(const std::string &dll, const std::string &name, ShimFn fn, int nargs) {
    g_tramps.push_back(std::make_unique<Tramp>(dll, name, fn, nargs));
    return kTrampBase + (uint32_t)(g_tramps.size() - 1) * kTrampStep;
}

int imports_bind() {
    std::lock_guard<std::mutex> lk(g_tramp_mu);
    g_tramps.reserve(4096);
    int missing = 0, mismatched = 0;
    for (const ImportAbi &im : kImportAbi) {
        ShimDef *d = shim_find(im.dll, im.name);
        if (d && d->nargs != im.nargs) {
            fprintf(stderr, "shim %s!%s declares %d argument slots, the import table says %d\n", im.dll, im.name, d->nargs, im.nargs);
            mismatched++;
        }
        uint32_t t = tramp_add(im.dll, im.name, d ? d->fn : nullptr, im.nargs);
        memcpy(g_mem + im.iat, &t, 4);
        if (!d) missing++;
    }
    if (mismatched) exit(2);
    return missing;
}

uint32_t import_lookup(const std::string &dll, const std::string &name) {
    std::lock_guard<std::mutex> lk(g_tramp_mu);
    for (size_t i = 0; i < g_tramps.size(); i++)
        if (g_tramps[i]->fn && g_tramps[i]->name == name && strcasecmp(g_tramps[i]->dll.c_str(), dll.c_str()) == 0)
            return kTrampBase + (uint32_t)i * kTrampStep;
    ShimDef *d = shim_find(dll, name);
    if (!d) return 0;
    return tramp_add(dll, name, d->fn, d->nargs);
}

uint32_t tramp_register(const std::string &dll, const std::string &name, ShimFn fn, int nargs) {
    std::lock_guard<std::mutex> lk(g_tramp_mu);
    return tramp_add(dll, name, fn, nargs);
}

static std::map<std::string, uint32_t> g_vtables;
uint32_t com_vtable(const std::string &iface, const ComMethod *methods, size_t count) {
    {
        std::lock_guard<std::mutex> lk(g_tramp_mu);
        auto it = g_vtables.find(iface);
        if (it != g_vtables.end()) return it->second;
    }
    uint32_t vt = heap_alloc((uint32_t)count * 4);
    for (size_t i = 0; i < count; i++) {
        uint32_t t = tramp_register("com", iface + "::" + methods[i].name, methods[i].fn, methods[i].words);
        memcpy(g_mem + vt + 4 * i, &t, 4);
    }
    std::lock_guard<std::mutex> lk(g_tramp_mu);
    g_vtables[iface] = vt;
    return vt;
}
uint32_t com_new(uint32_t vtable, uint32_t extra) {
    uint32_t o = heap_alloc(COM_DATA + extra, true);
    uint32_t one = 1;
    memcpy(g_mem + o + COM_VTBL, &vtable, 4);
    memcpy(g_mem + o + COM_REFS, &one, 4);
    return o;
}

uint32_t import_lookup_or_stub(const std::string &dll, const std::string &name_in) {
    // imports by ordinal that the shims know by name
    static const struct { const char *dll, *ord, *name; } kOrdinals[] = {{"ws2_32.dll", "#115", "WSAStartup"}, {"ws2_32.dll", "#116", "WSACleanup"},
                                                                         {"ws2_32.dll", "#111", "WSAGetLastError"}};
    std::string name = name_in;
    for (const auto &o : kOrdinals)
        if (name == o.ord && strcasecmp(dll.c_str(), o.dll) == 0) name = o.name;
    if (uint32_t t = import_lookup(dll, name)) return t;
    std::lock_guard<std::mutex> lk(g_tramp_mu);
    for (size_t i = 0; i < g_tramps.size(); i++) // one stub per function
        if (!g_tramps[i]->fn && g_tramps[i]->name == name && strcasecmp(g_tramps[i]->dll.c_str(), dll.c_str()) == 0)
            return kTrampBase + (uint32_t)i * kTrampStep;
    if (g_verbose) fprintf(stderr, "[bo1] no shim yet for %s!%s (resolved at run time; traps if called)\n", dll.c_str(), name.c_str());
    return tramp_add(dll, name, nullptr, 0);
}
void imports_redirect(const std::string &dll, const std::function<uint32_t(const std::string &)> &resolve) {
    for (const ImportAbi &im : kImportAbi) {
        if (strcasecmp(im.dll, dll.c_str()) != 0) continue;
        if (uint32_t va = resolve(im.name)) memcpy(g_mem + im.iat, &va, 4);
    }
}

const char *tramp_name(uint32_t target) {
    static thread_local char buf[160];
    uint32_t idx = (target - kTrampBase) / kTrampStep;
    if (target < kTrampBase || idx >= g_tramps.size()) return nullptr;
    snprintf(buf, sizeof(buf), "%s!%s", g_tramps[idx]->dll.c_str(), g_tramps[idx]->name.c_str());
    return buf;
}

void imports_report() {
    if (!getenv("BO1_IMPORT_STATS")) return;
    std::vector<Tramp *> v;
    for (auto &t : g_tramps)
        if (t->calls) v.push_back(t.get());
    std::sort(v.begin(), v.end(), [](Tramp *a, Tramp *b) { return a->calls > b->calls; });
    fprintf(stderr, "import calls (%zu distinct):\n", v.size());
    for (Tramp *t : v) fprintf(stderr, "  %10llu  %s!%s\n", (unsigned long long)t->calls.load(), t->dll.c_str(), t->name.c_str());
}

// Called by rt_dispatch for targets in the trampoline zone.
static void host_call(Cpu *cpu, uint32_t target) {
    uint32_t idx = (target - kTrampBase) / kTrampStep;
    if (idx >= g_tramps.size() || (target - kTrampBase) % kTrampStep) rt_trap(cpu, target, TRAP_NO_TARGET);
    Tramp &t = *g_tramps[idx];
    if (!t.fn) rt_trap(cpu, target, TRAP_UNIMPLEMENTED_IMPORT);
    const uint32_t *a = reinterpret_cast<const uint32_t *>(g_mem + cpu->esp + 4);
    t.calls.fetch_add(1, std::memory_order_relaxed);
    if (g_verbose >= 2) {
        uint32_t ret;
        memcpy(&ret, g_mem + cpu->esp, 4);
        log("%s!%s(%08x, %08x, %08x, %08x) from %08x", t.dll.c_str(), t.name.c_str(), t.nargs > 0 || t.nargs < 0 ? a[0] : 0,
            t.nargs > 1 || t.nargs < 0 ? a[1] : 0, t.nargs > 2 || t.nargs < 0 ? a[2] : 0, t.nargs > 3 || t.nargs < 0 ? a[3] : 0, ret);
    }
    uint32_t r = t.fn(cpu, a);
    cpu->eax = r;
    cpu->esp += 4 + (t.nargs > 0 ? 4u * (uint32_t)t.nargs : 0);
    if (g_verbose >= 3) log("  -> %08x", r);
}

struct HostCallInstaller {
    HostCallInstaller() { g_rt_host_call = host_call; }
};
static HostCallInstaller g_install_host_call;

} // namespace bo1
