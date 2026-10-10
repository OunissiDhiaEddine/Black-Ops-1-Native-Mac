# PLAN — BO1 native Metal / arm64 port

## 0. Reality check (read once)
A from-scratch manual decompile of a ~2010 AAA engine (millions of x86 instructions, no symbols) into clean 64-bit source is a multi-year, multi-person effort. This plan does NOT depend on that. It gets to a **native arm64 + Metal build early** by using **static recompilation** (lift the x86-32 code to LLVM IR/C, compile for arm64) and then **progressively replaces** lifted modules with clean hand-written 64-bit code ("strangler fig") where it pays off. Each phase produces something that runs.

Two things make this tractable:
1. The renderer is developed **in isolation** from a recorded D3D9 trace (P3) — you don't need the game running to build the Metal backend.
2. A **reference baseline** (the game running under Wine/DXVK) gives golden frames and behaviour to diff against (P1).

**Fallback if P4 stalls:** ship the "wrapper" path — wine32on64 + a Metal-based D3D9 layer. Keep P1 artifacts usable for that.

## Phase overview
| Phase | Name | Output |
|---|---|---|
| P0 | Recon & inventory | binary facts, import/middleware census |
| P1 | Reference baseline | game running under Wine; golden frames; D3D9 trace |
| P2 | Platform shim design | list of every Win32/3rd-party call to replace |
| P3 | Metal renderer (isolated) | trace-replay renders correctly in Metal |
| P4 | Static recompilation pipeline | lifted code passes differential tests; boots headless |
| P5 | Integration | main menu -> mission 1 in native arm64 + Metal |
| P6 | Strangler fig | hot modules replaced by clean 64-bit C++ |
| P7 | Metal optimization | frame time / memory / shader-cache work |
| P8 | QA & polish | saves, input, display modes, stability |

Decision gate **G1** after P0–P3: is P4 feasible? (See P4 risks.) Ask the user before proceeding past G1.

---
## P0 — Recon & inventory (no code changes to the game)
**Goal:** know exactly what we're dealing with.
- Record SHA-256, size, PE headers, sections, imports/exports, TLS callbacks, compiler/linker version (Rich header) for `BlackOps.exe` (SP), `BlackOpsMP.exe`, and every DLL in the install dir.
- Import census: histogram of Win32 APIs, d3d9/d3dx9, dsound/xaudio2, dinput, winsock, steam_api, bink, others. Output: `docs/RE_NOTES.md` § Imports and `re/imports.csv`.
- Check for packing/protection stubs. If present -> BLOCKED, tell user (this project does not do DRM work).
- Float model: SSE2 vs x87 usage ratio (drives lifter design).
- Load into Ghidra (headless script), auto-analyze, export function list + basic blocks + jump tables + vtable guesses to `re/`.
- Asset formats overview: fastfiles (.ff), IWD archives, localized dirs; note compressed/streaming formats. (The port only READS assets; no conversion unless a task says so.)
**Exit:** RE_NOTES has binaries table, import census, middleware list, float-model note; Ghidra export reproducible by one script.

## P1 — Reference baseline
**Goal:** a known-good way to run the game on the Mac, used as oracle.
- Run the SP exe via CrossOver or Wine with wine32on64 (D3D9 -> DXVK -> MoltenVK, or wined3d). Record chip, macOS version, wine version, settings in LOG.
- `scripts/run_baseline.sh` launches it repeatably.
- Build `tools/d3d9_proxy/`: a d3d9.dll proxy that logs the full D3D9 call stream (device creation params, render states, FVF/vertex decls, shader bytecode blobs, texture/surface formats, Draw* calls, Present) into a compact binary trace in `artifacts/traces/`.
- Capture golden frames at fixed moments (main menu, first loading screen, in-mission scenes A/B/C) + frame-time CSV.
- Produce the **D3D9 census**: which API functions/states/formats the game actually uses (expect a small subset). Output in `docs/RE_NOTES.md`.
**Exit:** golden frames + replayable trace of main menu and one in-game scene; census doc.

## P2 — Platform & middleware shim design
**Goal:** a complete, finite list of what must be replaced.
- From P0 imports: map each Win32 API to a host implementation (`src/platform/`): files (case-insensitive path mapping), threads/TLS/critical sections/events, virtual memory (VirtualAlloc -> mmap inside the guest arena), timers (QPC), window + input (SDL3 or AppKit + GameController.framework), audio (identify real backend; target CoreAudio/AVAudioEngine/miniaudio), networking (sockets; MP out of scope initially), Steamworks stubs (only what the SP exe calls).
- Middleware: Bink video (FFmpeg has Bink decoders — evaluate), audio middleware (identify), physics (identify). Each is either lifted too (if it's a 32-bit DLL) or replaced.
- Define the **host-call ABI**: how lifted code calls shims (see P4 guest model).
**Exit:** RE_NOTES § Platform map: every import -> {shim | lift | stub | not needed}.

## P3 — Metal renderer, built in isolation
**Goal:** replace DX9 with Metal *without needing the game to run*.
- Design `src/rhi/` as the **smallest interface the census (P1) demands**, not a general D3D9 clone. Decide: D3D9-semantics shim (fast to integrate) vs cleaner RHI + translation at the edge (ADR-004).
- Shader path (ADR-005; evaluate both on real shader blobs from the trace): (a) own SM2/3 bytecode -> MSL translator (token format is simple and finite); (b) SPIR-V + SPIRV-Cross like DXVK. Cache compiled `MTLLibrary`/PSOs on disk.
- D3D9 -> Metal semantic gaps to handle: half-pixel offset, clip-space Z, vertex declarations -> `MTLVertexDescriptor`, fixed-function leftovers (if any), alpha test (emulate in shader), user clip planes, render-state -> PSO/depth-stencil mapping + PSO cache, render-to-texture/format fallbacks (D3DFMT_* -> MTLPixelFormat), `DrawPrimitiveUP` -> transient buffers, sRGB handling.
- `tests/replay/` harness: replay a trace into the Metal backend, compare to golden frames (SSIM threshold + diff images).
**Exit:** replayed main-menu and in-game traces match golden frames within threshold; Metal validation layer clean.

## G1 — decision gate
Review with the user: lifter feasibility data from P0 (indirect-call / jump-table recoverability, x87 usage, SEH usage, runtime-generated code) + P3 results. Choose: continue P4 / adjust / fall back to wrapper path.

## P4 — Static recompilation pipeline
**Approach:** x86-32 PE -> function/CFG recovery (Ghidra export, verified) -> lift to LLVM IR (or generated C) -> compile to arm64.
**Guest memory model:** the game's 32-bit pointers stay 32-bit inside lifted code; one ~4 GB arena is reserved at startup and every guest access is `host_base + (uint32_t)addr` (on arm64: `ldr x, [x28, w_addr, uxtw]` — one instruction with a pinned base register). This yields a genuinely 64-bit native arm64 process without fixing every struct layout in the original.
Components:
- `tools/lifter/`: instruction semantics (integer, lazy flags, SSE/SSE2, x87), calling conventions (cdecl/stdcall/thiscall/fastcall), jump tables, indirect calls through a guest-function-address table, import thunks -> host shims (guest-visible vtables for COM-style objects live in guest memory and trap to host).
- `src/runtime/`: arena, TLS, minimal SEH support (or confirm the game doesn't rely on it), host-call trampolines.
- **Differential testing** (`tests/diff/`): run the original function under Unicorn (x86 emulation) and the lifted version on random/recorded inputs; compare registers + memory writes. Each instruction pattern gets a test before it's trusted.
- Debug aid: also compile lifted output as x86_64 (runs under Rosetta 2) to separate lifter bugs from arm64 codegen bugs.
- Milestones: M4a lift+run one leaf function correctly -> M4b lift whole binary, boots to a null-renderer main loop -> M4c diff-test suite green.
**Risks:** unresolved indirect jumps; x87 precision (software fallback if needed); runtime-generated code; SEH-based control flow; flag-emulation performance (mitigate with flag liveness analysis).
**Exit:** headless boot to main loop, no crash for 10 minutes, diff-test suite green.
**Status 2026-10-06: met** (`artifacts/boot/soak_run.log`: 615 s, 38,000 frames; diff tests: `re/difftest_report_all.json`).

## P5 — Integration
Lifted game + `src/platform` + Metal backend -> main menu -> mission 1. Triage crashes with address -> name via `re/symbols.csv`. Compare frames with golden.
**Exit:** campaign mission 1 playable start to finish.
**Status:** in progress — the current state is the STATE block of `LOG.md`; open work is the OPEN table of `TASKS.md`.

## P6 — Strangler fig (only where it pays)
Replace lifted modules with clean 64-bit C++ in `src/native/`, in priority order: (1) renderer front-end / draw submission (cut D3D9-shaped overhead, batch for Metal), (2) memory allocator, (3) file system + fastfile loading, (4) sound, (5) UI/menus, (6) whatever profiling shows is hot. Each replacement keeps diff-test + golden-frame suites green. Stop when perf and maintainability goals are met — full purity is optional.
**Status:** started 2026-10-10 with item 1. Its host half is done (T-063: the path from the game's Direct3D calls to Metal); its guest half — the game's own lifted submission functions — is T-064. Open work: the OPEN table of `TASKS.md`.

## P7 — Metal optimization
Profile with Instruments + Metal frame capture. Candidates: PSO + shader library cache at startup, argument buffers / fewer encoder switches, persistent mapped buffers (unified memory), triple buffering + frame pacing (ProMotion), tile memory where passes allow, MetalFX upscaling option, async texture streaming, fewer CPU->GPU copies.

## P8 — QA & polish
Save/load, controller mapping, resolution/fullscreen/Spaces, display scaling, audio device changes, soak tests, crash logs. Then evaluate Zombies, then MP.
