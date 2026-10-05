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

## ADR-007 — Reference baseline stack — ACCEPTED (2026-10-05; user delegated: "choose the most optimized free way")
**Decision:** WineHQ **wine-stable 11.0** for macOS (x86_64 build by Gcenx, the packaging WineHQ's macOS page and the Homebrew casks point to), running in new-WoW64 mode under **Rosetta 2**, D3D9 -> **wined3d** (Wine's built-in translation). Unpacked into `artifacts/wine/` (project-local, nothing in /Applications), prefix in `artifacts/wineprefix/`, launched only via `scripts/run_baseline.sh`.
**Why:** free; scriptable from the CLI; runs 32-bit PE on Apple Silicon without 32-bit host libraries; wined3d needs no extra components. Used only as an oracle (golden frames, D3D9 trace) — performance of the baseline is irrelevant, correctness and repeatability matter.
**Alternatives considered:** CrossOver (paid); Apple Game Porting Toolkit (D3DMetal targets D3D10+, download needs an Apple developer login); DXVK-macOS over MoltenVK (keep as fallback if wined3d misrenders: extra moving parts); Homebrew casks `wine-stable`/`wine@staging` (disabled by Homebrew on 2026-09-01: builds are not notarized and fail Gatekeeper).
**Constraints found (2026-10-05):** Rosetta 2 is **not installed** on this Mac (`arch -x86_64 /usr/bin/true` fails) and every macOS Wine build needs it. Wine builds are unsigned/un-notarized.
**Containment:** the prefix's `Z:` drive (whole filesystem) is removed; only `G:` -> `artifacts/game` is mapped; outbound network is denied with `sandbox-exec` unless `--net` is passed. Reason: the install's `steam_api.dll` is a packed third-party binary of unknown provenance.
