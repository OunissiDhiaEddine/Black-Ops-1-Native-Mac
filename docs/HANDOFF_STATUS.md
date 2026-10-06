# Hand-over — BO1 native arm64 + Metal port

Written 2026-10-06 for the next agent. This is a summary; the authoritative files are `AGENTS.md`
(rules, session protocol), `LOG.md` (STATE block = resume point, then the session log; entries of
P0–P4 are in `docs/LOG_ARCHIVE.md`), `TASKS.md` (NOW + one OPEN table, then the record of finished work),
`docs/DECISIONS.md` and `docs/RE_NOTES.md`. If this file and those disagree, they win.

## Where the project stands
The game runs as a native arm64 process with a Metal renderer and no Wine:
title screen -> main menu -> the first campaign mission (`cuba`) loads and its opening scene renders.
Evidence: `artifacts/boot/native_title_screen.png`, `native_main_menu.png`, `native_mission_cuba.png`;
10-minute soak `artifacts/boot/soak_run.log` (38,000 frames, no error).

| Phase | State |
|---|---|
| P0–P3 recon, baseline, shim design, renderer in isolation | done |
| P4 static recompilation | done — exit criteria met 2026-10-06 |
| P5 integration | in progress: boots, renders, plays its videos, loads a mission; no sound |
| P6–P8 | not started |

## Run it
```
scripts/run_native.sh                    # window; Ctrl-C in the terminal to stop
scripts/boot_try.sh 60 40                # headless test run, tail of artifacts/boot/last_run.log
BO1_KEYS="35000:0x20" BO1_CMDS="50000|devmap cuba" BO1_D3D_DUMP=artifacts/boot/frames \
    artifacts/boot/bo1_boot --seconds 170   # unattended: key at 35 s, mission at 50 s, PNG every 20th frame
```
The first ~25 s are the intro movie (video works since 2026-10-06; `BO1_BINK=off` skips all movies). With it the scripted
key is best sent at 45 s and the mission command at 60 s (`BO1_KEYS="45000:0x20" BO1_CMDS="60000|devmap cuba"`, verified).
`last_run.log` contains binary bytes: use `grep -a`.

## Build
- `scripts/build.sh` — CMake/Ninja: translator, Metal renderer library, window library, replay tool, unit tests.
- `scripts/build_boot.sh` — the runtime `artifacts/boot/bo1_boot` (not in CMake: it links the lifted
  objects in `artifacts/lifted/obj`, which come from `scripts/difftest.sh` / the lifter).
- The lifted C code is generated (`tools/lifter/`), lives in `artifacts/lifted/` and is never edited or committed.

## How it fits together
```
BlackOps.exe --tools/lifter--> artifacts/lifted/*.c --clang--> arm64 objects
        |                                                         |
        +-- loaded as data into a 4 GiB guest arena --------------+--> artifacts/boot/bo1_boot
src/runtime/guest_os.cpp     arena, heap, virtual memory, threads/TEB/TLS, import trampolines, COM objects
src/platform/win32/*.cpp     kernel32, user32/gdi32, winsock, misc DLLs
   bink_ffmpeg.cpp           Bink video on FFmpeg's decoder (ADR-008); contract in RE_NOTES "Bink as the game uses it"
   steam_local.cpp           Steam: local offline stand-in (ADR-015), table re/steam_abi.csv
   d3d9_null.cpp             guest-visible Direct3D objects and their lifetime, lock memory, capability answers
   d3d9_live.cpp             watches every Direct3D call and feeds it to the renderer as a call record
src/rhi + src/metal          renderer: Direct3D 9 call records in, Metal out (same code the trace replay uses)
src/shaders/sm3_msl.cpp      SM3 bytecode -> MSL
src/platform/window          SDL3 window on the process main thread, SDL events -> Win32 messages
tests/boot/main.cpp          entry point of bo1_boot; tests/boot/steam_selftest.cpp
```

## Regression set (all green on 2026-10-06)
| Command | Expected |
|---|---|
| `artifacts/boot/bo1_boot --steam-selftest` | `steam selftest: 76 checks, 0 failed, 24 interface methods` |
| `scripts/replay_check.sh` | worst SSIM cuba 0.9994, khe_sanh 0.9984, menu 0.9997, vorkuta 0.9985; no "unrecorded" lines |
| `build/test_win_messages`, `build/test_window_service` | `all checks passed` |
| `scripts/boot_try.sh 90` | `time limit reached (still running …)` |
| `scripts/difftest.sh 100000 2 5 all` (~6 min; lifter untouched since 2026-10-06 06:10) | 0 unexplained failures |

## What is missing, in the order I would do it
1. ~~T-054 Bink video~~ — done 2026-10-06 (video only; the movies' sound tracks come with T-053).
2. **T-053 sound** (ADR-009: XAudio2 2.7 on FAudio). `brew install faudio` was approved by the user on 2026-10-06 but has to be
   run by the user (the agent session's permission check refused the install) — check `brew list --versions faudio`.
3. **T-057 input with a human**: the window, keyboard and mouse position are wired, but only scripted
   keys were verified. Mouse look (relative mode), key repeat, focus changes, gamepad (the XInput shim is a stub).
4. **T-058 performance**: nothing is measured on the live path. The renderer re-uploads constants per draw
   and resolves hazards by flushing — it was written for correctness.
5. ~~T-059 Direct3D object lifetime~~ — done 2026-10-06: objects and their guest memory are freed when Direct3D would destroy them;
   `Reset` is implemented and forwarded but has never been called by the game yet (display-mode changes: T-061).
6. **`Com_Error` recovery** (ADR-012): the error path jumps to a point inside `Com_Init` (0x004069D5 / 0x00406A91)
   that the lifter does not know as an entry — any dropped error ends the process today.
7. Saves, display modes, the rest of the campaign: untested.

## Decisions a newcomer must know (docs/DECISIONS.md)
- **Steam = ADR-015**, user decision of 2026-10-06: a local, offline stand-in inside the runtime. It reports no
  DLC on purpose. Unknown Steam methods trap with their slot number: add them only with evidence from the
  game's call site (`tools/steam_sites.py`). The shipped `steam_api.dll` is not loaded, modified or analysed.
- The reference copy in `reference/` is read-only and is never committed; neither are `artifacts/`.
- The Wine baseline (ADR-007) exists only as an oracle: `scripts/run_baseline.sh`, golden frames in
  `tests/golden/` (PNG files are git-ignored), traces in `artifacts/traces/`.
- Ask the user before: system-wide installs, downloads > 500 MB, `git push`, changing an ADR.

## Things that cost time once (do not repeat)
- **Apple Silicon host pages are 16 KiB**, the guest's are 4 KiB. Anything that maps or protects memory per
  page must handle partial host pages (`vm_zero()` in `guest_os.cpp`).
- **Do not edit a shell script while it is running** (bash reads scripts incrementally; a capture run was lost).
- **Traces only contain what the proxy recorded.** The replay prints "draw reads an unrecorded …" when a
  trace lacks data; trust that line before suspecting the renderer.
- **Python text mode rewrites CRLF files** (`re/platform_map.csv`): edit such files in binary mode.
- **Finding a runtime bug:** compare the game's own debug strings in `last_run.log` with a baseline
  `artifacts/baseline/run-*.log` — they are identical up to the first fault. Then lldb on `bo1_boot`:
  break on `f_<VA>`, `x0` is the `Cpu *`, the global `g_mem` is the arena base.

## Repository state at hand-over
One branch (`main`), no worktrees, no stashes, working tree clean, no remote configured.
Open questions for the user are listed in the STATE block of `LOG.md`.
