# RUNBOOK — run, build and regression-test the native build

Reference page: commands, switches, expected results, the architecture map and the pitfalls. It holds no status.
What is done, what is open and the next step are in `LOG.md` (STATE block) and `TASKS.md`; rules are in `CLAUDE.md`.
Change this page in the same commit that changes a command, a switch or an expected result.

## Run it
```
scripts/run_native.sh                    # window + sound; Ctrl-C in the terminal to stop; log artifacts/boot/native_run.log
scripts/boot_try.sh 60 40                # builds, then a headless, silent test run; tail of artifacts/boot/last_run.log
SDL_AUDIO_DRIVER=dummy BO1_KEYS="45000:0x20" BO1_CMDS="60000|devmap cuba" BO1_D3D_DUMP=artifacts/boot/frames \
    artifacts/boot/bo1_boot --seconds 170   # unattended into mission 1: key at 45 s, mission at 60 s, PNG every 20th frame
```
- The first ~25 s are the intro movie, hence the key at 45 s.
- **Unattended runs must be silent**: `SDL_AUDIO_DRIVER=dummy` (mixes without output; `boot_try.sh` sets it) or
  `SDL_AUDIO_DRIVER=disk SDL_AUDIO_DISK_OUTPUT_FILE=file.raw` to capture the mix (float32 stereo 48 kHz).
- Headless runs never reach the game's mouse code (it needs the foreground window) — mouse work needs the window and a hand.
- Logs contain binary bytes: read them with `grep -a`.
- `artifacts/boot/bo1_boot [--exe PATH] [--seconds N] [--steam-selftest]`; `--seconds 0` = no limit.

### Switches (environment)
| Switch | Effect |
|---|---|
| `BO1_WINDOW=1` | real SDL3 window on the process main thread (`run_native.sh` sets it) |
| `BO1_D3D=null` | no rendering |
| `BO1_D3D_DUMP=dir` | PNG of every 20th frame |
| `BO1_KEYS="ms:vk,…"` | scripted key presses (ms after start : Windows virtual-key code) |
| `BO1_CMDS="ms\|command;…"` | scripted console commands, e.g. `60000\|devmap cuba` |
| `BO1_ARGS="…"` | extra arguments appended to the game's command line |
| `BO1_BINK=off` / `BO1_AUDIO=off` | no movies / no sound |
| `BO1_VERBOSE=0..3` | shim and game debug output in the log |
| `BO1_IMPORT_STATS=1` | import call counts at exit |
| `BO1_STEAM_ABSENT=1` | diagnostics: the stand-in answers "Steam is not running" |
| `BO1_EMULATED_STEAM_DLL=1` | the superseded ADR-010 path (shipped DLL in the x86 interpreter; it stops itself) |
| `BO1_NO_NET=1` | sockets exist but carry no traffic |
| `BO1_CPUS=n` | processor count reported to the game |
| `BO1_GAME_DIR`, `BO1_HOME_DIR` | the guest's `C:\bo1` (default `artifacts/game`) and home (default `artifacts/boot/home`) |
| `BO1_FRAME_STATS=N` | one "frame stats:" line per N frames (300 for `1`): fps, frame time avg / p50 / p95 / p99 / max, time inside the renderer and its share, Direct3D calls per frame |
| `BO1_MOUSE_LOG=file` | window only: every mouse motion event (`ms motion xrel yrel x y relative`), every `SetCursorPos` (`ms warp x y`) and every switch of relative mouse mode; the run log gets one "mouse look: relative mouse mode on / off …" line per switch regardless |
| `SDL_MOUSE_RELATIVE_SPEED_SCALE=f`, `SDL_MOUSE_RELATIVE_SYSTEM_SCALE=1` | SDL's own switches for mouse look: a speed factor, and the system acceleration curve (default: unscaled) |
| `BO1_DEBUG_FRAME=N BO1_DEBUG_DIR=dir` | renderer dumps every pass of frame N; `tools/pass_dump.py dir` turns it into PNGs |

## Build
- `scripts/build.sh` — CMake/Ninja: shader translator, Metal renderer library, window library, replay tool, unit tests.
- `scripts/build_boot.sh` — the runtime `artifacts/boot/bo1_boot` (not in CMake: it links the lifted objects in
  `artifacts/lifted/obj`, which come from `scripts/difftest.sh` / the lifter). Links Homebrew's SDL3, FFmpeg
  (libavformat / libavcodec / libavutil) and FAudio.
- The lifted C code is generated (`tools/lifter/`), lives in `artifacts/lifted/` and is never edited or committed.
- `scripts/doctor.sh` reports which tools are present; `scripts/setup_venv.sh` creates `.venv`.

### Machine and toolchain (as of 2026-10-06)
- Apple M1 (arm64, **16 KiB pages**), 8 GB RAM, macOS 27.0.1, Xcode 27.0.
- Homebrew: cmake, ninja, ffmpeg, sdl3 3.4.18, faudio 26.10, mingw-w64, ghidra, openjdk@21, sevenzip.
  An agent session cannot run `brew install` itself: give the user the command.
- `.venv`: pefile, capstone, lief, unicorn, numpy, pillow.
- Wine baseline (oracle only, ADR-007): `artifacts/wine/cx` (WineCX 24.0.7), prefix `artifacts/wineprefix-cx`,
  started with `scripts/run_baseline.sh`. Working copy of the game: `artifacts/game` (`reference/` stays read-only).
- A worktree thread links its inputs from the main checkout (`artifacts/lifted`, `artifacts/game`, `artifacts/traces`,
  `.venv`, `reference`, the golden PNGs) and keeps its own `build/` and `artifacts/boot/`.

## Regression set
| Command | Expected |
|---|---|
| `artifacts/boot/bo1_boot --steam-selftest` | `steam selftest: 76 checks, 0 failed, 24 interface methods` |
| `build/test_win_messages`, `build/test_window_service` | `all checks passed` |
| `scripts/boot_try.sh 90` | `time limit reached (still running …)` |
| the unattended run above | three `BinkOpen` / `BinkClose` pairs with sound-track sample counts and 0 dropped, `xaudio2: first xWMA voice … decoded with FFmpeg`, no `Com_ERROR`, no "host signal" |
| `scripts/replay_check.sh` (renderer changes) | worst SSIM cuba 0.9994, khe_sanh 0.9984, menu 0.9997, vorkuta 0.9985; no "unrecorded" lines |
| `scripts/difftest.sh 100000 2 5 all` (lifter changes; ~6 min) | 0 unexplained failures |

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
Design references: `docs/HOST_ABI.md` (guest/host calls), `docs/LIFTER.md`, `docs/TRACE_FORMAT.md`.

## Finding a bug
- **Runtime:** compare the game's own debug strings in the run log with a baseline `artifacts/baseline/run-*.log`
  (`OutputDebugStringA` lines) — they are identical up to the first fault. Then lldb on `bo1_boot`: break on
  `f_<VA>`, `x0` is the `Cpu *`, the global `g_mem` is the arena base; a write watchpoint on
  `g_mem + <guest address>` finds whoever clobbers a game variable.
- **Renderer:** `BO1_DEBUG_FRAME` / `BO1_DEBUG_DIR` with `tools/pass_dump.py`; `scripts/replay_window.sh <menu|cuba|vorkuta|khe_sanh> [first_frame]`
  shows a recorded trace live in a window.
- **Steam:** `tools/steam_sites.py` lists the game's call sites; unknown methods trap with their slot number.

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
