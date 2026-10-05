# LOG

## STATE  (OVERWRITE this block at every checkpoint — it is the resume point)
- **Date/time:** 2026-10-05 ~14:00
- **Phase / task:** P0 — T-001..T-005, T-007, T-008 DONE. **T-006 in progress** (Ghidra headless analysis running), T-009 preliminary.
- **Last completed:** fastfile/shader survey (commit after 16f8476).
- **Next exact step:**
  1. If `re/ghidra_summary.json` does not exist: `GHIDRA_HEADLESS_MAXMEM=5G scripts/ghidra_analyze.sh` (3G heap OOMs in X86Analyzer; watch `artifacts/ghidra/analyze.log`). If the project exists but export is missing: `scripts/ghidra_analyze.sh --export`.
  2. `.venv/bin/python tools/find_xrefs.py` (re-attributes xrefs with Ghidra function starts) -> write `re/symbols.csv` + RE_NOTES § Function index (T-009).
  3. P0 exit review entry in LOG, then ask user before P1.
- **Files in flight:** none uncommitted except Ghidra outputs (gitignored).
- **Open questions for the user:** P1 needs a Windows-capable baseline (CrossOver or Wine) — ADR-007; ask before installing.
- **Half-done / how to resume:** T-009 function index currently uses heuristic function starts (0xCC padding) — redo with Ghidra data before trusting addresses.
- **Machine:** Apple M1 (arm64), 8 GB RAM, macOS 27.0.1 (26A434), Xcode 27.0, ~56 GB free. Installed this session (user-approved): cmake, ninja, ffmpeg 9.0.2, sevenzip (`7zz`), ghidra 12.1.4 (formula, `/opt/homebrew/opt/ghidra/libexec`), openjdk@21.
- **Reference copy:** user confirmed (2026-10-05) they own BO1 and that the DRM-free copy in `reference/` is the project reference.

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
