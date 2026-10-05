# RE NOTES
Confidence tags: CONFIRMED (verified by test/trace) · LIKELY · GUESS. Always say how it was verified.

## Binaries
| File | SHA-256 | Size | Arch | Compiler/linker | Notes |
|---|---|---|---|---|---|
| BlackOps.exe | 2ba4a04e81de2b60582da0e4489efde21e6cb07904a223bae633b6ccaebbf759 | 8,099,608 | PE32 i386 GUI | TBD (T-002) | mtime 2016-12-25; from CODEX release, not verified against the Steam depot |
| binkw32.dll | ad55ec64a893f233ac648ac974562f53736003b17bfd14527e66b3ffc8c2cdcb | 174,080 | PE32 i386 DLL | TBD | Bink video |
| steam_api.dll | d1757479242125561628f75864eaef3118f7bf5b6b136cf3681c21e2da612b48 | 255,776 | PE32 i386 DLL | TBD | LIKELY the CODEX Steam emulator, not Valve's DLL (configured by steam_emu.ini) |
| steam_api.cdx | 9efe2c8336325f6f186af073213b84f6258643517d175bc8432e196b70722394 | 124,712 | PE32 i386 DLL | TBD | GUESS: original Valve steam_api.dll renamed by the release, or an emulator component — unverified |

(Preliminary rows from `shasum -a 256` + `file`, 2026-10-05. T-002 replaces this with `tools/pe_inventory.py` output.)

## Imports (census) — see re/imports.csv
## Middleware (video, audio, physics, networking, Steamworks)
## Float model (SSE2 vs x87)
## D3D9 census (functions, states, formats, shader models, per-frame call volumes)
## Platform map (every import -> shim | lift | stub | not needed)
## Engine structures (name, VA of known instances, layout, confidence)
## Function index (WinMain, Com_Init, R_Init, R_EndFrame, FS_*, Sys_*, ...)
## Asset formats
## Gotchas / surprises
- **Reference copy is a CODEX scene release, not a Steam install — CONFIRMED** (2026-10-05; read `steam_emu.ini`: CODEX banner, `UserName=CODEX`, `[Crack]` section with 5 key=value pairs, `DLCUnlockall=1`; `~Common Redist/aio_installer.bat` is repack tooling). Consequences: hashes will not match the Steam depot; `steam_api.dll` is an emulator, so its exports/behaviour are not Valve's; `BlackOps.exe` may carry third-party patches (LIKELY — same 2016-12-25 mtime as the emulator files; not yet diffed against anything). Whether this copy stays the project reference is an open question for the user (LOG STATE).
- `BlackOps.exe` contains the strings `Valve Corp.0` / `Valve Corp.1` (`strings -a | grep`), i.e. an Authenticode certificate naming Valve is embedded — GUESS: retail exe was Valve-signed; signature validity not checked (T-004).
- No `BlackOpsMP.exe` in this install (installscript.vdf references one). Fine for ADR-001 (SP first).
