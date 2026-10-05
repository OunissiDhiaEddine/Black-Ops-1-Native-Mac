# TASKS
Status: TODO · NOW · DONE · BLOCKED. Only ONE task is NOW at a time. Mark DONE only with an Evidence line.
Update this file at every checkpoint (see AGENTS.md rule 2).

## NOW
- **WAITING ON USER** — reference-copy provenance + tool installs (LOG STATE, open questions 1–2). T-002 starts once answered.

## DONE
- **T-001** (P0) DONE 2026-10-05 — game install present in `reference/`, `scripts/doctor.sh` run, tool availability recorded.
  - Evidence: doctor.sh output in LOG entry 2026-10-05 13:40. (chmod read-only on `reference/` deferred until the reference copy is settled.)

## P0 — Recon & inventory
- [ ] **T-002** Script `tools/pe_inventory.py`: SHA-256, size, PE headers, sections, imports/exports, TLS, Rich header for every exe/dll in `reference/`. Output `re/inventory.json` + RE_NOTES § Binaries.
- [ ] **T-003** Import census -> `re/imports.csv` + RE_NOTES § Imports (group by DLL; flag d3d9/d3dx9, audio, input, net, steam, bink).
- [ ] **T-004** Packing/protection check on `BlackOps.exe`. If packed/wrapped -> BLOCKED + tell user.
- [ ] **T-005** Float-model census (count x87 vs SSE/SSE2 instructions via capstone) -> RE_NOTES § Float model.
- [ ] **T-006** Ghidra headless project + export script (functions, basic blocks, jump tables, xrefs, strings) -> `re/` ; one-command reproducible.
- [ ] **T-007** Identify middleware (video/audio/physics/network) from imports, strings, bundled DLLs -> RE_NOTES § Middleware.
- [ ] **T-008** Asset layout survey (fastfiles/IWD/dirs; compression; sizes) -> RE_NOTES § Asset formats.
- [ ] **T-009** Locate key entry points (WinMain, main loop, R_Init/R_EndFrame equivalents via D3D9 import xrefs) -> `re/symbols.csv` with confidence tags.
- **P0 exit review** -> LOG entry, ask user to proceed.

## P1 — Reference baseline
- [ ] **T-010** Choose baseline stack (ADR-007): install CrossOver or Wine+wine32on64 (ASK USER before installing). Record versions.
- [ ] **T-011** `scripts/run_baseline.sh`: repeatable launch of SP; confirm main menu renders. Evidence: screenshot in `tests/golden/`.
- [ ] **T-012** `tools/d3d9_proxy/`: d3d9.dll proxy logger (32-bit PE built with mingw-w64 or clang --target=i686-windows). First: log call names only.
- [ ] **T-013** Extend proxy: full args, shader bytecode blobs, texture/surface descriptors, resource uploads -> binary trace format (document in docs/).
- [ ] **T-014** Golden capture script: main menu, loading, scenes A/B/C + frame-time CSV.
- [ ] **T-015** D3D9 census report -> RE_NOTES § D3D9 census.
- **P1 exit review.**

## P2 — Platform shim design
- [ ] **T-020** Map every import to {shim | lift | stub | not needed} -> RE_NOTES § Platform map.
- [ ] **T-021** Middleware decisions (Bink via FFmpeg? audio backend?) -> DECISIONS.
- [ ] **T-022** ADR-006 (SDL3 vs AppKit) — propose with evidence, ask user.
- [ ] **T-023** Specify host-call ABI (guest -> host trampolines, guest-visible COM vtables).

## P3 — Metal renderer (isolated)
- [ ] **T-030** ADR-004 / ADR-005 proposals backed by the census; ask user.
- [ ] **T-031** Trace reader + replay harness skeleton (`tests/replay/`), null backend.
- [ ] **T-032** Metal backend: device/swapchain (CAMetalLayer), buffers, textures, samplers.
- [ ] **T-033** Shader translation prototype on 10 real shaders; expand to full corpus; disk cache.
- [ ] **T-034** Render-state -> PSO mapping + cache; first triangle from trace; then main menu frame.
- [ ] **T-035** Golden-frame comparison (SSIM + diff image) automated in CI script.
- **P3 exit review + Gate G1 with user.**

## P4+ (expand into tasks only after G1)
Lifter design doc -> M4a/M4b/M4c -> integration -> strangler fig -> Metal optimization -> QA. See docs/PLAN.md.
