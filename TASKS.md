# TASKS
Status: TODO · NOW · DONE · BLOCKED. Only ONE task is NOW at a time. Mark DONE only with an Evidence line.
Update this file at every checkpoint (see AGENTS.md rule 2).

## NOW
- **T-031** (P3) Trace reader + replay harness skeleton (`tests/replay/`), null backend. In parallel: T-030 (ADR-004/005 proposals from the census).

## DONE
- **T-001** (P0) DONE 2026-10-05 — game install present in `reference/`, `scripts/doctor.sh` run, tool availability recorded.
  - Evidence: doctor.sh output in LOG entry 2026-10-05 13:40. (chmod read-only on `reference/` deferred until the reference copy is settled.)

## P0 — Recon & inventory
- [x] **T-002** DONE 2026-10-05 (evidence: `re/inventory.json`, `re/inventory.md`, RE_NOTES § Binaries; commit b0be0c7). Script `tools/pe_inventory.py`: SHA-256, size, PE headers, sections, imports/exports, TLS, Rich header for every exe/dll in `reference/`. Output `re/inventory.json` + RE_NOTES § Binaries.
- [x] **T-003** DONE 2026-10-05 (evidence: `re/imports.csv` 542 rows, RE_NOTES § Imports; commit b0be0c7). Import census -> `re/imports.csv` + RE_NOTES § Imports (group by DLL; flag d3d9/d3dx9, audio, input, net, steam, bink).
- [x] **T-004** DONE 2026-10-05 — not packed/wrapped (evidence: RE_NOTES § Protection / packing check: entry in .text, max window entropy 6.79, clean linear decode). Open risk logged for P1. Packing/protection check on `BlackOps.exe`. If packed/wrapped -> BLOCKED + tell user.
- [x] **T-005** DONE 2026-10-05 (evidence: `re/float_census.json`, RE_NOTES § Float model; commit b0be0c7). Float-model census (count x87 vs SSE/SSE2 instructions via capstone) -> RE_NOTES § Float model.
- [x] **T-006** DONE 2026-10-05 (evidence: `re/ghidra_summary.json` — 17,490 functions, 211,515 blocks; `scripts/ghidra_analyze.sh` one-command; `re/coverage_gaps.json`). Ghidra headless project + export script (functions, basic blocks, jump tables, xrefs, strings) -> `re/` ; one-command reproducible.
- [x] **T-007** DONE 2026-10-05 (evidence: RE_NOTES § Middleware table; sound codec still unidentified -> follow-up under T-009). Identify middleware (video/audio/physics/network) from imports, strings, bundled DLLs -> RE_NOTES § Middleware.
- [x] **T-008** DONE 2026-10-05 (evidence: RE_NOTES § Asset formats: 146 ff headers, 47 IWD listings, IWI/Bink header samples). Asset layout survey (fastfiles/IWD/dirs; compression; sizes) -> RE_NOTES § Asset formats.
- [x] **T-009** DONE 2026-10-05 (evidence: `re/symbols.csv` 52 entries, `re/xrefs_anchor.csv`; Com_Frame / CreateDevice site still open -> P1 trace). Locate key entry points (WinMain, main loop, R_Init/R_EndFrame equivalents via D3D9 import xrefs) -> `re/symbols.csv` with confidence tags.
- **P0 exit review** — DONE 2026-10-05, see LOG. **Waiting for user go-ahead for P1** (and ADR-007 choice).
- [x] **T-009a** DONE 2026-10-05 (evidence: `re/seeds.csv` 4,391 seeds; `re/coverage_gaps.json`: unreached likely code 10.1% -> 0.8%; functions 17,490 -> 21,960). Seed unreached code, re-run Ghidra, re-measure coverage.

## P1 — Reference baseline
- [x] **T-010** DECIDED 2026-10-05 (ADR-007 ACCEPTED: wine-stable 11.0 + wined3d under Rosetta 2, project-local). Install pending user action; versions to be recorded on first run. Choose baseline stack (ADR-007): install CrossOver or Wine+wine32on64 (ASK USER before installing). Record versions.
- [x] **T-011** DONE 2026-10-05 (evidence: `tests/golden/baseline_cx_main_menu_f1350_800x600.png`, `artifacts/baseline/d3d9_census-20261005-155243.txt` — main menu rendered on WineCX 24.0.7; `scripts/run_baseline.sh --wine cx --net --trace --keys "32000:0x20"`).
- [x] **T-012** DONE 2026-10-05 (evidence: `artifacts/baseline/d3d9_census-20261005-143041.txt` — 886 frames, 37/119 device methods, call names + counts; RE_NOTES § D3D9 census). Also does back-buffer capture and optional import-table diagnostics. `tools/d3d9_proxy/`: d3d9.dll proxy logger (32-bit PE built with mingw-w64 or clang --target=i686-windows). First: log call names only.
- [x] **T-013** DONE 2026-10-05 (evidence: `docs/TRACE_FORMAT.md`; `artifacts/traces/trace-20261005-161500.bin` — 16.9 M records, 2,282 frames, parsed end-to-end by `tools/trace_reader.py`; 1,592 shader blobs extracted). Extend proxy: full args, shader bytecode blobs, texture/surface descriptors, resource uploads -> binary trace format (document in docs/).
- [x] **T-014** DONE 2026-10-05 (evidence: `scripts/capture_golden.sh` / `capture_golden_all.sh`; `tests/golden/{menu,cuba,vorkuta,khe_sanh}/` = 4 PNG frames + `metrics.json` each; traces `artifacts/traces/<name>.bin`; per-frame CSV `artifacts/baseline/frametimes-*.csv`). Golden capture script: main menu, loading, scenes A/B/C + frame-time CSV.
- [x] **T-015** DONE 2026-10-05 (evidence: `re/d3d9_census_{menu,cuba,vorkuta,khe_sanh}.json`; RE_NOTES § D3D9 census — consolidated).
- **P1 exit review** — DONE 2026-10-05 (LOG). Waiting for user go-ahead for P2.

## P2 — Platform shim design
- [x] **T-020** DONE 2026-10-05 (evidence: `re/platform_map.csv` — 334 imports, static sites + runtime calls from 3 baseline runs; RE_NOTES § Platform map). Map every import to {shim | lift | stub | not needed} -> RE_NOTES § Platform map.
- [x] **T-021** DONE 2026-10-05 — PROPOSED ADR-008 (Bink/FFmpeg), ADR-009 (XAudio2/FAudio), ADR-010 (Steam bridge), ADR-011 (network), ADR-012 (exceptions); awaiting user approval. Middleware decisions (Bink via FFmpeg? audio backend?) -> DECISIONS.
- [x] **T-022** DONE 2026-10-05 — ADR-006 PROPOSED: SDL3; awaiting user approval. ADR-006 (SDL3 vs AppKit) — propose with evidence, ask user.
- [x] **T-023** DONE 2026-10-05 (evidence: `docs/HOST_ABI.md`). Specify host-call ABI (guest -> host trampolines, guest-visible COM vtables).

## P3 — Metal renderer (isolated)
- [ ] **T-030** ADR-004 / ADR-005 proposals backed by the census; ask user.
- [ ] **T-031** Trace reader + replay harness skeleton (`tests/replay/`), null backend.
- [ ] **T-032** Metal backend: device/swapchain (CAMetalLayer), buffers, textures, samplers.
- [ ] **T-033** Shader translation prototype on 10 real shaders; expand to full corpus; disk cache.
- [ ] **T-034** Render-state -> PSO mapping + cache; first triangle from trace; then main menu frame.
- [ ] **T-035** Golden-frame comparison (SSIM + diff image) automated in CI script.
- **P3 exit review + Gate G1 with user.**

- [ ] **T-024** (P2 follow-up, before P5) Spike for ADR-010: measure which Windows APIs the shipped `steam_api.dll` calls (baseline, `GetProcAddress` log / relay for that module), then run its `DllMain` + `SteamAPI_Init` under Unicorn with stub shims. Evidence: list of APIs + init returning.

## P4+ (expand into tasks only after G1)
Lifter design doc -> M4a/M4b/M4c -> integration -> strangler fig -> Metal optimization -> QA. See docs/PLAN.md.
