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

## ADR-007 — Reference baseline stack — OPEN (decide in P1)
CrossOver vs vanilla Wine + wine32on64; DXVK/MoltenVK vs wined3d. Pick whichever is most stable and scriptable.
