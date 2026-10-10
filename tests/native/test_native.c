// Native replacements (src/native) against the lifted originals they replace: same guest memory and
// registers in, every output byte and every register the original keeps compared (P6 / T-064).
//   scripts/test_native.sh          -> "test_native: N cases, 0 failed"
//   scripts/test_native.sh --bench  -> also ns per vertex / per copy, lifted and native, in this one process
// The lifted originals are what scripts/difftest.sh checks against the x86 reference, so this closes the chain.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "native.h"
#include "runtime.h"

void f_00967760(Cpu *), f_00745520(Cpu *), f_00745730(Cpu *), f_00745a00(Cpu *), f_00745d70(Cpu *);

enum { STACK = 0x10000000u, SRC = 0x20000000u, DST = 0x21000000u, MATS = 0x22000000u, BLEND = 0x23000000u,
       PINDEX = 0x24000000u, RET_VA = 0x00401000u, MAXV = 256, BONES = 64 };

static uint64_t rng_state = 0x9E3779B97F4A7C15ull;
static uint32_t rnd(void) {
    rng_state ^= rng_state << 13, rng_state ^= rng_state >> 7, rng_state ^= rng_state << 17;
    return (uint32_t)(rng_state >> 16);
}
static float rndf(float mag) { return ((int32_t)rnd() / 2147483648.0f) * mag; }

static int failures, cases;

static void run(void (*fn)(Cpu *), Cpu *cpu, int native) {
    g_native_off = !native;
    cpu->esp -= 4;
    M32(cpu->esp) = RET_VA;
    if (setjmp(g_trap_jmp)) {
        printf("FAIL trap %u at %08x\n", cpu->trap_code, cpu->trap_addr);
        failures++;
        return;
    }
    fn(cpu);
}

static int same_kept(const Cpu *a, const Cpu *b) {   // what a compiled x86 function keeps, and the stack pointer
    return a->ebx == b->ebx && a->esi == b->esi && a->edi == b->edi && a->ebp == b->ebp && a->esp == b->esp;
}

// ---- skinning ------------------------------------------------------------------------------------
static void skin_case(int bones, void (*fn)(Cpu *), int huge, int in_place) {
    static const int step[5] = {0, 2, 6, 10, 14};
    static uint8_t in_src[MAXV * 32 + 64], in_dst[MAXV * 32 + 64], out[2][MAXV * 32 + 64];
    int count = (int)(rnd() % 100) < 5 ? -(int)(rnd() % 3) : (int)(rnd() % 200);
    uint32_t index = rnd() % (MAXV - 200 + 1);
    uint32_t dst = in_place ? SRC : DST;
    float mag = huge ? 1e12f : (rnd() & 1 ? 2.0f : 300.0f);
    for (int i = 0; i < BONES * 16; i++) {
        float v = rndf(mag);
        if (i % 16 == 15 && (rnd() & 3)) v = 1.0f;
        memcpy(g_mem + MATS + 4 * i, &v, 4);
    }
    for (int i = 0; i < MAXV; i++) {
        float p[4] = {rndf(1000.0f), rndf(1000.0f), rndf(1000.0f), rnd() & 1 ? rndf(4.0f) : (rnd() & 1 ? 0.0f : -0.0f)};
        memcpy(in_src + 32 * i, p, 16);
        for (int k = 16; k < 32; k++) in_src[32 * i + k] = (uint8_t)rnd();
        if (rnd() % 8 == 0) in_src[32 * i + 27] = (uint8_t)(rnd() % 3 ? 255 : 0);   // length scale at its ends
    }
    for (size_t i = 0; i < sizeof in_dst; i++) in_dst[i] = (uint8_t)rnd();
    for (int i = 0; i < MAXV * 14; i += 2) {
        int slot = (i % step[bones]) / 2;        // 0 = first bone offset; then (offset, weight) pairs
        uint16_t v = (slot == 0 || (slot & 1)) ? (uint16_t)((rnd() % BONES) * 64) : (uint16_t)rnd();
        memcpy(g_mem + BLEND + i, &v, 2);
    }
    Cpu end[2];
    uint32_t index_out[2];
    for (int native = 0; native < 2; native++) {
        memcpy(g_mem + SRC, in_src, sizeof in_src);
        if (!in_place) memcpy(g_mem + DST, in_dst, sizeof in_dst);
        M32(PINDEX) = index;
        Cpu cpu;
        memset(&cpu, 0, sizeof cpu);
        cpu.mxcsr = 0x1F80, cpu.fcw = 0x27F;
        cpu.ebx = 0x11111111u, cpu.ebp = 0x22222222u, cpu.esi = 0x33333333u, cpu.edi = 0x44444444u;
        cpu.esp = STACK;
        uint32_t args[4];
        if (bones <= 2) {
            cpu.eax = (uint32_t)count, cpu.edx = BLEND;
            args[0] = SRC, args[1] = MATS, args[2] = dst, args[3] = PINDEX;
        } else {
            cpu.eax = dst;
            if (bones == 3) cpu.edi = MATS; else cpu.esi = MATS;
            args[0] = SRC, args[1] = BLEND, args[2] = (uint32_t)count, args[3] = PINDEX;
        }
        for (int k = 3; k >= 0; k--) cpu.esp -= 4, M32(cpu.esp) = args[k];
        run(fn, &cpu, native);
        end[native] = cpu;
        index_out[native] = M32(PINDEX);
        memcpy(out[native], g_mem + dst, sizeof out[0]);
    }
    cases++;
    int bad = memcmp(out[0], out[1], sizeof out[0]) != 0 || index_out[0] != index_out[1] || !same_kept(&end[0], &end[1]);
    if (bad) {
        failures++;
        size_t at = 0;
        while (at < sizeof out[0] && out[0][at] == out[1][at]) at++;
        printf("FAIL skin%d count %d index %u huge %d in_place %d: first differing byte %zu (vertex %zu, offset %zu), index %u / %u\n",
               bones, count, index, huge, in_place, at, at / 32, at % 32, index_out[0], index_out[1]);
    }
}

// ---- memcpy --------------------------------------------------------------------------------------
static void memcpy_case(void) {
    enum { N = 4096 };
    static uint8_t init[N], out[2][N];
    for (int i = 0; i < N; i++) init[i] = (uint8_t)rnd();
    uint32_t n = rnd() % 5 == 0 ? rnd() % 1500 : rnd() % 40;
    uint32_t s = rnd() % (N - n + 1), d = rnd() % 3 ? (s + rnd() % 64 + N - 32) % (N - n + 1) : rnd() % (N - n + 1);
    Cpu end[2];
    for (int native = 0; native < 2; native++) {
        memcpy(g_mem + SRC, init, N);
        Cpu cpu;
        memset(&cpu, 0, sizeof cpu);
        cpu.mxcsr = 0x1F80, cpu.fcw = 0x27F;
        cpu.ebx = 1, cpu.ebp = 2, cpu.esi = 3, cpu.edi = 4;
        cpu.esp = STACK;
        uint32_t args[3] = {SRC + d, SRC + s, n};
        for (int k = 2; k >= 0; k--) cpu.esp -= 4, M32(cpu.esp) = args[k];
        run(f_00967760, &cpu, native);
        end[native] = cpu;
        memcpy(out[native], g_mem + SRC, N);
    }
    cases++;
    if (memcmp(out[0], out[1], N) != 0 || end[0].eax != end[1].eax || !same_kept(&end[0], &end[1])) {
        failures++;
        printf("FAIL memcpy dst +%u src +%u n %u\n", d, s, n);
    }
}

// ---- timing (relative: both sides in one process, best of several rounds) ---------------------------
static double now_ns(void) { return (double)clock_gettime_nsec_np(CLOCK_UPTIME_RAW); }
static inline void call(void (*fn)(Cpu *), Cpu *cpu, int native) {   // run() without the trap guard (its setjmp costs more than a copy)
    g_native_off = !native;
    cpu->esp -= 4;
    M32(cpu->esp) = RET_VA;
    fn(cpu);
}

static void bench_skin(int bones, void (*fn)(Cpu *)) {
    enum { COUNT = 200, REPS = 2000 };
    double best[2] = {1e30, 1e30};
    for (int round = 0; round < 5; round++)
        for (int native = 0; native < 2; native++) {
            double t0 = now_ns();
            for (int r = 0; r < REPS; r++) {
                M32(PINDEX) = 0;
                Cpu cpu;
                memset(&cpu, 0, sizeof cpu);
                cpu.mxcsr = 0x1F80, cpu.esp = STACK;
                uint32_t args[4];
                if (bones <= 2) {
                    cpu.eax = COUNT, cpu.edx = BLEND;
                    args[0] = SRC, args[1] = MATS, args[2] = DST, args[3] = PINDEX;
                } else {
                    cpu.eax = DST;
                    if (bones == 3) cpu.edi = MATS; else cpu.esi = MATS;
                    args[0] = SRC, args[1] = BLEND, args[2] = COUNT, args[3] = PINDEX;
                }
                for (int k = 3; k >= 0; k--) cpu.esp -= 4, M32(cpu.esp) = args[k];
                call(fn, &cpu, native);
            }
            double per = (now_ns() - t0) / (REPS * COUNT);
            if (per < best[native]) best[native] = per;
        }
    printf("bench skin, %d bone(s): lifted %.1f ns per vertex, native %.1f ns (x%.1f)\n", bones, best[0], best[1], best[0] / best[1]);
}

static void bench_memcpy(uint32_t n) {
    enum { REPS = 200000 };
    double best[2] = {1e30, 1e30};
    for (int round = 0; round < 5; round++)
        for (int native = 0; native < 2; native++) {
            double t0 = now_ns();
            for (int r = 0; r < REPS; r++) {
                Cpu cpu;
                cpu.esp = STACK;
                uint32_t args[3] = {DST + (r & 3), SRC, n};
                for (int k = 2; k >= 0; k--) cpu.esp -= 4, M32(cpu.esp) = args[k];
                call(f_00967760, &cpu, native);
            }
            double per = (now_ns() - t0) / REPS;
            if (per < best[native]) best[native] = per;
        }
    printf("bench memcpy, %u bytes: lifted %.1f ns, native %.1f ns (x%.1f)\n", n, best[0], best[1], best[0] / best[1]);
}

int main(int argc, char **argv) {
    if (argc < 2) return fprintf(stderr, "usage: test_native <BlackOps.exe>\n"), 2;
    FILE *f = fopen(argv[1], "rb");
    if (!f) return perror(argv[1]), 2;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    rewind(f);
    void *exe = malloc((size_t)size);
    if (fread(exe, 1, (size_t)size, f) != (size_t)size) return fprintf(stderr, "short read\n"), 2;
    fclose(f);
    if (rt_arena_reset() != 0 || rt_load_image(exe, (size_t)size) != 0) return fprintf(stderr, "cannot load the image\n"), 2;
    void (*const skin[5])(Cpu *) = {0, f_00745520, f_00745730, f_00745a00, f_00745d70};
    for (int bones = 1; bones <= 4; bones++) {
        int before = cases, failed_before = failures;
        for (int i = 0; i < 400; i++) skin_case(bones, skin[bones], i % 10 == 9, i % 4 == 3);
        printf("skin, %d bone(s) per vertex: %d cases, %d failed\n", bones, cases - before, failures - failed_before);
    }
    int before = cases, failed_before = failures;
    for (int i = 0; i < 20000; i++) memcpy_case();
    printf("memcpy: %d cases, %d failed\n", cases - before, failures - failed_before);
    if (argc > 2 && strcmp(argv[2], "--bench") == 0) {
        for (int bones = 1; bones <= 4; bones++) skin_case(bones, skin[bones], 0, 0), bench_skin(bones, skin[bones]);
        cases -= 4;
        bench_memcpy(16), bench_memcpy(64), bench_memcpy(1024);
    }
    printf("test_native: %d cases, %d failed\n", cases, failures);
    return failures != 0;
}
