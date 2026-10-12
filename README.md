# Black Ops for Apple Silicon

Tooling that turns **your own copy** of Call of Duty: Black Ops (2010, PC, single-player `BlackOps.exe`) into a
native arm64 Mac app with a Metal renderer. Nothing is emulated when the game runs: no Wine, no Rosetta, no
Direct3D translation layer from someone else.

This repository holds source code only. It contains no game code, no game data and no built app. You need the
game's files from a copy you own, and everything made from them stays on your Mac.

## What works
The single-player campaign: intro movies, menus, sound, saves, window and full screen, keyboard, mouse, trackpad
and gamepad. Every campaign mission starts and plays on an M1 with 8 GB; nobody has played the whole campaign
through yet. Multiplayer and the Steam services (achievements, friends, DLC) are out of scope.

## How it works
| Part | Where | What it does |
|---|---|---|
| Static recompiler | `tools/lifter/` | reads the x86 code of the game's executable and writes C that does the same thing; clang compiles that for arm64 ([docs/LIFTER.md](docs/LIFTER.md)) |
| Runtime | `src/runtime/` | the game's 32-bit address space inside a 64-bit process, the loader, threads |
| Platform layer | `src/platform/` | replacements for what the game expects from Windows: files, threads, window and input (SDL3), Direct3D 9, XAudio2 (FAudio), Bink video (FFmpeg), and an offline stand-in for Steam |
| Renderer | `src/rhi/`, `src/metal/`, `src/shaders/` | the game's Direct3D 9 calls drawn with Metal; its shaders translated from D3D9 bytecode to Metal Shading Language |
| Native replacements | `src/native/` | hand-written 64-bit versions of a few hot functions, tested against the recompiled originals |

## What you need
- An Apple Silicon Mac, macOS 14 or newer, about 15 GB of free disk space (most of it is the working copy of the game).
- Xcode with its Metal toolchain: `xcodebuild -downloadComponent MetalToolchain`.
- Homebrew packages: `brew install cmake ninja sdl3 faudio ffmpeg ghidra`.
- The PC version of the game, installed or copied as a normal folder. The build is tied to one executable:

  | File | Size | SHA-256 |
  |---|---|---|
  | `BlackOps.exe` (linked 2011-09-01) | 8,099,608 bytes | `2ba4a04e81de2b60582da0e4489efde21e6cb07904a223bae633b6ccaebbf759` |

  The tables under `re/` (function entry points, import and interface layouts, symbol names) are addresses in
  that file. Another release of the executable needs them redone.

## Build
Run everything from the repository's root.

```bash
# 1. Your copy of the game: the original (only ever read) and a working copy (saves and settings are written here)
mkdir -p reference artifacts
cp -Rc "/path/to/Call of Duty Black Ops" "reference/Call of Duty Black Ops"
cp -Rc "reference/Call of Duty Black Ops" artifacts/game
shasum -a 256 "reference/Call of Duty Black Ops/BlackOps.exe"

# 2. Python tools (a local .venv, nothing system-wide)
scripts/setup_venv.sh

# 3. Find the functions of the executable with Ghidra (tens of minutes, once)
scripts/ghidra_analyze.sh
scripts/ghidra_analyze.sh --seed

# 4. Recompile the game's code for arm64 (about 67 C files of 2-3 MB each; JOBS=2 needs ~2 GB of memory)
scripts/build_lifted.sh

# 5. Build the runtime, the platform layer and the renderer, and link the game
scripts/build_boot.sh

# 6. Check it: the Steam stand-in's self-test, then one minute to the main menu without window or sound
artifacts/boot/bo1_boot --steam-selftest
scripts/boot_try.sh 60

# 7. Play
scripts/run_native.sh

# 8. Optional: a double-clickable app, and a disk image of it
scripts/make_app.sh --dmg
```

The first visit of a level compiles its shaders, which makes it stutter once. `scripts/run_native.sh` compiles
them next to the game and keeps them in `artifacts/shader_cache/`.

Step 8 writes `artifacts/app/Black Ops.app`. The app holds the recompiled game, so it is for you alone: do not
upload it or pass it on. It does not hold the game's data files; at first start it looks for your game folder or
asks for it, and keeps saves and settings in `~/Library/Application Support/Black Ops Mac`.

## Working on it
- [docs/RUNBOOK.md](docs/RUNBOOK.md): how to run, the switches, the tests and what they should print, how to find a bug.
- [docs/RE_NOTES.md](docs/RE_NOTES.md): what is known about the game's internals, each item with how sure it is.
- [docs/DECISIONS.md](docs/DECISIONS.md): why it is built this way.
- [CLAUDE.md](CLAUDE.md): the rules the project is run by.

Quick checks after a change are steps 6 above and, for `src/native` or the recompiler, `scripts/test_native.sh`.
`scripts/difftest.sh` runs recompiled functions against an x86 emulator. `scripts/replay_check.sh` compares the
renderer with recorded frames of the original; those recordings hold game data and are not in this repository.

## Open work
- Play mission 1 to its end in the window. The hand-over to the next mission is confirmed only in unattended runs, and only for the first of the five story interludes.
- Measure the frame rate of the big fights in a real window (`BO1_FRAME_STATS=300 scripts/run_native.sh`). Unattended, they hold the game's 62.5 fps cap.
- A real mouse and controller rumble are untested. Keyboard, trackpad and gamepad work.
- The very bright opening of the Hue City mission has not been compared with the original.
- If a measurement shows the processor as the limit: more hand-written replacements for the render thread's hot functions (`src/native/`).
- The shaders of a level compile at its first visit and make it stutter once.

## Legal
This is an unofficial interoperability project for people who own the game. It is not affiliated with or endorsed
by Activision or Treyarch; Call of Duty and Black Ops are their trademarks. The repository contains none of their
code or assets, and nothing in it removes copy protection. Keep what you build from your copy to yourself.
