# D3D9 trace format (v2; v1 is still readable)

Written by `tools/d3d9_proxy` when `BO1_TRACE_FILE` is set (`scripts/run_baseline.sh --trace --record`),
read by `tools/trace_reader.py`. Little-endian, 32-bit guest pointers. One file per run in
`artifacts/traces/` (gitignored: it contains game assets).

## File header (40 bytes)
| Offset | Type | Value |
|---|---|---|
| 0 | char[8] | `BO1D3D9T` |
| 8 | u32 | version = 2 (1 = first captures: `depth` not exact, 10 interfaces) |
| 12 | u32 | header size = 40 |
| 16 | u32 | guest pointer size = 4 |
| 20 | u32 | record header size = 16 |
| 24 | u32 | interface count = 13 |
| 28 | u32[3] | reserved |

## Record
One record per COM method call on a hooked interface, in the order the calls were made
(calls recorded after they return — see *Result* — appear at their completion).

| Offset | Type | Field |
|---|---|---|
| 0 | u8 | `iface` — interface id (table below) |
| 1 | u8 | `flags` — bit 0: `result` is valid (the call was recorded after it returned) |
| 2 | u8 | `depth` — number of hooked calls in progress on this thread when this call was made. **v2: exact** (every call is tracked from entry to return), so `depth > 0` means the call was made by the D3D9 implementation itself while servicing a game call, or is a tail call out of one; replayers skip those. In v1 only calls recorded on return were counted |
| 3 | u8 | `nblobs` |
| 4 | u16 | `slot` — vtable slot = method (names: `tools/d3d9_proxy/d3d9_methods.h`) |
| 6 | u16 | `nwords` |
| 8 | u32 | `tid` — Windows thread id |
| 12 | u32 | `result` — HRESULT / ULONG return value, if flag bit 0 |
| 16 | u32[nwords] | the call's stack words exactly as pushed: `words[0]` = `this`, then the parameters (floats as their bit pattern). Pointer parameters are guest addresses, meaningful only as object ids |
| … | blob[nblobs] | payloads |

Blob: `u32 tag, u32 length, u8 data[length]`, padded with zeros to a multiple of 4.

| Tag | Name | Content |
|---|---|---|
| 1 | OUT | u32: the interface pointer returned through the call's out parameter (the new object's id) |
| 2 | DATA | raw bytes: resource contents at `Unlock*`, shader constants (`Set*ShaderConstantF/I`: 16 bytes per register, `…B`: 4), vertex data of `Draw*UP`, `IDirect3DQuery9::GetData` result |
| 3 | STRUCT | the structure an input pointer refers to (`D3DVIEWPORT9`, scissor `RECT`, `D3DMATRIX`, `D3DGAMMARAMP`, …) |
| 4 / 5 | RECT_A / RECT_B | optional rectangles (Clear rect array; StretchRect/Present source and destination) |
| 6 | BYTECODE | shader token stream up to and including the END token |
| 7 | ELEMENTS | `D3DVERTEXELEMENT9[]` including the `D3DDECL_END` terminator |
| 8 | PP | `D3DPRESENT_PARAMETERS` (CreateDevice, Reset, CreateAdditionalSwapChain) |
| 9 | LOCK | lock description, 18 x u32 (below) |
| 10 | DATA_B | index data of `DrawIndexedPrimitiveUP` |

LOCK (on `Lock*` after it returns, and again on the matching `Unlock*` followed by DATA):
`format, width, height, depth, pitch, slice_pitch, lock_flags, has_rect, left, top, right, bottom, front, back, offset, size, data_ptr, dumped`.
For vertex/index buffers `width` is the buffer size and `offset`/`size` the locked range.
`dumped`: 0 = no data (read-only lock, or dynamic texture/video frame without `BO1_TRACE_VIDEO=1`);
1 = DATA follows with `size` bytes; 2 = DATA follows, `size` trimmed to what was actually written
into a DISCARD-locked buffer (the proxy pre-fills such locks with `0xCDCDCDCD` and cuts the
trailing fill); 3 = dynamic upload outside the `BO1_TRACE_FRAMES` window, not recorded.

## Interfaces
| id | Interface | Recorded after return (result valid) |
|---|---|---|
| 0 | IDirect3D9 | CreateDevice |
| 1 | IDirect3DDevice9 | every method with an interface out-parameter (Create*, Get*), Reset, Release |
| 2 | IDirect3DSwapChain9 | GetBackBuffer, Release |
| 3 | IDirect3DTexture9 | GetSurfaceLevel, LockRect, Release |
| 4 | IDirect3DCubeTexture9 | GetCubeMapSurface, LockRect, Release |
| 5 | IDirect3DVolumeTexture9 | GetVolumeLevel, LockBox, Release |
| 6 | IDirect3DVertexBuffer9 | Lock, Release |
| 7 | IDirect3DIndexBuffer9 | Lock, Release |
| 8 | IDirect3DSurface9 | LockRect, Release |
| 9 | IDirect3DQuery9 | GetData, Release |
| 10 | IDirect3DVertexShader9 | Release |
| 11 | IDirect3DPixelShader9 | Release |
| 12 | IDirect3DVertexDeclaration9 | Release |

State blocks are identified by their OUT pointer only (the game creates none). The proxy's own D3D
use (back-buffer capture, `GetDesc` queries) is never recorded.

## Semantics a replayer relies on
- **Frame boundary** = `IDirect3DSwapChain9::Present` (slot 3). The game never calls the device's Present.
- **Object ids** are guest pointers and are reused after the object is destroyed (`Release` with
  `result == 0`).
- **Recording window** (`BO1_TRACE_FRAMES=A-B`): every call is always recorded, so device state can be
  rebuilt by replaying from the start. Outside the window, uploads that look per-frame dynamic carry
  no data (`dumped = 3`): any resource (buffer, texture level) written more than 4 times. The first
  4 writes of every resource are always recorded, whatever the lock flags, and the count restarts
  when the object is destroyed (its address is reused). Until 2026-10-06 (T-056) every
  DISCARD/NOOVERWRITE buffer lock outside the window was dropped; the game fills some static buffers
  exactly once at start-up with a DISCARD lock (the full-screen quad that initialises the float-Z
  target), so traces made before that date draw those with zeros.
  Limitation that remains: a resource that was rewritten more than 4 times before the window and is
  used but not rewritten inside it has no data. The Metal replay reports every draw that reads
  such data ("draw reads an unrecorded vertex buffer / index buffer / texture" with object ids).
- **Validation:** `build/replay <trace> --frames A-B` (null backend) checks that every object used was created and is alive, locks pair with unlocks, and no upload is missing inside the window. The four golden traces pass with zero findings.
- A trace may end mid-record (the game is usually killed); readers stop at the first incomplete record.
- Not captured: calls made through interfaces of a second vtable for the same interface type (the
  proxy logs a WARNING in the census if it ever sees one), D3DX calls, anything the implementation
  does internally without going through a public vtable.

## Known sizes (main menu, wined3d on WineCX 24.0.7, 800x600)
~7,400 records and ~5 MiB of uploads per frame inside the window; ~0.5 MiB per frame outside it.
The game re-uploads a 2 MiB index buffer, ~0.8 MiB of dynamic vertices and a 1 MiB 256x256x4
volume texture every frame.
