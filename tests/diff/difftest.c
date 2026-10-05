// Differential test runner (T-041): runs lifted functions on the states recorded by
// tests/diff/gen_vectors.py (original x86 under Unicorn) and compares registers and memory.
//   difftest <BlackOps.exe> <vectors.bin> [--verbose]
#include <fcntl.h>
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
    if (!exe || !vec || vec_size < 12 || memcmp(vec, "BO1DIFF1", 8) != 0) {
        fprintf(stderr, "cannot read inputs\n");
        return 2;
    }
    const uint8_t *p = vec + 8;
    uint32_t count = u32(&p);
    signal(SIGALRM, on_alarm);
    unsigned pass = 0, fail = 0, trapped = 0, funcs_fail = 0;
    uint32_t last_fail_fn = 0;
    for (uint32_t t = 0; t < count; t++) {
        uint32_t fn = u32(&p);
        Cpu cpu;
        memset(&cpu, 0, sizeof(cpu));
        uint32_t *regs = &cpu.eax;
        for (int i = 0; i < 8; i++) regs[i] = u32(&p);
        cpu.df = (uint8_t)u32(&p);
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
        } else
            pass++;
    }
    printf("vectors %u: pass %u, fail %u (of which trapped %u); functions with a failure: %u\n", count, pass, fail, trapped, funcs_fail);
    return fail ? 1 : 0;
}
