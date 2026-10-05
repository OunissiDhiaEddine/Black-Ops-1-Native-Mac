# LIFTER — static recompilation of `BlackOps.exe` (x86-32) to C (ADR-013)

> Keep this file in step with `tools/lifter/lift.py`, `src/runtime/lifted.h` and `tests/diff/`.
> Generated code lives in `artifacts/lifted/` (gitignored, never hand-edited, never committed —
> it is a derivative of the game binary). Regenerate with `scripts/difftest.sh` or
> `.venv/bin/python tools/lifter/lift.py`.

## 1. Pipeline
```
reference/…/BlackOps.exe ──┐
re/ghidra_export/*.csv ────┼─> tools/lifter/lift.py ─> artifacts/lifted/lifted_NNNN.c   (400 functions each)
 (functions, blocks,       │                           artifacts/lifted/lifted_protos.h
  jumptables)              │                           artifacts/lifted/lifted_table.c  (VA -> function, stubs)
                           │                           artifacts/lifted/lifted_meta.json (per-function facts)
                           └─> re/lift_coverage.json   (what lifts, what blocks the rest)
artifacts/lifted/*.c + src/runtime/*.c ─ clang ─> arm64 objects
```
Front end: capstone (detail mode). Function boundaries, basic blocks and switch tables come from
the Ghidra export (`scripts/ghidra_analyze.sh --export`); the lifter does not rediscover code.
Back end: plain C11, one C function per guest function, compiled by Apple clang.

## 2. Machine model
| x86 | lifted C |
|---|---|
| eax … edi | `cpu->eax` … (`struct Cpu`, `src/runtime/lifted.h`); 8/16-bit sub-registers are masked reads/writes of the 32-bit field |
| esp / guest stack | `cpu->esp`, real guest stack inside the arena (`PUSH32` / `POP32`) |
| memory | `M8/M16/M32/M64(addr)` = unaligned access at `g_mem + addr` (4 GiB arena, ADR-003) |
| CF PF AF ZF SF OF | `Flags fl` — a **local variable of each lifted function** |
| DF | `cpu->df` (the only flag kept across calls) |
| fs:[x] | `M32(cpu->teb + x)` |
| x87 / SSE state | not modelled yet (T-042) |

Flag results are computed eagerly by helpers (`add32(&fl, a, b)` …). They are `always_inline`
and `fl` never escapes, so clang deletes every flag that is not read. Values the Intel SDM
leaves undefined get a fixed value (see the helpers) so that runs are deterministic.

**Assumption A1 — status flags are dead across call/ret.** True for compiler-generated Win32 code;
must be audited (T-044) for hand-written assembly that returns a result in CF/ZF.

## 3. Control flow
- Each basic block becomes a label `L_<va>`; direct jumps inside the function are `goto`.
- Direct jump to another function = tail call: `{ f_<va>(cpu); return; }`.
- Fallthrough into the next function is the same as a tail call.
- Switch tables recovered by Ghidra become a C `switch` on the computed target address with one
  case per table entry; anything else traps (`TRAP_BAD_JUMP`).
- Other indirect jumps and all indirect calls go through `rt_dispatch(cpu, target)`, which looks
  the target up in the sorted table `g_lifted_table` (guest VA -> host function).
- `call f`: `PUSH32(return_va); f_<va>(cpu);` — the return address is really pushed so that the
  guest stack layout is identical to the original (stack arguments, `ret N`, code that reads its
  own return address). `ret`/`ret N` pop it and `return`.

**Assumption A2 — every call returns to the instruction after it.** Code that pops the return
address and jumps elsewhere, or `longjmp`-style unwinding, breaks this; such functions must be
listed and handled individually (T-044; C++ EH/SEH policy is ADR-012).

**Host stack depth** equals guest call depth (one host frame per guest call). Guest threads get a
host stack sized accordingly (T-045).

## 4. What is supported today (T-041)
Integer subset: mov/movzx/movsx/lea/xchg, push/pop/leave, add/adc/sub/sbb/inc/dec/neg/cmp,
and/or/xor/not/test, shl/shr/sar/rol/ror/shld/shrd, imul/mul/div/idiv, setcc/cmovcc, bswap, bsf/bsr,
bt, cld/std, sahf/lahf, cdq/cwde/cbw, movs/stos/lods/scas/cmps with rep prefixes, jmp/jcc/jecxz,
call/ret. `lock` prefixes are accepted and ignored for now (atomicity: T-044).

A function is lifted only if **every** instruction in it is supported; otherwise it becomes a stub
in `lifted_table.c` that traps with `TRAP_UNSUPPORTED`. Current coverage and the list of blocking
instructions: `re/lift_coverage.json`.

## 5. Plan for the rest (T-042, by blocker frequency)
1. SSE/SSE2 scalar float (`movss`, `cvtsi2ss`, `cvttss2si`, `comiss`, arithmetic) — `Cpu.xmm[8]`
   as 128-bit unions; scalar ops map to C `float`/`double` (IEEE, round-to-nearest; MXCSR writes
   recorded, non-default rounding trapped until seen in practice).
2. SSE packed + integer SSE2 (`xorps`, `pxor`, `movq`, `movaps`, shuffles) — element-wise C;
   NEON intrinsics only where profiling demands it (P6/P7).
3. x87 (`fld/fstp/fild/fmul/…`) — register stack of 8 `double` + top-of-stack index + control word.
   x87 computes with 80-bit extended precision; the game sets the precision-control field
   (`re/float_census.json`), so `double` is the planned model. Differences against Unicorn are to
   be measured in the diff tests, not assumed away.
4. `xadd` / `cmpxchg` (with `lock`: C11 atomics on arena memory).
5. "fallthrough leaves the function" (77 functions): function-boundary errors in the Ghidra export —
   fix in the seeding step, not in the lifter.

## 6. Differential testing (`tests/diff/`, `scripts/difftest.sh`)
```
scripts/difftest.sh [max_funcs] [vectors_per_func] [seed]     # default 400 3 1; 100000 = every leaf
```
1. `tests/diff/gen_vectors.py` executes the **original** bytes of each selected function under
   Unicorn from a random state (registers; 32 stack slots; a 16 KiB random "heap" that 40% of the
   random values point into) until it returns to a sentinel address. It records the inputs, the
   final registers and every byte written. Vectors that fault, do not return within 200k
   instructions or write more than 64 KiB are dropped (and counted).
2. `tests/diff/difftest.c` loads the PE into a fresh arena, applies the same inputs, calls the
   lifted function and compares all eight registers and all written bytes.
3. `re/difftest_report.json` holds the totals, including how many of the instructions in the
   tested functions were actually executed by at least one vector.

Unmapped memory reads as zero on both sides, so wild pointers behave identically.
Selection today: leaf functions only (no calls, tail calls, indirect transfers or `fs:` access).

Known limits of the method (do not read more into a green run than this):
- status flags at return are not compared (A1);
- only the paths the random inputs reach are checked — see the instruction-coverage figure;
- a vector proves agreement with **Unicorn**, not with a real CPU (cross-check on x86_64: T-046);
- memory the original only *reads* outside the supplied blocks is zero, which hides bugs that
  depend on real global state.

Harness sanity check (mutation test, 2026-10-05): making `add` wrong for 1 input in 8 turned
32 of 1,160 vectors red; making `sub` wrong for a single input value turned 3 red.

## 7. Open items
- Non-leaf functions, `fs:`, FP state in the vectors (T-043).
- Compile time and binary size of the whole image; `-O1` vs `-O2` (T-044).
- Unicorn must be rebuilt after a faulting run (a failed run left it returning to garbage
  addresses for every later function) — handled in `gen_vectors.py`.
