// d3d9.dll for the native runtime: the guest-visible half of Direct3D 9 (T-045).
// Every interface the game uses exists as a guest-visible COM object (vtable layout and stack sizes
// from re/d3d9_abi.csv); resources hand out real guest memory from Lock*, reference counts are kept,
// state setters succeed. Nothing is drawn HERE: d3d9_live.cpp watches every call on these objects and
// forwards it to the Metal renderer (T-055, ADR-004); with BO1_D3D=null this file alone is the old
// "null device". Capability answers come from the Wine baseline (re/d3d9_adapter_probe.json) so that
// the game takes the same start-up decisions. Objects die the way Direct3D's do ("object lifetime" below, T-059).
#include <cstdlib>
#include <map>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "d3d9_objects.h"
#include "win32_util.h"

namespace bo1 {

enum : uint32_t { D3D_OK = 0, D3DERR_INVALIDCALL = 0x8876086Cu, D3DERR_NOTAVAILABLE = 0x8876086Au, E_NOINTERFACE = 0x80004002u };

struct MethodAbi {
    const char *iface;
    int slot;
    const char *name;
    int words;
};
static const MethodAbi kAbi[] = {
#include "d3d9_abi.inc"
};

// ---- answers recorded from the baseline ---------------------------------------------------------------
struct ProbeCheck {
    const char *method;
    uint32_t args[6];
    uint32_t hr, quality;
};
struct ProbeMode {
    uint32_t format, index, w, h, hz, fmt;
};
#include "d3d9_probe.inc" // kProbeCaps[], kProbeIdentifier[], kProbeChecks[], kProbeModes[], kProbeDisplayMode[4], kProbeAdapterCount

enum : uint32_t {
    DEV_D3D = COM_DATA + 0, DEV_BACKBUFFER = COM_DATA + 4, DEV_DEPTH = COM_DATA + 8, DEV_SWAPCHAIN = COM_DATA + 12, DEV_RT0 = COM_DATA + 16, // RT0..RT3
    DEV_DS = COM_DATA + 32, DEV_WIDTH = COM_DATA + 36, DEV_HEIGHT = COM_DATA + 40, DEV_FORMAT = COM_DATA + 44, DEV_HWND = COM_DATA + 48,
    DEV_BEHAVIOR = COM_DATA + 52, DEV_FRAMES = COM_DATA + 56, DEV_SIZE = 64,
};
enum : uint32_t { SC_DEVICE = COM_DATA };
static const uint32_t kTargetSlots[] = {DEV_RT0, DEV_RT0 + 4, DEV_RT0 + 8, DEV_RT0 + 12, DEV_DS}; // what the device draws to

// ---- object lifetime (T-059) ---------------------------------------------------------------------------------
// Reference counting follows Direct3D 9:
//  * a texture's level objects (surfaces, volumes) have no count of their own: AddRef / Release on them count
//    on the texture, and they live exactly as long as it does;
//  * the device holds what is bound to it (textures, stream sources, indices, shaders, vertex declaration,
//    render targets, depth-stencil): an object whose last reference the game drops while it is still bound
//    stays alive until it is unbound;
//  * the back buffer, the automatic depth-stencil surface and the swap chain belong to the device.
// When an object dies, its guest memory (the object, its lock memory, a texture's level table and level
// objects) goes back to the guest heap and the destroy hook tells the renderer: object ids are guest
// addresses, and the heap hands them out again.
struct ObjInfo {
    uint8_t kind;  // K_*
    bool implicit; // owned by the device: Release never destroys it
};
static std::recursive_mutex g_obj_mu;
static std::unordered_map<uint32_t, ObjInfo> g_objs; // every live object
static std::unordered_set<uint32_t> g_unreferenced;  // reference count 0, kept alive by a device binding
static D3d9DestroyHook g_destroy_hook = nullptr;
static uint32_t g_dev = 0; // the device whose bindings are tracked (the game has one at a time)
static struct Bindings {
    uint32_t tex[21]; // stages 0-15, D3DDMAPSAMPLER, D3DVERTEXTEXTURESAMPLER0-3
    uint32_t stream[16], ib, vs, ps, decl;
} g_bind;
static struct {
    uint64_t created, destroyed, lock_bytes;
    uint64_t outlived; // objects that a binding kept alive past their last reference
} g_stats;

void d3d9_set_destroy_hook(D3d9DestroyHook fn) { g_destroy_hook = fn; }

static uint32_t new_object(uint32_t vtable, uint32_t extra, uint32_t kind, bool implicit = false) {
    uint32_t o = com_new(vtable, extra);
    std::lock_guard<std::recursive_mutex> lk(g_obj_mu);
    g_objs[o] = {(uint8_t)kind, implicit};
    g_stats.created++;
    return o;
}
static uint32_t kind_of(uint32_t o) { // 0: not a live object
    auto it = g_objs.find(o);
    return it == g_objs.end() ? 0 : it->second.kind;
}
static bool is_resource(uint32_t kind) { return kind >= K_SURFACE && kind <= K_IB; }
// The object whose reference count `o` shares.
static uint32_t counted(uint32_t o, uint32_t kind) {
    if ((kind == K_SURFACE || kind == K_VOLUME) && rd32(o + R_PARENT)) return rd32(o + R_PARENT);
    return o;
}
static void stale(const char *what, uint32_t o) { // the game used an object after its destruction
    static int shown = 0;
    if (shown < 20 && ++shown) log("d3d9(null): %s on %08x, which is not a live object (ignored)", what, o);
}
static void free_backing(uint32_t o) {
    uint32_t m = rd32(o + R_MEM);
    if (!m) return;
    g_stats.lock_bytes -= heap_size(m);
    heap_free(m);
    wr32(o + R_MEM, 0);
}
static bool is_bound(uint32_t o) {
    for (uint32_t t : g_bind.tex)
        if (t == o) return true;
    for (uint32_t b : g_bind.stream)
        if (b == o) return true;
    if (g_bind.ib == o || g_bind.vs == o || g_bind.ps == o || g_bind.decl == o) return true;
    if (g_dev)
        for (uint32_t off : kTargetSlots) {
            uint32_t s = rd32(g_dev + off); // a bound level keeps its texture
            if (s && (s == o || counted(s, kind_of(s)) == o)) return true;
        }
    return false;
}
// One object: tell the renderer, give back its memory.
static void forget(uint32_t o, uint32_t kind) {
    if (g_destroy_hook) g_destroy_hook(o, kind);
    if (is_resource(kind)) free_backing(o);
    g_objs.erase(o);
    g_unreferenced.erase(o);
    g_stats.destroyed++;
    heap_free(o);
}
static void destroy_object(uint32_t o) {
    uint32_t kind = kind_of(o);
    if (kind == K_TEXTURE || kind == K_CUBE || kind == K_VOLUMETEX)
        if (uint32_t arr = rd32(o + R_CHILDREN)) {
            for (uint32_t i = 0, n = rd32(o + R_LEVELS) * 6; i < n; i++)
                if (uint32_t c = rd32(arr + 4 * i)) forget(c, kind_of(c));
            heap_free(arr);
        }
    forget(o, kind);
}
static void destroy_device(uint32_t dev);
static uint32_t add_ref(uint32_t o) {
    std::lock_guard<std::recursive_mutex> lk(g_obj_mu);
    uint32_t kind = kind_of(o);
    if (!kind) return stale("AddRef", o), 1;
    uint32_t t = counted(o, kind), n = rd32(t + COM_REFS) + 1;
    wr32(t + COM_REFS, n);
    return n;
}
static uint32_t release(uint32_t o) {
    std::lock_guard<std::recursive_mutex> lk(g_obj_mu);
    uint32_t kind = kind_of(o);
    if (!kind) return stale("Release", o), 0;
    uint32_t t = counted(o, kind), n = rd32(t + COM_REFS);
    if (!n) return 0; // nothing left to drop (device-owned, or only a binding keeps it)
    wr32(t + COM_REFS, --n);
    if (n) return n;
    ObjInfo info = g_objs[t];
    if (info.implicit) return 0;
    if (info.kind == K_DEVICE) destroy_device(t);
    else if (is_bound(t)) g_unreferenced.insert(t), g_stats.outlived++;
    else destroy_object(t);
    return 0;
}
// `o` left a binding slot: if only the binding kept it alive, it goes now.
static void unbound(uint32_t o) {
    if (!o || g_unreferenced.empty()) return;
    uint32_t t = counted(o, kind_of(o));
    auto it = g_unreferenced.find(t);
    if (it == g_unreferenced.end()) return;
    if (rd32(t + COM_REFS)) g_unreferenced.erase(it); // referenced again meanwhile (Get*)
    else if (!is_bound(t)) destroy_object(t);
}
static void bind(uint32_t &slot, uint32_t o) {
    std::lock_guard<std::recursive_mutex> lk(g_obj_mu);
    uint32_t old = slot;
    slot = o;
    if (old != o) unbound(old);
}
static void bind_guest(uint32_t slot_addr, uint32_t o) { // render targets / depth-stencil: the slot is a device field
    std::lock_guard<std::recursive_mutex> lk(g_obj_mu);
    uint32_t old = rd32(slot_addr);
    wr32(slot_addr, o);
    if (old != o) unbound(old);
}
// Everything the device has bound is let go (Reset, destruction of the device).
static void release_bindings(uint32_t dev) {
    std::vector<uint32_t> was;
    for (uint32_t t : g_bind.tex) was.push_back(t);
    for (uint32_t b : g_bind.stream) was.push_back(b);
    was.insert(was.end(), {g_bind.ib, g_bind.vs, g_bind.ps, g_bind.decl});
    g_bind = {};
    for (uint32_t off : kTargetSlots) {
        was.push_back(rd32(dev + off));
        wr32(dev + off, 0);
    }
    for (uint32_t o : was) unbound(o);
}
static void destroy_device(uint32_t dev) {
    release_bindings(dev);
    for (uint32_t off : {(uint32_t)DEV_BACKBUFFER, (uint32_t)DEV_DEPTH, (uint32_t)DEV_SWAPCHAIN})
        if (uint32_t o = rd32(dev + off)) forget(o, kind_of(o));
    uint32_t d3d = rd32(dev + DEV_D3D);
    forget(dev, K_DEVICE);
    if (g_dev == dev) g_dev = 0;
    log("d3d9(null): device %08x destroyed; %zu objects still alive (%llu created, %llu destroyed)", dev, g_objs.size(),
        (unsigned long long)g_stats.created, (unsigned long long)g_stats.destroyed);
    if (d3d) release(d3d); // the device held its IDirect3D9
}

// ---- generic IUnknown + default method ---------------------------------------------------------------------
static uint32_t m_ok(Cpu *, const uint32_t *) { return D3D_OK; }
static uint32_t m_addref(Cpu *, const uint32_t *a) { return add_ref(a[0]); }
static uint32_t m_release(Cpu *, const uint32_t *a) { return release(a[0]); }
static uint32_t m_query_interface(Cpu *, const uint32_t *a) {
    if (!a[2]) return 0x80004003u; // E_POINTER
    wr32(a[2], a[0]);
    add_ref(a[0]);
    return D3D_OK;
}

using Overrides = std::map<std::string, ShimFn>;
static uint32_t make_vtable(const char *iface, const Overrides &ov) {
    std::vector<ComMethod> methods;
    static std::vector<std::string> names; // ComMethod keeps const char*: names must outlive the table
    names.reserve(2048);
    for (const MethodAbi &m : kAbi) {
        if (strcmp(m.iface, iface) != 0) continue;
        ShimFn fn = m_ok;
        std::string label = m.name;
        auto it = ov.find(m.name);
        if (it != ov.end()) fn = it->second;
        else if (label == "AddRef") fn = m_addref;
        else if (label == "Release") fn = m_release;
        else if (label == "QueryInterface") fn = m_query_interface;
        else label += " [default: D3D_OK]";
        names.push_back(label);
        methods.push_back({names.back().c_str(), m.words, fn});
    }
    return com_vtable(iface, methods.data(), methods.size());
}

uint32_t d3d9_format_bytes(uint32_t fmt, uint32_t w, uint32_t h, uint32_t *pitch) {
    uint32_t bw = (w + 3) / 4, bh = (h + 3) / 4;
    auto fourcc = [](char a, char b, char c, char d) { return (uint32_t)(uint8_t)a | (uint32_t)(uint8_t)b << 8 | (uint32_t)(uint8_t)c << 16 | (uint32_t)(uint8_t)d << 24; };
    if (fmt == fourcc('D', 'X', 'T', '1')) return *pitch = bw * 8, bw * 8 * bh;
    if (fmt == fourcc('D', 'X', 'T', '2') || fmt == fourcc('D', 'X', 'T', '3') || fmt == fourcc('D', 'X', 'T', '4') || fmt == fourcc('D', 'X', 'T', '5'))
        return *pitch = bw * 16, bw * 16 * bh;
    uint32_t bpp;
    switch (fmt) {
    case 20: bpp = 3; break;                                                         // R8G8B8
    case 23: case 24: case 25: case 26: case 30: case 51: case 80: case 81: case 111: bpp = 2; break; // 16-bit colour, A8L8, D16, L16, R16F
    case 28: case 50: case 52: case 41: bpp = 1; break;                              // A8, L8, A4L4, P8
    case 36: case 113: case 115: bpp = 8; break;                                     // A16B16G16R16(F), G32R32F
    case 116: bpp = 16; break;                                                       // A32B32G32R32F
    default: bpp = 4; break;                                                         // 32-bit colour, depth formats, R32F, G16R16...
    }
    *pitch = w * bpp;
    return w * bpp * h;
}

static uint32_t vt(const char *iface);
static uint32_t new_resource(const char *iface, uint32_t kind, uint32_t fmt, uint32_t usage, uint32_t pool, uint32_t w, uint32_t h, uint32_t d,
                             uint32_t levels, bool implicit = false) {
    uint32_t o = new_object(vt(iface), R_SIZE, kind, implicit);
    wr32(o + R_KIND, kind);
    wr32(o + R_FORMAT, fmt);
    wr32(o + R_USAGE, usage);
    wr32(o + R_POOL, pool);
    wr32(o + R_WIDTH, w);
    wr32(o + R_HEIGHT, h);
    wr32(o + R_DEPTH, d);
    wr32(o + R_LEVELS, levels);
    return o;
}
static uint32_t backing(uint32_t o, uint32_t bytes) {
    uint32_t m = rd32(o + R_MEM);
    if (!m) {
        m = heap_alloc(bytes ? bytes : 16, true);
        wr32(o + R_MEM, m);
        std::lock_guard<std::recursive_mutex> lk(g_obj_mu);
        g_stats.lock_bytes += bytes ? bytes : 16;
    }
    return m;
}
static uint32_t level_count(uint32_t levels, uint32_t w, uint32_t h, uint32_t d) {
    if (levels) return levels;
    uint32_t n = 1, m = w > h ? w : h;
    if (d > m) m = d;
    while (m > 1) m >>= 1, n++;
    return n;
}
static uint32_t dim(uint32_t v, uint32_t level) { return (v >> level) ? (v >> level) : 1; }

// ---- surfaces / volumes ----------------------------------------------------------------------------------------
static uint32_t surf_get_desc(Cpu *, const uint32_t *a) {
    // D3DSURFACE_DESC: Format, Type, Usage, Pool, MultiSampleType, MultiSampleQuality, Width, Height
    uint32_t o = a[0], d = a[1];
    wr32(d, rd32(o + R_FORMAT));
    wr32(d + 4, 1); // D3DRTYPE_SURFACE
    wr32(d + 8, rd32(o + R_USAGE));
    wr32(d + 12, rd32(o + R_POOL));
    wr32(d + 16, 0);
    wr32(d + 20, 0);
    wr32(d + 24, rd32(o + R_WIDTH));
    wr32(d + 28, rd32(o + R_HEIGHT));
    return D3D_OK;
}
static uint32_t surf_lock(uint32_t o, uint32_t locked_rect, uint32_t rect) {
    uint32_t pitch, bytes = d3d9_format_bytes(rd32(o + R_FORMAT), rd32(o + R_WIDTH), rd32(o + R_HEIGHT), &pitch);
    uint32_t mem = backing(o, bytes);
    uint32_t off = 0;
    if (rect) { // RECT left, top, ...; block-compressed formats address whole 4x4 blocks
        uint32_t fmt = rd32(o + R_FORMAT), w = rd32(o + R_WIDTH);
        bool dxt = (fmt & 0x00FFFFFF) == 0x00545844; // 'DXT' + digit
        if (dxt) off = (rd32(rect + 4) / 4) * pitch + (rd32(rect) / 4) * (pitch / ((w + 3) / 4 ? (w + 3) / 4 : 1));
        else off = rd32(rect + 4) * pitch + rd32(rect) * (w ? pitch / w : 0);
        if (off >= bytes) off = 0;
    }
    wr32(locked_rect, pitch);
    wr32(locked_rect + 4, mem + off);
    return D3D_OK;
}
static uint32_t surf_lock_rect(Cpu *, const uint32_t *a) { return surf_lock(a[0], a[1], a[2]); }
static uint32_t surf_get_container(Cpu *, const uint32_t *a) {
    uint32_t p = rd32(a[0] + R_PARENT);
    if (!p) return E_NOINTERFACE;
    wr32(a[2], p);
    add_ref(p);
    return D3D_OK;
}
static uint32_t vol_get_desc(Cpu *, const uint32_t *a) {
    // D3DVOLUME_DESC: Format, Type, Usage, Pool, Width, Height, Depth
    uint32_t o = a[0], d = a[1];
    wr32(d, rd32(o + R_FORMAT));
    wr32(d + 4, 2); // D3DRTYPE_VOLUME
    wr32(d + 8, rd32(o + R_USAGE));
    wr32(d + 12, rd32(o + R_POOL));
    wr32(d + 16, rd32(o + R_WIDTH));
    wr32(d + 20, rd32(o + R_HEIGHT));
    wr32(d + 24, rd32(o + R_DEPTH));
    return D3D_OK;
}
static uint32_t vol_lock(uint32_t o, uint32_t locked_box) {
    uint32_t pitch, slice = d3d9_format_bytes(rd32(o + R_FORMAT), rd32(o + R_WIDTH), rd32(o + R_HEIGHT), &pitch);
    uint32_t mem = backing(o, slice * rd32(o + R_DEPTH));
    wr32(locked_box, pitch);
    wr32(locked_box + 4, slice);
    wr32(locked_box + 8, mem);
    return D3D_OK;
}
static uint32_t vol_lock_box(Cpu *, const uint32_t *a) { return vol_lock(a[0], a[1]); }

// ---- textures ----------------------------------------------------------------------------------------------------
uint32_t d3d9_tex_child(uint32_t tex, uint32_t face, uint32_t level) {
    uint32_t levels = rd32(tex + R_LEVELS), kind = rd32(tex + R_KIND);
    if (level >= levels) return 0;
    if (uint32_t have = rd32(tex + R_CHILDREN)) // the usual case, without the lock: d3d9_live.cpp only asks for levels that exist
        if (uint32_t c = rd32(have + 4 * (face * levels + level))) return c;
    std::lock_guard<std::recursive_mutex> lk(g_obj_mu);
    uint32_t arr = rd32(tex + R_CHILDREN);
    if (!arr) {
        arr = heap_alloc(4 * levels * 6, true);
        wr32(tex + R_CHILDREN, arr);
    }
    uint32_t slot = arr + 4 * (face * levels + level);
    uint32_t c = rd32(slot);
    if (!c) {
        if (kind == K_VOLUMETEX)
            c = new_resource("IDirect3DVolume9", K_VOLUME, rd32(tex + R_FORMAT), rd32(tex + R_USAGE), rd32(tex + R_POOL), dim(rd32(tex + R_WIDTH), level),
                             dim(rd32(tex + R_HEIGHT), level), dim(rd32(tex + R_DEPTH), level), 1);
        else
            c = new_resource("IDirect3DSurface9", K_SURFACE, rd32(tex + R_FORMAT), rd32(tex + R_USAGE), rd32(tex + R_POOL), dim(rd32(tex + R_WIDTH), level),
                             dim(rd32(tex + R_HEIGHT), level), 1, 1);
        wr32(c + R_PARENT, tex);
        wr32(slot, c);
    }
    return c;
}
static uint32_t tex_get_level_count(Cpu *, const uint32_t *a) { return rd32(a[0] + R_LEVELS); }
static uint32_t tex_get_type(Cpu *, const uint32_t *a) {
    switch (rd32(a[0] + R_KIND)) {
    case K_SURFACE: return 1;
    case K_VOLUME: return 2;
    case K_TEXTURE: return 3;
    case K_VOLUMETEX: return 4;
    case K_CUBE: return 5;
    case K_VB: return 6;
    default: return 7;
    }
}
static uint32_t tex_get_level_desc(Cpu *cpu, const uint32_t *a) {
    uint32_t c = d3d9_tex_child(a[0], 0, a[1]);
    if (!c) return D3DERR_INVALIDCALL;
    uint32_t args[2] = {c, a[2]};
    return rd32(a[0] + R_KIND) == K_VOLUMETEX ? vol_get_desc(cpu, args) : surf_get_desc(cpu, args);
}
static uint32_t tex_get_surface_level(Cpu *, const uint32_t *a) { // (this, Level, ppSurface)
    uint32_t c = d3d9_tex_child(a[0], 0, a[1]);
    if (!c) return D3DERR_INVALIDCALL;
    add_ref(c);
    wr32(a[2], c);
    return D3D_OK;
}
static uint32_t tex_lock_rect(Cpu *, const uint32_t *a) { // (this, Level, pLockedRect, pRect, Flags)
    uint32_t c = d3d9_tex_child(a[0], 0, a[1]);
    return c ? surf_lock(c, a[2], a[3]) : D3DERR_INVALIDCALL;
}
static uint32_t cube_get_surface(Cpu *, const uint32_t *a) { // (this, FaceType, Level, ppSurface)
    uint32_t c = a[1] < 6 ? d3d9_tex_child(a[0], a[1], a[2]) : 0;
    if (!c) return D3DERR_INVALIDCALL;
    add_ref(c);
    wr32(a[3], c);
    return D3D_OK;
}
static uint32_t cube_lock_rect(Cpu *, const uint32_t *a) { // (this, FaceType, Level, pLockedRect, pRect, Flags)
    uint32_t c = a[1] < 6 ? d3d9_tex_child(a[0], a[1], a[2]) : 0;
    return c ? surf_lock(c, a[3], a[4]) : D3DERR_INVALIDCALL;
}
static uint32_t voltex_get_volume(Cpu *, const uint32_t *a) { // (this, Level, ppVolume)
    uint32_t c = d3d9_tex_child(a[0], 0, a[1]);
    if (!c) return D3DERR_INVALIDCALL;
    add_ref(c);
    wr32(a[2], c);
    return D3D_OK;
}
static uint32_t voltex_lock_box(Cpu *, const uint32_t *a) { // (this, Level, pLockedVolume, pBox, Flags)
    uint32_t c = d3d9_tex_child(a[0], 0, a[1]);
    return c ? vol_lock(c, a[2]) : D3DERR_INVALIDCALL;
}

// ---- vertex / index buffers ------------------------------------------------------------------------------------
static uint32_t buf_lock(Cpu *, const uint32_t *a) { // (this, OffsetToLock, SizeToLock, ppbData, Flags)
    uint32_t len = rd32(a[0] + R_WIDTH);
    uint32_t mem = backing(a[0], len);
    wr32(a[3], mem + (a[1] < len ? a[1] : 0));
    return D3D_OK;
}
static uint32_t vb_get_desc(Cpu *, const uint32_t *a) { // D3DVERTEXBUFFER_DESC: Format, Type, Usage, Pool, Size, FVF
    wr32(a[1], 100); // D3DFMT_VERTEXDATA
    wr32(a[1] + 4, 6);
    wr32(a[1] + 8, rd32(a[0] + R_USAGE));
    wr32(a[1] + 12, rd32(a[0] + R_POOL));
    wr32(a[1] + 16, rd32(a[0] + R_WIDTH));
    wr32(a[1] + 20, rd32(a[0] + R_FVF));
    return D3D_OK;
}
static uint32_t ib_get_desc(Cpu *, const uint32_t *a) { // D3DINDEXBUFFER_DESC: Format, Type, Usage, Pool, Size
    wr32(a[1], rd32(a[0] + R_FORMAT));
    wr32(a[1] + 4, 7);
    wr32(a[1] + 8, rd32(a[0] + R_USAGE));
    wr32(a[1] + 12, rd32(a[0] + R_POOL));
    wr32(a[1] + 16, rd32(a[0] + R_WIDTH));
    return D3D_OK;
}

// ---- queries --------------------------------------------------------------------------------------------------------
enum : uint32_t { Q_TYPE = COM_DATA };
static uint32_t query_get_type(Cpu *, const uint32_t *a) { return rd32(a[0] + Q_TYPE); }
static uint32_t query_get_data_size(Cpu *, const uint32_t *a) { return 4; }
static uint32_t query_get_data(Cpu *, const uint32_t *a) { // (this, pData, dwSize, dwGetDataFlags)
    // Results are immediately available: an event query has completed, an occlusion query saw pixels.
    if (a[1] && a[2] >= 4) wr32(a[1], rd32(a[0] + Q_TYPE) == 9 /* OCCLUSION */ ? 1000 : 1);
    return D3D_OK;
}

// ---- device -------------------------------------------------------------------------------------------------------------
static void out_obj(uint32_t pp, uint32_t o) {
    if (o) add_ref(o);
    wr32(pp, o);
}

static uint32_t dev_get_direct3d(Cpu *, const uint32_t *a) { return out_obj(a[1], rd32(a[0] + DEV_D3D)), D3D_OK; }
static uint32_t dev_get_caps(Cpu *, const uint32_t *a) {
    memcpy(g_mem + a[1], kProbeCaps, sizeof(kProbeCaps));
    return D3D_OK;
}
static uint32_t dev_get_display_mode(Cpu *, const uint32_t *a) { // (this, iSwapChain, pMode)
    for (int i = 0; i < 4; i++) wr32(a[2] + 4 * i, kProbeDisplayMode[i]);
    return D3D_OK;
}
static uint32_t dev_get_creation_parameters(Cpu *, const uint32_t *a) { // AdapterOrdinal, DeviceType, hFocusWindow, BehaviorFlags
    wr32(a[1], 0);
    wr32(a[1] + 4, 1);
    wr32(a[1] + 8, rd32(a[0] + DEV_HWND));
    wr32(a[1] + 12, rd32(a[0] + DEV_BEHAVIOR));
    return D3D_OK;
}
static uint32_t dev_get_available_texture_mem(Cpu *, const uint32_t *) { return 1024u << 20; }
static uint32_t dev_get_swap_chain(Cpu *, const uint32_t *a) { return out_obj(a[2], rd32(a[0] + DEV_SWAPCHAIN)), D3D_OK; }
static uint32_t dev_get_number_of_swap_chains(Cpu *, const uint32_t *) { return 1; }
static uint32_t dev_get_back_buffer(Cpu *, const uint32_t *a) { // (this, iSwapChain, iBackBuffer, Type, ppBackBuffer)
    return out_obj(a[4], rd32(a[0] + DEV_BACKBUFFER)), D3D_OK;
}
static uint32_t dev_present(Cpu *, const uint32_t *a) {
    uint32_t n = rd32(a[0] + DEV_FRAMES) + 1;
    wr32(a[0] + DEV_FRAMES, n);
    if (g_verbose && (n <= 3 || n % 500 == 0)) {
        std::lock_guard<std::recursive_mutex> lk(g_obj_mu);
        log("d3d9(null): frame %u presented; objects: %zu alive, %llu created, %llu destroyed (%llu while still bound, %zu of them waiting), "
            "%.1f MiB lock memory",
            n, g_objs.size(), (unsigned long long)g_stats.created, (unsigned long long)g_stats.destroyed, (unsigned long long)g_stats.outlived,
            g_unreferenced.size(), g_stats.lock_bytes / 1048576.0);
    }
    return D3D_OK;
}
static uint32_t dev_create_texture(Cpu *, const uint32_t *a) { // (this, Width, Height, Levels, Usage, Format, Pool, ppTexture, pSharedHandle)
    out_obj(a[7], 0);
    uint32_t o = new_resource("IDirect3DTexture9", K_TEXTURE, a[5], a[4], a[6], a[1], a[2], 1, level_count(a[3], a[1], a[2], 1));
    wr32(a[7], o);
    return D3D_OK;
}
static uint32_t dev_create_volume_texture(Cpu *, const uint32_t *a) { // (this, W, H, D, Levels, Usage, Format, Pool, pp, shared)
    wr32(a[8], new_resource("IDirect3DVolumeTexture9", K_VOLUMETEX, a[6], a[5], a[7], a[1], a[2], a[3], level_count(a[4], a[1], a[2], a[3])));
    return D3D_OK;
}
static uint32_t dev_create_cube_texture(Cpu *, const uint32_t *a) { // (this, EdgeLength, Levels, Usage, Format, Pool, pp, shared)
    wr32(a[6], new_resource("IDirect3DCubeTexture9", K_CUBE, a[4], a[3], a[5], a[1], a[1], 1, level_count(a[2], a[1], a[1], 1)));
    return D3D_OK;
}
static uint32_t dev_create_vertex_buffer(Cpu *, const uint32_t *a) { // (this, Length, Usage, FVF, Pool, pp, shared)
    uint32_t o = new_resource("IDirect3DVertexBuffer9", K_VB, 100, a[2], a[4], a[1], 1, 1, 1);
    wr32(o + R_FVF, a[3]);
    wr32(a[5], o);
    return D3D_OK;
}
static uint32_t dev_create_index_buffer(Cpu *, const uint32_t *a) { // (this, Length, Usage, Format, Pool, pp, shared)
    wr32(a[5], new_resource("IDirect3DIndexBuffer9", K_IB, a[3], a[2], a[4], a[1], 1, 1, 1));
    return D3D_OK;
}
static uint32_t dev_create_render_target(Cpu *, const uint32_t *a) { // (this, W, H, Format, MultiSample, Quality, Lockable, ppSurface, shared)
    wr32(a[7], new_resource("IDirect3DSurface9", K_SURFACE, a[3], 1 /* RENDERTARGET */, 0, a[1], a[2], 1, 1));
    return D3D_OK;
}
static uint32_t dev_create_depth_stencil(Cpu *, const uint32_t *a) { // (this, W, H, Format, MultiSample, Quality, Discard, ppSurface, shared)
    wr32(a[7], new_resource("IDirect3DSurface9", K_SURFACE, a[3], 2 /* DEPTHSTENCIL */, 0, a[1], a[2], 1, 1));
    return D3D_OK;
}
static uint32_t dev_create_offscreen_surface(Cpu *, const uint32_t *a) { // (this, W, H, Format, Pool, ppSurface, shared)
    wr32(a[5], new_resource("IDirect3DSurface9", K_SURFACE, a[3], 0, a[4], a[1], a[2], 1, 1));
    return D3D_OK;
}
static uint32_t dev_create_opaque(const char *iface, uint32_t kind, uint32_t pp) {
    wr32(pp, new_object(vt(iface), 8, kind));
    return D3D_OK;
}
static uint32_t dev_create_vertex_declaration(Cpu *, const uint32_t *a) { return dev_create_opaque("IDirect3DVertexDeclaration9", K_VDECL, a[2]); }
static uint32_t dev_create_vertex_shader(Cpu *, const uint32_t *a) { return dev_create_opaque("IDirect3DVertexShader9", K_VSHADER, a[2]); }
static uint32_t dev_create_pixel_shader(Cpu *, const uint32_t *a) { return dev_create_opaque("IDirect3DPixelShader9", K_PSHADER, a[2]); }
static uint32_t dev_create_state_block(Cpu *, const uint32_t *a) { return dev_create_opaque("IDirect3DStateBlock9", K_STATEBLOCK, a[2]); }
static uint32_t dev_create_query(Cpu *, const uint32_t *a) { // (this, Type, ppQuery) — ppQuery may be NULL (support check)
    if (!a[2]) return D3D_OK;
    uint32_t q = new_object(vt("IDirect3DQuery9"), 8, K_QUERY);
    wr32(q + Q_TYPE, a[1]);
    wr32(a[2], q);
    return D3D_OK;
}
static uint32_t dev_get_render_target(Cpu *, const uint32_t *a) { // (this, Index, ppRenderTarget)
    uint32_t s = a[1] < 4 ? rd32(a[0] + DEV_RT0 + 4 * a[1]) : 0;
    if (!s) return 0x88760866u; // D3DERR_NOTFOUND
    return out_obj(a[2], s), D3D_OK;
}
static uint32_t dev_set_render_target(Cpu *, const uint32_t *a) {
    if (a[1] < 4) bind_guest(a[0] + DEV_RT0 + 4 * a[1], a[2]);
    return D3D_OK;
}
static uint32_t dev_get_depth_stencil(Cpu *, const uint32_t *a) {
    uint32_t s = rd32(a[0] + DEV_DS);
    if (!s) return 0x88760866u;
    return out_obj(a[1], s), D3D_OK;
}
static uint32_t dev_set_depth_stencil(Cpu *, const uint32_t *a) {
    bind_guest(a[0] + DEV_DS, a[1]);
    return D3D_OK;
}
// The other bindings exist only to keep bound objects alive (see "object lifetime").
static uint32_t dev_set_texture(Cpu *, const uint32_t *a) { // (this, Stage, pTexture)
    uint32_t s = a[1] < 16 ? a[1] : a[1] >= 256 && a[1] <= 260 ? a[1] - 240 : ~0u;
    if (s < 21) bind(g_bind.tex[s], a[2]);
    return D3D_OK;
}
static uint32_t dev_set_stream_source(Cpu *, const uint32_t *a) { // (this, StreamNumber, pStreamData, OffsetInBytes, Stride)
    if (a[1] < 16) bind(g_bind.stream[a[1]], a[2]);
    return D3D_OK;
}
static uint32_t dev_set_indices(Cpu *, const uint32_t *a) { return bind(g_bind.ib, a[1]), D3D_OK; }
static uint32_t dev_set_vertex_declaration(Cpu *, const uint32_t *a) { return bind(g_bind.decl, a[1]), D3D_OK; }
static uint32_t dev_set_vertex_shader(Cpu *, const uint32_t *a) { return bind(g_bind.vs, a[1]), D3D_OK; }
static uint32_t dev_set_pixel_shader(Cpu *, const uint32_t *a) { return bind(g_bind.ps, a[1]), D3D_OK; }

// Back buffer and automatic depth-stencil as D3DPRESENT_PARAMETERS ask for them (CreateDevice, Reset); they
// become the current targets.
// D3DPRESENT_PARAMETERS: BackBufferWidth, Height, Format, Count, MultiSampleType, Quality, SwapEffect, hDeviceWindow,
//                        Windowed, EnableAutoDepthStencil, AutoDepthStencilFormat, Flags, RefreshRate, PresentationInterval
static void apply_present_parameters(uint32_t dev, uint32_t pp) {
    uint32_t w = rd32(pp), h = rd32(pp + 4), fmt = rd32(pp + 8);
    if (!w || !h) w = 800, h = 600; // windowed default: client area (the headless windows are bookkeeping only)
    if (!fmt) fmt = kProbeDisplayMode[3];
    if (!rd32(pp + 12)) wr32(pp + 12, 1);
    wr32(pp, w), wr32(pp + 4, h), wr32(pp + 8, fmt);
    wr32(dev + DEV_WIDTH, w), wr32(dev + DEV_HEIGHT, h), wr32(dev + DEV_FORMAT, fmt);
    auto implicit_surface = [&](uint32_t field, uint32_t format, uint32_t usage) {
        uint32_t s = rd32(dev + field);
        if (!s) {
            s = new_resource("IDirect3DSurface9", K_SURFACE, format, usage, 0, w, h, 1, 1, true);
            wr32(dev + field, s);
        } else { // Reset: same object, new size
            free_backing(s);
            wr32(s + R_FORMAT, format), wr32(s + R_WIDTH, w), wr32(s + R_HEIGHT, h);
        }
        return s;
    };
    wr32(dev + DEV_RT0, implicit_surface(DEV_BACKBUFFER, fmt, 1 /* RENDERTARGET */));
    wr32(dev + DEV_DS, rd32(pp + 36) ? implicit_surface(DEV_DEPTH, rd32(pp + 40), 2 /* DEPTHSTENCIL */) : 0);
}
// Reset: every binding is dropped, the implicit surfaces take the new size. (Direct3D refuses a Reset while
// D3DPOOL_DEFAULT resources exist; the game has to release them first either way, so this one always succeeds.)
static uint32_t dev_reset(Cpu *, const uint32_t *a) { // (this, pPresentationParameters)
    std::lock_guard<std::recursive_mutex> lk(g_obj_mu);
    release_bindings(a[0]);
    apply_present_parameters(a[0], a[1]);
    log("d3d9(null): Reset %ux%u format %u; objects: %zu alive, %.1f MiB lock memory", rd32(a[0] + DEV_WIDTH), rd32(a[0] + DEV_HEIGHT),
        rd32(a[0] + DEV_FORMAT), g_objs.size(), g_stats.lock_bytes / 1048576.0);
    return D3D_OK;
}
static uint32_t sc_get_back_buffer(Cpu *, const uint32_t *a) { // (this, iBackBuffer, Type, ppBackBuffer)
    return out_obj(a[3], rd32(rd32(a[0] + SC_DEVICE) + DEV_BACKBUFFER)), D3D_OK;
}
static uint32_t sc_present(Cpu *cpu, const uint32_t *a) {
    uint32_t args[1] = {rd32(a[0] + SC_DEVICE)};
    return dev_present(cpu, args);
}
static uint32_t sc_get_device(Cpu *, const uint32_t *a) { return out_obj(a[1], rd32(a[0] + SC_DEVICE)), D3D_OK; }

// ---- IDirect3D9 --------------------------------------------------------------------------------------------------------
static uint32_t d3d_get_adapter_count(Cpu *, const uint32_t *) { return kProbeAdapterCount; }
static uint32_t d3d_get_adapter_identifier(Cpu *, const uint32_t *a) {
    memcpy(g_mem + a[3], kProbeIdentifier, sizeof(kProbeIdentifier));
    return D3D_OK;
}
static uint32_t d3d_get_adapter_mode_count(Cpu *, const uint32_t *a) {
    uint32_t n = 0;
    for (const ProbeMode &m : kProbeModes) n += m.format == a[2];
    return n;
}
static uint32_t d3d_enum_adapter_modes(Cpu *, const uint32_t *a) { // (this, Adapter, Format, Mode, pMode)
    for (const ProbeMode &m : kProbeModes)
        if (m.format == a[2] && m.index == a[3]) {
            wr32(a[4], m.w), wr32(a[4] + 4, m.h), wr32(a[4] + 8, m.hz), wr32(a[4] + 12, m.fmt);
            return D3D_OK;
        }
    return D3DERR_INVALIDCALL;
}
static uint32_t d3d_get_adapter_display_mode(Cpu *, const uint32_t *a) {
    for (int i = 0; i < 4; i++) wr32(a[2] + 4 * i, kProbeDisplayMode[i]);
    return D3D_OK;
}
static uint32_t d3d_get_device_caps(Cpu *, const uint32_t *a) {
    memcpy(g_mem + a[3], kProbeCaps, sizeof(kProbeCaps));
    return D3D_OK;
}
static uint32_t d3d_get_adapter_monitor(Cpu *, const uint32_t *) { return 0x00010001; } // same value as user32's monitor handle
// Check*: the recorded answer when the baseline was asked the same question, otherwise "not available"
// (logged: an unrecorded question means the game took a different path than in the baseline).
static uint32_t probe_check(const char *method, const uint32_t *a, int nargs, uint32_t out_quality) {
    for (const ProbeCheck &c : kProbeChecks) {
        if (strcmp(c.method, method) != 0) continue;
        bool same = true;
        for (int i = 0; i < nargs; i++) same &= c.args[i] == a[1 + i];
        if (!same) continue;
        if (out_quality) wr32(out_quality, c.quality);
        return c.hr;
    }
    log("d3d9(null): %s(%x, %x, %x, %x, %x, %x) was not asked in the baseline -> D3DERR_NOTAVAILABLE", method, a[1], nargs > 1 ? a[2] : 0,
        nargs > 2 ? a[3] : 0, nargs > 3 ? a[4] : 0, nargs > 4 ? a[5] : 0, nargs > 5 ? a[6] : 0);
    return D3DERR_NOTAVAILABLE;
}
static uint32_t d3d_check_device_type(Cpu *, const uint32_t *a) { return probe_check("CheckDeviceType", a, 5, 0); }
static uint32_t d3d_check_device_format(Cpu *, const uint32_t *a) { return probe_check("CheckDeviceFormat", a, 6, 0); }
static uint32_t d3d_check_multisample(Cpu *, const uint32_t *a) { return probe_check("CheckDeviceMultiSampleType", a, 5, a[6]); }
static uint32_t d3d_check_depth_stencil_match(Cpu *, const uint32_t *a) { return probe_check("CheckDepthStencilMatch", a, 5, 0); }
static uint32_t d3d_check_format_conversion(Cpu *, const uint32_t *a) { return probe_check("CheckDeviceFormatConversion", a, 4, 0); }

static uint32_t d3d_create_device(Cpu *, const uint32_t *a) {
    // (this, Adapter, DeviceType, hFocusWindow, BehaviorFlags, pPresentationParameters, ppReturnedDeviceInterface)
    std::lock_guard<std::recursive_mutex> lk(g_obj_mu);
    if (g_dev) log("d3d9(null): CreateDevice while device %08x exists: bindings are tracked for the new one only", g_dev);
    uint32_t dev = new_object(vt("IDirect3DDevice9"), DEV_SIZE, K_DEVICE);
    wr32(dev + DEV_D3D, a[0]);
    add_ref(a[0]); // the device holds its IDirect3D9
    wr32(dev + DEV_HWND, a[3]);
    wr32(dev + DEV_BEHAVIOR, a[4]);
    apply_present_parameters(dev, a[5]);
    uint32_t sc = new_object(vt("IDirect3DSwapChain9"), 8, K_SWAPCHAIN, true);
    wr32(sc + SC_DEVICE, dev);
    wr32(dev + DEV_SWAPCHAIN, sc);
    g_dev = dev;
    g_bind = {};
    wr32(a[6], dev);
    log("d3d9(null): CreateDevice %ux%u format %u behavior %#x -> %08x", rd32(dev + DEV_WIDTH), rd32(dev + DEV_HEIGHT), rd32(dev + DEV_FORMAT), a[4],
        dev);
    return D3D_OK;
}

static uint32_t vt(const char *iface) {
    static std::mutex mu;
    static std::map<std::string, uint32_t> cache;
    std::lock_guard<std::mutex> lk(mu);
    auto it = cache.find(iface);
    if (it != cache.end()) return it->second;
    static const std::map<std::string, Overrides> all = {
        {"IDirect3D9",
         {{"GetAdapterCount", d3d_get_adapter_count}, {"GetAdapterIdentifier", d3d_get_adapter_identifier},
          {"GetAdapterModeCount", d3d_get_adapter_mode_count}, {"EnumAdapterModes", d3d_enum_adapter_modes},
          {"GetAdapterDisplayMode", d3d_get_adapter_display_mode}, {"CheckDeviceType", d3d_check_device_type},
          {"CheckDeviceFormat", d3d_check_device_format}, {"CheckDeviceMultiSampleType", d3d_check_multisample},
          {"CheckDepthStencilMatch", d3d_check_depth_stencil_match}, {"CheckDeviceFormatConversion", d3d_check_format_conversion},
          {"GetDeviceCaps", d3d_get_device_caps}, {"GetAdapterMonitor", d3d_get_adapter_monitor}, {"CreateDevice", d3d_create_device}}},
        {"IDirect3DDevice9",
         {{"GetDirect3D", dev_get_direct3d}, {"GetDeviceCaps", dev_get_caps}, {"GetDisplayMode", dev_get_display_mode},
          {"GetCreationParameters", dev_get_creation_parameters}, {"GetAvailableTextureMem", dev_get_available_texture_mem},
          {"GetSwapChain", dev_get_swap_chain}, {"GetNumberOfSwapChains", dev_get_number_of_swap_chains}, {"GetBackBuffer", dev_get_back_buffer},
          {"Reset", dev_reset}, {"Present", dev_present}, {"CreateTexture", dev_create_texture}, {"CreateVolumeTexture", dev_create_volume_texture},
          {"CreateCubeTexture", dev_create_cube_texture}, {"CreateVertexBuffer", dev_create_vertex_buffer},
          {"CreateIndexBuffer", dev_create_index_buffer}, {"CreateRenderTarget", dev_create_render_target},
          {"CreateDepthStencilSurface", dev_create_depth_stencil}, {"CreateOffscreenPlainSurface", dev_create_offscreen_surface},
          {"CreateVertexDeclaration", dev_create_vertex_declaration}, {"CreateVertexShader", dev_create_vertex_shader},
          {"CreatePixelShader", dev_create_pixel_shader}, {"CreateStateBlock", dev_create_state_block}, {"CreateQuery", dev_create_query},
          {"GetRenderTarget", dev_get_render_target}, {"SetRenderTarget", dev_set_render_target},
          {"GetDepthStencilSurface", dev_get_depth_stencil}, {"SetDepthStencilSurface", dev_set_depth_stencil},
          {"SetTexture", dev_set_texture}, {"SetStreamSource", dev_set_stream_source}, {"SetIndices", dev_set_indices},
          {"SetVertexDeclaration", dev_set_vertex_declaration}, {"SetVertexShader", dev_set_vertex_shader},
          {"SetPixelShader", dev_set_pixel_shader}}},
        {"IDirect3DSwapChain9", {{"Present", sc_present}, {"GetBackBuffer", sc_get_back_buffer}, {"GetDevice", sc_get_device}}},
        {"IDirect3DSurface9", {{"GetDesc", surf_get_desc}, {"LockRect", surf_lock_rect}, {"GetContainer", surf_get_container}, {"GetType", tex_get_type}}},
        {"IDirect3DVolume9", {{"GetDesc", vol_get_desc}, {"LockBox", vol_lock_box}}},
        {"IDirect3DTexture9",
         {{"GetLevelCount", tex_get_level_count}, {"GetLevelDesc", tex_get_level_desc}, {"GetSurfaceLevel", tex_get_surface_level},
          {"LockRect", tex_lock_rect}, {"GetType", tex_get_type}}},
        {"IDirect3DCubeTexture9",
         {{"GetLevelCount", tex_get_level_count}, {"GetLevelDesc", tex_get_level_desc}, {"GetCubeMapSurface", cube_get_surface},
          {"LockRect", cube_lock_rect}, {"GetType", tex_get_type}}},
        {"IDirect3DVolumeTexture9",
         {{"GetLevelCount", tex_get_level_count}, {"GetLevelDesc", tex_get_level_desc}, {"GetVolumeLevel", voltex_get_volume},
          {"LockBox", voltex_lock_box}, {"GetType", tex_get_type}}},
        {"IDirect3DVertexBuffer9", {{"Lock", buf_lock}, {"GetDesc", vb_get_desc}, {"GetType", tex_get_type}}},
        {"IDirect3DIndexBuffer9", {{"Lock", buf_lock}, {"GetDesc", ib_get_desc}, {"GetType", tex_get_type}}},
        {"IDirect3DQuery9", {{"GetType", query_get_type}, {"GetDataSize", query_get_data_size}, {"GetData", query_get_data}}},
    };
    auto ov = all.find(iface);
    uint32_t v = make_vtable(iface, ov == all.end() ? Overrides{} : ov->second);
    cache[iface] = v;
    return v;
}

SHIM(d3d9, Direct3DCreate9, 1) {
    uint32_t o = new_object(vt("IDirect3D9"), 8, K_D3D);
    // build every interface table now, so that the renderer layer can watch them from the first call on
    for (const char *i : {"IDirect3DDevice9", "IDirect3DSwapChain9", "IDirect3DSurface9", "IDirect3DVolume9", "IDirect3DTexture9", "IDirect3DCubeTexture9",
                          "IDirect3DVolumeTexture9", "IDirect3DVertexBuffer9", "IDirect3DIndexBuffer9", "IDirect3DQuery9", "IDirect3DVertexDeclaration9",
                          "IDirect3DVertexShader9", "IDirect3DPixelShader9", "IDirect3DStateBlock9"})
        vt(i);
    d3d9_live_attach();
    log("d3d9(null): Direct3DCreate9(sdk %u) -> %08x", a[0], o);
    return o;
}
SHIM(d3d9, D3DPERF_BeginEvent, 2) { return 0; }
SHIM(d3d9, D3DPERF_EndEvent, 0) { return 0; }

} // namespace bo1
