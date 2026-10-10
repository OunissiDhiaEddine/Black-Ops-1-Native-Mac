# LIFTER — static recompilation of `BlackOps.exe` (x86-32) to C (ADR-013)

> Keep this file in step with `tools/lifter/lift.py`, `src/runtime/lifted.h`, `tests/diff/` and `src/native/native.h`.
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

**Address-taken code** (`tools/lifter/find_entries.py` -> `re/extra_entries.csv`). `rt_dispatch` only knows
entries, so every code address that is stored somewhere must become one: 32-bit immediates of all
instructions, aligned dwords of the data sections, runs of code pointers at odd offsets (the CRT's
math dispatch tables), dwords in the gaps of the code section, and direct `call`s to addresses that are
not functions (local subroutines of hand-written routines, SEH funclets). 4,344 such entries today;
most are functions Ghidra never created (reached only through vtables or callbacks). Each becomes a
function of its own, decoded by recursive descent; code it shares with an enclosing function is
duplicated. Candidates that are not really code simply fail to lift and stay trap stubs (226).
Indirect *jumps* copy the flags to `Cpu.xfl` first, because such an entry can be a label that reads them.

**Assumption A2 — every call returns to the instruction after it.** Code that pops the return
address and jumps elsewhere, or `longjmp`-style unwinding, breaks this; such functions must be
listed and handled individually (C++ EH/SEH policy is ADR-012).

*A2 audit (T-044, 2026-10-07): no violation found.* Two checks:
- Dynamic — `scripts/build_lifted.sh --a2-check` builds a second tree (`artifacts/lifted_a2`, `lift.py --a2-check`)
  in which every `ret` compares the address on the guest stack with the one the function was entered with, and
  `BO1_LIFT=artifacts/lifted_a2 scripts/build_boot.sh` links it as `bo1_boot_a2`. A 185 s run (start-up, menu,
  mission 1, `disconnect`, a failed `devmap` with recovery; `artifacts/boot/a2_audit_run.log`) printed 23 `[A2]`
  lines from 17 functions. In all 23 the `ret` goes to the instruction after a real `call` (checked against the
  image bytes) and the value at entry was not a return address at all (0, 0x20, stack pointers, …): these are
  code fragments the lifter enters by a jump inside another function's frame (shared tails, case labels listed
  as functions, entries found by the address scan) — the lifted tail call returns to the right host caller.
  No function that was entered by a call returned anywhere else. Limit: only code that ran.
- Static, over every lifted instruction: `call` to the next instruction — 1 (0x0096FDAA, an ordinary local
  subroutine in the CRT); `ret` directly after `push` — 1 (0x00977B17, `__SEH_epilog4`: pops its return address,
  unwinds the caller's frame, pushes the address back and returns — correct under the lifted `ret`, which only
  adjusts `esp`); functions whose first instruction pops or drops the return address — 2 real ones out of
  25,945 entries (0x00783740, a case label Ghidra lists as a function, and 0x0096C188 `_CallMemberFunction1`,
  the C++ EH helper `pop eax; pop ecx; xchg [esp], eax; jmp eax`, which the lifted indirect tail call handles and
  which only runs during C++ unwinding, ADR-012); the other 69 hits are entry-scan candidates that are text, not
  code (addresses like 0x00454E4F = "ONE").

*setjmp / longjmp (T-060).* The game leaves `Com_Error` — and with it every `disconnect` — through the CRT's
`longjmp`, which restores the callee-saved registers and the stack pointer from the buffer and ends in
`jmp [buf+0x14]`: a jump to the instruction after a `call _setjmp3`, which is no function entry. The lifter
therefore emits, after each of the 13 calls of `_setjmp3` (`SETJMP` in `lift.py`),
`if (_setjmp(*rt_setjmp_slot(cpu, <return address>))) fl = cpu->xfl;` — the guest routine has filled the guest
buffer as always, and the host context of the lifted caller is kept in a slot keyed by that buffer
(`src/runtime/runtime.c`). `rt_dispatch()` looks an unknown target up among the slots (return address and
stack pointer must both match) and `_longjmp`s to the host context; the lifted caller continues after its call
with the registers the guest `longjmp` restored. This relies on "host stack depth equals guest call depth"
(below): the lifted caller's host frame is alive exactly as long as its guest frame. Host frames of shims in
between are dropped without running C++ destructors, so a shim must not hold a lock while it calls guest code.

*Jump tables (T-061).* Ghidra lists only part of some `jmp [idx*4 + table]` tables (5 in this image, among
them the one at 0x00663218 in the save-game reader with 1 of its 4 targets). `Lifter.complete_tables()` adds
the pointers that stand at the table address in the image, up to the next table and as long as they point
into the function the jump is in (`re/lift_coverage.json` -> `jump_tables_completed`). A value outside the
recovered cases still traps as TRAP_BAD_JUMP.

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
| NaN results | `LIFT_EXACT_NAN=1`: x86 rules restored after each operation — SSE: first NaN operand quieted; x87: larger significand / QNaN over SNaN / positive on a tie; no NaN operand: the *negative* default NaN | arm64's default NaN is positive; without the fix-up the sign/payload differ |
| float -> int (`cvt(t)ss2si`, `fist(p)`, …) | explicit range check, x86 "integer indefinite" (0x80000000) on overflow/NaN | none (arm64 natively saturates) |
| MXCSR | rounding field honoured by conversions only; exception masks ignored; **FTZ/DAZ not modelled** | none in this game (audit T-044, 2026-10-07): the image has three `ldmxcsr`, all in the CRT — `__set_fpsr_sse2` (0x009811A1), whose only caller chain is `_control87` <- `_controlfp_s` <- `__setdefaultprecision` (0x00970B0B: `_controlfp_s(NULL, _PC_53, _MCW_PC)`) <- `__fpmath` <- `__cinit`, i.e. once at start-up and only the x87 precision field; and an unreferenced routine at 0x009811EA. None of `_control87` / `_controlfp_s` / `__set_fpsr_sse2` is address-taken (`re/ghidra_export/fnptr_refs.csv`, `re/extra_entries.csv`). So MXCSR keeps its reset value 0x1F80 in game code: round-to-nearest, FTZ and DAZ off — what the lifter assumes. x87 control-word writes (`fldcw`, 288 sites, the float-to-int idiom) are modelled. Not covered: whether XAudio2 itself sets FTZ/DAZ on its mixer thread, where the game's effects run (GUESS: it does; the difference would be confined to denormal-range samples) |
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
scripts/difftest.sh [max_funcs] [vectors_per_func] [seed] [leaf|all]   # default 400 3 1 leaf; 100000 = everything
  env: JOBS=4  SKIP_VECTORS=1 (reuse vectors)  REPORT=re/difftest_report.json
```
`leaf` = functions without calls, tail calls, indirect transfers or `fs:`. `all` = every lifted function:
the whole call tree executes on both sides, so calls, returns, stack discipline and callee effects are
compared too; a vector is dropped when Unicorn cannot finish it (import call — import slots are poisoned
on both sides —, wild pointer, execution outside the code sections, instruction limit).

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

Known limits of the method (do not read more into a green run than this):
- status flags at return are not compared (A1);
- only the paths the random inputs reach are checked — see the instruction-coverage figure;
- a vector proves agreement with **Unicorn**, not with a real CPU (cross-check on x86_64: T-046);
- memory the original only *reads* outside the supplied blocks is zero, which hides bugs that
  depend on real global state.

A function's inputs depend on the seed and the function's address only (the generator reseeds per function), so
the shard count (`JOBS`) and the selection do not change them (T-069; before, `JOBS=3` and `JOBS=4` tested
different inputs). Dropped as input artefacts, and counted: runs in which an access runs over the end of the
32-bit address space — the emulator continues at address 0, the arena has no wrap-around, Windows would fault.

Three result classes besides pass/fail keep known limits visible without hiding bugs:
"nan-only" (difference only in the sign/payload of NaNs), "fp-range" (a double overflowed or
underflowed while the lifted code ran — the x87's wider exponent, §5) and "approx" (x87 value within
1e-12). Inputs on which the reference itself is wrong are dropped when the vectors are generated
(hooks on the exact instruction addresses, `artifacts/lifted/special_sites.json`, written by the lifter):
`FYL2X` with ST(0) <= 0, `FPREM` with an exponent gap >= 53, every `FPREM1`, `FSIN/FCOS/FSINCOS/FPTAN` of
infinity, and anything executing `CPUID`/`RDTSC` (machine-dependent).

Deviations of the REFERENCE found so far (the lifted code follows the Intel SDM in every case):
- QEMU sets FSW.ZE for 0/0; a real x87 sets IE. Six functions return with that status word still in
  eax (`tests/diff/reference_deviations.json`).
- QEMU's `FCOMI` family, `FSIN`… of infinity, partial `FPREM`: see above.
- The QEMU inside Unicorn computes `FPREM1` like `FPREM`: it truncates the quotient where a real x87 (and the
  lifted code) rounds it to nearest. CONFIRMED on one vector (0x009710FC, the CRT's argument reduction for
  `fptan`): -5.6065526952890201e34 by 1.4488038916154245e19 gave -1.0701e19, larger in magnitude than half the
  divisor, which an IEEE remainder cannot be; the lifted code gave 3.7868e18. Vectors that execute it are dropped.
- QEMU/Unicorn chooses between two NaN operands with the x87 rule (larger significand) for SSE as
  well; real SSE returns the first operand. Such vectors are reported as "nan-only", not as passes.
- QEMU's `FYL2X` leaves the stack alone when ST(0) <= 0; a real x87 stores NaN/infinity and pops.
  Functions affected are listed in `tests/diff/reference_deviations.json` and reported separately.

Differences of an "fp-range" vector are printed as `RANGEDIFF …` lines, not `FAIL …` (the report counts failing
functions by the `FAIL` lines; before T-069 such a vector made its function "unexplained").

Debugging a failing vector: `tests/diff/debug_vector.sh <func_va> [nth]` (`VECTORS='dir/vectors_*.bin'` for another set) lifts the function (and its
callees) with `RT_TRACE` before every instruction, runs the vector through both sides and prints the
first instruction after which registers / XMM / x87 differ.

Harness sanity check (mutation test, 2026-10-05): making `add` wrong for 1 input in 8 turned
32 of 1,160 vectors red; making `sub` wrong for a single input value turned 3 red.

### x86_64 cross-check (T-046, 2026-10-06)
The same generated C, compiled for x86_64 and run under Rosetta 2 on the same 26,294 vectors:
- at **-O0**: identical verdicts to the arm64 build except three functions that differ by 1 ulp in a
  transcendental result (0x0048F160, 0x004D6A60, 0x009654FA) — the x86_64 and arm64 system libm disagree
  there. Consequence worth remembering: for `fsin/fcos/fyl2x/…` the reference (Unicorn) itself calls the
  host libm, so "matches the reference" says nothing about a real x87 for those instructions.
- at **-O1**: one more difference (0x0092C780), traced to a **miscompile in Apple clang 21.0.0's
  x86_64 backend**: `c->eax = load32(p); c->eax = (c->eax & 0xFFFFFF00) | g[c->eax];` stores the
  zero-extended byte and drops the upper 24 bits. Reduced, UB-free reproducer:
  `tests/diff/compiler_bugs/x86_64_byte_insert_after_load.c` (arm64 is correct at every level).
  So an optimised x86_64 build is not a usable oracle with this toolchain; the arm64 build is what the
  differential tests validate.

## 7. Open items
- 8 functions are not lifted (`re/lift_coverage.json` -> `unlifted`): 3 CRT crash handlers that push segment
  registers (`__invoke_watson`, `_abort`, `___report_gsfailure`), 2 SSSE3 (`palignr`) routines behind a CPUID
  check, `fnsave`/`fnstenv` users (2), one `pushaw` (LIKELY data decoded as code).
- 1 indirect jump through a table in `.data` that is filled at run time (0x00509821, 8 entries): goes through
  `rt_dispatch`; if the entries turn out to be labels inside a function it will trap with the address.
- 5,432 indirect call sites and 101 indirect jumps (34 import thunks) rely on `rt_dispatch`; targets that are
  not function entries trap (`TRAP_NO_TARGET`) — the list of such targets comes from running (T-045).
- Indirect transfers and imports are not exercised by the diff tests (they need a live process image): T-045.
- Whole-image numbers (measured on one 400-function unit, M1): ~5 s to compile at -O1/-O2, ~0.46 MB arm64
  text -> ~25 MB of code for the image (x4.3 over the original 5.9 MB), ~1.5 min full build with 6 jobs.
- Unicorn must be rebuilt after a faulting run (a failed run left it returning to garbage
  addresses for every later function) — handled in `gen_vectors.py`.

## 8. Native replacements (P6, `src/native/`)
A lifted function is handed to hand-written code by defining it in a C file under `src/native/` with the marker
`NATIVE(<8 hex digits>) { … }` (`src/native/native.h`). Nothing else names it: `lift.py` scans `src/native/*.c` for
the marker, emits the lifted body of that address as `f_<va>_lifted` and leaves `f_<va>` — what every lifted
caller and the `rt_dispatch` table reach — to the native source (`re/lift_coverage.json` -> `native_replacements`).
The lifter stops when a marked address is not a function or does not lift. `lifted_protos.h` does not change, so
replacing a function recompiles only the chunk it is in.

- **Contract of a replacement:** it is entered like the lifted function (return address at `M32(cpu->esp)`, stack
  arguments above it — `ARG(n)` —, register arguments in the `Cpu`), must leave `esp` as the original's `ret` does
  and keep what the original keeps. Anything it does not reproduce — caller-saved registers, scratch below `esp`,
  SIMD registers, NaN payloads — is written at the function together with the evidence that no caller reads it.
- **Switch:** `BO1_NATIVE=off` makes every replacement run its lifted original (one predictable branch per call):
  for A/B measurements, for bisecting a bug, and for `scripts/difftest.sh`, whose vectors keep testing the lifted
  bodies against the x86 reference.
- **Test:** `scripts/test_native.sh` (`tests/native/test_native.c`) runs each replacement and its lifted original
  on the same randomised guest memory and registers and compares every output byte, the registers the original
  keeps and `esp`; `--bench` adds the time of both sides. Lifted-vs-x86 (§6) plus native-vs-lifted is the chain.
  A new replacement gets its cases there, and a mutation check (break the native code on purpose, see the test
  fail) before it is trusted.
- **Flags:** `-O2 -ffp-contract=off -Wall -Wextra -Werror` (`scripts/build_boot.sh`); no fused multiply-add, as
  for the lifted code.
- `lift.py --no-native` ignores `src/native` (the single-function debug tree of `tests/diff/debug_vector.sh`).
- Replaced so far: `re/lift_coverage.json` -> `native_replacements`; what each one is: `re/symbols.csv`.
