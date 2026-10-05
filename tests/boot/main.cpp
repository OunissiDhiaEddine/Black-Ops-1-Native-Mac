// Headless boot harness (T-045, milestone M4b): loads BlackOps.exe into the guest arena, binds the
// imports to the Win32 shims and runs the lifted entry point until it exits, traps or times out.
//
//   artifacts/boot/bo1_boot [--exe PATH] [--seconds N]
//   env: BO1_VERBOSE=0..3, BO1_IMPORT_STATS=1, BO1_GAME_DIR=artifacts/game
//
// Every stop prints why (unimplemented import, bad indirect target, host fault) together with the
// guest registers and a stack scan, which is the work list for the next iteration.
#include <fcntl.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "guest_os.h"
#include "image_info.h"

using namespace bo1;

extern "C" {
extern const struct LiftedEntry {
    uint32_t va;
    LiftedFn fn;
} g_lifted_table[];
extern const unsigned g_lifted_count;
}

// Greatest lifted function start <= va (for annotating addresses found on the guest stack).
static uint32_t containing_function(uint32_t va) {
    unsigned lo = 0, hi = g_lifted_count;
    while (lo < hi) {
        unsigned mid = (lo + hi) / 2;
        if (g_lifted_table[mid].va <= va) lo = mid + 1;
        else hi = mid;
    }
    return lo ? g_lifted_table[lo - 1].va : 0;
}

static void dump_guest(Cpu *cpu) {
    fprintf(stderr, "  eax=%08x ecx=%08x edx=%08x ebx=%08x esp=%08x ebp=%08x esi=%08x edi=%08x teb=%08x\n", cpu->eax, cpu->ecx, cpu->edx,
            cpu->ebx, cpu->esp, cpu->ebp, cpu->esi, cpu->edi, cpu->teb);
    const ImageInfo &im = image_info();
    fprintf(stderr, "  return addresses on the guest stack (innermost first):\n");
    int shown = 0;
    for (uint32_t a = cpu->esp; shown < 24 && a < cpu->esp + 0x4000 && a >= 0x10000; a += 4) {
        uint32_t v;
        memcpy(&v, g_mem + a, 4);
        if (v >= im.text_lo && v < im.text_hi) {
            fprintf(stderr, "    [esp+%04x] %08x  (f_%08x+0x%x)\n", a - cpu->esp, v, containing_function(v), v - containing_function(v));
            shown++;
        }
    }
}

static const char *kTrapNames[] = {"?", "int3", "bad jump-table index", "divide error", "function not lifted", "indirect target is not a function",
                                   "timeout", "returned from a noreturn call", "host fault", "unimplemented import"};

static void on_trap(Cpu *cpu, uint32_t addr, uint32_t code) {
    const char *imp = tramp_name(addr);
    fprintf(stderr, "\n*** guest stopped: %s at %08x%s%s\n", code < 10 ? kTrapNames[code] : "?", addr, imp ? " = " : "", imp ? imp : "");
    dump_guest(cpu);
    process_exit(100 + code);
}

static void on_fault(int sig, siginfo_t *info, void *) {
    uintptr_t fa = (uintptr_t)info->si_addr, base = (uintptr_t)g_mem;
    fprintf(stderr, "\n*** host signal %d, fault address %p", sig, info->si_addr);
    if (fa >= base && fa < base + (1ull << 32)) fprintf(stderr, " = guest %08x", (uint32_t)(fa - base));
    fprintf(stderr, "\n");
    if (GuestThread *t = thread_current()) dump_guest(thread_cpu(t));
    imports_report();
    _exit(99);
}

static void on_alarm(int) {
    fprintf(stderr, "\n*** time limit reached (still running: this is the good outcome for a boot test)\n");
    if (GuestThread *t = thread_current()) dump_guest(thread_cpu(t));
    imports_report();
    _exit(0);
}

int main(int argc, char **argv) {
    const char *exe = "reference/Call of Duty Black Ops/BlackOps.exe";
    int seconds = 20;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--exe") && i + 1 < argc) exe = argv[++i];
        else if (!strcmp(argv[i], "--seconds") && i + 1 < argc) seconds = atoi(argv[++i]);
    }
    if (const char *v = getenv("BO1_VERBOSE")) g_verbose = atoi(v);

    int fd = open(exe, O_RDONLY);
    struct stat st;
    if (fd < 0 || fstat(fd, &st) != 0) {
        fprintf(stderr, "cannot open %s\n", exe);
        return 2;
    }
    void *file = mmap(nullptr, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    if (file == MAP_FAILED || !image_parse(file, (size_t)st.st_size) || rt_arena_reset() != 0 || rt_load_image(file, (size_t)st.st_size) != 0) {
        fprintf(stderr, "cannot load %s\n", exe);
        return 2;
    }
    const ImageInfo &im = image_info();
    mprotect(g_mem, kNullGuard, PROT_NONE); // null-pointer accesses fault instead of reading zeros
    vm_reserve_fixed(0, kNullGuard);
    vm_reserve_fixed(im.base, im.size);
    int missing = imports_bind();
    fprintf(stderr, "[bo1] image %08x-%08x entry %08x; imports without a shim: %d\n", im.base, im.base + im.size, im.entry, missing);

    g_rt_trap_handler = on_trap;
    static uint8_t alt[1 << 16];
    stack_t ss = {};
    ss.ss_sp = alt;
    ss.ss_size = sizeof(alt);
    sigaltstack(&ss, nullptr); // main thread only; guest threads fault on their own (large) stacks
    struct sigaction sa = {};
    sa.sa_sigaction = on_fault;
    sa.sa_flags = SA_SIGINFO;
    sigaction(SIGSEGV, &sa, nullptr);
    sigaction(SIGBUS, &sa, nullptr);
    sigaction(SIGFPE, &sa, nullptr);
    sigaction(SIGILL, &sa, nullptr);
    signal(SIGALRM, on_alarm);
    if (seconds > 0) alarm((unsigned)seconds);

    int rc = thread_run_main(im.stack_reserve, [&](Cpu *cpu) { guest_call(cpu, im.entry, {}); });
    fprintf(stderr, "[bo1] entry point returned (%d)\n", rc);
    imports_report();
    return rc;
}
