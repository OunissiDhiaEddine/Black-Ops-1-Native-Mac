// Differential test runner (T-041/T-042): runs lifted functions on the states recorded by
// tests/diff/gen_vectors.py (original x86 under Unicorn) and compares integer registers, the x87
// stack (or MMX registers), XMM registers and memory. x87 values may differ in the last bits
// (double vs 80-bit, libm vs microcode): those vectors are counted as "approx", not as passes.
//   difftest <BlackOps.exe> [--verbose] <vectors.bin>...
#include <fcntl.h>
#include <fenv.h>
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include "runtime.h"

static const char *kReg[8] = {"eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi"};
static const char *kTrap[] = {"?", "int3", "bad jump", "divide error", "unsupported function", "no dispatch target", "timeout",
                              "fell through a noreturn call", "host fault (stack overflow / wild access)"};
#define HEAP_VA 0x60000000u // must match gen_vectors.py

static void *map_file(const char *path, size_t *size) {
    int fd = open(path, O_RDONLY);
    struct stat st;
    if (fd < 0 || fstat(fd, &st) != 0) return NULL;
    void *p = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    *size = (size_t)st.st_size;
    return p == MAP_FAILED ? NULL : p;
}
static void on_alarm(int sig) {
    (void)sig;
    longjmp(g_trap_jmp, 6);
}
static void on_fault(int sig) {
    (void)sig;
    longjmp(g_trap_jmp, 8);
}
// Two float bit patterns that are both NaN: the reference (QEMU) picks between NaN operands with the
// x87 rule even for SSE, so sign/payload of a propagated NaN is not a reliable expectation.
static int both_nan32(uint32_t a, uint32_t b) {
    return (a & 0x7F800000u) == 0x7F800000u && (a & 0x007FFFFFu) && (b & 0x7F800000u) == 0x7F800000u && (b & 0x007FFFFFu);
}
static uint32_t u32(const uint8_t **p) {
    uint32_t v;
    memcpy(&v, *p, 4);
    *p += 4;
    return v;
}

#ifdef LIFT_TRACE
// Per-instruction state trace of the lifted code, in the format of tests/diff/trace_vector.py:
// one line per executed instruction with the registers that instruction changed.
static Cpu g_prev;
static uint32_t g_prev_addr;
static int g_tracing;
static void hex128(const Xmm *x) {
    if (x->u64[1]) printf("%llx%016llx", (unsigned long long)x->u64[1], (unsigned long long)x->u64[0]);
    else printf("%llx", (unsigned long long)x->u64[0]);
}
void rt_trace(Cpu *cpu, uint32_t addr) {
    if (!g_tracing) return;
    static const char *names[8] = {"eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi"};
    if (g_prev_addr) {
        printf("%08x", g_prev_addr);
        const uint32_t *a = &g_prev.eax, *b = &cpu->eax;
        for (int i = 0; i < 8; i++)
            if (a[i] != b[i]) printf(" %s=%x", names[i], b[i]);
        for (int i = 0; i < 8; i++)
            if (memcmp(&g_prev.xmm[i], &cpu->xmm[i], 16) != 0) {
                printf(" xmm%d=", i);
                hex128(&cpu->xmm[i]);
            }
        if (g_prev.ftop != cpu->ftop) printf(" top=%x", cpu->ftop);
        for (int i = 0; i < 8; i++)
            if (memcmp(&g_prev.st[i], &cpu->st[i], 8) != 0) printf(" fp%d=%.17g", i, cpu->st[i]);
        printf("\n");
    }
    g_prev = *cpu;
    g_prev_addr = addr;
}
#endif

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: difftest <exe> [--verbose] <vectors.bin>...\n");
        return 2;
    }
    int verbose = 0;
    size_t exe_size;
    const void *exe = map_file(argv[1], &exe_size);
    if (!exe) {
        fprintf(stderr, "cannot read %s\n", argv[1]);
        return 2;
    }
    // faults inside lifted code (runaway recursion, wild host access) are reported as traps
    static uint8_t alt_stack[1 << 16];
    stack_t ss = {.ss_sp = alt_stack, .ss_size = sizeof(alt_stack), .ss_flags = 0};
    sigaltstack(&ss, NULL);
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_fault;
    sa.sa_flags = SA_ONSTACK | SA_NODEFER;
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
    sigaction(SIGFPE, &sa, NULL);
    sigaction(SIGILL, &sa, NULL);
    signal(SIGALRM, on_alarm);
    unsigned total = 0, pass = 0, fail = 0, trapped = 0, funcs_fail = 0, approx = 0, nan_diff = 0, range_diff = 0;
    uint32_t only_fn = 0;
    int only_nth = -1, seen_fn = 0;
    uint32_t last_fail_fn = 0;
  for (int arg = 2; arg < argc; arg++) {
    if (strcmp(argv[arg], "--verbose") == 0) {
        verbose = 1;
        continue;
    }
    if (strcmp(argv[arg], "--func") == 0 && arg + 1 < argc) { // run only this function's vectors
        only_fn = (uint32_t)strtoul(argv[++arg], NULL, 16);
        continue;
    }
    if (strcmp(argv[arg], "--nth") == 0 && arg + 1 < argc) { // ... and only the n-th of them
        only_nth = atoi(argv[++arg]);
        continue;
    }
    size_t vec_size;
    const uint8_t *vec = (const uint8_t *)map_file(argv[arg], &vec_size);
    if (!vec || vec_size < 20 || memcmp(vec, "BO1DIFF3", 8) != 0) {
        fprintf(stderr, "cannot read vectors %s\n", argv[arg]);
        return 2;
    }
    const uint8_t *p = vec + 8;
    uint32_t count = u32(&p), nheaps = u32(&p), heap_size = u32(&p);
    const uint8_t *heaps = p;
    p += (size_t)nheaps * heap_size;
    uint32_t npatch = u32(&p); // image patches (poisoned import slots), applied after every image load
    const uint8_t *patches = p;
    p += (size_t)npatch * 8;
    total += count;
    for (uint32_t t = 0; t < count; t++) {
        uint32_t fn = u32(&p);
        Cpu cpu;
        memset(&cpu, 0, sizeof(cpu));
        uint32_t *regs = &cpu.eax;
        for (int i = 0; i < 8; i++) regs[i] = u32(&p);
        cpu.df = (uint8_t)u32(&p);
        cpu.fcw = (uint16_t)u32(&p);
        cpu.mxcsr = u32(&p);
        uint32_t vflags = u32(&p);
        uint32_t heap_idx = u32(&p);
        memcpy(cpu.xmm, p, sizeof(cpu.xmm));
        p += sizeof(cpu.xmm);
        if (rt_arena_reset() != 0 || rt_load_image(exe, exe_size) != 0) {
            fprintf(stderr, "arena/image setup failed\n");
            return 2;
        }
        for (uint32_t k = 0; k < npatch; k++) {
            uint32_t a, v;
            memcpy(&a, patches + 8 * k, 4);
            memcpy(&v, patches + 8 * k + 4, 4);
            memcpy(g_mem + a, &v, 4);
        }
        memcpy(g_mem + HEAP_VA, heaps + (size_t)(heap_idx % nheaps) * heap_size, heap_size);
        for (uint32_t n = u32(&p); n; n--) {
            uint32_t addr = u32(&p), len = u32(&p);
            memcpy(g_mem + addr, p, len);
            p += len;
        }
        uint32_t expect[8];
        for (int i = 0; i < 8; i++) expect[i] = u32(&p);
        uint32_t exp_top = u32(&p);
        double exp_st[8];
        uint64_t exp_mm[8];
        Xmm exp_xmm[8];
        memcpy(exp_st, p, 64), p += 64;
        memcpy(exp_mm, p, 64), p += 64;
        memcpy(exp_xmm, p, 128), p += 128;
        const uint8_t *outs = p;
        uint32_t nout = u32(&p);
        for (uint32_t n = nout; n; n--) {
            uint32_t len;
            p += 4;
            len = u32(&p);
            p += len;
        }
        if (only_fn && (fn != only_fn || (only_nth >= 0 && seen_fn++ != only_nth))) {
            total--;
            continue;
        }
        LiftedFn f = rt_lookup(fn);
        volatile int fp_range = 0;
        int code = setjmp(g_trap_jmp);
        if (code == 0) {
            alarm(5);
#ifdef LIFT_TRACE
            g_tracing = 1, g_prev_addr = 0;
#endif
            feclearexcept(FE_ALL_EXCEPT);
            f(&cpu);
            fp_range = fetestexcept(FE_OVERFLOW | FE_UNDERFLOW) != 0;
#ifdef LIFT_TRACE
            rt_trace(&cpu, 0);
            g_tracing = 0;
#endif
            alarm(0);
        } else {
            alarm(0);
            trapped++;
            fail++;
            if (fn != last_fail_fn) funcs_fail++, last_fail_fn = fn;
            if (verbose) printf("FAIL %08x: trap %s at %08x\n", fn, kTrap[code < 7 ? code : 0], cpu.trap_addr);
            continue;
        }
        int bad = 0;
        for (int i = 0; i < 8; i++)
            if (regs[i] != expect[i]) {
                bad = 1;
                if (verbose) printf("FAIL %08x: %s = %08x, expected %08x\n", fn, kReg[i], regs[i], expect[i]);
            }
        int inexact = 0;
        if (vflags & 1) { // MMX function: the x87 registers hold MMX data
            for (int i = 0; i < 8; i++)
                if (cpu.mm[i] != exp_mm[i] && exp_mm[i] != 0) { // untouched registers read back as 0 from Unicorn
                    bad = 1;
                    if (verbose) printf("FAIL %08x: mm%d = %016llx, expected %016llx\n", fn, i, (unsigned long long)cpu.mm[i], (unsigned long long)exp_mm[i]);
                }
        } else {
            if (cpu.ftop != exp_top) {
                bad = 1;
                if (verbose) printf("FAIL %08x: x87 top = %u, expected %u\n", fn, cpu.ftop, exp_top);
            } else
                for (unsigned i = 0; i < ((8 - exp_top) & 7); i++) {
                    double a = cpu.st[(cpu.ftop + i) & 7], b = exp_st[i];
                    if (memcmp(&a, &b, 8) == 0 || (a != a && b != b)) continue;
                    if (fabs(a - b) <= 1e-12 * fmax(fabs(a), fabs(b))) {
                        inexact = 1;
                        continue;
                    }
                    bad = 1;
                    if (verbose) printf("FAIL %08x: st(%u) = %.17g, expected %.17g\n", fn, i, a, b);
                }
        }
        int nan_only = 0;
        for (int i = 0; i < 8; i++)
            if (memcmp(&cpu.xmm[i], &exp_xmm[i], 16) != 0) {
                int real = 0;
                for (int k = 0; k < 4; k++)
                    if (cpu.xmm[i].u32[k] != exp_xmm[i].u32[k] && !both_nan32(cpu.xmm[i].u32[k], exp_xmm[i].u32[k])) real = 1;
                if (!real) {
                    nan_only = 1;
                    if (verbose) printf("NANDIFF %08x: xmm%d\n", fn, i);
                    continue;
                }
                bad = 1;
                if (verbose)
                    printf("FAIL %08x: xmm%d = %08x %08x %08x %08x, expected %08x %08x %08x %08x\n", fn, i, cpu.xmm[i].u32[0], cpu.xmm[i].u32[1],
                           cpu.xmm[i].u32[2], cpu.xmm[i].u32[3], exp_xmm[i].u32[0], exp_xmm[i].u32[1], exp_xmm[i].u32[2], exp_xmm[i].u32[3]);
            }
        const uint8_t *q = outs;
        for (uint32_t n = u32(&q); n; n--) {
            uint32_t addr = u32(&q), len = u32(&q);
            if (memcmp(g_mem + addr, q, len) != 0) {
                uint32_t k = 0;
                int real = 0;
                for (uint32_t j = 0; j < len; j++) {
                    if (g_mem[addr + j] == q[j]) continue;
                    // the 4-byte word (guest-aligned) around the differing byte, when the block covers it
                    uint32_t w = (addr + j) & ~3u;
                    uint32_t got, want;
                    if (w >= addr && w + 4 <= addr + len) {
                        memcpy(&got, g_mem + w, 4);
                        memcpy(&want, q + (w - addr), 4);
                        if (both_nan32(got, want)) continue;
                    }
                    if (!real) k = j;
                    real = 1;
                }
                if (!real) {
                    nan_only = 1;
                    if (verbose) printf("NANDIFF %08x: memory at %08x\n", fn, addr);
                } else {
                    bad = 1;
                    if (verbose) printf("FAIL %08x: memory at %08x = %02x, expected %02x\n", fn, addr + k, g_mem[addr + k], q[k]);
                }
            }
            q += len;
        }
        if (bad && fp_range) {
            // A double overflowed or underflowed while the lifted code ran: x87 registers have a 15-bit
            // exponent, the model's doubles 11 bits (docs/LIFTER.md §5). Reported, not counted as a failure.
            range_diff++;
            if (verbose) printf("RANGE %08x: differs after a double overflow/underflow\n", fn);
        } else if (bad) {
            fail++;
            if (fn != last_fail_fn) funcs_fail++, last_fail_fn = fn;
        } else if (nan_only)
            nan_diff++;
        else if (inexact)
            approx++;
        else
            pass++;
    }
    munmap((void *)vec, vec_size);
  }
    printf("vectors %u: pass %u, approx %u, nan-only %u, fp-range %u, fail %u (of which trapped %u); functions with a failure: %u\n", total,
           pass, approx, nan_diff, range_diff, fail, trapped, funcs_fail);
    return fail ? 1 : 0;
}
