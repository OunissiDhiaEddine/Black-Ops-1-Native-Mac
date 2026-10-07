// Direct3D 9 -> renderer (T-055, ADR-004): every call the game makes on the guest-visible Direct3D
// objects (d3d9_null.cpp keeps the objects, their reference counts and the lockable memory) is turned
// into the same call record the trace replay feeds the renderer (src/rhi/d3d9_call.h,
// docs/TRACE_FORMAT.md) and handed to the Metal renderer right after it returned. The records are built
// the way the tracing proxy builds them (tools/d3d9_proxy/proxy.cpp), so the renderer sees a live game
// exactly as it sees a recorded one. One difference, on purpose: an object's Release reaches the renderer
// when the object is really destroyed (on_destroy), not when the game drops its last reference.
//
//   BO1_D3D=null      keep the null device only (nothing is rendered; the old headless boot)
//   BO1_D3D_DUMP=dir  write the back buffer of every 20th frame as PNG into dir
#include <map>
#include <memory>
#include <mutex>
#include <vector>

#include "d3d9_objects.h"
#include "renderer.h"
#include "window_service.h"

namespace bo1 {
namespace {

using namespace trace; // IFACE_* ids, Record, Blob, LockDesc, blob tags

std::mutex g_mu; // one renderer, called from whichever thread the game uses Direct3D on
std::unique_ptr<rhi::Renderer> g_renderer;
int g_frame = 0;

struct Lock {
    LockDesc d;
};
std::map<uint64_t, Lock> g_locks; // (object, sub-resource) -> the lock being held

uint64_t lock_key(uint32_t obj, uint32_t sub) { return ((uint64_t)sub << 32) | obj; }
bool is_dxt(uint32_t fmt) { return (fmt & 0x00FFFFFF) == 0x00545844; } // 'D','X','T',digit

struct Rec {
    Record r{};
    uint32_t words[16]{};
    uint32_t out_id = 0;
    LockDesc lock{};
    void add(uint32_t tag, const void *p, size_t n) {
        if (r.nblobs < 6) r.blobs[r.nblobs++] = {tag, std::span<const uint8_t>(static_cast<const uint8_t *>(p), n)};
    }
};

// Token stream length of a shader, END token included (comments carry their own length).
size_t shader_bytes(uint32_t code) {
    size_t i = 1; // version token
    for (;; i++) {
        uint32_t t = rd32(code + 4 * (uint32_t)i);
        if (t == 0x0000FFFF) return (i + 1) * 4;
        if ((t & 0xFFFF) == 0xFFFE) i += (t >> 16) & 0x7FFF;      // comment
        else i += (t >> 24) & 0xF;                                // instruction: operand token count (SM2+)
        if (i > (1u << 20)) return 0;
    }
}

// A texture level / stand-alone surface being locked: remember what Unlock has to hand over.
void note_surface_lock(uint32_t obj, uint32_t sub, uint32_t level_obj, uint32_t locked, uint32_t rect, uint32_t flags, bool volume) {
    if (!level_obj) return;
    LockDesc d{};
    d.format = rd32(level_obj + R_FORMAT);
    d.width = rd32(level_obj + R_WIDTH), d.height = rd32(level_obj + R_HEIGHT), d.depth = rd32(level_obj + R_DEPTH);
    d.lock_flags = flags;
    d.pitch = rd32(locked);
    if (volume) {
        d.slice_pitch = rd32(locked + 4);
        d.data_ptr = rd32(locked + 8);
        d.size = d.slice_pitch * d.depth;
    } else {
        d.data_ptr = rd32(locked + 4);
        bool dxt = is_dxt(d.format);
        uint32_t w = d.width, h = d.height;
        if (rect) {
            d.has_rect = 1;
            d.left = rd32(rect), d.top = rd32(rect + 4), d.right = rd32(rect + 8), d.bottom = rd32(rect + 12);
            w = d.right - d.left, h = d.bottom - d.top;
        }
        uint32_t rows = dxt ? (h + 3) / 4 : h;
        uint32_t full_pitch;
        d3d9_format_bytes(d.format, d.width, 1, &full_pitch);
        uint32_t rowbytes = dxt ? ((w + 3) / 4) * (full_pitch / (((d.width + 3) / 4) ? (d.width + 3) / 4 : 1)) : w * (d.width ? full_pitch / d.width : 0);
        d.size = rect ? (rows ? d.pitch * (rows - 1) + rowbytes : 0) : d.pitch * rows;
    }
    g_locks[lock_key(obj, sub)] = {d};
}

// Unlock: describe the lock and attach what the game wrote (still in the object's guest memory).
bool finish_lock(Rec &rec, uint32_t obj, uint32_t sub) {
    auto it = g_locks.find(lock_key(obj, sub));
    if (it == g_locks.end()) return false;
    rec.lock = it->second.d;
    g_locks.erase(it);
    LockDesc &d = rec.lock;
    bool has_data = d.data_ptr && d.size && !(d.lock_flags & 0x10); // not D3DLOCK_READONLY
    d.dumped = has_data ? 1 : 0;
    if (!has_data) d.size = 0;
    rec.add(kLock, &rec.lock, sizeof(LockDesc));
    if (has_data) rec.add(kData, g_mem + d.data_ptr, d.size);
    return true;
}

void observe(uint32_t tag, Cpu *, const uint32_t *a, int words, uint32_t result) {
    unsigned iface = tag >> 16, slot = tag & 0xFFFF;
    std::lock_guard<std::mutex> lk(g_mu);
    if (!g_renderer) {
        if (iface != IFACE_D3D9 || slot != 16 || (result & 0x80000000u)) return; // nothing to draw with before CreateDevice
        rhi::Options o;
        if (const char *d = getenv("BO1_D3D_DUMP")) o.dump_dir = d;
        o.layer = win::WindowService::get().metal_layer(); // NULL when headless: frames are rendered, not shown
        try {
            g_renderer = rhi::make_metal_renderer(o);
        } catch (const std::exception &e) {
            log("d3d9: no renderer (%s) — continuing without rendering", e.what());
            return;
        }
        log("d3d9: Metal renderer attached");
    }
    Rec rec;
    int n = words > 16 ? 16 : (words < 0 ? 0 : words);
    for (int i = 0; i < n; i++) rec.words[i] = a[i];
    rec.r.iface = (uint8_t)iface, rec.r.slot = (uint16_t)slot, rec.r.flags = 1, rec.r.result = result;
    rec.r.nwords = (uint16_t)n, rec.r.words = rec.words;
    bool ok = !(result & 0x80000000u), send = false, present = false;
    auto out = [&](uint32_t pp) { // object returned through a pointer argument
        if (ok && pp) {
            rec.out_id = rd32(pp);
            rec.add(kOut, &rec.out_id, 4);
        }
        send = true;
    };
    switch (iface) {
    case IFACE_D3D9:
        if (slot == 16) { // CreateDevice: D3DPRESENT_PARAMETERS
            rec.add(kPP, g_mem + a[5], 56), send = true;
            if (ok) win::WindowService::get().set_display((int)rd32(a[5]), (int)rd32(a[5] + 4), rd32(a[5] + 32) == 0);
        }
        break;
    case IFACE_DEVICE:
        switch (slot) {
        case 16: // Reset: D3DPRESENT_PARAMETERS (BackBufferWidth, Height at +0 / +4, Windowed at +32)
            rec.add(kPP, g_mem + a[1], 56), send = true;
            if (ok) win::WindowService::get().set_display((int)rd32(a[1]), (int)rd32(a[1] + 4), rd32(a[1] + 32) == 0);
            break;
        case 17: present = send = true; break;                       // Present
        case 18: out(a[4]); break;                                   // GetBackBuffer
        case 23: out(a[7]); break;                                   // CreateTexture
        case 24: out(a[8]); break;                                   // CreateVolumeTexture
        case 25: out(a[6]); break;                                   // CreateCubeTexture
        case 26: case 27: out(a[5]); break;                          // CreateVertexBuffer / CreateIndexBuffer
        case 28: case 29: out(a[7]); break;                          // CreateRenderTarget / CreateDepthStencilSurface
        case 34: case 37: case 39: case 57: case 65: case 69: case 82: case 87: case 92: case 100: case 104: case 107:
            send = true;                                             // state setters, StretchRect, DrawIndexedPrimitive: arguments only
            break;
        case 43:                                                     // Clear(Count, pRects, ...)
            if (a[1] && a[2]) rec.add(kRectA, g_mem + a[2], 16 * (size_t)a[1]);
            send = true;
            break;
        case 47: if (a[1]) rec.add(kStruct, g_mem + a[1], 24), send = true; break; // SetViewport
        case 75: if (a[1]) rec.add(kStruct, g_mem + a[1], 16), send = true; break; // SetScissorRect
        case 86: {                                                   // CreateVertexDeclaration(pElements, ppDecl)
            size_t count = 0;
            while (count < 64 && rd16(a[1] + 8 * (uint32_t)count) != 0xFF) count++;
            rec.add(kElements, g_mem + a[1], (count + 1) * 8);
            out(a[2]);
            break;
        }
        case 91: case 106:                                           // CreateVertexShader / CreatePixelShader(pFunction, pp)
            if (a[1]) rec.add(kBytecode, g_mem + a[1], shader_bytes(a[1]));
            out(a[2]);
            break;
        case 94: case 109:                                           // Set{Vertex,Pixel}ShaderConstantF(Start, pData, Count)
            if (a[2]) rec.add(kData, g_mem + a[2], 16 * (size_t)a[3]), send = true;
            break;
        default: break;
        }
        break;
    case IFACE_SWAPCHAIN:
        if (slot == 3) present = send = true;                        // Present
        else if (slot == 5) out(a[3]);                               // GetBackBuffer
        break;
    case IFACE_TEXTURE:
        if (slot == 18) out(a[2]);                                   // GetSurfaceLevel(Level, pp)
        else if (slot == 19 && ok) note_surface_lock(a[0], a[1], d3d9_tex_child(a[0], 0, a[1]), a[2], a[3], a[4], false);
        else if (slot == 20) send = finish_lock(rec, a[0], a[1]);
        break;
    case IFACE_CUBE:
        if (slot == 18) out(a[3]);                                   // GetCubeMapSurface(Face, Level, pp)
        else if (slot == 19 && ok) note_surface_lock(a[0], a[2] | (a[1] << 8), d3d9_tex_child(a[0], a[1], a[2]), a[3], a[4], a[5], false);
        else if (slot == 20) send = finish_lock(rec, a[0], a[2] | (a[1] << 8));
        break;
    case IFACE_VOLUME:
        if (slot == 19 && ok) note_surface_lock(a[0], a[1], d3d9_tex_child(a[0], 0, a[1]), a[2], 0, a[4], true);
        else if (slot == 20) send = finish_lock(rec, a[0], a[1]);
        break;
    case IFACE_VB: case IFACE_IB:
        if (slot == 11 && ok) {                                      // Lock(Offset, Size, ppData, Flags)
            LockDesc d{};
            d.width = rd32(a[0] + R_WIDTH);
            d.format = iface == IFACE_IB ? rd32(a[0] + R_FORMAT) : 0;
            d.offset = a[1] < d.width ? a[1] : 0;
            d.size = a[2] && a[2] <= d.width - d.offset ? a[2] : d.width - d.offset;
            d.lock_flags = a[4];
            d.data_ptr = a[3] ? rd32(a[3]) : 0;
            g_locks[lock_key(a[0], 0)] = {d};
        } else if (slot == 12) send = finish_lock(rec, a[0], 0);
        break;
    case IFACE_SURFACE:
        if (slot == 13 && ok) note_surface_lock(a[0], 0, a[0], a[1], a[2], a[3], false);
        else if (slot == 14) send = finish_lock(rec, a[0], 0);
        break;
    default: break; // Release is not forwarded: see on_destroy
    }
    if (!send) return;
    g_renderer->call(rec.r, g_frame);
    if (present) g_renderer->end_frame(++g_frame);
}

// An object is gone for good (d3d9_null.cpp, "object lifetime"). The renderer is told with the record the
// trace replay shows it for the same event, a Release that returned 0 — and only now: the game's own Release
// calls are not forwarded, because an object the device still has bound outlives its last reference. After
// this the object's address, which is its id, may be handed out again.
void on_destroy(uint32_t obj, uint32_t kind) {
    std::lock_guard<std::mutex> lk(g_mu);
    std::erase_if(g_locks, [obj](const auto &kv) { return (uint32_t)kv.first == obj; });
    unsigned iface;
    switch (kind) {
    case K_SURFACE: iface = IFACE_SURFACE; break;
    case K_TEXTURE: iface = IFACE_TEXTURE; break;
    case K_CUBE: iface = IFACE_CUBE; break;
    case K_VOLUMETEX: iface = IFACE_VOLUME; break;
    case K_VB: iface = IFACE_VB; break;
    case K_IB: iface = IFACE_IB; break;
    case K_VDECL: iface = IFACE_VDECL; break;
    case K_VSHADER: iface = IFACE_VSHADER; break;
    case K_PSHADER: iface = IFACE_PSHADER; break;
    case K_DEVICE: iface = IFACE_DEVICE; break;
    default: return; // the renderer keeps nothing for the others
    }
    if (!g_renderer) return;
    Rec rec;
    rec.words[0] = obj;
    rec.r.iface = (uint8_t)iface, rec.r.slot = 2, rec.r.flags = 1, rec.r.result = 0;
    rec.r.nwords = 1, rec.r.words = rec.words;
    g_renderer->call(rec.r, g_frame);
}

} // namespace

void d3d9_live_attach() {
    static bool done = false;
    if (done) return;
    done = true;
    const char *mode = getenv("BO1_D3D");
    if (mode && !strcmp(mode, "null")) return;
    static const struct { const char *name; unsigned id; } kIfaces[] = {
        {"IDirect3D9", IFACE_D3D9}, {"IDirect3DDevice9", IFACE_DEVICE}, {"IDirect3DSwapChain9", IFACE_SWAPCHAIN},
        {"IDirect3DTexture9", IFACE_TEXTURE}, {"IDirect3DCubeTexture9", IFACE_CUBE}, {"IDirect3DVolumeTexture9", IFACE_VOLUME},
        {"IDirect3DVertexBuffer9", IFACE_VB}, {"IDirect3DIndexBuffer9", IFACE_IB}, {"IDirect3DSurface9", IFACE_SURFACE},
        {"IDirect3DVertexShader9", IFACE_VSHADER}, {"IDirect3DPixelShader9", IFACE_PSHADER}, {"IDirect3DVertexDeclaration9", IFACE_VDECL},
    };
    for (const auto &i : kIfaces) com_observe(i.name, i.id << 16, observe);
    d3d9_set_destroy_hook(on_destroy);
}

} // namespace bo1
