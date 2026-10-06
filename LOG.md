# LOG

## STATE  (OVERWRITE this block at every checkpoint — it is the resume point)
- **Date/time:** 2026-10-06 ~06:45
- **Phase / task:** **P5 started 2026-10-06** (user approval; SDL3 3.4.18 installed). T-050, T-051 DONE, T-036 closed by measurement. **NOW: T-052** (SDL3 user32). P4 exit was only partly met (diff tests green; the native boot stops at the game's Steam check — see docs/DECISIONS.md).
- **Last completed:** boot runtime runs the lifted game from the entry point through CRT start-up, static initialisers, WinMain, CPU detection, D3D9 probe, window-class registration and the build banner up to `SteamAPI_Init` (87 imports, ~4,900 calls, no trap). The shipped steam_api.dll runs unmodified under the interpreter but stops itself: "Internal error 0x0A: Protection initialization failed!".
- **Next exact step:** depends on the user's answer (options A/B/C in the ADR note). Work that does NOT depend on it: T-036 (shader/pipeline disk cache for the replay renderer), T-038 (video textures in a trace), T-046 (x86_64 cross-build of the lifted code), T-044 leftovers (MXCSR FTZ/DAZ: the game only reaches LDMXCSR through the CRT's `_controlfp` — log what it sets once the game runs further; A2 audit).
- **Standard commands:** `scripts/boot_try.sh [seconds] [lines]` (builds + runs `artifacts/boot/bo1_boot`, log in `artifacts/boot/last_run.log`; `BO1_NO_STEAM_DLL=1` = truthful "no Steam" shims instead of loading the shipped DLL; `BO1_VERBOSE=2` = every import call; `BO1_IMPORT_STATS=1`); `scripts/difftest.sh [max_funcs] [per_func] [seed] [leaf|all]` (`all` ≈ 6 min; `SKIP_VECTORS=1` ≈ 1-3 min); `tests/diff/debug_vector.sh <func> [nth]`; lifter inputs: `tools/lifter/find_entries.py` -> `tools/lifter/flag_audit.py` -> `tools/lifter/lift.py --stats re/lift_coverage.json`; `tools/import_abi.py`; `tools/com_abi.py`; `tools/adapter_probe.py`. P3: `scripts/build.sh`, `scripts/replay_check.sh`. Baseline: `scripts/run_baseline.sh --wine cx --net --trace …`.
- **Files in flight:** none.
- **Open questions for the user:** none about direction — the user chose real Steam (ADR-014). WAITING for the user to (1) install Steam for macOS and log in to the account that owns Black Ops, (2) put Valve's Steamworks SDK (at least `redistributable_bin/osx/libsteam_api.dylib` and `public/steam/*.h`) under `artifacts/steamworks/`. Then: T-047.
- **Half-done / how to resume:** boot shims exist for kernel32 (core/sync/file/nls/extra), headless user32+gdi32, winmm/advapi32/shell32/ole32/psapi/shlwapi/xinput/dsound, winsock (real sockets), Bink stubs, null D3D9 device (never exercised past `Direct3DCreate9`), "Steam absent" shims, x86 module interpreter. Not written: XAudio2, real Bink, SDL window/input. Renderer still lives in `tests/replay/`.
- **Machine:** Apple M1 (arm64), 8 GB RAM, macOS 27.0.1, Xcode 27.0. brew: cmake, ninja, ffmpeg, sevenzip, ghidra, openjdk@21, mingw-w64. venv: pefile, capstone, lief, unicorn, numpy, pillow. Wine baseline: `artifacts/wine/cx` (WineCX 24.0.7), prefix `artifacts/wineprefix-cx`.
- **Reference copy / decisions:** user owns BO1 (stated 2026-10-05); `reference/` read-only; working copy `artifacts/game`. ADR-004/005/006/008/009/011/012 ACCEPTED; ADR-010 = shipped steam_api.dll unmodified in an embedded x86 interpreter (spike T-024 pending); ADR-013 lifter = Python -> C; G1 = go. Windows PC: last resort only (user).

---
## SESSION LOG (append-only; newest at the bottom)
Entry template:
```
### YYYY-MM-DD HH:MM — <task id(s)> — <one-line outcome>
- Did: ...
- Evidence: <command / file path / screenshot / test result>
- Learned: <facts worth keeping; also copy to docs/RE_NOTES.md with confidence tag>
- Problems: ...
- Next: <exact next command or step>
- Commit: <short hash>
```

### Entry 0 — scaffold — project created
- Did: created repo structure, AGENTS.md, PLAN, TASKS, DECISIONS, RE_NOTES.
- Next: T-001.

### 2026-10-05 13:40 — T-001 — tools inventoried, repo initialised; reference copy is a cracked scene release -> paused for user
- Did: read AGENTS/PLAN/TASKS/DECISIONS; ran `scripts/doctor.sh`; `git init`; added `scripts/setup_venv.sh` + `tools/requirements.txt` (project-local `.venv`); fixed doctor.sh (Ghidra check never printed MISS; now uses `.venv`, prints chip/RAM/OS/Xcode/disk).
- Evidence (doctor.sh, 2026-10-05 13:36):
  ```
  OK   git, clang, python3 (3.14.7, /opt/homebrew), java (Oracle JDK 21 LTS), xcodebuild, xcrun, brew
  MISS cmake, ninja, ffmpeg, wine, wine64, 7z, i686-w64-mingw32-gcc, CrossOver.app, ghidra
  OK   py (.venv): pefile 2024.8.26, capstone 5.0.9, lief 1.0.0, unicorn 2.1.4      MISS py: frida
  chip: Apple M1 (arm64) · ram: 8 GB · os: macOS 27.0.1 (26A434) · xcode: 27.0 (27A266a) · disk free: 57Gi
  ```
- Learned (also in RE_NOTES § Gotchas):
  - Install contents: `BlackOps.exe` (8,099,608 B, PE32 i386 GUI), `binkw32.dll`, `steam_api.dll`, `steam_api.cdx` (also a PE32 DLL), `main/` 7.0 G (iw_00..41.iwd, localized_English_iw00..14.iwd, video/), `zone/` 2.8 G (Common, English), no `BlackOpsMP.exe`. Total 10 G.
  - **Not a stock Steam install — CONFIRMED** by `steam_emu.ini` (CODEX banner, `UserName=CODEX`, `[Crack]` section, `DLCUnlockall=1`, save path `...\Steam\CODEX\42700`) and a repack-style `~Common Redist/aio_installer.bat`. `BlackOps.exe`, `steam_api.dll`, `steam_api.cdx` share mtime 2016-12-25; everything else 2025-09-01.
  - SHA-256: BlackOps.exe `2ba4a04e…bbf759`, binkw32.dll `ad55ec64…c2cdcb`, steam_api.dll `d1757479…612b48`, steam_api.cdx `9efe2c83…722394` (full values in RE_NOTES).
- Problems: ground rule 1 scopes the project to the owner's own copy and excludes DRM work. The protection here was already removed by a third party, so nothing asks me to circumvent anything — but (a) I can't tell from the files whether there is a licence behind this copy, and (b) every address/hash/import we record depends on which exe is the reference, and a third-party-patched exe + emulator DLL is not the retail build. Both are the user's call -> stopped before T-002 rather than build `re/` on a binary that may be swapped.
- Next: user answers the two open questions in STATE; then T-002.
- Commit: (see git log — first commit)

### 2026-10-05 ~14:00 — T-002..T-005, T-007, T-008 — P0 inventory done except Ghidra export
- Did: user confirmed ownership + approved tool installs -> `brew install cmake ninja ffmpeg sevenzip ghidra`. Wrote `tools/pe_inventory.py`, `tools/float_census.py`, `tools/find_xrefs.py`, `tools/ghidra/ExportProgramFacts.java`, `scripts/ghidra_analyze.sh`. Filled RE_NOTES § Binaries, Imports, Protection check, Middleware, Float model, Asset formats.
- Evidence: `re/inventory.json`, `re/inventory.md`, `re/imports.csv`, `re/float_census.json`, `re/xrefs_anchor.csv`; commits b0be0c7, c213d9d, 16f8476.
- Learned (details + confidence tags in RE_NOTES):
  - Exe: VS2008, 2011-09-01 build, base 0x400000, relocs stripped, CRT static, `.data` 62 MB virtual, TLS without callbacks, not packed (max .text window entropy 6.79).
  - 334 imports / 16 DLLs. Only 3 d3d9 + 6 d3dx9 imports (rest is COM vtables). Audio = XAudio2 via COM (+DirectSound for Bink/voice). Input = XInput + window messages. No DirectInput import.
  - FP: SSE scalar 61%, x87 27.6% (mostly fld/fstp at ABI edges), packed SSE 10%. 1,320 `fs:[0]` SEH frame sites, 557 jump tables, ~5.5k indirect calls, 430 lock-prefixed ops.
  - Middleware: Bink 1.9n (DLL), Demonware, libtomcrypt 1.17, zlib/minizip, LZO 2.02, libjpeg 6b, in-house physics + GSC VM. No Havok.
  - Assets: 146 fastfiles `IWffu100` v473 = plain zlib; shaders inside are precompiled SM3 bytecode (no HLSL text); IWD = zip; IWI v13 (mostly DXT1/3/5); sound files are a custom non-RIFF container (codec unidentified); Bink 1 720p video decodable by FFmpeg.
  - WinMain candidate 0x0050A730 (called from CRT startup at 0x0096D9E7 after `push 0x400000`) — LIKELY.
- Problems: Ghidra OOM at 3G heap in X86Analyzer on this 8 GB machine; rerunning with 5G. `brew install --cask ghidra` no longer exists — it is a formula now.
- Next: see STATE.

### 2026-10-05 — T-006, T-009, P0 exit review — P0 complete
- Did: Ghidra 12.1.4 headless analysis + structural export; coverage-gap measurement; xref-based entry-point location; `re/symbols.csv`; identified sound codec (MS-ADPCM) and confirmed fastfiles are plain zlib with precompiled SM3 shaders.
- Evidence: `re/ghidra_summary.json`, `re/coverage_gaps.json`, `re/xrefs_anchor.csv`, `re/symbols.csv`; `artifacts/ghidra/analyze.log` ("Analysis succeeded", 156 s).
- **P0 exit criteria (docs/PLAN.md):** binaries table OK · import census OK · middleware list OK · float-model note OK · Ghidra export reproducible by one script OK (`scripts/ghidra_analyze.sh`). -> **P0 exit: PASS.**
- Lifter-feasibility data for G1 (all in RE_NOTES):
  - Favourable: not packed; fixed image base, no relocs needed; no TLS callbacks; no VirtualProtect; clean decode; SM3-only precompiled shaders; tiny import surface (334); D3D9 use is all through COM vtables; assets readable with stock zlib/zip; Bink decodable by FFmpeg; audio is MS-ADPCM -> XAudio2.
  - Risks: (1) **10.1% of .text is code Ghidra did not reach** (pointer-only reachability) + 5,407 indirect call sites -> need pointer-seeded discovery + runtime trap/logging; (2) 51 unresolved switch tables; (3) x87 is 27.6% of FP instructions (mostly fld/fstp, but real arithmetic exists); (4) 1,320 SEH-frame sites + RaiseException/RtlUnwind -> C++ EH may be live; (5) heavily multi-threaded (430 lock-prefixed ops, 10 CreateThread sites); (6) `D3DXCompileShader` is imported — runtime HLSL compile must be ruled out by the P1 trace; (7) this Mac has 8 GB RAM / 56 GB disk — tight for compiling ~1.5 M lifted instructions and for a Wine baseline + traces.
- Problems: none blocking.
- Next: ask user for P1 go-ahead + ADR-007.

### 2026-10-05 — T-010, T-012 (partial) — P1 started; baseline stack chosen, blocked on Rosetta + Wine download
- Did: user gave go-ahead for P1, delegated ADR-007 ("most optimized free way"), approved making `reference/` read-only. `chmod -R a-w reference/`; `cp -c -R` clone to `artifacts/game` (minus Redist/Soundtrack); `brew install mingw-w64`; wrote ADR-007, `scripts/run_baseline.sh`, `tools/d3d9_proxy/{proxy.cpp,thunks.S,d3d9_methods.h,d3d9.def,build.sh}`.
- Evidence: `artifacts/d3d9_proxy/d3d9.dll` (231,128 B) builds with `-Wall -Wextra -Werror`; objdump shows exports `Direct3DCreate9`, `D3DPERF_BeginEvent`, `D3DPERF_EndEvent` and thunk `_thunk_device_17` = `push esp; push 0x11; call _trace_device@8; jmp [orig]`. Slot tables are `static_assert`ed against mingw's `d3d9.h` Vtbl structs (17 + 119 methods).
- Learned: Homebrew disabled all Wine casks on 2026-09-01 (not notarized); Rosetta 2 is absent on this Mac; `brew cat` silently turned Homebrew developer mode on — turned back off with `brew developer off`.
- Proxy design: in-place vtable patching (no wrapper objects, because Wine's d3d9 asserts vtable identity on some arguments); one generic asm thunk per slot reports (slot, args*) then tail-jumps to the original; `CreateDevice` has a post-hook to patch the device vtable. T-013 extends the same thunks with per-method argument decoders.
- Problems: cannot run anything until Rosetta + Wine exist -> T-011 not started, T-012 not verified.
- Next: see STATE.

### 2026-10-05 — T-009a — unreached code seeded: 10.1% -> 0.8%
- Did: `tools/seed_functions.py` (data-pointer + aligned-after-padding seeds), `tools/ghidra/SeedFunctions.java`, `scripts/ghidra_analyze.sh --seed`; two rounds to fixpoint; re-ran coverage and xref attribution.
- Evidence: `re/seeds.csv` (4,391), `re/ghidra_summary.json` (21,960 functions, 1,643,274 instructions), `re/coverage_gaps.json` (likely-unreached code 46,882 B = 0.8%).
- Learned: almost all pointer-only code is recoverable statically; 64 switch tables remain unresolved; 499 pointer targets without preceding padding were deliberately not seeded.
- Problems: Wine is unpacked but quarantined (see STATE) — cannot run the baseline until the user clears it.
- Next: user clears quarantine -> `scripts/run_baseline.sh`.

### 2026-10-05 ~15:05 — T-011 (blocked), T-012 (done) — baseline boots, renders intro, dies before the menu
- Did: user installed Rosetta, supplied wine-devel/staging 11.18 and approved the build in Gatekeeper; wineprefix created (Z: removed, G: = artifacts/game, network denied by sandbox-exec). 24 launches. Proxy grew: swap-chain Present hook (the game never calls Device::Present), 2 s flush thread, back-buffer capture to BMP -> PNG, opt-in import hooks (CreateThread, MessageBoxA auto-answer, VirtualAlloc, GlobalMemoryStatus, HeapAlloc), heuristic backtrace. Script: clears the `__BlackOps` crash marker, rewrites video dvars in config.cfg, `--capture`, `--seconds` (with timer cleanup — a leaked timer killed the following run once).
- Evidence: `artifacts/baseline/d3d9_census-20261005-143041.txt` (886 frames), `frames-20261005-143041/` (14 PNGs; frame 60 = legal screen, copied to `tests/golden/`), `run-20261005-143041.log` (11,054 lines incl. engine console).
- Learned: see RE_NOTES § D3D9 census, § Baseline status, § Gotchas. Headlines: single render thread; Present via swap chain; only DrawIndexedPrimitive; 2,700 shaders created from bytecode at load; XAudio2 2.7; engine console available through OutputDebugString; `R_CreateD3D9`, `R_CreateDevice` (0x006B7530), `Sys_CreateThread` confirmed by logged return addresses.
- Problems: (1) baseline unstable — different crash each run, root cause not established (LIKELY Wine WoW64/OpenGL on this macOS; one reproducible 2.85 GB heap request is unexplained). (2) `screencapture` needs Screen Recording permission -> replaced by in-proxy capture. (3) heuristic backtrace is polluted by stale stack slots; not reliable for Com_Error's origin.
- Not done: main menu (T-011), full-argument trace (T-013), golden scenes (T-014), census of menu/in-game (T-015).
- Next: see STATE.

### 2026-10-05 ~15:25 — T-011 — title screen reached under wine-staging; still dies there
- Did: `--wine NAME` (per-build prefix) and `--shots S` (window-only screenshots via `tools/macos/winlist.swift`) in run_baseline.sh; 16 more runs on wine-staging 11.18; proxy: ordinal-import hooks, `BO1_HOSTNAME` experiment.
- Evidence: `tests/golden/baseline_wine_title_press_any_key_t31s.png` (from `artifacts/baseline/shots-20261005-150910/`), `d3d9_census-20261005-150910.txt` (927 frames), `run-20261005-150910.log`.
- Learned: the good runs reach the title screen; failure there follows "Failed to log on." (network is denied by the sandbox — untested with network). Startup crashes are in Wine's 64-bit WoW64 layer during GL init (LIKELY). Hostname-overflow and Offline=1 hypotheses refuted.
- Problems: about half of all launches still die before the first frame.
- Next: user decision (STATE).

### 2026-10-05 ~15:50 — T-011 — network test, scripted input; Wine stack still the blocker
- Did: user allowed network and provided a CrossOver copy. Ran 14 more launches on wine-staging with `--net`; added `--keys` (proxy posts WM_KEYDOWN/WM_CHAR/WM_KEYUP to the device window) and fixed the script's cleanup trap (under `set -e` a failed `kill` aborted the trap and leaked the screenshot loop). Tried wined3d `csmt=0`.
- Evidence: `artifacts/baseline/shots-20261005-153413/shot_00{3,4,5}_*.png` (title screen held for 20+ s with network), `run-20261005-153413.log` (963 "Failed to log on.", no Com_ERROR), census events `key vk=0x20 posted …`.
- Learned: the title-screen fatal error goes away with network allowed (LIKELY sandbox-refusal specific). Everything else unchanged: most launches die in Wine's 64-bit layer at GL init or when the render thread starts.
- Problems: CrossOver copy not usable (modified main executable, ad-hoc signature) -> not executed. No run after the key-press feature survived long enough to test it.
- Next: see STATE.

### 2026-10-05 ~15:50 — T-011 — DXVK on WineHQ builds: dead end
- Did: downloaded DXVK-macOS v1.10.3-20230507 (user-approved) to `artifacts/dxvk/`; proxy can wrap any D3D9 implementation (`BO1_REAL_D3D9`); `run_baseline.sh --dxvk`; 3 runs.
- Evidence: `artifacts/baseline/BlackOps_d3d9-20261005-154538.log`, census events (`CreateDevice -> hr=0x8876086a` x20), MoltenVK `VK_ERROR_FEATURE_NOT_PRESENT` lines in `run-20261005-154538.log`.
- Learned: stock MoltenVK lacks geometryShader + shaderCullDistance that DXVK's D3D9 requests; DXVK runs never showed the random startup crash (3/3), which supports "OpenGL path is the unstable part".
- Problems: user asked to fall back to the CrossOver copy in `artifacts/crossover/`; declined again (modified main executable, ad-hoc signature — RE_NOTES § Baseline status). Proposed the open-source CrossOver-engine build instead.
- Next: see STATE.

### 2026-10-05 ~16:00 — T-011 DONE — main menu on WineCX 24.0.7
- Did: with the user's OK downloaded `WS12WineCX24.0.7_7.tar.xz` (172,348,356 B, Sikarugir-App/Engines) -> `artifacts/wine/cx/`; `run_baseline.sh --wine cx` (symlinks libinotify/libintl from the WineHQ build, `DYLD_FALLBACK_LIBRARY_PATH` for the rest); 2 runs with `--net --keys`.
- Evidence: `tests/golden/baseline_cx_main_menu_f1350_800x600.png`, `baseline_cx_settings_sound_f2400_800x600.png`; `artifacts/baseline/d3d9_census-20261005-155243.txt` (2,496 frames) and the following run (2,276 frames, 1.84 M draws); `frames-20261005-155243/` (16 PNGs).
- Learned: CrossOver's Wine is stable where WineHQ 11.18 is not, on the same OpenGL route -> the earlier crashes were the WineHQ WoW64/GL layer, not the game. The main menu is a real 3D scene (~800 draws/frame). `size aa010000` allocation failure is benign.
- Problems: WMA decoding unavailable in this Wine (`wma_decoder_create GStreamer doesn't support WMA` x260) -> some audio may be silent in the baseline; irrelevant for D3D capture. `--shots` found no window because the game was on another Space.
- Next: T-013.

### 2026-10-05 ~16:40 — T-013 DONE, T-015 (menu + cuba), T-014 in progress
- Did: rewrote the proxy around a generic pre/post thunk (per-thread shadow stack, results and out-pointers, 10 hooked interface types, slot tables and argument word counts derived from the SDK header at compile time); binary trace writer with a recording window; `tools/trace_reader.py`, `tools/trace_census.py`; `docs/TRACE_FORMAT.md`; scripted console commands through the game's `Cbuf_AddText`; per-frame timing CSV; `scripts/capture_golden.sh`.
- Evidence: `re/d3d9_census_menu.json`, `re/d3d9_census_cuba.json`; `tests/golden/baseline_cx_cuba_bar_f5200_800x600.png`; traces of 16.9 M (menu) and 25.7 M (cuba) records parsed end to end.
- Learned: the D3D9 surface is small and identical between menu and mission (37 device methods, SM3 only, one draw path, 30 render states); 5,314 unique shaders so far; ~2,000 draws and ~390k triangles per frame in the first mission; the game re-uploads a 2 MiB index buffer and a 1 MiB volume texture every frame.
- Problems solved: (1) result capture crashed the game after ~900 frames — d3d9's surface AddRef/Release tail-jump into the texture's method, so two hooked calls share one return slot; fixed by recognising the tail call. (2) a naive trace was 4 GB in 474 frames (whole 8 MiB dynamic buffer dumped per lock, nested surface locks duplicating texture data, Bink frames) -> sentinel trimming, nesting detection, video frames off by default, recording window.
- Next: see STATE.

### 2026-10-05 ~17:00 — T-014, T-015, P1 exit review — P1 complete
- Did: `scripts/capture_golden.sh` + `capture_golden_all.sh`; captured menu, cuba, vorkuta, khe_sanh (frames in a 61-frame fully recorded window, 2 GB traces, frame-time CSV); census on all four; import-table hook proving `D3DXCompileShader` is not called.
- Evidence: `tests/golden/<name>/metrics.json` (committed) + frames (gitignored); `re/d3d9_census_*.json`; `artifacts/traces/{menu,cuba,vorkuta,khe_sanh}.bin`; 6,845 unique shader blobs in `artifacts/shaders/`.
- **P1 exit criteria (docs/PLAN.md):** golden frames OK (4 scenarios) · replayable trace of the main menu and an in-game scene OK (format documented, parsed end to end; the replay harness itself is P3/T-031) · census doc OK. -> **P1 exit: PASS.**
- Input for G1 / ADRs: SM3-only precompiled shaders, 25 VS + 34 PS opcodes, no runtime compile -> ADR-005 option (a) (own SM3 -> MSL translator) looks sufficient; single render thread, 37 device methods, one draw path, no fixed function -> ADR-004 can be a thin D3D9-shaped RHI. Workload up to ~3,100 draws / 720k triangles per frame.
- Problems: baseline is slow (18–31 fps traced) — fine for an oracle. Later mission content and non-default settings are untraced.
- Next: ask the user before P2.

### 2026-10-05 ~17:35 — T-020..T-023, P2 exit review — P2 complete
- Did: proxy import census (all 334 IAT slots counted with transparent stubs), dynamic-import logging (`LoadLibraryA`/`GetProcAddress`), `tools/platform_map.py` -> `re/platform_map.csv`, `tools/com_steam_usage.py` -> `re/com_steam_usage.json`, `docs/HOST_ABI.md`, ADR proposals 006 and 008–012. Cleanup of obsolete artifacts.
- Evidence: `re/platform_map.csv`; `artifacts/baseline/import_census-20261005-{165933,170331,170721}.csv`; RE_NOTES § Platform map.
- **P2 exit criterion (docs/PLAN.md):** "RE_NOTES § Platform map: every import -> {shim | lift | stub | not needed}" — met: 286 shim / 46 stub / 2 not needed, 0 unclassified ("lift" applies to no import: the only liftable DLL, binkw32, is proposed as a shim in ADR-008). -> **P2 exit: PASS.**
- Learned: 184 of 334 imports are called from boot to mission; sync primitives dominate (InterlockedExchange ~35 M calls per run); exceptions are not used on the normal path beyond 13 thread-name raises; async file I/O (ReadFileEx + SleepEx) is used; Bink, Steam polling and sockets are all live in single player; no D3DX call at all; only XAudio2 2.7 is created through COM.
- Problems: first census build crashed the game at start — stubs were 13 bytes in 12-byte slots, each overwriting the previous stub's last address byte; fixed (16-byte slots).
- Decision flagged for the user: ADR-010 — the Steam layer will be a bridge to the real Steam client; an emulating stand-in is out of scope under ground rule 1.
- Next: see STATE.

### 2026-10-05 ~17:45 — user decisions on the P2 ADRs; P3 go-ahead
- Did: user approved ADR-006, 008, 009, 011, 012 -> ACCEPTED. User rejected the Steam-client bridge (will not install Steam) and decided the port keeps using the shipped `steam_api.dll` -> ADR-010 rewritten: the DLL runs unmodified in an embedded x86 interpreter; it is not unpacked, patched or re-implemented (HOST_ABI §7a). Follow-up spike T-024 added. P3 approved.
- Evidence: `docs/DECISIONS.md`, `docs/HOST_ABI.md` §7a.
- Next: P3 — T-031 replay harness, T-030 ADR-004/005 proposals.

### 2026-10-05 ~17:40 — T-031 DONE — replay harness + trace format v2
- Did: C++ trace reader and replay harness (`tests/replay/`, CMake target `replay`, `scripts/build.sh`) with a validating null backend. Its first run exposed two recorder flaws: (1) calls the D3D9 implementation makes inside a call recorded on entry showed up as game calls; (2) shaders and vertex declarations never reported destruction, so reused pointers looked like wrong-kind objects. Fixed in the proxy: every call now gets a shadow frame (exact depth), 3 more interfaces hooked, the proxy's own D3D use is excluded. Trace format v2; all four scenarios re-captured.
- Evidence: validator output for the four traces — 0 findings each (menu 16.9 M records, cuba 24.8 M, vorkuta 23.4 M, khe_sanh 24.3 M); `tests/golden/*/metrics.json` refreshed.
- Next: T-033 shader translator prototype; T-030 ADR-004/005 proposals.

### 2026-10-05 ~18:15 — T-033 prototype — SM3 -> MSL translator
- Did: `src/shaders/sm3.h` + `sm3_msl.cpp` (decode + emit, ~600 lines, no dependencies), `tools/sm3c` (translate + compile through `MTLDevice newLibraryWithSource`, parallel), CMake targets `bo1_shaders`, `sm3c`.
- Evidence: `build/sm3c --no-compile artifacts/shaders/all` -> 6845/6845 translated in 0.6 s (1,142,172 instructions, 65 MiB of MSL); first full compile: 260/260 VS OK, all PS failed on one missing `[[stage_in]]`; after the fix a 299-PS sample compiles 299/299; full re-run in the background.
- Learned: Metal compiles ~11 shaders/s here (~90 ms each) -> a mission's ~5,000 shaders would take minutes on first use: the on-disk shader/pipeline cache planned in docs/PLAN.md is mandatory, whichever translation route is chosen.
- Next: see STATE.

### 2026-10-05 ~19:10 — T-030, T-032..T-035, P3 exit review
- Did: Metal replay backend (`tests/replay/metal_backend.mm`): D3D9 state machine -> Metal objects, lazy shader translation with shadow-sampler variants, pipeline/depth/sampler caches, quad clears, StretchRect, PNG dump. `tools/compare_frames.py` (SSIM/MAE + diff images), `scripts/replay_check.sh`. ADR-004/005 proposals.
- Evidence: `re/replay_check.json`, `re/shader_translation_report.json`, `artifacts/replay/<name>/` + `<name>_diff/`; RE_NOTES § Metal replay results.
- **P3 exit criteria (docs/PLAN.md):** "replayed main-menu and in-game traces match golden frames within threshold" — met at SSIM >= 0.95 (menu 0.982, cuba 0.999, vorkuta 0.998, khe_sanh 0.968); "Metal validation layer clean" — no errors/assertions, only redundant-state warnings. -> **P3 exit: PASS**, with three explained/open differences (video textures not recorded; menu red specks; khe_sanh sky/far blur) and one piece of deferred work (shader/pipeline disk cache).
- Learned: first-run fidelity is high because the D3D9 surface is small; Metal forbids swizzled render targets (sample through a view); fragment inputs need `[[stage_in]]`; Metal compiles ~10 generated shaders/s -> caching is mandatory.
- Problems: which side is right for the two open differences cannot be decided without a native D3D9 image.
- Next: user decisions (STATE).

### 2026-10-05 ~19:20 — user decisions: ADR-004/005 accepted, G1 delegated -> go; P4 started
- Did: recorded ADR-004/005 ACCEPTED; G1 decision (go) with rationale; ADR-013 (lifter = Python + capstone -> generated C); P4 task list. User wants the Windows PC kept as a last resort (T-037 parked).
- Next: T-041 lifter first cut + differential tests.

### 2026-10-05 23:32 — T-040, T-041 — integer lifter + differential tests: 11,212/11,212 vectors match Unicorn
- Did: `src/runtime/lifted.h` (Cpu, function-local flags, arithmetic helpers), `tools/lifter/lift.py` (capstone + Ghidra export -> C in `artifacts/lifted/`), `src/runtime/runtime.{h,c}` (4 GiB arena, PE loader, dispatch table, traps), `tests/diff/gen_vectors.py` (Unicorn runs the original bytes from random state), `tests/diff/difftest.c` (same inputs through the lifted code, compares registers + written memory), `scripts/difftest.sh`, `docs/LIFTER.md`.
- Evidence: `re/difftest_report.json` — 15,439 / 21,960 functions lift (774,039 instructions); 2,924 leaf functions tried, 2,867 produced vectors, **11,212 vectors: 11,212 pass, 0 fail**; 45,720 of 76,883 instructions in those functions executed (59.5%). Mutation check: breaking `add` for 1 input in 8 -> 32 of 1,160 vectors fail; breaking `sub` for one input value -> 3 fail.
- Learned: (1) Unicorn must be rebuilt after a faulting run — one bad function made every later run "return" to garbage (0x1f4f…), which first looked like 90% of functions failing. (2) "Leaf" must exclude tail calls (`xfer` in `lifted_meta.json`). (3) What blocks the remaining 30%: `re/lift_coverage.json` -> `blockers_all_instructions` (movss 86k, mulss 26k, fstp 26k, fld 23k, addss 17k, movaps 17k … 217 distinct; MMX `movq` 8.4k).
- Problems / limits (also in docs/LIFTER.md §6): flags at return not compared; 40% of instructions in the tested functions not reached by random inputs; agreement is with Unicorn, not a real CPU; non-leaf functions untested (T-043).
- Next: T-042.

### 2026-10-06 00:22 — T-042 (+ part of T-044) — whole image lifts; FP/SIMD verified on the leaf set
- Did: `tools/lifter/lift_fp.py` (x87 decoded from opcode bytes, SSE/SSE2 scalar+packed, SSE2 integer, MMX, xadd/cmpxchg/cmpxchg8b/bt*/rcl/rcr/pushfd/pushal/cpuid/rdtsc…), `src/runtime/lifted_fp.h` (helpers), function-body repair in `lift.py` (`extend`: shared tails, truncated bodies, code after "noreturn" calls -> 150 functions, 455 blocks), import thunks as indirect jumps, `rep ret`, stable chunking + incremental build, `tools/lifter/flag_audit.py` + `Cpu.xfl`, atomic LOCK ops, vector format v3 (shared heap pool, 4 parallel shards, `leaf|all`), poisoned import slots, `tests/diff/trace_vector.py` + `tests/diff/debug_vector.sh` (instruction-level trace diff), `tools/import_abi.py` -> `re/import_abi.csv` (315 stdcall / 19 cdecl / 0 unknown).
- Evidence: `re/lift_coverage.json` (21,952 / 21,960 functions, 1,588,514 instructions, 0 x87 decode mismatches against capstone, 0 non-atomic locked instructions); `re/difftest_report.json` (4,194 leaf functions tried, 4,082 with vectors, 11,926 vectors: **11,923 pass, 2 differ only in NaN sign/payload, 1 function where the reference is wrong, 0 unexplained**; 107,256 of 167,517 instructions executed = 64%); `re/flag_audit.json`.
- Bugs the tests found in MY code (all fixed): function entry not being the lowest block after body repair (wrong start label); x87 precision control flushing values outside the float exponent range; x87 NaN selection rule (larger significand, not first operand); invalid 80-bit encodings; FSIN/FCOS/FSINCOS/FPTAN out-of-range path; misaligned LOCK operand crashed the host.
- Reference (Unicorn/QEMU) deviations found — NOT bugs in the lifted code, documented in docs/LIFTER.md §6 and `tests/diff/reference_deviations.json`: QEMU applies the x87 NaN rule to SSE (real SSE returns the first operand); QEMU's FYL2X does not pop for ST(0) <= 0.
- Learned: A1 (flags dead across calls) holds for all but 9 functions / 3 call sites; all 430 LOCK-prefixed instructions are xadd/cmpxchg/cmpxchg8b; `CreateDevice` without FPU_PRESERVE -> render thread runs x87 at 24-bit precision on Windows (modelled, runtime must set it); arm64 clang needs `-ffp-contract=off`; the 8 unlifted functions are CRT crash handlers, 2 SSSE3 routines, fnsave/fnstenv users.
- Problems / limits: diff tests cover leaf functions only so far (T-043 next); indirect calls, imports, threads untested until the boot runtime exists; agreement is with Unicorn, whose own FP emulation is imperfect (see deviations) — a real-CPU cross-check is still open (T-046).
- Next: T-043 run, then T-045.

### 2026-10-06 06:10 — T-043 — whole-call-tree differential tests: 0 unexplained failures
- Did: `scripts/difftest.sh 100000 2 5 all` until clean. New since the T-042 entry: `tools/lifter/find_entries.py` (address-taken code -> `re/extra_entries.csv`, 4,344 entries incl. unaligned CRT tables and non-function call targets), interprocedural `flag_audit.py`, FCOMI flag semantics, EFLAGS ID/AC, x87 ZE/IE on division, FNSAVE/FRSTOR/FNSTENV/FLDENV, arena slack above 4 GiB, generator watchdog + exact-site hooks for inputs the reference gets wrong, "fp-range" and unaligned NaN classification, `tests/diff/reference_deviations.json`.
- Evidence: `re/difftest_report_all.json` — 26,304 entries known, 25,990 lifted (1,876,842 instructions; the 314 that do not lift are junk candidates from the entry scan + the 8 known), 24,385 tried, 13,840 with vectors (the rest need imports / live state), **26,294 vectors: 26,275 pass, 1 approx, 6 NaN-only, 5 fp-range, 7 fail — all 9 failing functions explained, 0 unexplained**; 426,308 distinct instructions executed (23% of the lifted image).
- Bugs found in MY code by this pass (all fixed): FCOMI/FUCOMI cleared OF/SF/AF; PUSHFD/POPFD lost EFLAGS.ID (the game's CPUID-presence test would have said "no CPUID"); flags returned by a callee and consumed by the NEXT callee were lost; host crash for accesses straddling 4 GiB; indirect/direct transfers to code that was not a known entry (CRT dispatch tables at odd addresses, local `call label` subroutines).
- Reference/input artefacts (documented, not lifter bugs): QEMU FYL2X/FPREM/F2XM1/trig-of-infinity/0-over-0 behaviour, CPUID/RDTSC, random 80-bit operands, random "function pointers" that are return addresses.
- Problems / limits: 43% of functions never produce a vector under random state (imports, pointers into live structures) — they get exercised only by really running (T-045); coverage of executed instructions is 23%.
- Lost time: one run hung in a generator shard for >40 min (no per-vector limit) and one was 5x slow (thousands of byte-pattern hooks); both fixed in `gen_vectors.py`.
- Next: T-045 (boot), with T-024 inside it.

### 2026-10-06 06:45 — T-045 (partly), T-024 (BLOCKED) — the lifted game boots to the Steam check
- Did: boot runtime (`src/runtime/guest_os.{h,cpp}`: address space, heap, handles, guest threads/TEB/TLS, import trampolines, COM objects; `tests/boot/main.cpp`; `scripts/build_boot.sh`, `scripts/boot_try.sh`), Win32 shims (`src/platform/win32/`: kernel32 x5 files, user32/gdi32 headless, misc DLLs, winsock, null D3D9 with answers from `re/d3d9_adapter_probe.json`, "Steam absent"), import ABI tables (`re/import_abi.csv`, `re/d3d9_abi.csv`), the x86 module interpreter host (`x86_module.cpp`, Unicorn C library from the venv, GDT-based FS), 16-bit PUSHA/POPA in the lifter.
- Evidence: `artifacts/boot/last_run.log` (BO1_NO_STEAM_DLL=1): static initialisers -> WinMain -> "7.0.152 CL(966072) CODPCAB-V64 CEG … SP build win-x86" -> `SteamAPI_Init -> false` -> MessageBox "Steam must be running to play this game." -> ExitProcess(0); 87 distinct imports, 4,947 calls, no trap. With the DLL: loads at 0x10000000, 19/19 game imports bound, DllMain runs, reads steam_emu.ini, then "Internal error 0x0A: Protection initialization failed!".
- Learned: see docs/RE_NOTES.md "Boot path as executed by the lifted image". The first real run of the lifted code needed exactly one lifter addition (PUSHAW/POPAW) and the address-taken entries — nothing else trapped in ~1.9M lifted instructions' worth of start-up code paths.
- Problems: Steam. The shipped DLL is protected and rejects our environment; I stopped at the API boundary (ground rule 1). Details and options: docs/DECISIONS.md.
- Next: user decision; meanwhile only Steam-independent tasks.

### 2026-10-06 07:05 — T-046 — x86_64 cross-check of the lifted code
- Did: compiled `artifacts/lifted/*.c` for x86_64 (-O1, then -O0), ran all 26,294 vectors under Rosetta 2, diffed the verdicts against the arm64 run.
- Evidence: `artifacts/lifted/difftest_x64.log`; docs/LIFTER.md "x86_64 cross-check". -O0: same verdicts except three functions with 1-ulp transcendental differences (host libm). -O1: additionally 0x0092C780 — a miscompile in Apple clang 21.0.0 (x86_64 only), reduced to `tests/diff/compiler_bugs/x86_64_byte_insert_after_load.c` (prints 00000005 instead of 00001205 at -O1/-O2; arm64 correct).
- Learned: the reference emulator computes x87 transcendentals with the host libm, so agreement there is circular; an optimised x86_64 build cannot serve as an oracle with this toolchain.
- Next: user decision on Steam (STATE).

### 2026-10-06 07:15 — Steam decision — user asked for a crack; declined
- User's answer to the ADR-010 spike note: go DRM-free without Steam, "ok if u crack it", take inspiration from the shipped DLL, modify the non-cracking rule.
- Agent: will not write a bypass of the game's Steam check, a Steam emulator, or reverse-engineer the shipped crack; AGENTS.md ground rule 1 not edited. No code changed.
- Open: option A (real Steam through Valve's macOS library) or option B (Steam-independent work only).

### 2026-10-06 07:30 — ADR-014 — user chose real Steam
- User: "use real steam i wanna see the game running". Recorded as ADR-014 (supersedes ADR-010). The shipped steam_api.dll is no longer loaded by default.
- Scope from the game's own code (`re/com_steam_usage.json`): about 25 interface methods on 8 Steam interfaces + the 19 flat imports + callback registration.
- Waiting for: Steam for macOS (installed, logged in) and the Steamworks SDK in `artifacts/steamworks/`. Next: T-047 spike.

### 2026-10-06 06:50 — P5 start, T-050 — live window for the Metal replay
- User approved P5 and SDL3 (already present: 3.4.18). The real-Steam bridge (ADR-014) was withdrawn by the user; its spike files were removed.
- Did: `--window` / `--fps` for `build/replay` (SDL3 window, CAMetalLayer, back buffer blitted to the drawable each Present; Escape or closing the window stops), `scripts/replay_window.sh`.
- Evidence: menu trace frames 1900-2000: "window: 102 frames shown", 156,725 draws executed, 0 skipped, 2.6 s wall; dumped frame `artifacts/replay_window/frame_002000.png` shows the menu room.
- Next: T-051.

### 2026-10-06 06:58 — T-051 — renderer is a library
- Did: `src/rhi/d3d9_call.h` (the call record), `src/rhi/renderer.h` (`bo1::rhi::Renderer`, `make_metal_renderer`), `src/metal/metal_renderer.mm` (moved from tests/replay), CMake target `bo1_metal`; `tests/replay/backend.h` now aliases the shared types.
- Evidence: `scripts/replay_check.sh 0.95` — `re/replay_check.json` identical to the pre-split file (cuba 0.999, vorkuta 0.998, menu 0.983, khe_sanh 0.973 mean SSIM; 0 skipped draws). User ran `scripts/replay_window.sh menu 1500`: 762 frames shown, 1,183,336 draws, 0 skipped.
- Next: T-052.

### 2026-10-06 07:05 — T-036 — shader/pipeline start-up cost measured; no project cache needed yet
- Did: timing of SM3->MSL translation, MSL compile and pipeline creation in the renderer's report line.
- Evidence: menu window (182 functions, 171 pipelines): 7.6 + 29.3 + 15.9 ms; khe_sanh (209, 131): 8.4 + 31.9 + 21.9 ms — warm runs, i.e. served by Metal's system shader cache.
- Learned: only the first-ever run pays the ~10 functions/s compile cost seen in P3; an explicit MTLBinaryArchive is a first-run optimisation -> P7.
- Next: T-052.
