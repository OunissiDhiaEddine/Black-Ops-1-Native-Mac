# LOG

## STATE  (OVERWRITE this block at every checkpoint — it is the resume point)
- **Date/time:** 2026-10-05 ~16:00
- **Phase / task:** P1 — T-010, T-011, T-012 DONE. **NOW: T-013** (full-argument trace). Then T-014 (golden captures), T-015 (census).
- **Last completed:** stable baseline found: WineCX 24.0.7 (ADR-007 revised); main menu reached and captured.
- **Standard baseline command:** `WINEDEBUG=-all,warn+debugstr scripts/run_baseline.sh --wine cx --net --trace --capture 150 --seconds 90 --keys "32000:0x20"` (Space at 32 s passes "Press Any Key"; add more `ms:vk` pairs to drive menus: 0x0D enter, 0x26/0x28 up/down, 0x1B esc).
- **Next exact step:** T-013 — add per-method argument decoders + post-hooks to `tools/d3d9_proxy` and a binary trace writer; then try reaching a mission (menu keys or `+devmap cuba`) for T-014.
- **How to read a run:** `artifacts/baseline/d3d9_census-<stamp>.txt`, `run-<stamp>.log` (`debugstr` = engine console), `frames-<stamp>/` (back buffers). `--shots` only works while the game window is on the visible Space.
- **Files in flight:** none uncommitted.
- **Open questions for the user:** none blocking. (User offered a Windows PC — optional native-D3D9 ground truth later.)
- **Half-done / how to resume:** nothing half-done. WineHQ prefixes (`artifacts/wineprefix`, `-staging`) are obsolete and can be deleted.
- **Machine:** Apple M1 (arm64), 8 GB RAM, macOS 27.0.1 (26A434), Xcode 27.0, ~51 GB free. Rosetta 2 installed; Screen Recording granted. brew: cmake, ninja, ffmpeg, sevenzip, ghidra 12.1.4, openjdk@21, mingw-w64 14.0.0. Wine: `artifacts/wine/cx/wswine.bundle` (WineCX 24.0.7, baseline), `Wine Devel.app` / `Wine Staging.app` (11.18; kept for their support dylibs). DXVK-macOS in `artifacts/dxvk` (unused).
- **Reference copy:** user confirmed (2026-10-05) they own BO1; `reference/` read-only; working copy `artifacts/game` (APFS clone).

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
