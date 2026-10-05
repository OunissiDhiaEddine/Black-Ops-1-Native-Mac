// Differential test runner (T-041/T-042): runs lifted functions on the states recorded by
// tests/diff/gen_vectors.py (original x86 under Unicorn) and compares integer registers, the x87
// stack (or MMX registers), XMM registers and memory. x87 values may differ in the last bits
// (double vs 80-bit, libm vs microcode): those vectors are counted as "approx", not as passes.
//   difftest <BlackOps.exe> <vectors.bin> [--verbose]
#include <fcntl.h>
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include "runtime.h"

static const char *kReg[8] = {"eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi"};
static const char *kTrap[] = {"?", "int3", "bad jump", "divide error", "unsupported function", "no dispatch target", "timeout"};

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
static uint32_t u32(const uint8_t **p) {
    uint32_t v;
    memcpy(&v, *p, 4);
    *p += 4;
    return v;
}

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: difftest <exe> <vectors.bin> [--verbose]\n");
        return 2;
    }
    int verbose = argc > 3;
    size_t exe_size, vec_size;
    const void *exe = map_file(argv[1], &exe_size);
    const uint8_t *vec = (const uint8_t *)map_file(argv[2], &vec_size);
    if (!exe || !vec || vec_size < 12 || memcmp(vec, "BO1DIFF2", 8) != 0) {
        fprintf(stderr, "cannot read inputs\n");
        return 2;
    }
    const uint8_t *p = vec + 8;
    uint32_t count = u32(&p);
    signal(SIGALRM, on_alarm);
    unsigned pass = 0, fail = 0, trapped = 0, funcs_fail = 0, approx = 0;
    uint32_t last_fail_fn = 0;
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
        memcpy(cpu.xmm, p, sizeof(cpu.xmm));
        p += sizeof(cpu.xmm);
        if (rt_arena_reset() != 0 || rt_load_image(exe, exe_size) != 0) {
            fprintf(stderr, "arena/image setup failed\n");
            return 2;
        }
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
        LiftedFn f = rt_lookup(fn);
        int code = setjmp(g_trap_jmp);
        if (code == 0) {
            alarm(5);
            f(&cpu);
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
        for (int i = 0; i < 8; i++)
            if (memcmp(&cpu.xmm[i], &exp_xmm[i], 16) != 0) {
                bad = 1;
                if (verbose)
                    printf("FAIL %08x: xmm%d = %08x %08x %08x %08x, expected %08x %08x %08x %08x\n", fn, i, cpu.xmm[i].u32[0], cpu.xmm[i].u32[1],
                           cpu.xmm[i].u32[2], cpu.xmm[i].u32[3], exp_xmm[i].u32[0], exp_xmm[i].u32[1], exp_xmm[i].u32[2], exp_xmm[i].u32[3]);
            }
        const uint8_t *q = outs;
        for (uint32_t n = u32(&q); n; n--) {
            uint32_t addr = u32(&q), len = u32(&q);
            if (memcmp(g_mem + addr, q, len) != 0) {
                bad = 1;
                if (verbose) {
                    uint32_t k = 0;
                    while (k < len && g_mem[addr + k] == q[k]) k++;
                    printf("FAIL %08x: memory at %08x = %02x, expected %02x\n", fn, addr + k, g_mem[addr + k], q[k]);
                }
            }
            q += len;
        }
        if (bad) {
            fail++;
            if (fn != last_fail_fn) funcs_fail++, last_fail_fn = fn;
        } else if (inexact)
            approx++;
        else
            pass++;
    }
    printf("vectors %u: pass %u, approx %u, fail %u (of which trapped %u); functions with a failure: %u\n", count, pass, approx, fail, trapped,
           funcs_fail);
    return fail ? 1 : 0;
}
