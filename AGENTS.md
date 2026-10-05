# AGENTS.md — BO1 → native Apple Silicon (Metal + arm64)

> Entry point for any coding agent (Claude Code etc.). Read this fully at the start of EVERY session.
> `CLAUDE.md` just imports this file.

## Mission
Run Call of Duty: Black Ops (2010, Treyarch "T5" engine, PE32 / x86, Direct3D 9) as a **native arm64 Mach-O app on Apple Silicon with a Metal renderer and no Wine at runtime**.
First target: **Singleplayer campaign** (`BlackOps.exe`). Zombies / MP come later (see docs/DECISIONS.md ADR-001).

Success = campaign boots to main menu, loads a mission, plays at >= 60 fps on an M-series Mac, controller + KB/M work, saves work.

## Ground rules (non-negotiable)
1. **Scope.** Personal-use interoperability research on the owner's installed copy. Do NOT do DRM / anti-tamper / online-auth circumvention work (the user states the binary is already unprotected — if you find it isn't, mark the task BLOCKED and tell the user, don't write unpackers/cracks). Do NOT commit or distribute game binaries, assets, or raw decompiler output. `reference/` and `artifacts/` are gitignored; keep it that way.
2. **Checkpoint discipline.** The user can run out of tokens at any moment. Update `LOG.md` (STATE block + new entry) and `git commit` after EVERY completed task, every significant tool result, and at least every ~20 minutes of work. Never batch up logging for "the end".
3. **Small verifiable steps.** One task at a time from `TASKS.md`. Every task has an "Evidence" line — don't mark DONE without it (command output, screenshot path, test result).
4. **Never guess silently.** Unknown address/struct/behaviour -> write it in `docs/RE_NOTES.md` with a confidence tag (`CONFIRMED` / `LIKELY` / `GUESS`) and how it was verified.
5. **Reproducible.** Every analysis/build step is a script in `scripts/` or `tools/`. No "I ran this once in a terminal" results.
6. **Originals are read-only.** Never modify files in `reference/`. Work on copies in `artifacts/`.
7. **Ask the user before:** installing system-wide software, downloads > 500 MB, touching anything outside this directory, `git push`, or changing a decision in `docs/DECISIONS.md`.

## Session protocol
**Start:** read `LOG.md` STATE block + last 3 entries -> `TASKS.md` NOW section -> state in one sentence what you'll do -> do it.
**During:** follow rule 2. Keep `TASKS.md` statuses current.
**Low budget / long step ahead / user says "wrap up":** overwrite STATE block with: exact next command to run, files in flight, open questions, anything half-done and how to resume it. Then commit.
**Finish a phase:** check its exit criteria in `docs/PLAN.md`, record the result in `LOG.md`, then ask the user before starting the next phase.

## Repo map
```
AGENTS.md / CLAUDE.md     this file
LOG.md                    STATE block (overwritten) + append-only session log  <- resume point
TASKS.md                  backlog with IDs, status, evidence
docs/PLAN.md              phases, approach, exit criteria, risks
docs/DECISIONS.md         architecture decision records (ADR-xxx)
docs/RE_NOTES.md          reverse-engineering knowledge base (structs, functions, middleware)
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
reference/                USER'S ORIGINAL GAME FILES (gitignored, read-only)
artifacts/                build outputs, traces, captures (gitignored)
scripts/                  bootstrap.sh, build.sh, run_baseline.sh, ...
```

## Conventions
- C++20 for runtime/shim/rhi; Objective-C++/Swift only in the platform + Metal glue; Python 3 for tooling; MSL for shaders.
- Build: CMake + Ninja, Apple clang. Entry: `scripts/build.sh`. Warnings as errors in `src/native` and `src/rhi`.
- Address naming: original virtual addresses in hex in comments (`// VA 0x004A1B20`). All names live in `re/symbols.csv` (`addr,name,kind,confidence,notes`); headers are generated from it.
- Guest pointers in lifted code are `uint32_t`; host pointers are real pointers. Never mix them without the `guest_to_host()` / `host_to_guest()` helpers.
- Commit messages: `P<phase>/<TASK-ID>: <what> [evidence: ...]`.

## Toolbox (check what's installed with scripts/doctor.sh before assuming)
Ghidra (headless + scripts), Python: pefile, capstone, lief, unicorn; Frida; Xcode Metal frame capture + Instruments; CrossOver or Wine (wine32on64) + DXVK/MoltenVK for the *reference baseline only*; FFmpeg (Bink video/audio decoding).
