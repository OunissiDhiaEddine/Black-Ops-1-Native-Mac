# TASKS
Status: TODO · NOW · DONE · BLOCKED. Only ONE task is NOW at a time. Mark DONE only with an Evidence line.
Update this file at every checkpoint (see AGENTS.md rule 2).

## NOW
- **T-052** (P5) SDL3-backed user32 (window, messages, keyboard/mouse, gamepad) replacing `user32_headless.cpp`.

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
- [x] **T-030** DONE 2026-10-05 — both PROPOSED with evidence in docs/DECISIONS.md; awaiting user approval. ADR-004 / ADR-005 proposals backed by the census; ask user.
- [x] **T-031** DONE 2026-10-05 (evidence: `build/replay artifacts/traces/{menu,cuba,vorkuta,khe_sanh}.bin` — 17–25 M records each, 6–7 M records/s, null backend reports 0 unknown objects / 0 wrong kinds / 0 unmatched unlocks / 0 missing uploads). Trace reader + replay harness skeleton (`tests/replay/`), null backend.
- [x] **T-032** DONE 2026-10-05 as the replay backend `tests/replay/metal_backend.mm` (offscreen target instead of CAMetalLayer; the windowed swap chain comes with P5). Evidence: `scripts/replay_check.sh`. Metal backend: device/swapchain (CAMetalLayer), buffers, textures, samplers.
- [x] **T-033** DONE 2026-10-05 (evidence: `re/shader_translation_report.json` — 6,845/6,845 translate and compile). Disk cache NOT done -> T-036. Shader translation prototype on 10 real shaders; expand to full corpus; disk cache.
- [x] **T-034** DONE 2026-10-05 (evidence: menu + 3 mission frames rendered, 0 skipped draws; RE_NOTES § Metal replay results). Render-state -> PSO mapping + cache; first triangle from trace; then main menu frame.
- [x] **T-035** DONE 2026-10-05 (evidence: `scripts/replay_check.sh` exit 0 at SSIM >= 0.95; `re/replay_check.json`). Golden-frame comparison (SSIM + diff image) automated in CI script.
- [ ] **T-036** Persistent shader/pipeline cache (translated MSL by bytecode hash + MTLBinaryArchive); target: second run of a scenario compiles nothing.
- [ ] **T-037** Settle the two open differences (menu red specks, khe_sanh sky/far blur) against native D3D9 frames from the user's Windows PC (the proxy DLL works on Windows unchanged).
- [ ] **T-038** Record video textures (`--record-video`) for one scenario and confirm the TV screens match.
- **P3 exit review** — DONE 2026-10-05 (LOG): PASS with the open items above. **Gate G1 with the user pending.**

- [ ] **T-024** (P2 follow-up, before P5) Spike for ADR-010: measure which Windows APIs the shipped `steam_api.dll` calls (baseline, `GetProcAddress` log / relay for that module), then run its `DllMain` + `SteamAPI_Init` under Unicorn with stub shims. Evidence: list of APIs + init returning.

## P4 — Static recompilation (G1 passed 2026-10-05; ADR-013)
- [x] **T-040** DONE 2026-10-05 (evidence: `docs/LIFTER.md`). `docs/LIFTER.md`: design (register/flag/memory model, control flow, calls, tables, x87/SSE plan, test method). Keep it current with the code.
- [x] **T-041** DONE 2026-10-05 (evidence: `re/difftest_report.json` — 15,439/21,960 functions lift; 11,212/11,212 vectors over 2,867 leaf functions match Unicorn; 59.5% of their instructions executed; mutation check catches injected faults). (M4a) Lifter first cut: integer instructions; runtime skeleton (`src/runtime`: Cpu, arena, PE loader, address table); diff harness (`tests/diff`): Unicorn vs lifted on leaf functions. Evidence: pass rate report `re/difftest_report.json`.
- [x] **T-042** DONE 2026-10-06 (evidence: `re/lift_coverage.json` — 21,952 / 21,960 functions lift, 1,588,514 instructions; `re/difftest_report.json` — leaf set with FP/SIMD state: 11,926 vectors, 11,923 pass, 2 NaN-payload-only, 1 known-wrong reference, 0 unexplained). Coverage loop: lift ALL functions, histogram of unsupported instructions (`re/lift_coverage.json`), implement by frequency until >= 99% of functions lift. Order: remaining integer ops, string ops, x87, SSE/SSE2 scalar, SSE packed, MMX/SSSE3 leftovers.
- [x] **T-043** DONE 2026-10-06 (evidence: `re/difftest_report_all.json` — every lifted function attempted with its whole call tree: 24,385 tried, 13,840 produced vectors, 26,294 vectors -> 26,275 pass, 1 x87-approx, 6 NaN-payload-only, 5 double-range, 7 fail in 9 functions that are all explained in `tests/diff/reference_deviations.json`; 0 unexplained). Diff tests for non-leaf functions (calls stubbed/recorded), fs: (TEB) support in the harness, x87/SSE state comparison.
- [ ] **T-044** PARTLY DONE 2026-10-06: flag audit (`re/flag_audit.json`: 9 entry-live functions + 3 call sites, all handled via `Cpu.xfl`), LOCKed RMW atomic (0 non-atomic left), compile cost measured (docs/LIFTER.md §7), unlifted list = 8 (`re/lift_coverage.json`). Left: FTZ/DAZ/MXCSR audit, A2 (non-returning calls / stack tricks) audit. Whole-image build: split into translation units, measure compile time and binary size on this machine; flags-live-across-call audit; unresolved switch tables (64) and unreached code (0.8%) lists.
- [ ] **T-045** PARTLY DONE 2026-10-06 (evidence: `artifacts/boot/last_run.log` / LOG — CRT start-up, C++ static initialisers, WinMain, CPU detection, D3D9 probe, window class, build banner, up to the Steam check: 87 distinct imports, ~4,900 import calls, no trap). Remaining part depends on T-024. Runtime for M4b: loader, TEB/TLS, threads, the shims the boot path calls (from `re/platform_map.csv`, hottest first), null D3D9 device -> boot headless to the main loop.
- [x] **T-046** DONE 2026-10-06 (evidence: docs/LIFTER.md "x86_64 cross-check"; `artifacts/lifted/difftest_x64.log` — x86_64 -O0 under Rosetta agrees with arm64 on all 26,294 vectors except three 1-ulp libm differences; at -O1 an Apple clang x86_64 miscompile was found and reduced: `tests/diff/compiler_bugs/x86_64_byte_insert_after_load.c`). (M4c) Diff-test suite green; x86_64 (Rosetta) build of the lifted code as a cross-check.
- [ ] **T-024** BLOCKED 2026-10-06 (from P2) steam_api.dll interpreter spike (ADR-010). Done: interpreter host (`src/platform/win32/x86_module.cpp`), the DLL loads unmodified, runs its start-up, reads `steam_emu.ini` and its state files (evidence: `artifacts/boot/last_run.log`, LOG). Blocked: it then stops itself with "Internal error 0x0A: Protection initialization failed!" after enumerating the loaded modules. Finding out what that check wants = analysing the anti-tamper of a DRM-circumvention DLL -> ground rule 1: stop and ask the user.
- [ ] **T-036** (from P3) shader/pipeline disk cache. **T-037** native D3D9 reference (Windows PC: user prefers to keep it as a last resort). **T-038** video textures in a trace.

## P5 — Integration (started 2026-10-06 with the user's approval)
- [x] **T-050** DONE 2026-10-06 (evidence: `build/replay artifacts/traces/menu.bin --window --frames 1900-2000` -> "window: 102 frames shown", 156,725 draws, 0 skipped; `artifacts/replay_window/frame_002000.png`). SDL3 window + CAMetalLayer presentation for the replay tool; `scripts/replay_window.sh <scenario> [first_frame] [fps]`.
- [x] **T-051** DONE 2026-10-06 (evidence: `scripts/replay_check.sh` -> `re/replay_check.json` byte-identical to before the split: cuba 0.999, vorkuta 0.998, menu 0.983, khe_sanh 0.973; 0 skipped draws). Renderer library `bo1_metal` = `src/rhi/{d3d9_call.h,renderer.h}` + `src/metal/metal_renderer.mm`; the replay tool links it.
- [ ] **T-055** Native `d3d9` shim that turns the game's live calls into `d3d9_call.h` records for the renderer (replaces `d3d9_null.cpp`). Can only be exercised once the game gets past its start-up checks.
- [ ] **T-052** SDL3-backed user32. PART 1 DONE 2026-10-06 (evidence: `build/test_win_messages` -> "99 keys mapped; all checks passed"): `src/platform/window/win_messages.{h,cpp}` translates SDL3 events into Win32 messages (keys with virtual key + set-1 scan code + extended/repeat/transition bits, WM_SYSKEY*, WM_CHAR, mouse buttons/move/wheel, focus, size, close). PART 2 open: main-thread SDL service (window creation, event pump, message queue) and the user32 shims on top of it, replacing `user32_headless.cpp`; gamepad through SDL for the XInput shim.
- [ ] **T-053** XAudio2 2.7 on FAudio (ADR-009; `brew install faudio` needs the user's OK). **T-054** Bink on FFmpeg (ADR-008).
- [x] **T-036** CLOSED 2026-10-06 by measurement (evidence: `build/replay … --backend metal` now prints "shader work": menu window 182 functions / 171 pipelines = translate 7.6 ms + MSL compile 29.3 ms + pipeline creation 15.9 ms; khe_sanh 209 / 131 = 8.4 + 31.9 + 21.9 ms). On every run after the first, Metal's own on-disk shader cache already makes this ~60 ms in total, so a project-side cache would only help the very first run (measured in P3 at ~10 shader functions/s); an explicit `MTLBinaryArchive` moves to P7 (start-up polish).

## P5+ (later)
Integration -> strangler fig -> Metal optimisation -> QA. See docs/PLAN.md.
