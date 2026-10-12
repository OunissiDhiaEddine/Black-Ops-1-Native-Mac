// Direct3D 9 -> renderer (T-055, ADR-004): every call the game makes on the guest-visible Direct3D
// objects (d3d9_null.cpp keeps the objects, their reference counts and the lockable memory) is turned
// into the same call record the trace replay feeds the renderer (src/rhi/d3d9_call.h,
// docs/TRACE_FORMAT.md) and handed to the Metal renderer right after it returned. The records are built
// the way the tracing proxy builds them (tools/d3d9_proxy/proxy.cpp), so the renderer sees a live game
// exactly as it sees a recorded one. One difference, on purpose: an object's Release reaches the renderer
// when the object is really destroyed (on_destroy), not when the game drops its last reference.
//
// Draw submission (P6): the calls a frame consists of — state setters, constants, buffer locks, draws; 20,000 to
// 31,000 per frame in a fight — do not go through a record. Each has a small observer of its own that hands
// its arguments to the renderer's typed function (rhi::Renderer, "draw submission"); `observe` sees the rest.
//
//   BO1_D3D=null      keep the null device only (nothing is rendered; the old headless boot)
//   BO1_D3D_DUMP=dir  write the back buffer of every 20th frame as PNG into dir
#include <thread>
#include "boot_splash.h"
#include <chrono>
#include <algorithm>
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
std::map<uint64_t, Lock> g_locks; // (object, sub-resource) -> the lock being held (textures, surfaces, volumes)
// Vertex / index buffer locks: the game locks its dynamic index buffer about once per draw, and holds one or
// two buffer locks at a time — a few entries, searched linearly, nothing allocated.
struct BufLock {
    uint32_t obj, offset, size, flags, data_ptr;
};
std::vector<BufLock> g_buf_locks;

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

// BO1_FRAME_STATS=N (N frames per line, default 300 for "1"): frame-time statistics of the live path (T-058).
struct FrameStats {
    bool on = false;
    size_t window = 300;
    std::chrono::steady_clock::time_point last{};
    std::vector<double> frame_ms, render_ms;
    double renderer_ms = 0;
    uint64_t calls = 0, calls_sum = 0;
    rhi::Renderer::Work at_frame{}, at_window{}; // the renderer's totals at the start of the frame / of the window
    size_t slow = 0;
    FrameStats() {
        if (const char *v = getenv("BO1_FRAME_STATS")) {
            on = true;
            int n = atoi(v);
            if (n > 1) window = (size_t)n;
        }
    }
    static double pct(std::vector<double> v, double p) {
        std::sort(v.begin(), v.end());
        return v[(size_t)((double)(v.size() - 1) * p)];
    }
    void frame_done(std::chrono::steady_clock::time_point now, const rhi::Renderer::Work &w) {
        if (last.time_since_epoch().count()) {
            double ms = std::chrono::duration<double, std::milli>(now - last).count();
            frame_ms.push_back(ms);
            render_ms.push_back(renderer_ms);
            calls_sum += calls;
            // A frame that took more than two 60 Hz periods says what it spent: inside the renderer, and there on
            // shader translation / MSL compile / pipeline creation / waiting for the GPU / texture uploads.
            if (ms > 33.4) {
                slow++;
                log("slow frame %d: %.1f ms, game %.1f ms, renderer %.1f ms (shaders %.1f + %.1f ms for %llu functions, pipelines %.1f ms for %llu, "
                    "GPU wait %.1f ms, drawable wait %.1f ms, texture uploads %.1f ms for %llu / %.1f MiB); %llu calls",
                    g_frame, ms, ms - renderer_ms, renderer_ms, (double)(w.ns_translate - at_frame.ns_translate) / 1e6,
                    (double)(w.ns_compile - at_frame.ns_compile) / 1e6, (unsigned long long)(w.functions - at_frame.functions),
                    (double)(w.ns_pipeline - at_frame.ns_pipeline) / 1e6, (unsigned long long)(w.pipelines - at_frame.pipelines),
                    (double)(w.ns_gpu_wait - at_frame.ns_gpu_wait) / 1e6, (double)(w.ns_drawable - at_frame.ns_drawable) / 1e6,
                    (double)(w.ns_upload - at_frame.ns_upload) / 1e6,
                    (unsigned long long)(w.uploads - at_frame.uploads), (double)(w.upload_bytes - at_frame.upload_bytes) / 1048576.0,
                    (unsigned long long)calls);
            }
        }
        at_frame = w;
        last = now;
        renderer_ms = 0;
        calls = 0;
        if (frame_ms.size() < window) return;
        double sum = 0, rsum = 0;
        for (double x : frame_ms) sum += x;
        for (double x : render_ms) rsum += x;
        double n = (double)frame_ms.size();
        log("frame stats: frame %d, %.1f fps; frame ms avg %.2f p50 %.2f p95 %.2f p99 %.2f max %.1f; renderer ms avg %.2f p95 %.2f (%.0f%% of the frame); %.0f calls/frame",
            g_frame, 1000.0 * n / sum, sum / n, pct(frame_ms, 0.5), pct(frame_ms, 0.95), pct(frame_ms, 0.99), pct(frame_ms, 1.0), rsum / n,
            pct(render_ms, 0.95), 100.0 * rsum / sum, (double)calls_sum / n);
        log("frame stats: in these %zu frames: %zu slow (> 33.4 ms); shaders %.1f ms (%llu functions, %llu from the cache), pipelines %.1f ms (%llu), GPU busy %.1f ms, GPU wait %.1f ms, drawable wait %.1f ms, "
            "texture uploads %.1f ms (%llu, %.1f MiB)",
            frame_ms.size(), slow, (double)(w.ns_translate + w.ns_compile - at_window.ns_translate - at_window.ns_compile) / 1e6,
            (unsigned long long)(w.functions - at_window.functions), (unsigned long long)(w.precompiled - at_window.precompiled),
            (double)(w.ns_pipeline - at_window.ns_pipeline) / 1e6,
            (unsigned long long)(w.pipelines - at_window.pipelines), (double)(w.ns_gpu - at_window.ns_gpu) / 1e6, (double)(w.ns_gpu_wait - at_window.ns_gpu_wait) / 1e6,
            (double)(w.ns_drawable - at_window.ns_drawable) / 1e6, (double)(w.ns_upload - at_window.ns_upload) / 1e6, (unsigned long long)(w.uploads - at_window.uploads),
            (double)(w.upload_bytes - at_window.upload_bytes) / 1048576.0);
        at_window = w;
        slow = 0;
        frame_ms.clear();
        render_ms.clear();
        calls_sum = 0;
    }
};
static FrameStats g_stats;

void observe(uint32_t tag, Cpu *, const uint32_t *a, int words, uint32_t result) {
    unsigned iface = tag >> 16, slot = tag & 0xFFFF;
    std::lock_guard<std::mutex> lk(g_mu);
    if (!g_renderer) {
        if (iface != IFACE_D3D9 || slot != 16 || (result & 0x80000000u)) return; // nothing to draw with before CreateDevice
        rhi::Options o;
        if (const char *d = getenv("BO1_D3D_DUMP")) o.dump_dir = d;
        // BO1_SHADER_CACHE_ALL=0: the shader cache only takes the shaders that were used, not every shader a level
        // creates (far fewer to compile; a shader never met before still stalls once).
        o.cache_created = !(getenv("BO1_SHADER_CACHE_ALL") && atoi(getenv("BO1_SHADER_CACHE_ALL")) == 0);
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
            // Boot credit (T-065): once, when the display comes up. The game is held back for as long, so that
            // nothing of its own start is hidden behind it. Window only; BO1_SPLASH=off skips it.
            static bool splash_shown = false;
            if (ok && !splash_shown) {
                splash_shown = true;
                const double seconds = 3.5;
                if (win::show_boot_splash(win::WindowService::get().metal_layer(), seconds))
                    std::this_thread::sleep_for(std::chrono::milliseconds((int)(seconds * 1000)));
            }
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
        case 34: case 37: case 39:
            send = true;                                             // StretchRect, SetRenderTarget, SetDepthStencilSurface: arguments only
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
    case IFACE_SURFACE:
        if (slot == 13 && ok) note_surface_lock(a[0], 0, a[0], a[1], a[2], a[3], false);
        else if (slot == 14) send = finish_lock(rec, a[0], 0);
        break;
    default: break; // Release is not forwarded: see on_destroy. Buffer locks: on_buffer_lock / on_buffer_unlock
    }
    if (!send) return;
    if (!g_stats.on) {
        g_renderer->call(rec.r, g_frame);
        if (present) g_renderer->end_frame(++g_frame);
        return;
    }
    // BO1_FRAME_STATS: time between Presents (what the player sees) and how much of it the renderer took
    auto t0 = std::chrono::steady_clock::now();
    g_renderer->call(rec.r, g_frame);
    if (present) g_renderer->end_frame(++g_frame);
    auto t1 = std::chrono::steady_clock::now();
    g_stats.renderer_ms += std::chrono::duration<double, std::milli>(t1 - t0).count();
    g_stats.calls++;
    if (present && g_stats.on) g_stats.frame_done(t1, g_renderer->work());
}

// ---- draw submission (P6) ------------------------------------------------------------------------------------
// `f(renderer)` under the renderer's lock. With BO1_FRAME_STATS the calls that do work (draws, buffer writes)
// are timed and count as renderer time, like a record's; the state setters, which only store a value, are
// counted but not timed — two clock reads cost several times what they do, and at 31,000 calls per frame the
// reads alone were 1.8 ms.
template <class F> inline void submit(F &&f, bool timed = false) {
    std::lock_guard<std::mutex> lk(g_mu);
    if (!g_renderer) return;
    if (!g_stats.on) {
        f(*g_renderer);
        return;
    }
    g_stats.calls++;
    if (!timed) {
        f(*g_renderer);
        return;
    }
    auto t0 = std::chrono::steady_clock::now();
    f(*g_renderer);
    g_stats.renderer_ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}
// IDirect3DDevice9 (argument 0 is `this`)
void on_set_render_state(uint32_t, Cpu *, const uint32_t *a, int, uint32_t) { // (State, Value)
    submit([a](rhi::Renderer &r) { r.set_render_state(a[1], a[2]); });
}
void on_set_texture(uint32_t, Cpu *, const uint32_t *a, int, uint32_t) { // (Stage, pTexture)
    submit([a](rhi::Renderer &r) { r.set_texture(a[1], a[2]); });
}
void on_set_sampler_state(uint32_t, Cpu *, const uint32_t *a, int, uint32_t) { // (Sampler, Type, Value)
    submit([a](rhi::Renderer &r) { r.set_sampler_state(a[1], a[2], a[3]); });
}
void on_set_vertex_declaration(uint32_t, Cpu *, const uint32_t *a, int, uint32_t) {
    submit([a](rhi::Renderer &r) { r.set_vertex_declaration(a[1]); });
}
void on_set_vertex_shader(uint32_t, Cpu *, const uint32_t *a, int, uint32_t) {
    submit([a](rhi::Renderer &r) { r.set_shader(true, a[1]); });
}
void on_set_pixel_shader(uint32_t, Cpu *, const uint32_t *a, int, uint32_t) {
    submit([a](rhi::Renderer &r) { r.set_shader(false, a[1]); });
}
void on_set_vs_constants(uint32_t, Cpu *, const uint32_t *a, int, uint32_t) { // (StartRegister, pConstantData, Vector4fCount)
    if (a[2]) submit([a](rhi::Renderer &r) { r.set_shader_constants(true, a[1], g_mem + a[2], a[3]); });
}
void on_set_ps_constants(uint32_t, Cpu *, const uint32_t *a, int, uint32_t) {
    if (a[2]) submit([a](rhi::Renderer &r) { r.set_shader_constants(false, a[1], g_mem + a[2], a[3]); });
}
void on_set_stream_source(uint32_t, Cpu *, const uint32_t *a, int, uint32_t) { // (StreamNumber, pStreamData, OffsetInBytes, Stride)
    submit([a](rhi::Renderer &r) { r.set_stream_source(a[1], a[2], a[3], a[4]); });
}
void on_set_indices(uint32_t, Cpu *, const uint32_t *a, int, uint32_t) {
    submit([a](rhi::Renderer &r) { r.set_indices(a[1]); });
}
void on_draw_indexed(uint32_t, Cpu *, const uint32_t *a, int, uint32_t) { // (Type, BaseVertexIndex, MinVertexIndex, NumVertices, startIndex, primCount)
    submit([a](rhi::Renderer &r) { r.draw_indexed(a[1], (int32_t)a[2], a[3], a[4], a[5], a[6]); }, true);
}
// IDirect3DVertexBuffer9 / IDirect3DIndexBuffer9
void on_buffer_lock(uint32_t, Cpu *, const uint32_t *a, int, uint32_t result) { // Lock(Offset, Size, ppData, Flags)
    if (result & 0x80000000u) return;
    uint32_t width = rd32(a[0] + R_WIDTH);
    BufLock l{a[0], a[1] < width ? a[1] : 0, 0, a[4], a[3] ? rd32(a[3]) : 0};
    l.size = a[2] && a[2] <= width - l.offset ? a[2] : width - l.offset;
    std::lock_guard<std::mutex> lk(g_mu);
    for (BufLock &have : g_buf_locks)
        if (have.obj == l.obj) {
            have = l;
            return;
        }
    g_buf_locks.push_back(l);
}
void on_buffer_unlock(uint32_t, Cpu *, const uint32_t *a, int, uint32_t) {
    BufLock l{};
    {
        std::lock_guard<std::mutex> lk(g_mu);
        size_t i = 0;
        while (i < g_buf_locks.size() && g_buf_locks[i].obj != a[0]) i++;
        if (i == g_buf_locks.size()) return;
        l = g_buf_locks[i];
        g_buf_locks[i] = g_buf_locks.back();
        g_buf_locks.pop_back();
    }
    // what the game wrote is still in the buffer's guest memory; a read-only lock hands nothing over
    bool has_data = l.data_ptr && l.size && !(l.flags & 0x10);
    submit([&](rhi::Renderer &r) { r.write_buffer(l.obj, l.flags, l.offset, has_data ? g_mem + l.data_ptr : nullptr, has_data ? l.size : 0); }, true);
}

// An object is gone for good (d3d9_null.cpp, "object lifetime"). The renderer is told with the record the
// trace replay shows it for the same event, a Release that returned 0 — and only now: the game's own Release
// calls are not forwarded, because an object the device still has bound outlives its last reference. After
// this the object's address, which is its id, may be handed out again.
void on_destroy(uint32_t obj, uint32_t kind) {
    std::lock_guard<std::mutex> lk(g_mu);
    std::erase_if(g_locks, [obj](const auto &kv) { return (uint32_t)kv.first == obj; });
    std::erase_if(g_buf_locks, [obj](const BufLock &l) { return l.obj == obj; });
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
    // the calls a frame consists of bypass `observe` (slot numbers: tools/d3d9_proxy/d3d9_methods.h)
    static const struct { unsigned slot; ComObserver fn; } kDevice[] = {
        {57, on_set_render_state}, {65, on_set_texture}, {69, on_set_sampler_state}, {82, on_draw_indexed},
        {87, on_set_vertex_declaration}, {92, on_set_vertex_shader}, {94, on_set_vs_constants}, {100, on_set_stream_source},
        {104, on_set_indices}, {107, on_set_pixel_shader}, {109, on_set_ps_constants},
    };
    for (const auto &m : kDevice) com_observe_slot("IDirect3DDevice9", m.slot, (IFACE_DEVICE << 16) | m.slot, m.fn);
    for (const char *iface : {"IDirect3DVertexBuffer9", "IDirect3DIndexBuffer9"}) {
        com_observe_slot(iface, 11, 11, on_buffer_lock);
        com_observe_slot(iface, 12, 12, on_buffer_unlock);
    }
    d3d9_set_destroy_hook(on_destroy);
}

} // namespace bo1
