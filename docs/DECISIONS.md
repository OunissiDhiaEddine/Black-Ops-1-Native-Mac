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

## ADR-006 — Platform layer: SDL3 vs raw AppKit/GameController — OPEN (decide in P2)

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
