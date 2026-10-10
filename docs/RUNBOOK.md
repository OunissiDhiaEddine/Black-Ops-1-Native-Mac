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
- `scripts/map_sweep.sh [seconds] [map …]` loads campaign maps one per process (headless, silent, no movies) and
  writes `artifacts/boot/sweep/summary.txt`: level started, start save written, frames presented, first stop.

### Console commands that are useful in `BO1_CMDS`
| Command | What it does here |
|---|---|
| `devmap <map>` | starts a mission (cuba, vorkuta, pentagon, flashpoint, khe_sanh, hue_city, kowloon, fullahead, creek_1, river, wmd_sr71, wmd, pow, rebirth, int_escape, underwaterbase, outro) |
| `loadgame <name>` / `loadgame_continue` | loads `players/save/<name>.svg` / the newest save; every mission start writes `<map>.svg` |
| `disconnect` | leaves the mission for the menu |
| `r_mode 1024x768` then `vid_restart` | display-mode change = a Direct3D `Reset`. `vid_restart` alone does nothing: it acts only when a renderer setting changed |
There is no console command that saves (`savegame` is a script call): RE_NOTES "Saves, disconnect and display restart".

### Switches (environment)
| Switch | Effect |
|---|---|
| `BO1_WINDOW=1` | real SDL3 window on the process main thread (`run_native.sh` sets it) |
| `BO1_D3D=null` | no rendering |
| `BO1_D3D_DUMP=dir` | PNG of every 20th frame |
| `BO1_KEYS="ms:vk,…"` | scripted key presses (ms after start : Windows virtual-key code) |
| `BO1_CMDS="ms\|command;…"` | scripted console commands, e.g. `60000\|devmap cuba`. `;` separates entries; several commands in one entry go on separate lines (`BO1_CMDS=$'127000\|disconnect\nloadgame cuba'`) |
| `BO1_ARGS="…"` | extra arguments appended to the game's command line |
| `BO1_BINK=off` / `BO1_AUDIO=off` | no movies / no sound |
| `BO1_VERBOSE=0..3` | shim and game debug output in the log |
| `BO1_IMPORT_STATS=1` | import call counts at exit |
| `BO1_STEAM_ABSENT=1` | diagnostics: the stand-in answers "Steam is not running" |
| `BO1_EMULATED_STEAM_DLL=1` | the superseded ADR-010 path (shipped DLL in the x86 interpreter; it stops itself) |
| `BO1_NO_NET=1` | sockets exist but carry no traffic |
| `BO1_CPUS=n` | processor count reported to the game |
| `BO1_GAME_DIR`, `BO1_HOME_DIR` | the guest's `C:\bo1` (default `artifacts/game`) and home (default `artifacts/boot/home`) |
| `BO1_NATIVE=off` | every hand-written replacement (`src/native`) runs its lifted original instead: A/B measurements and bisecting (docs/LIFTER.md §8) |
| `BO1_AUDIO_MUTE=1` | the real audio device and its clock, mastering volume 0: for timing checks (SDL's dummy / disk devices consume samples about 8% slower than real time, so anything timed against the sound drifts there) |
| `BO1_FRAME_STATS=N` | one "frame stats:" line per N frames (300 for `1`): fps, frame time avg / p50 / p95 / p99 / max, time inside the renderer and its share (state setters are not timed: see "Where the time goes"), Direct3D calls per frame; plus a line of what was spent on shaders (and how many were compiled ahead), pipelines, GPU and drawable waits, texture uploads, and one "slow frame N: …" line with the same breakdown for every frame over 33.4 ms |
| `BO1_GPU_PASSES=N` | in frame N every pass (and every `StretchRect`) is committed on its own and prints "gpu pass …" with the GPU's time for it, its draw count and its target: which passes cost what at a resolution. The first line of the report covers everything before that frame: skip it |
| `BO1_SHADER_CACHE=dir` / `off` | the shader cache's directory (default `artifacts/shader_cache`, see "Shader cache" below) / no cache: every shader is compiled in the game at its first use, as before T-067 |
| `BO1_SHADER_CACHE_ALL=0` | the cache takes only the shaders a session used, not every shader a level creates: a few hundred to compile instead of ~5,500, and a shader never met before still stalls once |
| `BO1_SHADER_LOG=1` | one line for every shader compiled at its first use (not ahead, in the background), with the frames it was created and used in |
| `BO1_PRECOMPILE=1` | compile every shader in the background when the game creates it (off by default: it slowed play, LOG 2026-10-07) |
| `BO1_MOUSE_LOG=file` | window only: every mouse motion event (`ms motion xrel yrel x y relative`), every `SetCursorPos` (`ms warp x y`) and every switch of relative mouse mode; the run log gets one "mouse look: relative mouse mode on / off …" line per switch regardless |
| `SDL_MOUSE_RELATIVE_SPEED_SCALE=f`, `SDL_MOUSE_RELATIVE_SYSTEM_SCALE=1` | SDL's own switches for mouse look: a speed factor, and the system acceleration curve (default: unscaled) |
| `BO1_DEBUG_FRAME=N BO1_DEBUG_DIR=dir` | renderer dumps every pass of frame N; `tools/pass_dump.py dir` turns it into PNGs |

## Build
- `scripts/build.sh` — CMake/Ninja: shader translator, Metal renderer library, window library, replay tool, unit tests.
- `scripts/build_lifted.sh` — lift the image and compile only the chunks that changed (objects are renamed into place, so a parallel thread never links a half-written file); then `scripts/build_boot.sh`. A change of `src/runtime/lifted.h` recompiles all 67 chunks (~4 min at `JOBS=2`).
- `scripts/build_lifted.sh --a2-check` + `BO1_LIFT=artifacts/lifted_a2 scripts/build_boot.sh` -> `artifacts/boot/bo1_boot_a2`: audit build whose every `ret` reports (`[A2] …` on stderr) when it is about to return somewhere else than the function was entered with (docs/LIFTER.md, assumption A2). ~4 min, ~400 MB; delete `artifacts/lifted_a2` afterwards.
- `src/native/*.c` — hand-written replacements of lifted functions (`NATIVE(<va>)`, docs/LIFTER.md §8). After adding one: `scripts/build_lifted.sh` (re-lifts; recompiles the one chunk that holds the function), `scripts/test_native.sh`, `scripts/build_boot.sh`.
- `scripts/build_boot.sh` — the runtime `artifacts/boot/bo1_boot` (not in CMake: it links the lifted objects in
  `artifacts/lifted/obj`, which come from `scripts/difftest.sh` / the lifter). Links Homebrew's SDL3, FFmpeg
  (libavformat / libavcodec / libavutil) and FAudio.
- The lifted C code is generated (`tools/lifter/`), lives in `artifacts/lifted/` and is never edited or committed.
- `scripts/doctor.sh` reports which tools are present; `scripts/setup_venv.sh` creates `.venv`.

### Shader cache (T-067)
- What stalls a frame at a shader's first use is compiling its MSL (130–340 ms each when Metal has never seen it).
  `artifacts/shader_cache/` holds Metal libraries compiled outside the game, named by the hash of the MSL text; the
  renderer loads one in ~0.15 ms instead of compiling. Pipelines are still created at first use: 7–12 ms each the
  first time ever a cached library is used on the machine (a level's first frame: ~240 of them), then nothing, in
  any folder — which is why there is no pipeline archive (T-068).
- The game never compiles in the background and never leaves a draw out. It leaves the source (`<key>.metal`) of
  every shader it had to compile itself and of every shader a level creates when it loads (~5,500 per level).
- `scripts/shader_cache.sh` compiles the waiting sources with Apple's offline compiler (`xcrun metal`, part of
  Xcode's Metal toolchain) at low priority, `JOBS=3` at a time: 0.3–0.7 CPU-seconds per shader, so a level's first
  load means 10–30 minutes once. `scripts/run_native.sh` runs it when the game exits (Ctrl-C is fine: it goes on
  next time); `scripts/shader_cache.sh status` counts libraries and waiting sources.
- The directory belongs to the main folder; a worktree links it like its other inputs. Deleting it is harmless.
  After a change of the shader translator the keys change: old libraries are simply never asked for again
  (delete the directory now and then).

### Machine and toolchain (as of 2026-10-06)
- Apple M1 (arm64, **16 KiB pages**), 8 GB RAM, macOS 27.0.1, Xcode 27.0.
- Homebrew: cmake, ninja, ffmpeg, sdl3 3.4.18, faudio 26.10, mingw-w64, ghidra, openjdk@21, sevenzip.
  An agent session cannot run `brew install` itself: give the user the command.
- `.venv`: pefile, capstone, lief, unicorn, numpy, pillow.
- Wine baseline (oracle only, ADR-007): `artifacts/wine/cx` (WineCX 24.0.7), prefix `artifacts/wineprefix-cx`,
  started with `scripts/run_baseline.sh`. Working copy of the game: `artifacts/game` (`reference/` stays read-only).
- A worktree thread links its inputs from the main checkout (`artifacts/lifted`, `artifacts/game`, `artifacts/traces`, `artifacts/shader_cache`,
  `.venv`, `reference`, `re/ghidra_export`, the golden PNGs) and keeps its own `build/` and `artifacts/boot/`.
  Two exceptions: a thread that writes saves or settings makes `artifacts/game` a folder of links with its own
  `players/` (saves and `config.cfg` live there); a thread that changes the lifter or `src/runtime/lifted.h` works
  on its own clone of the lifted code (`cp -cp` of `artifacts/lifted/*.c *.h *.json` and `obj/lifted_*.o`) and
  regenerates the shared one when it merges. `touch` the cloned objects afterwards: a fresh worktree's
  `src/runtime/lifted.h` is newer than they are, and `build_lifted.sh` then recompiles all 67 chunks (8 min at `JOBS=1`; it
  happened on 2026-10-10).
- **After a lifter change:** `.venv/bin/python tools/lifter/lift.py --out artifacts/lifted --stats re/lift_coverage.json`
  rewrites only the chunks whose text changed; compile those (`scripts/difftest.sh` does it, and re-tests), then
  `scripts/build_boot.sh`. A change of `src/runtime/lifted.h` makes `difftest.sh` recompile all 66 chunks (~4 min).

## Regression set
| Command | Expected |
|---|---|
| `artifacts/boot/bo1_boot --steam-selftest` | `steam selftest: 76 checks, 0 failed, 24 interface methods` |
| `build/test_win_messages`, `build/test_window_service` | `all checks passed` |
| `scripts/boot_try.sh 90` | `time limit reached (still running …)` |
| the unattended run above | three `BinkOpen` / `BinkClose` pairs with sound-track sample counts and 0 dropped, `xaudio2: first xWMA voice … decoded with FFmpeg`, no `Com_ERROR`, no "host signal" |
| `scripts/replay_check.sh` (renderer changes) | worst SSIM cuba 0.9994, khe_sanh 0.9984, menu 0.9997, vorkuta 0.9985; no "unrecorded" lines |
| `scripts/test_native.sh` (changes in `src/native`, the lifter or `lifted_fp.h`; a few seconds) | last line `test_native: 21600 cases, 0 failed` |
| `REPORT=re/difftest_report_all.json scripts/difftest.sh 100000 2 5 all` (lifter changes; ~6 min, ~10 when `lifted.h` changed) | last line "functions failing: 9 (9 with a known-wrong reference, 0 unexplained)". Without `REPORT=` the summary overwrites the leaf-set report `re/difftest_report.json` |
| saves: `BO1_BINK=off BO1_KEYS="35000:0x20" BO1_CMDS="50000\|devmap cuba;125000\|loadgame cuba"`, 200 s | `players\save\cuba.svg` written, "Save game build number: 152", "=== G_LoadGame ===", then frames again; no "guest stopped" |
| leave and resume: `… BO1_CMDS=$'50000\|devmap cuba;127000\|disconnect\nloadgame cuba'`, 250 s | "Disconnecting: Disconnected from server", "Server: frontend", "Server: cuba", "=== G_LoadGame ===" |
| display mode: `… BO1_CMDS="50000\|devmap cuba;125000\|r_mode 1024x768;127000\|vid_restart;175000\|r_mode 800x600;177000\|vid_restart"`, 220 s | two "d3d9(null): Reset …" lines, each followed by "Finished recovering lost device."; dumped frames change size; restore `players/config.cfg` afterwards if the run is cut short (it keeps the last `r_mode`) |

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
   d3d9_live.cpp             watches every Direct3D call and feeds it to the renderer: the calls a frame consists
                             of (state setters, constants, buffer locks, draws) straight into the renderer's typed
                             functions, the rest as a call record; tells the window service the game's display
                             mode after CreateDevice / Reset
   user32_headless.cpp       windows, messages, cursor (despite the name it also serves the real window)
src/rhi + src/metal          renderer: Direct3D 9 calls in (typed functions and call records; a record of a typed
                             call is decoded into the same function, so the trace replay runs the same code),
                             Metal out. A draw tells Metal only the state that changed since the draw before.
src/native/*.c               hand-written replacements of lifted functions (memcpy, vertex skinning); LIFTER.md §8
src/shaders/sm3_msl.cpp      SM3 bytecode -> MSL
src/platform/window          SDL3 window on the process main thread (size / full screen follow the game's display
                             mode), SDL events -> Win32 messages, virtual cursor /
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
- **Where the time goes:** `sample <pid> 10 -file out.txt` on a running `bo1_boot`, then `tools/sample_tree.py out.txt "MetalRenderer::draw_indexed" 3` (callees merged over the report). A heavy scene without a hand: `BO1_CMDS="45000|loadgame <save>"` with a checkpoint save in `players/save/`.
  `scripts/perf_scene.sh <tag>` does both for the two checkpoint saves `cuba-1` (street fight, ~31,000 Direct3D calls per frame) and `cuba-2` (~15,000): frame statistics in `artifacts/boot/<tag>.log`, two profiles next to it; `BIN=<other bo1_boot>` measures another build on the same saves. Expected on the M1 at 800x600 (2026-10-10): renderer 3.8 ms per frame in `cuba-1`, 2.5 ms in `cuba-2`, 62.5 fps in both. The profiler slows the game while it samples (those windows show it). "GPU busy" in the second statistics line is the GPU's own time for the frames' command buffers (the budget is 16.7 ms per frame = 5,000 ms per 300 frames; expected 2026-10-10: ~3,000 ms in `cuba-1` at a true 2560x1600, ~2,500 ms at 1280x800); it rises when anything else uses the GPU. "renderer ms" times draws, buffer writes and everything that goes as a record; the state setters (most of the calls) are counted but not timed since 2026-10-10, so numbers in older logs are 0.6–1.8 ms higher for the clock reads alone.
- **Steam:** `tools/steam_sites.py` lists the game's call sites; unknown methods trap with their slot number.
- **Reading the game's code:** `tools/xref.py` (who references a string or an address, who calls a function, plain
  disassembly at a VA) and `tools/bink_sites.py --range LO HI` (annotated listing of whole functions).
- **"bad jump-table index at VA":** a `jmp [idx*4 + table]` whose cases the lifter does not know. Check the table in the
  image against `re/ghidra_export/jumptables.csv`; `Lifter.complete_tables()` handles tables Ghidra cut short.
- **"indirect target is not a function at VA":** the game jumped to an address that is no function entry. If the
  instruction before VA is a call of `_setjmp3` (0x00969EAC) it is a `longjmp` — see docs/LIFTER.md A2.

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
- **Scripted commands wait for the message pump**: while a level loads, `BO1_CMDS` entries that fall due are held and
  then run together — on a loaded machine a `loadgame` timed "after the load" can arrive before the start save exists.
- **`git add -A` picks up the worktree's links** (`.venv`, `reference`, `re/ghidra_export` are links there and the
  ignore patterns name directories): add paths explicitly.
- **Compiled shaders are remembered per folder, not per machine** (LIKELY; seen 2026-10-10): the first run from a new
  worktree compiled the menu's 271 shader functions again (50 s of stalls), and the main folder's build stalled 36 s
  on a scene that had only ever been played from a worktree. A rebuild in the same folder kept its shaders. So: run a
  measurement twice in a new folder, and expect the user's first visit of a scene from a folder to hitch once.
- **A windowed mode larger than the desktop is not what it says**: with `r_fullscreen 0` the game creates the device at
  the desktop's size (`r_mode 2560x1600` -> "CreateDevice 1440x900"). To measure a resolution, set `r_fullscreen 1`
  (headless runs accept it) and read the "CreateDevice" line (`BO1_VERBOSE=1`).
- **Other programs distort a measurement, the GPU's too**: on 2026-10-10 one run "showed" the menu at 43 fps with the
  renderer waiting 8–14 ms per frame for the GPU, while a system photo-analysis job, Spotlight and an editor were
  busy; the same scene held 60 fps an hour later. Check `uptime` and `ps -Ao pcpu,comm -r | head` first, do not
  dump frames (`BO1_D3D_DUMP`) in a timed run, and repeat a surprising result before writing it down.
- **Withdrawn work is kept as patches** in `docs/patches/` (apply with `git apply` on the commit named in the log entry of 2026-10-07): the stall-free shader compile and the alternative setjmp / longjmp implementation.
- **CSV files under `re/` have Windows line endings** (`platform_map.csv`, `symbols.csv`): edit them in binary mode.
