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
| xmm0–7 | `cpu->xmm[n]` (`union Xmm`, 128 bits, bit-exact) |
| x87 stack | `cpu->st[8]` as **`double`** + `cpu->ftop`; `ST(i)` macro; control word `cpu->fcw`; C0–C3 in `cpu->fsw` |
| mm0–7 | `cpu->mm[n]`, separate from the x87 registers (A3) |
| MXCSR | `cpu->mxcsr` (value kept; only the rounding field is honoured, and only by float->int conversions) |

Flag results are computed eagerly by helpers (`add32(&fl, a, b)` …). They are `always_inline`
and `fl` never escapes, so clang deletes every flag that is not read. Values the Intel SDM
leaves undefined get a fixed value (see the helpers) so that runs are deterministic.

**Assumption A1 — status flags are dead across call/ret**, audited by `tools/lifter/flag_audit.py`
(backward liveness of CF/PF/AF/ZF/SF/OF over every function, `re/flag_audit.json`): of 21,960
functions, 9 read a flag at entry before writing it and 3 call sites read a flag after the call
returns (all three call the CRT helper `__fload_withFB`, which answers in ZF). Seven of the nine are
CRT math internals reached by jumps; two (0x0042E8B0, 0x007DC550) are not real functions but
mid-function code that the function seeding promoted to an entry. For exactly these the lifter
hands the flags over through `Cpu.xfl` (`cpu->xfl = fl` before the transfer, `fl = cpu->xfl` after
it). Not covered: flags passed through an *indirect* call (none known).

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

## 4. What is supported (T-041 + T-042)
Integer: mov/movzx/movsx/lea/xchg, push/pop/leave/pushal/popal/pushfd/popfd, add/adc/sub/sbb/inc/dec/neg/cmp,
and/or/xor/not/test, shl/shr/sar/rol/ror/rcl/rcr/shld/shrd, imul/mul/div/idiv, setcc/cmovcc, bswap, bsf/bsr,
bt/bts/btr/btc, xadd, cmpxchg, cmpxchg8b, xlatb, cld/std, sahf/lahf, cdq/cwde/cbw, string ops with rep,
jmp/jcc/jecxz, call/ret, cpuid, rdtsc. FP/SIMD (`tools/lifter/lift_fp.py`, helpers in
`src/runtime/lifted_fp.h`): x87 (decoded from the opcode bytes D8–DF + ModRM, cross-checked against
capstone's mnemonic: 0 mismatches over the image), SSE/SSE2 scalar and packed float, SSE2 integer, MMX.

A function is lifted only if **every** instruction in it is supported; otherwise it becomes a stub
in `lifted_table.c` that traps with `TRAP_UNSUPPORTED`. Current numbers, the remaining blockers and
the list of unlifted functions (`unlifted`): `re/lift_coverage.json`.

**Function-body repair** (`Lifter.extend`): when a direct jump or fallthrough leaves the Ghidra body
without entering another function at its entry (tails shared between functions, bodies Ghidra cut
short, code after calls Ghidra believes never return), the lifter decodes the missing blocks itself
and emits them as part of the jumping function (tail duplication). Code after a "noreturn" call that
does not decode becomes `rt_trap(TRAP_FALLTHROUGH)`.

## 5. Floating-point model and its known deviations from x86
Build flag that matters: **`-ffp-contract=off`** — arm64 clang otherwise fuses `a*b+c` into FMADD,
which changes results.

| Topic | Model | Deviation |
|---|---|---|
| SSE scalar/packed arithmetic | C `float`/`double` (IEEE-754, round-to-nearest-even) | none for finite values |
| NaN results | `LIFT_EXACT_NAN=1`: x86 rule (first NaN operand quieted, else the *negative* default NaN) restored after each operation | arm64's default NaN is positive; without the fix-up the sign/payload differ |
| float -> int (`cvt(t)ss2si`, `fist(p)`, …) | explicit range check, x86 "integer indefinite" (0x80000000) on overflow/NaN | none (arm64 natively saturates) |
| MXCSR | rounding field honoured by conversions only; exception masks ignored; **FTZ/DAZ not modelled** | arithmetic under a non-default rounding mode or with FTZ/DAZ set would differ — check what the game sets (T-044) |
| `rcpss/rsqrtss/rcpps/rsqrtps` | exact `1/x`, `1/sqrt(x)` | x86 returns 12-bit table approximations (differs in low bits; 1 use in the image) |
| x87 registers | `double` | x87 keeps 64-bit significands and a 15-bit exponent. With precision control = 53 bits (Windows default, FCW 0x27F) results are identical except for double overflow/underflow in intermediates; with PC = 24 bits (what D3D9 sets without `D3DCREATE_FPU_PRESERVE`) every arithmetic result is additionally rounded to `float` (`x87_pc`) |
| `fild m64` / `fistp m64` | via `double` | integers beyond 2^53 lose low bits (x87 holds all 64) |
| `fld m80` / `fstp m80` | converted to/from `double` | precision/range of the 80-bit format lost |
| `fsin/fcos/fsincos/fptan/fpatan/fyl2x/f2xm1/…` | libm | last-bit differences against x87 microcode (and against Unicorn) |
| x87 rounding control | honoured by `fist(p)`/`frndint` only (the `fldcw` truncation idiom) | arithmetic always rounds to nearest |
| x87 stack faults, tag word, exceptions | not modelled | code relying on FP exceptions would differ |
| `fnstenv/fldenv/fnsave/frstor` | unsupported (2 functions) | — |
| CPUID | fixed: Core 2-class, MMX+SSE+SSE2, **no SSE3/SSSE3/SSE4** | CPU-dispatched code takes its SSE2 paths; `palignr` (SSSE3, 2 functions) is not lifted |
| `lock` prefix | `xadd`/`cmpxchg`/`cmpxchg8b` on memory are atomic (aligned operands); other locked RMW are **not atomic yet** | T-044 |

**Assumption A3 — MMX and x87 do not exchange data through the aliased registers.** On x86 mm0–7 are
the x87 significands; here they are separate arrays and `emms` is a no-op.

## 6. Differential testing (`tests/diff/`, `scripts/difftest.sh`)
```
scripts/difftest.sh [max_funcs] [vectors_per_func] [seed]     # default 400 3 1; 100000 = every leaf
```
1. `tests/diff/gen_vectors.py` executes the **original** bytes of each selected function under
   Unicorn from a random state (registers; 32 stack slots; XMM registers; a 16 KiB "heap" of
   floats, small integers, pointers and random words that 40% of the random values point into;
   FCW 0x27F or 0x07F) until it returns to a sentinel address. It records the inputs, the final
   registers, x87 stack (as doubles), XMM registers, MMX registers and every byte written. Vectors
   that fault, do not return within 100k instructions or write more than 64 KiB are dropped
   (and counted).
2. `tests/diff/difftest.c` loads the PE into a fresh arena, applies the same inputs, calls the
   lifted function and compares all eight registers, the x87 stack depth and values, XMM (bit-exact),
   MMX for functions that use it, and all written bytes. A vector whose only difference is an x87
   value within 1e-12 relative is reported separately ("approx"), not as a pass.
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
- Non-leaf functions and `fs:` in the vectors (T-043).
- Compile time and binary size of the whole image; `-O1` vs `-O2` (T-044).
- Unicorn must be rebuilt after a faulting run (a failed run left it returning to garbage
  addresses for every later function) — handled in `gen_vectors.py`.
