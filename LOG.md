# LOG

## STATE  (OVERWRITE this block at every checkpoint — it is the resume point)
- **Date/time:** 2026-10-05 (P1 started)
- **Phase / task:** P1 — T-010 decided (ADR-007 ACCEPTED), **blocked on two user actions** before T-011; T-012 proxy v0 built but untested.
- **Last completed:** `scripts/run_baseline.sh`, `tools/d3d9_proxy/` (builds to `artifacts/d3d9_proxy/d3d9.dll`), ADR-007, `reference/` made read-only, working copy cloned to `artifacts/game` (APFS clone, 0 extra bytes).
- **Next exact step (after the user answers):**
  1. User installs Rosetta 2 themselves: `softwareupdate --install-rosetta` (licence prompt is theirs to accept). Verify: `arch -x86_64 /usr/bin/true`.
  2. With the user's yes: `mkdir -p artifacts/wine && curl -L -o artifacts/wine/wine-stable-11.0_1-osx64.tar.xz https://github.com/Gcenx/macOS_Wine_builds/releases/download/11.0_1/wine-stable-11.0_1-osx64.tar.xz` (185,303,032 B), check SHA-256 against `brew info --cask --json=v2 wine-stable`, `tar -xJf` into `artifacts/wine/`.
  3. `scripts/run_baseline.sh` (T-011: main menu renders -> screenshot into `tests/golden/`), then `scripts/run_baseline.sh --trace` (T-012 evidence: `artifacts/baseline/d3d9_census-*.txt`).
- **Files in flight:** none uncommitted.
- **Open questions for the user:** (a) OK to download the Wine build above? (b) OK that I launch the game (incl. its third-party `steam_api.dll`) under Wine with the containment in ADR-007, or do they prefer to launch `scripts/run_baseline.sh` themselves?
- **Half-done / how to resume:** `run_baseline.sh` and the proxy have never been executed — expect fixes on first run (sandbox profile syntax, `+set r_fullscreen 0` handling, UCRT imports of the proxy under Wine).
- **Machine:** Apple M1 (arm64), 8 GB RAM, macOS 27.0.1 (26A434), Xcode 27.0, ~53 GB free. **Rosetta 2 not installed.** Installed via brew (user-approved): cmake, ninja, ffmpeg 9.0.2, sevenzip (`7zz`), ghidra 12.1.4, openjdk@21, mingw-w64 14.0.0. Python tooling in `.venv`.
- **Reference copy:** user confirmed (2026-10-05) they own BO1 and that the DRM-free copy in `reference/` is the project reference. `reference/` is now read-only (`chmod -R a-w`).

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
