# DECISIONS (ADR log)
Status = ACCEPTED | OPEN | SUPERSEDED. Agents must ask the user before changing an ACCEPTED decision.

## ADR-001 — First target is the Singleplayer campaign — ACCEPTED
MP/Zombies need networking/online services and more middleware. Campaign exercises engine, renderer, audio, input and file loading with the fewest external dependencies.

## ADR-002 — Static recompilation first, manual decompilation incrementally — ACCEPTED
Full manual decompilation is the end-state at best, not the starting point. Lift -> run -> replace hot/valuable modules (P6).

## ADR-003 — 32-bit guest address space inside a 64-bit host process — ACCEPTED
Lifted code uses `uint32_t` guest pointers over a reserved arena + base register. Avoids rewriting every struct for 64-bit pointers. Clean 64-bit pointers arrive only in `src/native/` modules.

## ADR-004 — Renderer interface shape — OPEN (decide in P3)
(a) D3D9-semantics shim over Metal (fast integration, less optimal) vs (b) minimal RHI with translation at the edge (cleaner, more work). Decide using the D3D9 census.

## ADR-005 — Shader translation route — OPEN (decide in P3)
(a) own SM2/3 -> MSL translator vs (b) SM3 -> SPIR-V -> SPIRV-Cross -> MSL. Evaluate on real shader blobs: correctness, compile time, cache size, maintainability.

## ADR-006 — Platform layer: SDL3 vs raw AppKit/GameController — ACCEPTED 2026-10-05 (proposed and approved by the user the same day)
**Proposal: SDL3** for window, events, keyboard/mouse, gamepads, clipboard, message boxes and the audio device; the Metal layer is taken from SDL (`SDL_Metal_CreateView` -> `CAMetalLayer`) and the renderer never calls SDL after that.
**Evidence (RE_NOTES § Platform map):** the game needs one window with a Win32-style message pump (`PeekMessageA` once per frame), polled cursor position, keyboard through window messages, three XInput calls, a clipboard read and message boxes — all direct SDL3 features; nothing in the import census needs AppKit-only behaviour. FAudio (ADR-009) sits on SDL anyway, so SDL is a dependency either way. SDL3 3.4.18 is in Homebrew.
**Cost of the alternative:** raw AppKit + GameController means writing the event translation, relative-mouse handling, controller mapping and fullscreen/Spaces handling ourselves, for no measured benefit at this stage.
**Escape hatch:** SDL exposes the `NSWindow`; ProMotion pacing or fullscreen behaviour that SDL cannot express (P7/P8) can be done on it directly.

## ADR-007 — Reference baseline stack — ACCEPTED (revised 2026-10-05 15:55 after testing; user delegated the choice and approved each download)
**Decision:** **WineCX 24.0.7** — the open-source build of CrossOver 24's Wine (`WS12WineCX24.0.7_7.tar.xz` from github.com/Sikarugir-App/Engines, SHA-256 `203f9e9f…445fd1c`, reports `wine-9.0 (SikarugirCX 24.0.7)`), x86_64 under Rosetta 2, WoW64 mode, D3D9 -> its **wined3d (OpenGL)**. Unpacked in `artifacts/wine/cx/`, prefix `artifacts/wineprefix-cx`, launched with `scripts/run_baseline.sh --wine cx --net`. Support dylibs (libinotify, libintl, freetype, gnutls…) are borrowed from the WineHQ build in `artifacts/wine/Wine Staging.app`.
**Why:** the only stack tested on this Mac on which the game reaches and holds the main menu (2 of 2 runs, 2,276–2,496 frames, no crash). Free and scriptable.
**Rejected after testing (details: RE_NOTES § Baseline status):**
- WineHQ wine-devel / wine-staging 11.18 + wined3d-GL: ~60 launches, most die in Wine's 64-bit WoW64/OpenGL layer; never past the title screen.
- Same + wined3d Vulkan backend: `vkCreateBufferView` crash, 3/3.
- Same + DXVK-macOS 1.10.3: device creation fails, stock MoltenVK lacks geometryShader / shaderCullDistance, 3/3.
- CrossOver.app supplied by the user: not executed (modified main executable, ad-hoc signature).
**Run conditions:** network allowed (`--net`): with the sandbox denying network the game hits a fatal error at the title screen. Prefix `Z:` drive removed, `G:` = `artifacts/game`.
**Option noted:** the user has a Windows PC available; the proxy is an ordinary Win32 DLL, so a native-D3D9 trace/golden set can be captured there if the Wine baseline's rendering is ever in doubt.

## ADR-008 — Bink video: FFmpeg-backed shim, not the original DLL — ACCEPTED 2026-10-05 (proposed and approved by the user the same day)
**Proposal:** replace `binkw32.dll` (18 imports) with a host shim built on FFmpeg's `bink` / `binkaudio_dct` decoders. Do not lift the DLL.
**Evidence:** 17 of the 18 Bink functions are called in normal play (`BinkDoFrame` ~1,700 times per mission run) — Bink is on the critical path for the menu TVs and loading screens. All 50 shipped files are `BIKi` 1280x720; FFmpeg 9.0.2 decodes them (300 frames of `treyarch.bik` in 0.44 s CPU on the M1, yuv420p output of the expected size). The game supplies its own planes (`BinkRegisterFrameBuffers` / `BinkGetFrameBuffersInfo`) and uploads them as L8 textures (trace: DYNAMIC L8 1280x720 + 640x360), so the shim's job is "decode frame N into three guest buffers" plus timing (`BinkWait`, `BinkGetRealtime`, `BinkShouldSkip`) and audio.
**Open work (P5):** the layout of the `BINK` and `BINKFRAMEBUFFERS` structures the game reads directly (field offsets to be taken from the five game functions at 0x006D9000–0x006DA290), background-I/O semantics, audio routed into the XAudio2 shim instead of DirectSound.
**Alternative kept in reserve:** lift `binkw32.dll` too (174 KB of hand-optimised MMX/SSE code with its own imports) — more lifter work and no arm64-native decoder at the end.
**Licence note:** FFmpeg's Bink decoders are LGPL; Homebrew's ffmpeg is a GPL build, fine for personal use. A redistributable build would need an LGPL configuration.

## ADR-009 — Audio: XAudio2 2.7 implemented with FAudio — ACCEPTED 2026-10-05 (proposed and approved by the user the same day)
**Proposal:** guest-visible `IXAudio2` / voice COM objects (HOST_ABI §4.3) backed by **FAudio** (zlib licence, the XAudio2 re-implementation Wine itself uses), output through SDL3 -> CoreAudio. DirectSound (`DSOUND` #11/#6) and the WinMM mixer are stubbed.
**Evidence:** the only COM class the exe ever creates is XAudio2 2.7 (both `CoCreateInstance` sites pass `CLSID_XAudio2` {5a508685-…}; baseline loads `xaudio2_7.dll`); sound assets are MS-ADPCM, which XAudio2/FAudio decode natively; the exe contains its own XAPO effect classes (`CXAPOBase` RTTI) and `IXAudio2VoiceCallback` implementations, both of which FAudio supports as callbacks (FAPO / voice callbacks) — they become host->guest calls on the audio thread (HOST_ABI §5). FAudio 26.10 is in Homebrew.
**Why not write a mixer:** resampling, ADPCM, submix graphs, 3D/X3DAudio and effect chains already exist and are tested in FAudio against real games.
**Risk:** real-time callbacks into lifted code; X3DAudio usage not yet confirmed (no import — it would be statically linked math).

## ADR-010 — Steamworks: run the shipped `steam_api.dll` unmodified in an embedded x86 emulator — ACCEPTED 2026-10-05 (user decision; replaces the "bridge to the real Steam client" proposal)
**User decision:** no Steam client will be installed. The port must keep using the `steam_api.dll` that ships in the reference copy, as the Wine baseline already does.
**Context:** the SP exe imports 19 `steam_api` functions and polls them every frame (`SteamUser()` ~28,000 calls per mission run); if `SteamAPI_Init` fails the game exits with "Steam must be running". The shipped DLL is a packed, self-modifying 32-bit x86 PE (sections `WUS0`/`WUS1`, entropy 7.93), so it cannot be statically recompiled like the game.
**Design:** load the DLL **byte-for-byte unmodified** into the guest arena and execute it with an embedded x86 interpreter/JIT (Unicorn is already in the toolbox for differential tests). Calls from lifted game code into the DLL (the 19 exports and the C++ interface vtables it returns) leave the lifted world through the dispatcher's "target not lifted" path into the interpreter; Windows calls made by the DLL go to the same host shims as the game's (trampoline zone, HOST_ABI §4); calls from the DLL back into game code (Steam callbacks) re-enter lifted code through the dispatcher. This is the same thing Wine does for it, with our shims in place of Wine's.
**Boundaries (unchanged from ground rule 1, and mine):** the DLL is not unpacked, patched, disassembled for its protection logic, or re-implemented; no Steam emulator is written in this project. If hosting it unmodified turns out not to work, the options go back to the user — they are not worked around.
**Containment:** the interpreter gives a natural choke point: the DLL only reaches the host through shims, so its file access is confined to the game directory and the emulated `C:\Users\Public\Documents\Steam\…` folder, and its sockets go through ADR-011's layer with a per-module switch (default: no network for this module).
**Unknowns to resolve with a spike before P5 depends on it:** which Windows APIs the DLL needs beyond the game's 334 (it resolves most of them dynamically after unpacking — measure in the baseline with Wine's `+relay` restricted to that module or by logging `GetProcAddress`); whether it creates threads or relies on SEH / self-debugging tricks; whether Unicorn's speed is adequate for ~10 calls per frame (expected: yes).
**Consequences:** the runtime gains an interpreter path (which also gives a fallback for the 0.8% of game code not recovered statically — to be decided separately); x86 emulation stays in the shipped process for this one module; Steam features (achievements, friends, cloud) remain whatever the shipped DLL provides.

## ADR-011 — Networking in single player: real sockets, failures tolerated — ACCEPTED 2026-10-05 (proposed and approved by the user the same day)
**Evidence:** even in SP the game creates sockets, resolves hosts and polls a bound UDP socket (`recvfrom` ~8,700 calls per run); Demonware log-on fails continuously in the baseline ("Failed to log on." ~1,000 times) without consequence, but when the baseline's sandbox *refused* connections the game raised a fatal error at the title screen.
**Proposal:** implement the 21 Winsock imports as a thin layer over BSD sockets with faithful error codes (`WSAECONNREFUSED`, `WSAEWOULDBLOCK`, …), allowed to use the network. Add an "offline" switch that makes name resolution fail the way an unplugged Windows machine does, and verify the game tolerates that, before relying on it.

## ADR-012 — Exceptions: support the thread-name raise only, until a real path needs more — ACCEPTED 2026-10-05 (proposed and approved by the user the same day)
**Evidence:** `RaiseException` is called exactly 13 times per run (= number of threads created; LIKELY the 0x406D1388 thread-naming convention), `RtlUnwind` and `UnhandledExceptionFilter` never, across three boot-to-mission runs (RE_NOTES § Platform map).
**Proposal:** P4 implements `RaiseException(0x406D1388)` as a no-op and turns every other raise or hardware fault into a crash report with the guest EIP resolved through `re/symbols.csv`. Full SEH/C++ unwinding with landing in lifted code (HOST_ABI §7) is deferred until a gameplay path is shown to need it.
**Risk accepted:** error-handling paths that unwind (e.g. `Com_Error(ERR_DROP)` back to the main loop — seen as the "Fatal Error" message box when the baseline's network was blocked) will terminate instead of recovering.
