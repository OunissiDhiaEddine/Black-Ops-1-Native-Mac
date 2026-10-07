# CLAUDE.md — BO1 → native Apple Silicon (Metal + arm64)

> The single entry point for any coding agent. Read it fully at the start of EVERY session.
> `AGENTS.md` is only a pointer to this file. The rule numbers below are cited by older log entries and ADRs — keep them stable.

## Mission
Run Call of Duty: Black Ops (2010, Treyarch "T5" engine, PE32 / x86, Direct3D 9) as a **native arm64 Mach-O app on Apple Silicon with a Metal renderer and no Wine at runtime**.
First target: **Singleplayer campaign** (`BlackOps.exe`). Zombies / MP come later (see docs/DECISIONS.md ADR-001).

Success = campaign boots to main menu, loads a mission, plays at >= 60 fps on an M-series Mac, controller + KB/M work, saves work.

## Where things are written down
| File | Holds | Changes |
|---|---|---|
| `CLAUDE.md` | rules, session protocol, repo map (this file) | rarely |
| `LOG.md` | STATE block (the resume point, overwritten) + append-only session log | after every task |
| `TASKS.md` | NOW, the OPEN table, then the record of finished work with its evidence | after every task |
| `docs/RUNBOOK.md` | how to run, build and regression-test; switches; architecture map; debugging; pitfalls | when a command or expected result changes |
| `docs/DECISIONS.md` | architecture decision records (ADR-xxx) | only with the user's OK |
| `docs/RE_NOTES.md` | reverse-engineering knowledge base (structs, functions, middleware), confidence-tagged | whenever something is learned |
| `docs/PLAN.md` | phases, approach, exit criteria, risks | at phase boundaries |
| `docs/HOST_ABI.md`, `docs/LIFTER.md`, `docs/TRACE_FORMAT.md` | design references for the guest/host ABI, the lifter and the D3D9 trace | with the code they describe |
| `docs/LOG_ARCHIVE.md` | session-log entries of P0–P4, moved out of `LOG.md` unchanged | never |

One fact lives in one place. Status goes in `LOG.md` / `TASKS.md`, commands in `docs/RUNBOOK.md`, knowledge in `docs/RE_NOTES.md`; the others link to it instead of repeating it.

## Ground rules (non-negotiable)
1. **Scope.** Personal-use interoperability research on the owner's installed copy. Do NOT commit or distribute game binaries, assets, or raw decompiler output. `reference/` and `artifacts/` are gitignored; keep it that way.
2. **Checkpoint discipline.** The user can run out of tokens at any moment. Update `LOG.md` (STATE block + new entry) and `git commit` after EVERY completed task, every significant tool result, and at least every ~20 minutes of work. Never batch up logging for "the end".
3. **Small verifiable steps.** One task at a time from `TASKS.md`. Every task has an "Evidence" line — don't mark DONE without it (command output, screenshot path, test result).
4. **Never guess silently.** Unknown address/struct/behaviour -> write it in `docs/RE_NOTES.md` with a confidence tag (`CONFIRMED` / `LIKELY` / `GUESS`) and how it was verified.
5. **Reproducible.** Every analysis/build step is a script in `scripts/` or `tools/`. No "I ran this once in a terminal" results.
6. **Originals are read-only.** Never modify files in `reference/`. Work on copies in `artifacts/`.
7. **Ask the user before:** installing system-wide software, downloads > 500 MB, touching anything outside this directory, `git push`, or changing a decision in `docs/DECISIONS.md`.

## Session protocol
**Start:** read `LOG.md` STATE block + last 3 entries -> `TASKS.md` NOW section -> state in one sentence what you'll do -> do it. Look up commands in `docs/RUNBOOK.md` when you need them.
**During:** follow rule 2. Keep `TASKS.md` statuses current.
**Low budget / long step ahead / user says "wrap up":** overwrite STATE block with: exact next command to run, files in flight, open questions, anything half-done and how to resume it. Then commit.
**Finish a phase:** check its exit criteria in `docs/PLAN.md`, record the result in `LOG.md`, then ask the user before starting the next phase.

**What goes in the STATE block:** only what changes from checkpoint to checkpoint — date/time, phase and task, what works, what is missing, the next exact step, whether the regression set is green, files in flight, open questions for the user. Commands, switches and machine facts belong in `docs/RUNBOOK.md`.

## Working in parallel threads
- The work runs as a Claude project: one thread per piece of work. The user allows git worktrees, one branch per task.
- A thread in a worktree (`.claude/worktrees/<name>`, branch `claude/<name>`) has its own `build/` and `artifacts/boot/` and merges into `main` when its task is done; the thread in the main folder commits on `main` directly.
- `LOG.md` and `TASKS.md` are edited by every thread: re-read them right before editing, keep edits small, append log entries with the real wall-clock time (`date`), and merge `main` before finishing.
- One 8 GB machine: do not overlap long builds or runs more than necessary, and never play sound or open a window on the user's desktop unasked (unattended runs: `SDL_AUDIO_DRIVER=dummy`).

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
