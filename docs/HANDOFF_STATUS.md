# Project status — BO1 native arm64 + Metal port

Orientation page for anyone (any thread) starting work. Last brought up to date: 2026-10-06 23:40.
It began as a hand-over note between agents; it is now kept current, but it is still a SUMMARY.
What changes daily lives elsewhere and wins when this page disagrees:
`LOG.md` (STATE block = resume point, then the session log; P0–P4 entries are in `docs/LOG_ARCHIVE.md`),
`TASKS.md` (NOW + one OPEN table, then the record of finished work), `docs/DECISIONS.md`, `docs/RE_NOTES.md`.
Rules and session protocol: `AGENTS.md`.

## Where the project stands
The game runs as a native arm64 process with a Metal renderer and no Wine, and the user has played it:
intro movie -> title screen -> main menu -> the first campaign mission (`cuba`) loads and plays, with video and sound.

| Phase | State |
|---|---|
| P0–P3 recon, baseline, shim design, renderer in isolation | done |
| P4 static recompilation | done — exit criteria met 2026-10-06 |
| P5 integration | in progress — see the table below |
| P6–P8 strangler fig, Metal optimisation, QA | not started (ask the user before starting a phase) |

P5 in one table (IDs are those of `TASKS.md`, where the evidence is):

| Area | State on 2026-10-06 |
|---|---|
| Boot, file system, fastfiles, threads, Steam stand-in | done (T-045, T-024) |
| Window, keyboard | done; keyboard confirmed by the user |
| Rendering live through Metal | done (T-055); Direct3D objects are freed (T-059) |
| Video (Bink on FFmpeg) | done (T-054): intro, menu screens, loading movies |
| Sound (XAudio2 on FAudio) | done (T-053), movies included; the user: "sound is great" |
| Mouse look | fixed in code (T-057), waiting for the user to confirm by hand; a real mouse, gamepad, key repeat untested |
| Frame rate | nothing measured yet (T-058) |
| Saves, display modes, rest of the campaign | being checked (T-061) |
| `Com_Error` recovery | open (T-060): any dropped error still ends the process |

## Run it
```
scripts/run_native.sh                    # window + sound; Ctrl-C in the terminal to stop
scripts/boot_try.sh 60 40                # headless, silent test run; tail of artifacts/boot/last_run.log
SDL_AUDIO_DRIVER=dummy BO1_KEYS="45000:0x20" BO1_CMDS="60000|devmap cuba" BO1_D3D_DUMP=artifacts/boot/frames \
    artifacts/boot/bo1_boot --seconds 170   # unattended into mission 1: key at 45 s, mission at 60 s, PNG every 20th frame
```
- The first ~25 s are the intro movie, hence the key at 45 s.
- **Unattended runs must be silent**: `SDL_AUDIO_DRIVER=dummy` (mixes without output; `boot_try.sh` sets it) or
  `SDL_AUDIO_DRIVER=disk SDL_AUDIO_DISK_OUTPUT_FILE=file.raw` to capture the mix (float32 stereo 48 kHz).
- Headless runs never reach the game's mouse code (it needs the foreground window) — mouse work needs the window and a hand.
- Switches: `BO1_WINDOW=1` window (run_native.sh sets it) · `BO1_D3D=null` no rendering · `BO1_D3D_DUMP=dir` ·
  `BO1_KEYS="ms:vk,…"` · `BO1_CMDS="ms|command;…"` · `BO1_BINK=off` no movies · `BO1_AUDIO=off` no sound ·
  `BO1_VERBOSE=0..3` · `BO1_IMPORT_STATS=1` call counts at exit · `BO1_STEAM_ABSENT=1` · `BO1_EMULATED_STEAM_DLL=1`.
- Logs contain binary bytes: read them with `grep -a`.

## Build
- `scripts/build.sh` — CMake/Ninja: shader translator, Metal renderer library, window library, replay tool, unit tests.
- `scripts/build_boot.sh` — the runtime `artifacts/boot/bo1_boot` (not in CMake: it links the lifted objects in
  `artifacts/lifted/obj`, which come from `scripts/difftest.sh` / the lifter). Links Homebrew's SDL3, FFmpeg
  (libavformat / libavcodec / libavutil) and FAudio.
- The lifted C code is generated (`tools/lifter/`), lives in `artifacts/lifted/` and is never edited or committed.
- Homebrew packages in use: cmake, ninja, ffmpeg, sdl3, faudio, mingw-w64, ghidra, openjdk@21, sevenzip. An agent
  session cannot run `brew install` itself: give the user the command.

## How it fits together
```
BlackOps.exe --tools/lifter--> artifacts/lifted/*.c --clang--> arm64 objects
        |                                                         |
        +-- loaded as data into a 4 GiB guest arena --------------+--> artifacts/boot/bo1_boot
src/runtime/guest_os.cpp     arena, heap, virtual memory, threads/TEB/TLS, import trampolines, COM objects
src/platform/win32/*.cpp     kernel32, user32/gdi32, winsock, misc DLLs
   bink_ffmpeg.cpp           Bink video and sound tracks on FFmpeg (ADR-008); RE_NOTES "Bink as the game uses it"
   xaudio2_faudio.cpp        XAudio2 2.7 on FAudio (ADR-009): voices, the game's own effects and callbacks on a
                             mixer thread with guest state, xWMA via FFmpeg; RE_NOTES "XAudio2 as the game uses it"
   steam_local.cpp           Steam: local offline stand-in (ADR-015), table re/steam_abi.csv
   d3d9_null.cpp             guest-visible Direct3D objects and their lifetime, lock memory, capability answers
   d3d9_live.cpp             watches every Direct3D call and feeds it to the renderer as a call record
   user32_headless.cpp       windows, messages, cursor (despite the name it also serves the real window)
src/rhi + src/metal          renderer: Direct3D 9 call records in, Metal out (same code the trace replay uses)
src/shaders/sm3_msl.cpp      SM3 bytecode -> MSL
src/platform/window          SDL3 window on the process main thread, SDL events -> Win32 messages, virtual cursor /
                             relative mouse mode for mouse look; RE_NOTES "Mouse as the game uses it"
tests/boot/main.cpp          entry point of bo1_boot; tests/boot/steam_selftest.cpp
```

## Regression set
| Command | Expected |
|---|---|
| `artifacts/boot/bo1_boot --steam-selftest` | `steam selftest: 76 checks, 0 failed, 24 interface methods` |
| `build/test_win_messages`, `build/test_window_service` | `all checks passed` |
| `scripts/boot_try.sh 90` | `time limit reached (still running …)` |
| the unattended run above | three `BinkOpen` / `BinkClose` pairs with sound-track sample counts and 0 dropped, `xaudio2: first xWMA voice … decoded with FFmpeg`, no `Com_ERROR`, no "host signal" |
| `scripts/replay_check.sh` (renderer changes) | worst SSIM cuba 0.9994, khe_sanh 0.9984, menu 0.9997, vorkuta 0.9985; no "unrecorded" lines |
| `scripts/difftest.sh 100000 2 5 all` (lifter changes; ~6 min) | 0 unexplained failures |

## Working in parallel threads
- The work runs as a Claude project: one thread per piece of work. The user allows git worktrees, one branch per task.
- A thread in a worktree (`.claude/worktrees/<name>`, branch `claude/<name>`) has its own `build/` and `artifacts/boot/`
  and merges into `main` when its task is done; the thread in the main folder commits on `main` directly.
- `LOG.md` and `TASKS.md` are edited by every thread: re-read them right before editing, keep edits small, append
  log entries with the real wall-clock time (`date`), and merge `main` before finishing.
- One 8 GB machine: do not overlap long builds or runs more than necessary, and never play sound or open a window
  on the user's desktop unasked.

## Decisions a newcomer must know (docs/DECISIONS.md)
- **Steam = ADR-015**, user decision of 2026-10-06: a local, offline stand-in inside the runtime. It reports no
  DLC on purpose. Unknown Steam methods trap with their slot number: add them only with evidence from the
  game's call site (`tools/steam_sites.py`). The shipped `steam_api.dll` is not loaded, modified or analysed.
  The user will not install the Steam client.
- The reference copy in `reference/` is read-only and is never committed; neither are `artifacts/`.
- The Wine baseline (ADR-007) exists only as an oracle: `scripts/run_baseline.sh`, golden frames in
  `tests/golden/` (PNG files are git-ignored), traces in `artifacts/traces/`.
- Ask the user before: system-wide installs, downloads > 500 MB, `git push`, changing an ADR, starting a new phase.

## Things that cost time once (do not repeat)
- **The sound engine must run at 48 kHz**: the game's effects write fixed 480-frame blocks; at 44.1 kHz they overwrite
  game variables (crash in the next sound update). `xaudio2_faudio.cpp` forces it — do not "fix" that to the device rate.
- **`SetCursorPos` is not optional**: the game re-centres the cursor every frame in mouse look and measures from there.
- **Homebrew's FAudio has no WMA decoder** and calls a null function for xWMA voices; the port decodes them with FFmpeg.
- **Apple Silicon host pages are 16 KiB**, the guest's are 4 KiB. Anything that maps or protects memory per
  page must handle partial host pages (`vm_zero()` in `guest_os.cpp`).
- **Do not edit a shell script while it is running** (bash reads scripts incrementally; a capture run was lost).
- **Traces only contain what the proxy recorded.** The replay prints "draw reads an unrecorded …" when a
  trace lacks data; trust that line before suspecting the renderer.
- **CSV files under `re/` have Windows line endings** (`platform_map.csv`, `symbols.csv`): edit them in binary mode.
- **Finding a runtime bug:** compare the game's own debug strings in the run log with a baseline
  `artifacts/baseline/run-*.log` — they are identical up to the first fault. Then lldb on `bo1_boot`:
  break on `f_<VA>`, `x0` is the `Cpu *`, the global `g_mem` is the arena base; a write watchpoint on
  `g_mem + <guest address>` finds whoever clobbers a game variable.
