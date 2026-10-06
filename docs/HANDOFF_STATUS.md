# Project status — BO1 native arm64 + Metal port

Written 2026-10-06 as a plain status report. It describes what exists and what was observed; it
contains no task list. Detailed records: `LOG.md`, `TASKS.md`, `docs/DECISIONS.md`, `docs/RE_NOTES.md`.

## What the project is
Call of Duty: Black Ops (2010, Treyarch T5 engine, PE32 x86, Direct3D 9) being ported to a native arm64
Mach-O process on Apple Silicon with a Metal renderer and no Wine at runtime. First target: the
single-player campaign (`BlackOps.exe`). The owner states they own the game.

## How it works
- **Static recompilation** (`tools/lifter/`, `src/runtime/`): the x86 code is lifted function by function
  to C (`artifacts/lifted/`, generated, gitignored) and compiled for arm64. The guest address space is a
  4 GiB arena; flags are function-local; indirect calls go through a dispatch table.
- **Renderer** (`src/rhi`, `src/metal`): Direct3D 9 calls in, Metal out; SM3 bytecode is translated to MSL.
- **Platform shims** (`src/platform/win32`): kernel32/user32/winsock/etc. on macOS, a null D3D9 device,
  an SDL3 window service (`src/platform/window`).
- **Boot harness** (`tests/boot`, `scripts/boot_try.sh`): runs the lifted game headless and reports where it stops.

## What has been achieved (with evidence files)
- **Lifter:** 25,991 of 26,304 entry points lift (1,876,989 instructions): integer, x87, SSE/SSE2, MMX.
  The remainder is junk candidates plus a handful of known unsupported functions (`re/lift_coverage.json`).
- **Differential tests** against an x86 emulator (Unicorn), whole call trees: 26,294 vectors over
  13,840 functions, 26,275 pass, 0 unexplained failures; the 7 failures are documented reference-emulator
  or input artefacts (`re/difftest_report_all.json`, `tests/diff/reference_deviations.json`).
  An x86_64 cross-build under Rosetta agrees apart from three 1-ulp libm differences (`docs/LIFTER.md`).
- **Renderer vs the Wine/wined3d baseline** on four recorded scenes (menu, three missions), mean SSIM
  0.973 to 0.999, 0 skipped draws (`re/replay_check.json`). Known open differences: Khe Sanh sky and far
  field, small specks in the menu; which side is right is unknown without a native D3D9 reference.
- **Live view:** `scripts/replay_window.sh menu 1500` plays a recorded session through Metal in an SDL3 window.
- **Native boot:** the lifted game runs from the entry point through CRT start-up, static initialisers,
  `WinMain`, CPU detection, the D3D9 probe and window-class registration (87 distinct imports, about
  4,900 calls, no crash) and stops at the game's Steam check.

## The blocker
The game calls `SteamAPI_Init` early in `WinMain`. With no Steam implementation present it fails and the
game shows "Steam must be running to play this game." and exits (`BO1_NO_STEAM_DLL=1 scripts/boot_try.sh`;
the "absent" stand-ins are `src/platform/win32/steam_absent.cpp`).

## What the shipped `steam_api.dll` did
`artifacts/game/steam_api.dll` (255,776 bytes, PE32, link date 2016, three sections, one with entropy 7.93) is
supplied by the owner together with the game. It was mapped unmodified into the arena and run by an x86
interpreter host (`src/platform/win32/x86_module.cpp`; Unicorn, FS segment set to the thread block). Observed:
- its start-up code runs and resolves about 200 Win32 functions;
- it reads `C:\bo1\steam_emu.ini` and creates files under `C:\users\Public\Documents\Steam\CODEX\42700\`;
- it enumerates the process's modules (EnumProcessModules, then GetModuleFileNameExA for each);
- it then shows a message box "Internal error 0x0A: Protection initialization failed!" and calls ExitProcess(0);
- the game's 19 Steam imports bind to its 88 exports without error;
- a trace of its reads of the process/thread blocks showed one field of the thread block and two bytes of a
  system-module handle, nothing else (`BO1_ENV_TRACE`, not kept in the tree).
- a fidelity bug on our side (incomplete module list) was fixed on the way; the result did not change.

## What the earlier agent did not do
It did not investigate why the DLL's protection rejects this environment, did not patch, unpack or replace
the DLL, and did not write a Steam emulator. These were outside what it was willing to do. The project owner
has said they own the game, consider the DLL part of it, and have changed the project's ground rule on this
(see the latest commit message and `AGENTS.md`).

## Other open items
Sound (XAudio2 on FAudio needs `brew install faudio`), video (Bink via FFmpeg), wiring the game's live
D3D9 and window calls to the renderer and window service (untestable until the game passes the check),
gamepad. Real Steam via Valve's macOS library was considered and then withdrawn by the owner
(`docs/DECISIONS.md`, ADR-014).

## Running things
`scripts/boot_try.sh [seconds]` (native boot, log in `artifacts/boot/last_run.log`), `scripts/difftest.sh`,
`scripts/replay_check.sh`, `scripts/replay_window.sh`, `build/test_win_messages`, `build/test_window_service`.
Game files: `artifacts/game/` (working copy), originals in `reference/` (read-only, gitignored).
