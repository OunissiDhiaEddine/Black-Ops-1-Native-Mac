# CLAUDE.md — BO1 → native Apple Silicon (Metal + arm64)

> The single entry point for any coding agent. `AGENTS.md` is only a pointer to this file.
> The rule numbers below are cited by older log entries and ADRs — keep them stable (3 and 5 were retired on 2026-10-07).

## Mission
Run Call of Duty: Black Ops (2010, Treyarch "T5" engine, PE32 / x86, Direct3D 9) as a **native arm64 Mach-O app on Apple Silicon with a Metal renderer and no Wine at runtime**.
First target: **Singleplayer campaign** (`BlackOps.exe`). Zombies / MP come later (see docs/DECISIONS.md ADR-001).

Success = campaign boots to main menu, loads a mission, plays at >= 60 fps on an M-series Mac, controller + KB/M work, saves work.

## Where things are written down
| File | Holds | Changes |
|---|---|---|
| `CLAUDE.md` | rules, repo map (this file) | rarely |
| `LOG.md` | STATE block (the resume point, overwritten) + append-only session log | after every task |
| `TASKS.md` | NOW, the OPEN table, then the record of finished work with its evidence | after every task |
| `docs/RUNBOOK.md` | how to run, build and regression-test; switches; architecture map; debugging; pitfalls | when a command or expected result changes |
| `docs/DECISIONS.md` | architecture decision records (ADR-xxx) | only with the user's OK |
| `docs/RE_NOTES.md` | reverse-engineering knowledge base (structs, functions, middleware), confidence-tagged | whenever something is learned |
| `docs/PLAN.md` | phases, approach, exit criteria, risks | at phase boundaries |
| `docs/HOST_ABI.md`, `docs/LIFTER.md`, `docs/TRACE_FORMAT.md` | design references for the guest/host ABI, the lifter and the D3D9 trace | with the code they describe |
| `docs/LOG_ARCHIVE.md` | session-log entries of P0–P4, moved out of `LOG.md` unchanged | never |

One fact lives in one place. Status goes in `LOG.md` / `TASKS.md`, commands in `docs/RUNBOOK.md`, knowledge in `docs/RE_NOTES.md`; the others link to it instead of repeating it.

## Ground rules
1. **Scope.** Personal-use interoperability research on the owner's installed copy. Do NOT commit or distribute game binaries, assets, lifted code, or raw decompiler output. `reference/`, `artifacts/` and `src/lifted/` are gitignored; keep it that way.
2. **Log after every task.** The user can run out of tokens at any moment. After EVERY completed task: update `LOG.md` (STATE block + new entry) and `TASKS.md` (status + evidence: command output, screenshot path, test result), then `git commit`. Never batch up logging for "the end".
4. **Never guess silently.** Unknown address/struct/behaviour -> write it in `docs/RE_NOTES.md` with a confidence tag (`CONFIRMED` / `LIKELY` / `GUESS`) and how it was verified.
6. **Originals are read-only.** Never modify files in `reference/`. Work on copies in `artifacts/`.
7. **Ask the user before:** installing system-wide software, downloads > 500 MB, changing files outside the repository, `git push`, or changing a decision in `docs/DECISIONS.md`.

Everything else is yours to decide: which open task to take, the approach, names, when to build or run, merging your branch, cleaning up after yourself. Pick the reasonable option, note it in the log, and keep going.

## Resuming
`LOG.md`'s STATE block is the resume point and `TASKS.md` lists what is open; read what your task needs. Commands are in `docs/RUNBOOK.md`.
The STATE block holds only what changes from checkpoint to checkpoint: date/time, phase and task, what works, what is missing, the next exact step, whether the regression set is green, files in flight, open questions for the user.

## Working in parallel threads
- One thread per piece of work, each in its own worktree (`.claude/worktrees/<name>`, branch `claude/<name>`) with its own `build/` and `artifacts/boot/`; the thread in the main folder commits on `main` directly.
- When its task is done, a thread merges `main` into its branch, merges the branch into `main`, then removes its own worktree and branch (without force).
- `LOG.md` and `TASKS.md` are edited by every thread: re-read them right before editing, keep edits small, and append log entries with the real wall-clock time (`date`).
- One 8 GB machine: do not overlap long builds or runs, and never play sound or open a window on the user's desktop unasked (unattended runs: `SDL_AUDIO_DRIVER=dummy`).

## Settled by the user (do not reopen without asking)
- **Steam = ADR-015** (2026-10-06): a local, offline stand-in inside the runtime. It reports no DLC on purpose. Unknown Steam methods trap with their slot number: add them only with evidence from the game's call site (`tools/steam_sites.py`). The shipped `steam_api.dll` is not loaded, modified or analysed. The user will not install the Steam client.
- **Wine is only the oracle** (ADR-007): `scripts/run_baseline.sh`, golden frames in `tests/golden/`, traces in `artifacts/traces/`. The user's Windows PC is a last resort.
- **Installs:** an agent session cannot run `brew install` itself — give the user the command, then verify with `brew list --versions <name>`.

## Repo map
```
CLAUDE.md                 this file (AGENTS.md points here)
LOG.md                    STATE block (overwritten) + append-only session log  <- resume point
TASKS.md                  backlog with IDs, status, evidence
docs/                     RUNBOOK, DECISIONS, RE_NOTES, PLAN, HOST_ABI, LIFTER, TRACE_FORMAT, LOG_ARCHIVE (table above)
re/                       symbols.csv (single source of truth), Ghidra projects/scripts exports
tools/                    analysis tooling (python, ghidra scripts, d3d9 proxy logger, lifter)
src/runtime/              guest memory model, CPU-state helpers, lifted-code support
src/platform/             Win32/DirectX/Steam-API replacement shims (SDL3 / AppKit / CoreAudio)
src/rhi/                  minimal renderer interface (what the game actually needs from D3D9)
src/metal/                Metal backend for rhi + shader cache
src/shaders/              SM3 -> MSL translation + hand-written MSL
src/lifted/               GENERATED code from the lifter (never hand-edit; regenerate)
src/native/               hand-written 64-bit replacements for lifted modules (strangler fig)
tests/golden/             reference frames + metrics from the Wine/DXVK baseline (gitignored if large)
tests/diff/               per-function differential tests (original x86 vs lifted)
tests/replay/             D3D9 trace replay harness for renderer development
tests/window/             unit tests of the SDL3 window layer (events -> Win32 messages, window service)
tests/boot/               the native runtime's entry point (bo1_boot) + Steam self-test; built by scripts/build_boot.sh
reference/                USER'S ORIGINAL GAME FILES (gitignored, read-only)
artifacts/                build outputs, traces, captures (gitignored)
scripts/                  bootstrap.sh, build.sh, run_baseline.sh, ...; run_native.sh = play the native build in a window
```

## Conventions
- C++20 for runtime/shim/rhi; Objective-C++/Swift only in the platform + Metal glue; Python 3 for tooling; MSL for shaders.
- Build: CMake + Ninja, Apple clang. Entry: `scripts/build.sh`. Warnings as errors in `src/native` and `src/rhi`.
- Address naming: original virtual addresses in hex in comments (`// VA 0x004A1B20`). All names live in `re/symbols.csv` (`addr,name,kind,confidence,notes`); headers are generated from it.
- Guest pointers in lifted code are `uint32_t`; host pointers are real pointers. Never mix them without the `guest_to_host()` / `host_to_guest()` helpers.
- Commit messages: `P<phase>/<TASK-ID>: <what> [evidence: ...]`.

## Toolbox (check what's installed with scripts/doctor.sh before assuming)
Ghidra (headless + scripts), Python: pefile, capstone, lief, unicorn; Frida; Xcode Metal frame capture + Instruments; CrossOver or Wine (wine32on64) + DXVK/MoltenVK for the *reference baseline only*; FFmpeg (Bink video/audio decoding).
