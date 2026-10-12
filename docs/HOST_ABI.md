# Host-call ABI (T-023) — how lifted guest code and native host code call each other

Status: **specification for P4/P5, not yet implemented.** Every rule here is derived from measured
facts in `docs/RE_NOTES.md` (tagged there) or marked as a design choice. Addresses are guest VAs of
`BlackOps.exe` (2011-09-01 SP build).

## 0. Vocabulary
- **Guest**: the game's x86-32 world — 32-bit pointers, Win32 calling conventions, its own stacks.
- **Host**: the arm64 macOS process — real pointers, AAPCS64, pthreads.
- **Lifted code**: guest functions recompiled to arm64. Each one is a host function
  `void fn_XXXXXXXX(Cpu *cpu)` operating on guest registers in `Cpu` and guest memory in the arena.
- **Shim**: a host function that implements an imported Windows / middleware API.

## 1. Guest memory (ADR-003)
- One 4 GiB reservation; guest address `a` is host `arena_base + a`. `guest_to_host()` /
  `host_to_guest()` are the only conversions (CLAUDE.md conventions).
- Fixed mappings: the image at `0x00400000–0x04677000` (sections from the PE; `.data` is 238,592 B
  on disk but 61.8 MB virtual, so the tail is zero-filled); a 64 KiB guard at 0; nothing else fixed.
- Everything dynamic (heaps, `VirtualAlloc`, stacks, TEBs, COM stubs) comes from the arena
  allocator. It must satisfy one **300 MiB contiguous commit** at startup (`PMem_Init`,
  0x006122F0) and report `LARGE_ADDRESS_AWARE`-style limits (the exe has the LAA flag).
- A request the game makes for ~2.85 GB of heap (seen on every baseline run) fails on Wine and the
  game carries on; our allocator must also fail it cleanly (return NULL), not abort.

## 2. CPU state
```c
struct Cpu {                       // as implemented: src/runtime/lifted.h (P4)
    uint32_t eax, ecx, edx, ebx, esp, ebp, esi, edi;   // esp/ebp are guest addresses
    uint32_t teb;                                       // guest address of this thread's TEB
    uint8_t  df;                                        // direction flag
    Flags    xfl;                                       // flags handed across the few audited call boundaries
    uint8_t  ftop; uint16_t fcw, fsw; double st[8];     // x87 (doubles; see docs/LIFTER.md §5)
    uint64_t mm[8]; Xmm xmm[8]; uint32_t mxcsr;
    uint32_t trap_addr, trap_code;
};
```
- Status flags are NOT in `Cpu`: each lifted function keeps them in a local `Flags fl` (eager helpers,
  dead ones removed by the C compiler). Shims never see or return flags.
- One `Cpu` per guest thread, owned by its host pthread; a pinned host register holds `arena_base`.
- **x87 is live at ABI edges**: 27.6% of FP instructions are x87, mostly `fld`/`fstp` because
  `float`/`double` are returned in ST(0). Shims that return floating point must push onto
  `cpu->fpu`. No imported function in the platform map returns FP, so this only matters for
  guest-to-guest calls and for D3DX stubs if ever added.
- MMX/SSE/SSE2 state is per thread; SSE3/SSSE3 code exists in small amounts (CPU-dispatched), so
  the emulated `cpuid` decides which paths run — report SSE2 only unless the lifter covers more.

## 3. Calls inside the guest
- Direct calls are lifted to direct host calls. The guest return address is still pushed on the
  guest stack (code reads it: 2,401 `ret imm` sites, tail jumps, the SEH prologs).
- **Indirect calls and jumps** (5,548 computed call sites, 690 switch jumps) go through
  `dispatch(cpu, target)`: a perfect-hash table `guest VA -> host function` holding every lifted
  function start (21,960 after seeding) plus the trampoline zone (§4).
- A target that is not in the table is a **CFG-recovery miss** (0.8% of `.text` is still unreached
  statically): `dispatch` logs `{target, caller}` and stops. The log is the work list for the
  lifter; there is no interpreter fallback in the first iteration (design choice).

## 4. Guest -> host: imports and host-implemented objects
### 4.1 Trampoline zone
A guest range that is never mapped (`0xFFF00000–0xFFFFFFFF`, design choice) is reserved for
host entry points. Each one gets a unique guest address `T_i = 0xFFF00000 + 16*i`. The dispatcher
table maps `T_i` to a host thunk. Guest code never executes bytes there; it only ever reaches a
`T_i` through `dispatch`.

### 4.2 Imports
- At startup the loader writes `T_i` into each of the **334 IAT slots** (`0x009A3000…`, 16 DLLs).
  The lifted `call dword ptr [slot]` / `jmp [slot]` thunks then dispatch to the shim without any
  special casing in the lifter.
- Shim signature and calling convention:
  ```c
  // stdcall (all Win32, D3D9/XAudio2 COM methods): callee pops
  SHIM_STDCALL(kernel32, CreateFileA, 7)(Cpu *cpu, const uint32_t *args) -> uint32_t
  // cdecl (binkw32 exports are stdcall `_Name@N`; steam_api flat functions are cdecl): caller pops
  SHIM_CDECL(steam_api, SteamAPI_Init, 0)
  ```
  The generated thunk reads `args = guest_to_host(cpu->esp + 4)`, calls the C function, stores the
  result in `eax` (and `edx` for 64-bit results), pops the return address, and for stdcall adds
  `4*nargs` to `esp`. Argument counts come from one table that is checked against the SDK headers
  the same way the trace recorder did it (`nw()` over the prototype) — no hand-counted stacks.
- Shims must preserve `ebx, esi, edi, ebp` and may clobber `eax, ecx, edx` and flags, exactly like
  the originals. They must **not** touch the guest stack below `esp`.
- Last-error lives in the guest TEB (`teb+0x34`); shims set it only where Windows does. The
  baseline shows `GetLastError`/`SetLastError` ~330k calls per mission run, so this is not optional.
- `GetProcAddress`/`LoadLibraryA` resolve only DLLs we provide and return the same `T_i` values;
  anything else fails with a logged name (needed to learn what the game probes for dynamically).

### 4.3 Host-implemented COM / C++ objects
The game holds pointers to objects whose methods live on the host: `IDirect3D9`,
`IDirect3DDevice9` and the resource interfaces (P3), `IXAudio2` and its voices (ADR-009),
`ID3DXBuffer`, and the Steam interfaces (ADR-010).
- A guest-visible object is a small guest allocation `{ uint32_t vtbl; uint32_t host_id; … }`.
  `vtbl` points to a guest array of `T_i` addresses, one per method. The game's
  `mov edx,[obj]; call [edx+off]` works unmodified.
- The host thunk for a method receives `this` as a guest address and looks up the host object via
  `host_id` (a table index, never a raw host pointer in guest memory).
- **Two conventions**, both measured in the exe:
  - COM (`IDirect3D*`, `IXAudio2*`, `ID3DXBuffer`): **stdcall, `this` is the first stack word**.
  - Steam C++ interfaces (`ISteamUser`, `ISteamFriends`, …): **MSVC thiscall — `this` in `ecx`**,
    Implemented by `com_vtable_thiscall()` (the shim sees `this` as `a[0]`); methods and stack words: `re/steam_abi.csv`. Struct results such as `CSteamID` use a hidden result pointer as first stack argument (CONFIRMED, RE_NOTES "Steam as the game uses it").
    remaining arguments on the stack, callee pops. Evidence: of the 37 accessor call sites
    (`call [SteamUser]` etc.), 18 do `mov ecx, eax` within five instructions before the vtable
    call and the sampled others load `ecx` later; none pushes `this` (LIKELY, static sample).
- Objects the game passes back (e.g. a texture handed to `SetTexture`) arrive as guest addresses
  and are translated through the same table. Vtable identity is ours to define, so there is no
  equivalent of Wine's vtable-identity asserts.
- D3D9 specifically: **all device calls come from one guest thread** (render thread, engine
  thread index 1 — CONFIRMED in four scenarios), so the D3D9 object layer needs no locking.

## 5. Host -> guest: callbacks
Host code calls guest code with `guest_call(cpu, target, nargs, …)`:
1. push the arguments and a sentinel return address `T_return` on the *current* guest stack of that
   thread, 2. run `dispatch(cpu, target)`, 3. the lifted `ret` to `T_return` returns to the host,
   which reads `eax` (and fixes `esp` for cdecl). Nesting is allowed (a shim called from a callback
   may call back again).
A callback must run on the guest thread that owns the stack it uses. Callbacks the game registers
(all confirmed by imports or RTTI):
| Callback | Registered through | Runs on | Notes |
|---|---|---|---|
| Thread start routine | `CreateThread` (10 sites; engine threads use `Sys_ThreadMain` 0x0082FDE0 with an index) | new host pthread | stack size from the call (0 -> 1 MiB PE default; two helpers use 64 KiB), `CREATE_SUSPENDED` honoured |
| Window procedure | `RegisterClassA/ExA` | the thread that pumps messages | called from the `DispatchMessageA`/`PeekMessageA` shims and from `SendMessageA` |
| Keyboard hook | `SetWindowsHookExA` | — | never invoked (stub) |
| File completion routine | `ReadFileEx` (2 sites; **used**: ~450 calls per mission run, paired 1:1 with `SleepEx`) | the requesting thread, inside `SleepEx`/alertable waits | queued by the host I/O thread, drained in the shim |
| `IXAudio2VoiceCallback` methods | `CreateSourceVoice` | **host audio thread** | guest object, stdcall COM; needs its own `Cpu` + guest stack for the audio thread |
| XAPO `Process`/`LockForProcess`… (`CXAPOBase` subclass in the exe) | `CreateSubmixVoice`/effect chains | host audio thread | real-time: lifted DSP code runs inside the audio callback |
| Steam `CCallback<…>::Run` | `SteamAPI_RegisterCallback` (12 sites) | thread calling `SteamAPI_RunCallbacks` | thiscall virtual call on a guest object |
| Unhandled-exception filter | `SetUnhandledExceptionFilter` | faulting thread | called by the SEH shim (§7) |
Host threads that enter the guest without having been created by it (audio) get a `Cpu`, a guest
stack and a TEB from `guest_thread_attach()`.

## 6. Threads, TEB, TLS
- Each guest thread has a guest-memory **TEB** (4 KiB is enough) at `cpu->teb`; lifted `fs:[x]`
  becomes `[teb + x]`. Fields the code is known to use: `fs:[0]` SEH chain head (1,320 sites),
  `fs:[0x18]` self pointer, `fs:[0x2C]` static-TLS array (204 sites), `fs:[4]`/`fs:[8]` stack
  bounds, `+0x24` thread id, `+0x34` last error, `+0xE10…` dynamic TLS slots (`TlsAlloc`).
- **Static TLS**: the PE TLS directory has a 108-byte template at `0x04660000` and its index
  variable at `0x03956508` (no callbacks). Each thread gets a copy; `fs:[0x2C]` points to a
  one-entry array; the index variable is 0.
- The engine relies on real preemptive threads and atomics (430 `lock`-prefixed instructions;
  `InterlockedExchange` alone is called ~38 M times in a mission run): lifted `lock` ops are host
  atomics on arena memory, and guest memory ordering must not be weakened across shims (use
  sequentially-consistent atomics; arm64 is weaker than x86-TSO — see Risks).
- `ResumeThread` is used only to start threads created with `CREATE_SUSPENDED` (the thread simply
  does not begin until resumed). `SuspendThread` has call sites but was **never called** in the
  baseline runs: implement it as a cooperative suspend honoured at dispatch points, and log its
  first use.

## 7. Exceptions
- The code is built with MSVC SEH and C++ EH: 1,320 frame registrations through `fs:[0]`,
  `__except_handler4` (0x0096E0E0) with the security cookie at `0x00BA3178`, and imports of
  `RaiseException` / `RtlUnwind`. Registering and unregistering frames is plain memory traffic and
  needs nothing from the host.
- What needs the host: **raising and unwinding.** `RaiseException` shim walks the guest chain from
  `[teb+0]`, calls each guest handler with `guest_call` (`EXCEPTION_RECORD`, frame, `CONTEXT`,
  dispatcher context built in guest memory), honours `ExceptionContinueSearch/Execution`, and
  implements `RtlUnwind` as a second walk with `EXCEPTION_UNWINDING`.
- The hard part is the **landing**: after unwinding, MSVC handlers resume at a catch funclet / an
  `__except` block inside a function whose lifted host frame is already gone. Plan: lifted
  functions that contain EH scopes (identified from the scope tables) are compiled so that each
  resume address is an extra dispatch entry that rebuilds its locals from `ebp`
  (frame-pointer-based functions only), and the shim performs the resume with a host `longjmp` to
  the dispatcher loop of that guest thread.
- Hardware faults (null dereference in guest code) arrive as host signals; they are converted to a
  guest `EXCEPTION_ACCESS_VIOLATION` raise only if a handler exists, otherwise they are a crash
  report with the guest EIP mapped through `re/symbols.csv`.
- **Measured (RE_NOTES § Platform map):** in three baseline runs from boot to in-mission play,
  `RaiseException` is called exactly 13 times per run — the same as `CreateThread` — and
  `RtlUnwind` / `UnhandledExceptionFilter` never. LIKELY these are the MSVC thread-naming
  exceptions (code 0x406D1388), which a handler dismisses with "continue execution".
  **Consequence (ADR-012):** P4 starts with: `RaiseException(0x406D1388)` returns immediately;
  any other raise, and any hardware fault, produces a crash report with the guest EIP mapped
  through `re/symbols.csv`. The dispatch/landing machinery above is built only if a real game path
  turns out to need it.

## 7a. Emulated x86 modules (ADR-010)
One module is not lifted: the shipped `steam_api.dll`. It is mapped unmodified into the arena at
its preferred base (0x10000000; it carries relocations if that range is taken) and run by an
embedded x86 interpreter that shares the arena and the calling thread's `Cpu` (registers are
copied in and out at each crossing).
- **Game -> module:** `dispatch(cpu, target)` finds no lifted function for a target inside the
  module's address range and calls `interp_run(cpu, target)`, which executes until the return
  address pushed by the caller is reached. The 19 imported entry points are IAT slots filled with
  the module's real export addresses (not trampolines); the Steam interface vtables live inside the
  module, so `call [vtbl+off]` lands there the same way.
- **Module -> host:** the module's own import slots and whatever it resolves through
  `GetProcAddress` hold trampoline-zone addresses; executing one makes the interpreter leave and
  run the shim (§4.2). This is also the containment boundary (file roots, no network by default).
- **Module -> game:** an address inside the lifted image reached from interpreted code (Steam
  callbacks: `CCallback::Run`) goes through `dispatch` to the lifted function and returns into the
  interpreter.
- The module's `DllMain(DLL_PROCESS_ATTACH)` runs in the interpreter before the game's entry point,
  like every other DLL initialiser; it unpacks itself there. Self-modifying code is the
  interpreter's problem by design — nothing in the pipeline looks inside the module.
- Threads the module creates get a `Cpu`/TEB like any guest thread and run entirely interpreted.

## 8. What the shim layer may assume (measured)
- Single process, single window, one D3D9 device, one render thread.
- No runtime-generated code observed (`VirtualProtect` is not imported; shaders are precompiled and
  `D3DXCompileShader` is never called).
- No DirectInput, no XAudio2 version other than 2.7, no COM class other than XAudio2
  (`tools/com_steam_usage.py`: both `CoCreateInstance` sites pass `CLSID_XAudio2`).
- Paths are Windows-style, case-insensitive, relative to the install directory or to
  `%LOCALAPPDATA%\Activision\CoD` / `players\`.

## 9. Risks specific to this ABI
1. **Memory ordering**: x86 code compiled for TSO, running as native arm64. Lock-free patterns
   that rely on TSO (store-store ordering without a `lock` prefix) can break. Mitigations, in
   order of cost: lifted stores/loads to shared globals stay plain (fast) and we test under
   stress; if races appear, enable Apple Silicon's TSO mode for guest threads (a per-thread
   processor mode Rosetta 2 uses; availability to third-party processes must be verified) or fall
   back to barriers around accesses to memory touched by `lock`-prefixed code.
2. **Exception landing** (§7).
3. **Real-time audio callbacks into lifted code** (§5): lifted XAPO code must not take locks that
   game threads hold across long operations; measure before optimising.
4. **Suspend/resume** semantics with host locks (§6).
