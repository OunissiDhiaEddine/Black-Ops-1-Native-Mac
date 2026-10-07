#include "runtime.h"

#include <pthread.h>
#include <stdio.h>
#include <sys/mman.h>

uint8_t *g_mem;
jmp_buf g_trap_jmp;

extern const struct LiftedEntry { uint32_t va; LiftedFn fn; } g_lifted_table[];
extern const unsigned g_lifted_count;

#define ARENA_SIZE (1ull << 32)
#define ARENA_SLACK 0x10000ull

int rt_arena_reset(void) {
    // Anonymous, lazily zero-filled, fully accessible: any guest address reads as zero until
    // written. MAP_FIXED over the old mapping drops every page touched by the previous user.
    void *want = g_mem;
    // + ARENA_SLACK: an access that starts in the last bytes of the address space runs past 4 GiB in host
    // terms (x86 would wrap to 0); the slack keeps that inside the mapping instead of faulting the host.
    void *p = mmap(want, ARENA_SIZE + ARENA_SLACK, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON | MAP_NORESERVE | (want ? MAP_FIXED : 0), -1, 0);
    if (p == MAP_FAILED) return -1;
    g_mem = (uint8_t *)p;
    return 0;
}

static uint32_t rd32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static uint16_t rd16(const uint8_t *p) { uint16_t v; memcpy(&v, p, 2); return v; }

int rt_load_image(const void *file, size_t size) {
    const uint8_t *f = (const uint8_t *)file;
    if (size < 0x40 || rd16(f) != 0x5A4D) return -1;
    uint32_t nt = rd32(f + 0x3C);
    if (nt + 0xF8 > size || rd32(f + nt) != 0x00004550) return -1;
    uint16_t nsec = rd16(f + nt + 6), opt_size = rd16(f + nt + 20);
    const uint8_t *opt = f + nt + 24;
    if (rd16(opt) != 0x10B) return -1; // PE32 only
    uint32_t base = rd32(opt + 28), headers = rd32(opt + 60);
    if (headers <= size) memcpy(g_mem + base, f, headers);
    const uint8_t *sec = opt + opt_size;
    for (unsigned i = 0; i < nsec; i++, sec += 40) {
        uint32_t vsize = rd32(sec + 8), va = rd32(sec + 12), raw_size = rd32(sec + 16), raw = rd32(sec + 20);
        uint32_t n = raw_size < vsize ? raw_size : vsize;
        if ((uint64_t)raw + n > size) return -1;
        memcpy(g_mem + base + va, f + raw, n); // the rest of the virtual size is already zero
    }
    return 0;
}

LiftedFn rt_lookup(uint32_t va) {
    unsigned lo = 0, hi = g_lifted_count;
    while (lo < hi) {
        unsigned mid = (lo + hi) / 2;
        if (g_lifted_table[mid].va < va) lo = mid + 1;
        else hi = mid;
    }
    return lo < g_lifted_count && g_lifted_table[lo].va == va ? g_lifted_table[lo].fn : NULL;
}

void (*g_rt_host_call)(Cpu *cpu, uint32_t target);
void (*g_rt_trap_handler)(Cpu *cpu, uint32_t addr, uint32_t code);
int (*g_rt_unknown_target)(Cpu *cpu, uint32_t target);

// ---- setjmp / longjmp ------------------------------------------------------------------------------------
// One slot per guest jmp_buf (the game keeps one per thread and a few on the stack). A slot remembers where the
// guest's _setjmp3 returned to (address and stack pointer) and the host context of the lifted function that
// called it. That function's host frame lives exactly as long as its guest frame, so a longjmp the guest may
// legally make always finds it. Host frames in between are dropped the way longjmp drops them: without running
// C++ destructors — a shim that calls back into guest code must not hold a lock across the call.
#define RT_SETJMP_SLOTS 64
static struct SetjmpSlot {
    uint32_t buf, ret, esp;
    uint64_t seq; // 0 = never used; otherwise the order of use (the oldest slot is recycled)
    jmp_buf host;
} g_setjmp[RT_SETJMP_SLOTS];
static uint64_t g_setjmp_seq;
static pthread_mutex_t g_setjmp_mu = PTHREAD_MUTEX_INITIALIZER;

jmp_buf *rt_setjmp_slot(Cpu *cpu, uint32_t ret) {
    uint32_t buf = M32(cpu->esp); // _setjmp3(buf, ...) has just returned: its first argument is on top again
    pthread_mutex_lock(&g_setjmp_mu);
    struct SetjmpSlot *s = &g_setjmp[0];
    for (unsigned i = 0; i < RT_SETJMP_SLOTS; i++) {
        if (g_setjmp[i].seq && g_setjmp[i].buf == buf) {
            s = &g_setjmp[i];
            break;
        }
        if (g_setjmp[i].seq < s->seq) s = &g_setjmp[i];
    }
    s->buf = buf, s->ret = ret, s->esp = cpu->esp, s->seq = ++g_setjmp_seq;
    pthread_mutex_unlock(&g_setjmp_mu);
    return &s->host;
}

// The tail of the guest's longjmp: registers are restored, esp is what it was when _setjmp3 returned.
static jmp_buf *setjmp_landing(Cpu *cpu, uint32_t target) {
    struct SetjmpSlot *s = NULL;
    pthread_mutex_lock(&g_setjmp_mu);
    for (unsigned i = 0; i < RT_SETJMP_SLOTS; i++)
        if (g_setjmp[i].seq && g_setjmp[i].ret == target && g_setjmp[i].esp == cpu->esp && (!s || g_setjmp[i].seq > s->seq)) s = &g_setjmp[i];
    pthread_mutex_unlock(&g_setjmp_mu);
    return s ? &s->host : NULL;
}

void rt_dispatch(Cpu *cpu, uint32_t target) {
    if (target >= RT_TRAMP_BASE && g_rt_host_call) {
        g_rt_host_call(cpu, target);
        return;
    }
    LiftedFn fn = rt_lookup(target);
    if (!fn) {
        jmp_buf *land = setjmp_landing(cpu, target);
        if (land) _longjmp(*land, 1);
        if (g_rt_unknown_target && g_rt_unknown_target(cpu, target)) return;
        rt_trap(cpu, target, TRAP_NO_TARGET);
    }
    fn(cpu);
}

void rt_trap(Cpu *cpu, uint32_t addr, uint32_t code) {
    cpu->trap_addr = addr;
    cpu->trap_code = code;
    if (g_rt_trap_handler) g_rt_trap_handler(cpu, addr, code);
    longjmp(g_trap_jmp, (int)code);
}

// CPUID: a fixed, conservative x86 feature set. SSE3/SSSE3/SSE4 are deliberately NOT reported so
// that CPU-dispatched code takes its SSE2 paths (the lifter does not implement the newer sets).
void rt_cpuid(Cpu *cpu) {
    uint32_t leaf = cpu->eax;
    cpu->eax = cpu->ebx = cpu->ecx = cpu->edx = 0;
    if (leaf == 0) {
        cpu->eax = 1;
        cpu->ebx = 0x756E6547, cpu->edx = 0x49656E69, cpu->ecx = 0x6C65746E; // "GenuineIntel"
    } else if (leaf == 1) {
        cpu->eax = 0x000006FB; // family 6 model 15 (Core 2)
        cpu->ebx = 0x00010800; // CLFLUSH line 64, 1 logical processor
        cpu->edx = (1u << 0) | (1u << 4) | (1u << 8) | (1u << 15) | (1u << 23) | (1u << 24) | (1u << 25) | (1u << 26);
    } else if (leaf == 0x80000000u) {
        cpu->eax = 0x80000000u;
    }
}

#include <time.h>
void rt_rdtsc(Cpu *cpu) {
    uint64_t t = clock_gettime_nsec_np(CLOCK_UPTIME_RAW) * 3; // pretend 3 GHz
    cpu->eax = (uint32_t)t;
    cpu->edx = (uint32_t)(t >> 32);
}
