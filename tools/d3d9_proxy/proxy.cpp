// d3d9.dll proxy logger for the reference baseline (32-bit PE, runs under Wine or Windows).
//
// Drop the built d3d9.dll next to BlackOps.exe and force the native override
// (WINEDLLOVERRIDES=d3d9=n,b). It loads the real d3d9, forwards Direct3DCreate9, and patches
// the vtables of the D3D9 interfaces IN PLACE with the thunks from thunks.S. Objects are never
// wrapped, so pointers the game hands back to D3D are the real ones (Wine's d3d9 asserts on
// vtable identity for some arguments).
//
// Two levels of output:
//   census (always)     text file: per-method call counts, device creation, dialogs  (T-012)
//   trace  (opt-in)     binary file: every call with arguments, results, shader bytecode,
//                       vertex declarations and resource uploads — docs/TRACE_FORMAT.md (T-013)
//
// Environment:
//   BO1_D3D9_LOG        census file (default: d3d9_census.txt in the cwd)
//   BO1_TRACE_FILE      binary trace file; unset = no trace
//   BO1_TRACE_MAX_MB    stop tracing after this many MiB (default 4096)
//   BO1_TRACE_FRAMES    "A-B": frames (1-based, counted at Present) whose resource uploads are
//                       recorded in full. Outside the window every call is still recorded, but
//                       dynamic uploads are not: DISCARD/NOOVERWRITE buffer locks and resources
//                       rewritten more than CHURN_LIMIT times carry no data. Default: all frames.
//   BO1_TRACE_VIDEO     1: also dump DISCARD-locked (dynamic) textures, i.e. Bink video frames;
//                       default 0 records the lock but not the pixels (~40 MiB/s during videos)
//   BO1_CAPTURE_EVERY   N > 0: dump the back buffer to a BMP every N presented frames (T-014)
//   BO1_CAPTURE_DIR     directory for frame_NNNNNN.bmp (default: cwd)
//   BO1_FRAMETIMES      CSV file: one line per presented frame, "frame,microseconds since load"
//   BO1_REAL_D3D9       full path of the D3D9 implementation to wrap (default: system d3d9.dll)
//   BO1_KEYS            "ms:vk,ms:vk": post key presses to the game window (ms after device creation)
//   BO1_CMDS            "ms|command;ms|command": queue engine console commands (ms after device
//                       creation) through the game's own Cbuf_AddText — e.g. "40000|devmap cuba"
//   BO1_IMPORT_CENSUS   file: count every call the exe makes through its import table (all DLLs)
//                       and write "dll,function,ordinal,calls" there — input for the platform map
//   BO1_MSGBOX_AUTO     1: answer message boxes without showing them (and log them)
//   BO1_IAT_HOOKS       1: also log CreateThread / VirtualAlloc / GlobalMemoryStatus (diagnostics)
//   BO1_HOOK_HEAP       1 (with BO1_IAT_HOOKS): also log HeapAlloc requests >= 256 MiB
//   BO1_HOSTNAME        name: make WSOCK32!gethostname return this name (experiment)
#define WIN32_LEAN_AND_MEAN
#define CINTERFACE
#define COBJMACROS
#include <windows.h>
#include <d3d9.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <type_traits>

#include "d3d9_methods.h"

// ---- interface tables, verified against the SDK header ----------------------------------
// Slot order: a wrong index cannot compile.
#define CHK(com, i, name) static_assert(offsetof(com##Vtbl, name) == (i) * sizeof(void *), #com " slot " #name);
#define CHK_d3d9(i, name) CHK(IDirect3D9, i, name)
#define CHK_device(i, name) CHK(IDirect3DDevice9, i, name)
#define CHK_swapchain(i, name) CHK(IDirect3DSwapChain9, i, name)
#define CHK_texture(i, name) CHK(IDirect3DTexture9, i, name)
#define CHK_cube(i, name) CHK(IDirect3DCubeTexture9, i, name)
#define CHK_volume(i, name) CHK(IDirect3DVolumeTexture9, i, name)
#define CHK_vb(i, name) CHK(IDirect3DVertexBuffer9, i, name)
#define CHK_ib(i, name) CHK(IDirect3DIndexBuffer9, i, name)
#define CHK_surface(i, name) CHK(IDirect3DSurface9, i, name)
#define CHK_query(i, name) CHK(IDirect3DQuery9, i, name)
#define CHK_vshader(i, name) CHK(IDirect3DVertexShader9, i, name)
#define CHK_pshader(i, name) CHK(IDirect3DPixelShader9, i, name)
#define CHK_vdecl(i, name) CHK(IDirect3DVertexDeclaration9, i, name)
#define GEN_CHK(id, sym, com, METHODS, count)                                                      \
    METHODS(CHK_##sym) static_assert(sizeof(com##Vtbl) == (count) * sizeof(void *), #com " size"); \
    static_assert((count) <= IFACE_MAX_SLOTS, #com " too many slots");
FOR_EACH_IFACE(GEN_CHK)

// Stack words each method takes, including `this` — derived from the SDK's own prototypes.
template <class T> constexpr unsigned argw() {
    return std::is_reference<T>::value ? 1u : (unsigned)((sizeof(T) + 3) / 4);
}
template <class R, class... A> constexpr unsigned char nw(R(__stdcall *)(A...)) {
    return (unsigned char)(0u + ... + argw<A>());
}
#define NWX(com, name) nw(decltype(com##Vtbl::name){}),
#define NW_d3d9(i, name) NWX(IDirect3D9, name)
#define NW_device(i, name) NWX(IDirect3DDevice9, name)
#define NW_swapchain(i, name) NWX(IDirect3DSwapChain9, name)
#define NW_texture(i, name) NWX(IDirect3DTexture9, name)
#define NW_cube(i, name) NWX(IDirect3DCubeTexture9, name)
#define NW_volume(i, name) NWX(IDirect3DVolumeTexture9, name)
#define NW_vb(i, name) NWX(IDirect3DVertexBuffer9, name)
#define NW_ib(i, name) NWX(IDirect3DIndexBuffer9, name)
#define NW_surface(i, name) NWX(IDirect3DSurface9, name)
#define NW_query(i, name) NWX(IDirect3DQuery9, name)
#define NW_vshader(i, name) NWX(IDirect3DVertexShader9, name)
#define NW_pshader(i, name) NWX(IDirect3DPixelShader9, name)
#define NW_vdecl(i, name) NWX(IDirect3DVertexDeclaration9, name)
#define NAME(i, name) #name,

#define GEN_STORAGE(id, sym, com, METHODS, count)                         \
    extern "C" {                                                          \
    void *orig_##sym[count];                                              \
    extern void *const thunks_##sym[count];                               \
    }                                                                     \
    static const char *const names_##sym[count] = {METHODS(NAME)};        \
    static const unsigned char nwords_##sym[count] = {METHODS(NW_##sym)}; \
    static volatile LONG count_##sym[count];
FOR_EACH_IFACE(GEN_STORAGE)

struct Iface {
    const char *com;
    int count;
    void **orig;
    void *const *thunks;
    const char *const *names;
    const unsigned char *nwords;
    volatile LONG *calls;
    void **hooked; // vtable address we patched (one per interface type)
};
#define GEN_IFACE(id, sym, com, METHODS, count) \
    {#com, count, orig_##sym, thunks_##sym, names_##sym, nwords_##sym, count_##sym, NULL},
static Iface g_if[IFACE_COUNT] = {FOR_EACH_IFACE(GEN_IFACE)};

extern "C" {
void *__stdcall trace_pre(unsigned iface, unsigned slot, void **frame);
void *__stdcall trace_post(unsigned result);
void post_tramp(void);
}

// ---- state ---------------------------------------------------------------------------
static HMODULE g_real;
static CRITICAL_SECTION g_lock;                           // census + events
static LONG g_first_frame_device[IDIRECT3DDEVICE9_COUNT]; // frame index of first use, -1 = never
static volatile LONG g_frames;
static IDirect3DDevice9 *g_device;
static DWORD g_render_thread;
static LONG g_other_thread_calls;
static char g_log_path[MAX_PATH];
static char g_events[65536]; // device creation, dialogs, captures, diagnostics
static size_t g_events_len;
static DWORD g_t0;
static void write_census(void);
struct ThreadState;
static ThreadState *thread_state(void);
static void proxy_calls_begin(void);
static void proxy_calls_end(void);
static void write_import_census(void);
static LONG g_d3dx_compiles; // D3DXCompileShader calls seen (IAT hook)

// Events are rare, and the process is usually killed rather than unloaded, so every event
// flushes the census file.
static void event(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    EnterCriticalSection(&g_lock);
    if (g_events_len < sizeof(g_events) - 1024)
        g_events_len += vsnprintf(g_events + g_events_len, sizeof(g_events) - g_events_len, fmt, ap);
    LeaveCriticalSection(&g_lock);
    va_end(ap);
    write_census();
}

// ---- binary trace writer (docs/TRACE_FORMAT.md) -------------------------------------------
#define TRACE_VERSION 2
#define TBUF_SIZE (8u << 20)
enum { // blob tags
    TB_OUT = 1,      // u32: object pointer returned through an out parameter
    TB_DATA = 2,     // raw bytes: unlocked resource contents, shader constants, UP vertex data
    TB_STRUCT = 3,   // the structure/array an input pointer refers to
    TB_RECT_A = 4,   // first optional RECT (array)
    TB_RECT_B = 5,   // second optional RECT
    TB_BYTECODE = 6, // shader tokens up to and including the END token
    TB_ELEMENTS = 7, // D3DVERTEXELEMENT9[] including D3DDECL_END
    TB_PP = 8,       // D3DPRESENT_PARAMETERS
    TB_LOCK = 9,     // lock description, see LockDesc
    TB_DATA_B = 10,  // second data block (index data of DrawIndexedPrimitiveUP)
};
struct Blob {
    DWORD tag;
    const void *p;
    DWORD len;
};
#pragma pack(push, 1)
struct RecHdr {
    BYTE iface, flags, depth, nblobs; // flags bit0: result valid
    WORD slot, nwords;
    DWORD tid, result;
};
struct LockDesc { // TB_LOCK payload (u32 fields)
    DWORD format, width, height, depth, pitch, slice_pitch, lock_flags;
    DWORD has_rect, left, top, right, bottom, front, back;
    DWORD offset, size, data_ptr;
    DWORD dumped; // 0 = no data (read-only lock, or video frame without BO1_TRACE_VIDEO)
                  // 1 = TB_DATA with `size` bytes follows
                  // 2 = same, but `size` was trimmed to the bytes actually written into a
                  //     DISCARD-locked buffer (sentinel scan)
                  // 3 = dynamic upload outside the BO1_TRACE_FRAMES window: data not recorded
};
#pragma pack(pop)
static HANDLE g_trace = INVALID_HANDLE_VALUE;
static CRITICAL_SECTION g_tlock;
static BYTE *g_tbuf;
static DWORD g_tlen;
static volatile LONG g_trace_on;
static unsigned long long g_trace_bytes, g_trace_max = 4096ull << 20;
static bool g_trace_video;
static LONG g_win_first = 0, g_win_last = 0x7FFFFFFF; // BO1_TRACE_FRAMES
static inline bool in_window(void) {
    LONG fr = g_frames + 1; // frame being built
    return fr >= g_win_first && fr <= g_win_last;
}
// Upload counter per (object, sub-resource): a resource rewritten over and over is per-frame
// dynamic data, which only matters inside the recording window. Guarded by g_tlock.
#define CHURN_LIMIT 4
#define CHURN_SLOTS 32768
static struct { DWORD key; BYTE count; } g_churn[CHURN_SLOTS];
static BYTE churn_bump(void *obj, DWORD sub) {
    DWORD key = ((DWORD)(DWORD_PTR)obj >> 3) * 2654435761u ^ (sub * 40503u) ^ 0x9E3779B9u;
    if (!key) key = 1;
    for (DWORD i = key % CHURN_SLOTS, n = 0; n < 64; n++, i = (i + 1) % CHURN_SLOTS) {
        if (g_churn[i].key == key || !g_churn[i].key) {
            g_churn[i].key = key;
            if (g_churn[i].count < 255) g_churn[i].count++;
            return g_churn[i].count;
        }
    }
    return 1; // table crowded: treat as a first upload (record it)
}
#define SENTINEL 0xCDCDCDCDu // written into DISCARD-locked buffers so Unlock can tell how much was filled
static LONG g_trace_records;

static void t_flush(void) {
    DWORD w;
    if (g_tlen) WriteFile(g_trace, g_tbuf, g_tlen, &w, NULL);
    g_tlen = 0;
}
static void t_write(const void *p, DWORD n) {
    g_trace_bytes += n;
    if (n > TBUF_SIZE - g_tlen) t_flush();
    if (n >= TBUF_SIZE) {
        DWORD w;
        WriteFile(g_trace, p, n, &w, NULL);
        return;
    }
    memcpy(g_tbuf + g_tlen, p, n);
    g_tlen += n;
}
static void t_emit(unsigned iface, unsigned slot, BYTE flags, BYTE depth, DWORD result, const DWORD *words,
                   unsigned nwords, const Blob *blobs, int nblobs) {
    static const BYTE zero[4] = {0, 0, 0, 0};
    RecHdr h = {(BYTE)iface, flags, depth, (BYTE)nblobs, (WORD)slot, (WORD)nwords, GetCurrentThreadId(), result};
    bool stopped = false;
    EnterCriticalSection(&g_tlock);
    if (g_trace_on) {
        t_write(&h, sizeof(h));
        t_write(words, nwords * 4);
        for (int i = 0; i < nblobs; i++) {
            DWORD hd[2] = {blobs[i].tag, blobs[i].len};
            t_write(hd, 8);
            t_write(blobs[i].p, blobs[i].len);
            if (blobs[i].len & 3) t_write(zero, 4 - (blobs[i].len & 3));
        }
        g_trace_records++;
        if (g_trace_bytes > g_trace_max) {
            t_flush();
            g_trace_on = 0;
            stopped = true;
        }
    }
    LeaveCriticalSection(&g_tlock);
    if (stopped)
        event("trace stopped: size limit reached (%lu MiB, %ld records, frame %ld)\n",
              (unsigned long)(g_trace_bytes >> 20), g_trace_records, g_frames);
}

// ---- census file -----------------------------------------------------------------------
static void write_census(void) {
    EnterCriticalSection(&g_lock);
    FILE *f = fopen(g_log_path, "w");
    if (f) {
        LONG frames = g_frames;
        fprintf(f, "# bo1 d3d9 proxy census (call names + counts)\n");
        fprintf(f, "frames_presented %ld\nelapsed_ms %lu\nrender_thread 0x%lx\ncalls_from_other_threads %ld\n",
                frames, GetTickCount() - g_t0, g_render_thread, g_other_thread_calls);
        fprintf(f, "trace_on %ld\ntrace_records %ld\ntrace_bytes %llu\n", g_trace_on, g_trace_records, g_trace_bytes);
        fprintf(f, "d3dx_compile_shader_calls %ld\n", g_d3dx_compiles);
        fprintf(f, "\n[events]\n%s", g_events);
        for (int k = 0; k < IFACE_COUNT; k++) {
            if (k == IFACE_DEVICE) continue;
            fprintf(f, "\n[%s]  slot name calls\n", g_if[k].com);
            for (int i = 0; i < g_if[k].count; i++)
                if (g_if[k].calls[i]) fprintf(f, "%3d %-28s %ld\n", i, g_if[k].names[i], g_if[k].calls[i]);
        }
        const Iface &d = g_if[IFACE_DEVICE];
        fprintf(f, "\n[IDirect3DDevice9]  slot name calls calls_per_frame first_frame\n");
        int used = 0;
        for (int i = 0; i < d.count; i++) {
            if (!d.calls[i]) continue;
            used++;
            fprintf(f, "%3d %-28s %10ld %10.1f %6ld\n", i, d.names[i], d.calls[i],
                    frames ? (double)d.calls[i] / frames : 0.0, g_first_frame_device[i]);
        }
        fprintf(f, "\ndevice_methods_used %d of %d\n[unused IDirect3DDevice9]\n", used, d.count);
        for (int i = 0; i < d.count; i++)
            if (!d.calls[i]) fprintf(f, "%s ", d.names[i]);
        fprintf(f, "\n");
        fclose(f);
    }
    LeaveCriticalSection(&g_lock);
}

static void dump_pp(const char *what, const D3DPRESENT_PARAMETERS *pp) {
    if (!pp) return;
    event("%s: backbuffer %ux%u fmt=%d count=%u msaa=%d/%lu swap=%d windowed=%d autoDS=%d dsfmt=%d "
          "flags=0x%lx hz=%u interval=0x%x\n",
          what, pp->BackBufferWidth, pp->BackBufferHeight, (int)pp->BackBufferFormat, pp->BackBufferCount,
          (int)pp->MultiSampleType, pp->MultiSampleQuality, (int)pp->SwapEffect, pp->Windowed,
          pp->EnableAutoDepthStencil, (int)pp->AutoDepthStencilFormat, pp->Flags,
          pp->FullScreen_RefreshRateInHz, pp->PresentationInterval);
}

// Patch a live vtable with our thunks, saving the originals once per interface type.
static void hook_iface(int k, void *obj) {
    if (!obj) return;
    void **vtbl = *(void ***)obj;
    Iface &f = g_if[k];
    if (f.hooked == vtbl) return;
    EnterCriticalSection(&g_lock);
    if (f.hooked == vtbl) {
        LeaveCriticalSection(&g_lock);
        return;
    }
    bool second = f.hooked != NULL, ok = false;
    DWORD old, err = 0;
    if (!second) {
        if (VirtualProtect(vtbl, f.count * sizeof(void *), PAGE_EXECUTE_READWRITE, &old)) {
            for (int i = 0; i < f.count; i++) f.orig[i] = vtbl[i];
            for (int i = 0; i < f.count; i++) vtbl[i] = f.thunks[i];
            VirtualProtect(vtbl, f.count * sizeof(void *), old, &old);
            f.hooked = vtbl;
            ok = true;
        } else
            err = GetLastError();
    }
    LeaveCriticalSection(&g_lock);
    if (second)
        event("WARNING: second distinct vtable %p for %s (already hooked at %p) — calls on it are not seen\n",
              (void *)vtbl, f.com, (void *)f.hooked);
    else if (!ok)
        event("ERROR: VirtualProtect failed on %s vtable %p (%lu)\n", f.com, (void *)vtbl, err);
    else
        event("hooked %s vtable %p\n", f.com, (void *)vtbl);
}

// ---- golden-frame capture: back buffer -> 24-bit BMP, just before Present -------------
// Every call goes through the saved originals so it shows up in neither census nor trace.
static LONG g_capture_every, g_captures;
static char g_capture_dir[MAX_PATH];
#define CAPTURE_MAX 400

static void capture_backbuffer(IDirect3DDevice9 *dev, LONG frame) {
    typedef HRESULT(__stdcall *GetBackBuffer_t)(IDirect3DDevice9 *, UINT, UINT, D3DBACKBUFFER_TYPE, IDirect3DSurface9 **);
    typedef HRESULT(__stdcall *CreateRT_t)(IDirect3DDevice9 *, UINT, UINT, D3DFORMAT, D3DMULTISAMPLE_TYPE, DWORD, BOOL, IDirect3DSurface9 **, HANDLE *);
    typedef HRESULT(__stdcall *CreatePlain_t)(IDirect3DDevice9 *, UINT, UINT, D3DFORMAT, D3DPOOL, IDirect3DSurface9 **, HANDLE *);
    typedef HRESULT(__stdcall *StretchRect_t)(IDirect3DDevice9 *, IDirect3DSurface9 *, const RECT *, IDirect3DSurface9 *, const RECT *, D3DTEXTUREFILTERTYPE);
    typedef HRESULT(__stdcall *GetRTData_t)(IDirect3DDevice9 *, IDirect3DSurface9 *, IDirect3DSurface9 *);
    typedef HRESULT(__stdcall *GetDesc_t)(IDirect3DSurface9 *, D3DSURFACE_DESC *);
    typedef HRESULT(__stdcall *LockRect_t)(IDirect3DSurface9 *, D3DLOCKED_RECT *, const RECT *, DWORD);
    typedef HRESULT(__stdcall *UnlockRect_t)(IDirect3DSurface9 *);
    typedef ULONG(__stdcall *Release_t)(IDirect3DSurface9 *);
    void **sv = g_if[IFACE_SURFACE].hooked ? orig_surface : NULL; // bypass surface thunks once hooked
#define SURF(s, slot, type) ((type)(sv ? sv[slot] : (*(void ***)(s))[slot]))
    IDirect3DSurface9 *bb = NULL, *resolved = NULL, *sys = NULL;
    HRESULT hr = ((GetBackBuffer_t)orig_device[18])(dev, 0, 0, D3DBACKBUFFER_TYPE_MONO, &bb);
    if (FAILED(hr) || !bb) {
        event("capture frame %ld: GetBackBuffer failed 0x%08lx\n", frame, (unsigned long)hr);
        return;
    }
    D3DSURFACE_DESC d;
    SURF(bb, 12, GetDesc_t)(bb, &d);
    IDirect3DSurface9 *src = bb;
    if (d.MultiSampleType != D3DMULTISAMPLE_NONE) { // GetRenderTargetData cannot read MSAA surfaces
        hr = ((CreateRT_t)orig_device[28])(dev, d.Width, d.Height, d.Format, D3DMULTISAMPLE_NONE, 0, FALSE, &resolved, NULL);
        if (SUCCEEDED(hr)) hr = ((StretchRect_t)orig_device[34])(dev, bb, NULL, resolved, NULL, D3DTEXF_NONE);
        src = resolved;
    }
    if (SUCCEEDED(hr))
        hr = ((CreatePlain_t)orig_device[36])(dev, d.Width, d.Height, d.Format, D3DPOOL_SYSTEMMEM, &sys, NULL);
    if (SUCCEEDED(hr)) hr = ((GetRTData_t)orig_device[32])(dev, src, sys);
    D3DLOCKED_RECT lr;
    if (SUCCEEDED(hr)) hr = SURF(sys, 13, LockRect_t)(sys, &lr, NULL, D3DLOCK_READONLY);
    if (SUCCEEDED(hr)) {
        bool is32 = d.Format == D3DFMT_X8R8G8B8 || d.Format == D3DFMT_A8R8G8B8;
        char path[MAX_PATH + 32];
        snprintf(path, sizeof(path), "%s\\frame_%06ld.bmp", g_capture_dir, frame);
        FILE *f = is32 ? fopen(path, "wb") : NULL;
        if (f) {
            DWORD row = (d.Width * 3 + 3) & ~3u, size = row * d.Height;
            BITMAPFILEHEADER fh = {0x4D42, (DWORD)(sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER) + size), 0, 0,
                                   (DWORD)(sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER))};
            BITMAPINFOHEADER ih = {sizeof(BITMAPINFOHEADER), (LONG)d.Width, (LONG)d.Height, 1, 24, BI_RGB, size, 2835, 2835, 0, 0};
            fwrite(&fh, sizeof(fh), 1, f);
            fwrite(&ih, sizeof(ih), 1, f);
            unsigned char *line = (unsigned char *)calloc(1, row);
            for (int y = (int)d.Height - 1; y >= 0; y--) { // BMP rows are bottom-up
                const unsigned char *p = (const unsigned char *)lr.pBits + (size_t)y * lr.Pitch;
                for (UINT x = 0; x < d.Width; x++) {
                    line[x * 3 + 0] = p[x * 4 + 0];
                    line[x * 3 + 1] = p[x * 4 + 1];
                    line[x * 3 + 2] = p[x * 4 + 2];
                }
                fwrite(line, 1, row, f);
            }
            free(line);
            fclose(f);
            g_captures++;
            event("capture frame %ld t=%lums %ux%u fmt=%d msaa=%d\n", frame, GetTickCount() - g_t0, d.Width,
                  d.Height, (int)d.Format, (int)d.MultiSampleType);
        } else
            event("capture frame %ld: %s (fmt=%d)\n", frame, is32 ? "cannot open output file" : "unsupported format",
                  (int)d.Format);
        SURF(sys, 14, UnlockRect_t)(sys);
    } else
        event("capture frame %ld: failed 0x%08lx (fmt=%d msaa=%d)\n", frame, (unsigned long)hr, (int)d.Format,
              (int)d.MultiSampleType);
    if (sys) SURF(sys, 2, Release_t)(sys);
    if (resolved) SURF(resolved, 2, Release_t)(resolved);
    SURF(bb, 2, Release_t)(bb);
#undef SURF
}

// Frame times for the golden metrics: microseconds since proxy load, one entry per Present.
#define FT_MAX 400000
static DWORD *g_ft;
static char g_ft_path[MAX_PATH];
static LONG g_ft_written;
static LARGE_INTEGER g_qpc0, g_qpf;
static void write_frametimes(void) {
    if (!g_ft) return;
    LONG n = g_frames < FT_MAX ? g_frames : FT_MAX;
    if (n <= g_ft_written) return;
    FILE *f = fopen(g_ft_path, g_ft_written ? "a" : "w");
    if (!f) return;
    if (!g_ft_written) fprintf(f, "frame,us\n");
    for (LONG i = g_ft_written; i < n; i++) fprintf(f, "%ld,%lu\n", i + 1, g_ft[i]);
    fclose(f);
    g_ft_written = n;
}

// Frame boundary (device or swap-chain Present), called BEFORE the real Present.
static void on_present(IDirect3DDevice9 *dev) {
    if (!g_render_thread) g_render_thread = GetCurrentThreadId();
    LONG fr = InterlockedIncrement(&g_frames);
    if (g_ft && fr <= FT_MAX) {
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        g_ft[fr - 1] = (DWORD)((now.QuadPart - g_qpc0.QuadPart) * 1000000 / g_qpf.QuadPart);
    }
    if (g_capture_every > 0 && fr % g_capture_every == 0 && g_captures < CAPTURE_MAX) {
        proxy_calls_begin(); // the capture's own surfaces must not appear in census or trace
        capture_backbuffer(dev, fr);
        proxy_calls_end();
    }
}

// ---- scripted input: BO1_KEYS="ms:vk,ms:vk,..." posts key presses to the game window --------
// Times are milliseconds after device creation; vk is a Windows virtual-key code (hex or dec).
static HWND g_hwnd;
static DWORD g_device_t0;
static DWORD WINAPI input_thread(LPVOID) {
    char spec[512];
    if (!GetEnvironmentVariableA("BO1_KEYS", spec, sizeof(spec))) return 0;
    while (!g_hwnd) Sleep(100);
    for (char *tok = strtok(spec, ","); tok; tok = strtok(NULL, ",")) {
        char *colon = strchr(tok, ':');
        if (!colon) continue;
        DWORD at = strtoul(tok, NULL, 0), vk = strtoul(colon + 1, NULL, 0);
        while (GetTickCount() - g_device_t0 < at) Sleep(50);
        UINT scan = MapVirtualKeyA(vk, 0 /*MAPVK_VK_TO_VSC*/);
        LPARAM down = 1 | (scan << 16), up = down | (1u << 30) | (1u << 31);
        PostMessageA(g_hwnd, WM_KEYDOWN, vk, down);
        UINT ch = MapVirtualKeyA(vk, 2 /*MAPVK_VK_TO_CHAR*/);
        if (ch) PostMessageA(g_hwnd, WM_CHAR, ch, down);
        Sleep(80);
        PostMessageA(g_hwnd, WM_KEYUP, vk, up);
        event("key vk=0x%lx posted to hwnd=%p at t=%lums (frame %ld, foreground=%p)\n", vk, (void *)g_hwnd,
              GetTickCount() - g_device_t0, g_frames, (void *)GetForegroundWindow());
    }
    return 0;
}

// ---- scripted console commands: BO1_CMDS="ms|command;ms|command" ---------------------------
// Calls the game's Cbuf_AddText(localClientNum, text) (VA 0x0049B930, re/symbols.csv), which takes
// the engine's command-buffer critical section itself, so it is safe from this thread. The
// address is specific to the 2011-09-01 SP build: the link timestamp and the function's first
// bytes are checked before anything is called.
#define BO1_SP_TIMESTAMP 0x4E601011u
#define VA_CBUF_ADDTEXT 0x0049B930u
static bool game_build_ok(void) {
    BYTE *base = (BYTE *)GetModuleHandleA(NULL);
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
    static const BYTE prologue[] = {0x6A, 0x35, 0xE8}; // push 0x35 ; call Sys_EnterCriticalSection
    return (DWORD_PTR)base == 0x00400000 && nt->FileHeader.TimeDateStamp == BO1_SP_TIMESTAMP &&
           memcmp((void *)VA_CBUF_ADDTEXT, prologue, sizeof(prologue)) == 0;
}
static DWORD WINAPI command_thread(LPVOID) {
    static char spec[1024];
    if (!GetEnvironmentVariableA("BO1_CMDS", spec, sizeof(spec))) return 0;
    if (!game_build_ok()) {
        event("BO1_CMDS ignored: this is not the SP build the Cbuf_AddText address was taken from\n");
        return 0;
    }
    while (!g_hwnd) Sleep(100);
    for (char *tok = strtok(spec, ";"); tok; tok = strtok(NULL, ";")) {
        char *bar = strchr(tok, '|');
        if (!bar) continue;
        DWORD at = strtoul(tok, NULL, 0);
        while (GetTickCount() - g_device_t0 < at) Sleep(50);
        char line[256];
        snprintf(line, sizeof(line), "%s\n", bar + 1);
        ((void(__cdecl *)(int, const char *))VA_CBUF_ADDTEXT)(0, line);
        event("command \"%s\" queued at t=%lums (frame %ld)\n", bar + 1, GetTickCount() - g_device_t0, g_frames);
    }
    return 0;
}

// The game is normally killed, not unloaded, and may stop calling D3D entirely: flush on a timer.
static DWORD WINAPI flush_thread(LPVOID) {
    for (;;) {
        Sleep(2000);
        write_census();
        write_frametimes();
        write_import_census();
        if (g_trace != INVALID_HANDLE_VALUE) {
            EnterCriticalSection(&g_tlock);
            t_flush();
            LeaveCriticalSection(&g_tlock);
        }
    }
}

// ---- argument knowledge --------------------------------------------------------------------
// Word index (0 = this) of the out parameter that receives a new interface pointer, and the
// interface table entry to hook for that object (-1: record the pointer only).
struct OutSpec {
    BYTE iface, slot, word;
    signed char hook;
};
static const OutSpec g_outs[] = {
    {IFACE_D3D9, 16, 6, IFACE_DEVICE},
    {IFACE_DEVICE, 13, 2, IFACE_SWAPCHAIN}, {IFACE_DEVICE, 14, 2, IFACE_SWAPCHAIN},
    {IFACE_DEVICE, 18, 4, IFACE_SURFACE},   {IFACE_DEVICE, 23, 7, IFACE_TEXTURE},
    {IFACE_DEVICE, 24, 8, IFACE_VOLUME},    {IFACE_DEVICE, 25, 6, IFACE_CUBE},
    {IFACE_DEVICE, 26, 5, IFACE_VB},        {IFACE_DEVICE, 27, 5, IFACE_IB},
    {IFACE_DEVICE, 28, 7, IFACE_SURFACE},   {IFACE_DEVICE, 29, 7, IFACE_SURFACE},
    {IFACE_DEVICE, 36, 5, IFACE_SURFACE},   {IFACE_DEVICE, 38, 2, IFACE_SURFACE},
    {IFACE_DEVICE, 40, 1, IFACE_SURFACE},   {IFACE_DEVICE, 59, 2, -1},
    {IFACE_DEVICE, 61, 1, -1},              {IFACE_DEVICE, 64, 2, -1},
    {IFACE_DEVICE, 86, 2, IFACE_VDECL},     {IFACE_DEVICE, 91, 2, IFACE_VSHADER},
    {IFACE_DEVICE, 106, 2, IFACE_PSHADER},  {IFACE_DEVICE, 118, 2, IFACE_QUERY},
    {IFACE_SWAPCHAIN, 5, 3, IFACE_SURFACE}, {IFACE_TEXTURE, 18, 2, IFACE_SURFACE},
    {IFACE_CUBE, 18, 3, IFACE_SURFACE},     {IFACE_VOLUME, 18, 2, -1},
};
static signed char g_out_word[IFACE_COUNT][IFACE_MAX_SLOTS]; // 0 = none
static signed char g_out_hook[IFACE_COUNT][IFACE_MAX_SLOTS];
static bool g_want_post[IFACE_COUNT][IFACE_MAX_SLOTS]; // only consulted while tracing

static void init_tables(void) {
    for (const OutSpec &o : g_outs) {
        g_out_word[o.iface][o.slot] = (signed char)o.word;
        g_out_hook[o.iface][o.slot] = o.hook;
        g_want_post[o.iface][o.slot] = true;
    }
    for (int k = IFACE_DEVICE; k < IFACE_COUNT; k++) g_want_post[k][2] = true; // Release -> refcount
    g_want_post[IFACE_DEVICE][16] = true;                                      // Reset
    g_want_post[IFACE_TEXTURE][19] = g_want_post[IFACE_CUBE][19] = g_want_post[IFACE_VOLUME][19] = true; // Lock*
    g_want_post[IFACE_VB][11] = g_want_post[IFACE_IB][11] = true;                                         // Lock
    g_want_post[IFACE_SURFACE][13] = true;                                                                // LockRect
    g_want_post[IFACE_QUERY][7] = true;                                                                   // GetData
}

static bool is_dxt(DWORD fmt) {
    return fmt == D3DFMT_DXT1 || fmt == D3DFMT_DXT2 || fmt == D3DFMT_DXT3 || fmt == D3DFMT_DXT4 || fmt == D3DFMT_DXT5;
}
static unsigned bits_per_pixel(DWORD fmt) {
    switch (fmt) {
    case D3DFMT_A8R8G8B8: case D3DFMT_X8R8G8B8: case D3DFMT_A8B8G8R8: case D3DFMT_X8B8G8R8:
    case D3DFMT_A2B10G10R10: case D3DFMT_A2R10G10B10: case D3DFMT_G16R16: case D3DFMT_G16R16F:
    case D3DFMT_R32F: case D3DFMT_Q8W8V8U8: case D3DFMT_D24S8: case D3DFMT_D24X8: case D3DFMT_D32:
    case D3DFMT_X8L8V8U8: case D3DFMT_INDEX32:
        return 32;
    case D3DFMT_R8G8B8: return 24;
    case D3DFMT_R5G6B5: case D3DFMT_X1R5G5B5: case D3DFMT_A1R5G5B5: case D3DFMT_A4R4G4B4: case D3DFMT_X4R4G4B4:
    case D3DFMT_A8L8: case D3DFMT_V8U8: case D3DFMT_L16: case D3DFMT_R16F: case D3DFMT_D16: case D3DFMT_INDEX16:
    case D3DFMT_L6V5U5: case D3DFMT_A8R3G3B2:
        return 16;
    case D3DFMT_A8: case D3DFMT_L8: case D3DFMT_P8: case D3DFMT_A4L4: case D3DFMT_R3G3B2: return 8;
    case D3DFMT_A16B16G16R16: case D3DFMT_A16B16G16R16F: case D3DFMT_G32R32F: return 64;
    case D3DFMT_A32B32G32R32F: return 128;
    default: return 0;
    }
}
static DWORD shader_bytes(const DWORD *p) { // length of a shader token stream incl. END
    if (!p) return 0;
    DWORD ver = p[0];
    if ((ver & 0xFFFF0000u) != 0xFFFE0000u && (ver & 0xFFFF0000u) != 0xFFFF0000u) return 0;
    bool sm1 = ((ver >> 8) & 0xFF) < 2;
    const DWORD *q = p + 1;
    for (int guard = 0; guard < (1 << 20); guard++) {
        DWORD t = *q;
        if (t == 0x0000FFFFu) return (DWORD)((q + 1 - p) * 4);
        if ((t & 0xFFFF) == 0xFFFE) { // comment
            q += 1 + ((t >> 16) & 0x7FFF);
        } else if (!sm1) {
            q += 1 + ((t >> 24) & 0xF);
        } else { // SM1: no length field; operand tokens have bit 31 set
            q++;
            while (*q & 0x80000000u) q++;
        }
    }
    return 0;
}
static DWORD prim_vertices(DWORD type, DWORD prims) {
    switch (type) {
    case D3DPT_POINTLIST: return prims;
    case D3DPT_LINELIST: return prims * 2;
    case D3DPT_LINESTRIP: return prims + 1;
    case D3DPT_TRIANGLELIST: return prims * 3;
    case D3DPT_TRIANGLESTRIP: case D3DPT_TRIANGLEFAN: return prims + 2;
    default: return 0;
    }
}

// Locks in progress: filled when a Lock* call returns, consumed by the matching Unlock*.
// Guarded by g_tlock.
struct Pending {
    void *obj;
    DWORD sub; // level | face << 8
    LockDesc d;
};
static Pending g_pending[256];
static Pending *pending_find(void *obj, DWORD sub, bool create) {
    Pending *free_slot = NULL;
    for (Pending &p : g_pending) {
        if (p.obj == obj && p.sub == sub) return &p;
        if (!p.obj && !free_slot) free_slot = &p;
    }
    if (create && free_slot) {
        memset(free_slot, 0, sizeof(*free_slot));
        free_slot->obj = obj;
        free_slot->sub = sub;
        return free_slot;
    }
    return NULL;
}
static void pending_forget(void *obj) {
    for (Pending &p : g_pending)
        if (p.obj == obj) p.obj = NULL;
}

struct Emit {
    Blob b[6];
    int nb;
    DWORD scratch[4];
    LockDesc lock;
    void add(DWORD tag, const void *p, DWORD len) {
        if (p && len && len < (256u << 20) && nb < 6) b[nb++] = {tag, p, len};
    }
};

// Input-side payloads: what the pointer arguments refer to. `w[0]` is `this`.
static void collect_inputs(unsigned k, unsigned slot, const DWORD *w, Emit &e) {
    switch (k) {
    case IFACE_D3D9:
        if (slot == 16) e.add(TB_PP, (void *)w[5], sizeof(D3DPRESENT_PARAMETERS));
        break;
    case IFACE_SWAPCHAIN:
        if (slot == 3) {
            e.add(TB_RECT_A, (void *)w[1], 16);
            e.add(TB_RECT_B, (void *)w[2], 16);
        }
        break;
    case IFACE_DEVICE:
        switch (slot) {
        case 13: case 16: e.add(TB_PP, (void *)w[1], sizeof(D3DPRESENT_PARAMETERS)); break;
        case 17: e.add(TB_RECT_A, (void *)w[1], 16); e.add(TB_RECT_B, (void *)w[2], 16); break;
        case 21: e.add(TB_STRUCT, (void *)w[3], sizeof(D3DGAMMARAMP)); break;
        case 34: e.add(TB_RECT_A, (void *)w[2], 16); e.add(TB_RECT_B, (void *)w[4], 16); break;
        case 35: e.add(TB_RECT_A, (void *)w[2], 16); break;
        case 43: e.add(TB_RECT_A, (void *)w[2], 16 * w[1]); break;
        case 44: case 46: e.add(TB_STRUCT, (void *)w[2], sizeof(D3DMATRIX)); break;
        case 47: e.add(TB_STRUCT, (void *)w[1], sizeof(D3DVIEWPORT9)); break;
        case 49: e.add(TB_STRUCT, (void *)w[1], sizeof(D3DMATERIAL9)); break;
        case 51: e.add(TB_STRUCT, (void *)w[2], sizeof(D3DLIGHT9)); break;
        case 55: e.add(TB_STRUCT, (void *)w[2], 16); break;
        case 75: e.add(TB_STRUCT, (void *)w[1], 16); break;
        case 83: e.add(TB_DATA, (void *)w[3], prim_vertices(w[1], w[2]) * w[4]); break;
        case 84:
            e.add(TB_DATA_B, (void *)w[5], prim_vertices(w[1], w[4]) * (w[6] == D3DFMT_INDEX16 ? 2 : 4));
            e.add(TB_DATA, (void *)w[7], (w[2] + w[3]) * w[8]);
            break;
        case 86: {
            const D3DVERTEXELEMENT9 *el = (const D3DVERTEXELEMENT9 *)w[1];
            DWORD n = 0;
            if (el) {
                while (n < 64 && el[n].Stream != 0xFF) n++;
                e.add(TB_ELEMENTS, el, (n + 1) * sizeof(D3DVERTEXELEMENT9));
            }
            break;
        }
        case 91: case 106: e.add(TB_BYTECODE, (void *)w[1], shader_bytes((const DWORD *)w[1])); break;
        case 94: case 109: case 96: case 111: e.add(TB_DATA, (void *)w[2], 16 * w[3]); break;
        case 98: case 113: e.add(TB_DATA, (void *)w[2], 4 * w[3]); break;
        }
        break;
    case IFACE_TEXTURE:
        if (slot == 21) e.add(TB_RECT_A, (void *)w[1], 16);
        break;
    case IFACE_CUBE:
        if (slot == 21) e.add(TB_RECT_A, (void *)w[2], 16);
        break;
    }
}

// Unlock*: describe the lock and dump the bytes the game wrote, BEFORE the real unlock.
static void collect_unlock(unsigned k, unsigned slot, const DWORD *w, Emit &e, int depth) {
    void *obj = (void *)w[0];
    DWORD sub = 0;
    if (k == IFACE_TEXTURE && slot == 20) sub = w[1];
    else if (k == IFACE_CUBE && slot == 20) sub = w[2] | (w[1] << 8);
    else if (k == IFACE_VOLUME && slot == 20) sub = w[1];
    else if (!((k == IFACE_VB || k == IFACE_IB) && slot == 12) && !(k == IFACE_SURFACE && slot == 14)) return;
    if (depth > 0) return; // the implementation unlocking a sub-object: the outer call has the data
    Pending *p = pending_find(obj, sub, false);
    if (!p) return;
    LockDesc &d = e.lock = p->d;
    p->obj = NULL;
    const BYTE *data = (const BYTE *)d.data_ptr;
    DWORD bytes = 0;
    bool trimmed = false, skip = false, outside = false;
    if (!in_window()) {
        if (k == IFACE_VB || k == IFACE_IB)
            outside = (d.lock_flags & (D3DLOCK_DISCARD | D3DLOCK_NOOVERWRITE)) != 0 || churn_bump(obj, sub) > CHURN_LIMIT;
        else
            outside = churn_bump(obj, sub) > CHURN_LIMIT;
    }
    if (k == IFACE_VB || k == IFACE_IB) {
        bytes = d.size;
        if (d.dumped == 2 && data) { // sentinel-filled at Lock: keep only what the game wrote
            const DWORD *q = (const DWORD *)data;
            DWORD nd = bytes / 4;
            while (nd > 0 && q[nd - 1] == SENTINEL) nd--;
            bytes = nd * 4;
            trimmed = true;
        }
    } else if (k == IFACE_VOLUME) {
        if (!d.has_rect) bytes = d.slice_pitch * d.depth;
    } else {
        DWORD wd = d.has_rect ? d.right - d.left : d.width, ht = d.has_rect ? d.bottom - d.top : d.height;
        bool dxt = is_dxt(d.format);
        DWORD rows = dxt ? (ht + 3) / 4 : ht;
        if (!d.has_rect) {
            bytes = d.pitch * rows;
        } else {
            DWORD rowbytes = dxt ? ((wd + 3) / 4) * (d.format == D3DFMT_DXT1 ? 8 : 16) : wd * bits_per_pixel(d.format) / 8;
            if (rowbytes && rows) bytes = d.pitch * (rows - 1) + rowbytes;
        }
    }
    if (k != IFACE_VB && k != IFACE_IB && (d.lock_flags & D3DLOCK_DISCARD) && !g_trace_video) skip = true;
    if ((d.lock_flags & D3DLOCK_READONLY) || !data || skip || outside) bytes = 0;
    d.size = bytes;
    d.dumped = bytes ? (trimmed ? 2 : 1) : (outside ? 3 : 0);
    e.add(TB_LOCK, &e.lock, sizeof(LockDesc));
    e.add(TB_DATA, data, bytes);
}

// Output side of a completed call: new objects, lock results, query data. May hook vtables.
// Called with g_tlock held when tracing.
static void collect_outputs(unsigned k, unsigned slot, DWORD result, const DWORD *w, Emit &e, bool tracing,
                            int depth) {
    int ow = g_out_word[k][slot];
    if (ow && SUCCEEDED((HRESULT)result) && w[ow]) {
        void *obj = *(void **)w[ow];
        e.scratch[0] = (DWORD)(DWORD_PTR)obj;
        e.add(TB_OUT, &e.scratch[0], 4);
        int hook = g_out_hook[k][slot];
        if (obj && hook >= 0 && (tracing || hook == IFACE_DEVICE || hook == IFACE_SWAPCHAIN)) hook_iface(hook, obj);
    }
    if (!tracing) return;
    void *obj = (void *)w[0];
    struct Guard { // the GetDesc/GetLevelDesc queries below are ours, not the game's
        Guard() { proxy_calls_begin(); }
        ~Guard() { proxy_calls_end(); }
    } guard;
    if (slot == 2 && k >= IFACE_TEXTURE && k <= IFACE_SURFACE && result == 0) pending_forget(obj); // destroyed
    if (FAILED((HRESULT)result)) return;
    LockDesc d;
    memset(&d, 0, sizeof(d));
    Pending *p = NULL;
    if ((k == IFACE_VB || k == IFACE_IB) && slot == 11) { // Lock(this, Offset, Size, ppData, Flags)
        DWORD total = 0;
        if (k == IFACE_VB) {
            D3DVERTEXBUFFER_DESC bd;
            if (SUCCEEDED(((HRESULT(__stdcall *)(void *, D3DVERTEXBUFFER_DESC *))orig_vb[13])(obj, &bd))) total = bd.Size;
        } else {
            D3DINDEXBUFFER_DESC bd;
            if (SUCCEEDED(((HRESULT(__stdcall *)(void *, D3DINDEXBUFFER_DESC *))orig_ib[13])(obj, &bd))) {
                total = bd.Size;
                d.format = bd.Format;
            }
        }
        d.offset = w[1];
        d.size = w[2] ? w[2] : (total > w[1] ? total - w[1] : 0);
        d.width = total;
        d.lock_flags = w[4];
        d.data_ptr = w[3] ? *(DWORD *)w[3] : 0;
        if (depth > 0) d.data_ptr = 0; // nested lock: describe only
        // A whole-buffer DISCARD lock says nothing about how much will be written (the game locks
        // its 8 MiB dynamic buffer like this every frame). Contents are undefined after DISCARD,
        // so fill with a sentinel and let Unlock trim the dump to the bytes actually written.
        if ((d.lock_flags & D3DLOCK_DISCARD) && d.data_ptr && d.size >= 4096 && depth == 0 && in_window()) {
            DWORD *q = (DWORD *)d.data_ptr;
            for (DWORD i = 0, nd = d.size / 4; i < nd; i++) q[i] = SENTINEL;
            d.dumped = 2;
        }
        p = depth > 0 ? NULL : pending_find(obj, 0, true);
    } else if ((k == IFACE_TEXTURE && slot == 19) || (k == IFACE_CUBE && slot == 19) || (k == IFACE_SURFACE && slot == 13)) {
        // Texture: LockRect(this, Level, pLocked, pRect, Flags)   Cube: (this, Face, Level, pLocked, pRect, Flags)
        // Surface: LockRect(this, pLocked, pRect, Flags)
        int base = k == IFACE_TEXTURE ? 2 : k == IFACE_CUBE ? 3 : 1;
        DWORD sub = k == IFACE_TEXTURE ? w[1] : k == IFACE_CUBE ? (w[2] | (w[1] << 8)) : 0;
        D3DSURFACE_DESC sd;
        memset(&sd, 0, sizeof(sd));
        if (k == IFACE_TEXTURE) ((HRESULT(__stdcall *)(void *, UINT, D3DSURFACE_DESC *))orig_texture[17])(obj, w[1], &sd);
        else if (k == IFACE_CUBE) ((HRESULT(__stdcall *)(void *, UINT, D3DSURFACE_DESC *))orig_cube[17])(obj, w[2], &sd);
        else ((HRESULT(__stdcall *)(void *, D3DSURFACE_DESC *))orig_surface[12])(obj, &sd);
        const D3DLOCKED_RECT *lr = (const D3DLOCKED_RECT *)w[base];
        const RECT *rc = (const RECT *)w[base + 1];
        d.format = sd.Format;
        d.width = sd.Width;
        d.height = sd.Height;
        d.depth = 1;
        d.lock_flags = w[base + 2];
        if (lr) {
            d.pitch = lr->Pitch;
            d.data_ptr = (DWORD)(DWORD_PTR)lr->pBits;
        }
        if (rc) {
            d.has_rect = 1;
            d.left = rc->left; d.top = rc->top; d.right = rc->right; d.bottom = rc->bottom;
        }
        // A surface lock made by the implementation while it services a texture lock (depth > 0)
        // would duplicate the texture's data: describe it, but do not track it for a dump.
        p = depth > 0 ? NULL : pending_find(obj, sub, true);
    } else if (k == IFACE_VOLUME && slot == 19) { // LockBox(this, Level, pLockedBox, pBox, Flags)
        D3DVOLUME_DESC vd;
        memset(&vd, 0, sizeof(vd));
        ((HRESULT(__stdcall *)(void *, UINT, D3DVOLUME_DESC *))orig_volume[17])(obj, w[1], &vd);
        const D3DLOCKED_BOX *lb = (const D3DLOCKED_BOX *)w[2];
        const D3DBOX *bx = (const D3DBOX *)w[3];
        d.format = vd.Format;
        d.width = vd.Width; d.height = vd.Height; d.depth = vd.Depth;
        d.lock_flags = w[4];
        if (lb) {
            d.pitch = lb->RowPitch;
            d.slice_pitch = lb->SlicePitch;
            d.data_ptr = (DWORD)(DWORD_PTR)lb->pBits;
        }
        if (bx) {
            d.has_rect = 1;
            d.left = bx->Left; d.top = bx->Top; d.right = bx->Right; d.bottom = bx->Bottom;
            d.front = bx->Front; d.back = bx->Back;
        }
        p = depth > 0 ? NULL : pending_find(obj, w[1], true);
    } else if (k == IFACE_QUERY && slot == 7) { // GetData(this, pData, dwSize, dwGetDataFlags)
        if (result == S_OK) e.add(TB_DATA, (void *)w[1], w[2]);
        return;
    } else
        return;
    e.lock = d;
    e.add(TB_LOCK, &e.lock, sizeof(LockDesc));
    if (p) p->d = d;
}

// ---- per-thread shadow stack for calls whose result we want ---------------------------------
struct Frame {
    void *ret;
    void **frame;
    BYTE iface, slot, nwords;
    bool emit; // record this call when it returns (false: it was recorded on entry)
    DWORD w[16];
};
struct ThreadState {
    int depth;
    int in_proxy; // > 0 while the proxy itself is calling D3D: nothing those calls trigger is recorded
    Frame f[24];
};
static DWORD g_tls = TLS_OUT_OF_INDEXES;
static void proxy_calls_begin(void) { thread_state()->in_proxy++; }
static void proxy_calls_end(void) { thread_state()->in_proxy--; }
static ThreadState *thread_state(void) {
    // Read the slot straight from the TEB: TlsGetValue would reset the caller's last error.
    void **slots = (void **)((BYTE *)NtCurrentTeb() + 0xE10);
    ThreadState *t = (ThreadState *)slots[g_tls];
    if (!t) {
        DWORD err = GetLastError();
        t = (ThreadState *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(ThreadState));
        slots[g_tls] = t;
        SetLastError(err);
    }
    return t;
}

// ---- hot path: called by every thunk before the original method runs ------------------------
extern "C" void *__stdcall trace_pre(unsigned k, unsigned slot, void **frame) {
    if (g_tls != TLS_OUT_OF_INDEXES && thread_state()->in_proxy) return NULL; // our own D3D use
    const Iface &f = g_if[k];
    LONG n = InterlockedIncrement(&f.calls[slot]);
    const DWORD *w = (const DWORD *)(frame + 1);
    unsigned nwords = f.nwords[slot];
    if (k == IFACE_DEVICE) {
        if (n == 1) g_first_frame_device[slot] = g_frames;
        if (slot == 17) on_present((IDirect3DDevice9 *)w[0]);
        else if (slot == 16) dump_pp("Reset", (const D3DPRESENT_PARAMETERS *)w[1]);
        if (g_render_thread && GetCurrentThreadId() != g_render_thread) InterlockedIncrement(&g_other_thread_calls);
    } else if (k == IFACE_SWAPCHAIN) {
        if (slot == 3 && g_device) on_present(g_device); // IDirect3DSwapChain9::Present = frame boundary
    } else if (k == IFACE_D3D9 && slot == 16) {
        event("CreateDevice: adapter=%lu devtype=%ld hwnd=%p behavior=0x%lx (caller ret=%p)\n", w[1], (long)w[2],
              (void *)w[3], w[4], frame[0]);
        dump_pp("CreateDevice", (const D3DPRESENT_PARAMETERS *)w[5]);
    }
    bool tracing = g_trace_on != 0;
    bool create_device = k == IFACE_D3D9 && slot == 16;
    if (!tracing && !create_device) return NULL;
    ThreadState *t = thread_state();
    // Shadow frames still in progress sit at HIGHER addresses than this call's frame. Anything
    // lower was unwound by an exception. An equal address is a tail call out of a hooked method
    // (d3d9's surface AddRef/Release forward to the parent texture with `jmp`): the return slot
    // already holds post_tramp, and the outer frame must stay so that it receives the result.
    bool tail = false;
    while (t->depth > 0) {
        void **top = t->f[t->depth - 1].frame;
        if (top == frame && frame[0] == (void *)post_tramp) {
            tail = true;
            break;
        }
        if (top > frame) break;
        t->depth--;
    }
    // While tracing, EVERY call gets a shadow frame so that `depth` counts all hooked calls in
    // progress: depth > 0 then reliably means "made by the D3D9 implementation, not the game".
    // Calls whose result or out-parameters matter are recorded when they return; the rest are
    // recorded here, before they run (Unlock* must read the data before the real unlock).
    bool emit_on_return = create_device || g_want_post[k][slot];
    bool can_push = !tail && t->depth < 24;
    if (tracing && (!emit_on_return || !can_push)) {
        Emit e;
        e.nb = 0;
        collect_inputs(k, slot, w, e);
        EnterCriticalSection(&g_tlock); // the pending-lock table is shared with collect_outputs
        collect_unlock(k, slot, w, e, t->depth);
        LeaveCriticalSection(&g_tlock);
        t_emit(k, slot, 0, (BYTE)t->depth, 0, w, nwords, e.b, e.nb);
    }
    if (!can_push) return NULL;
    Frame &fr = t->f[t->depth++];
    fr.ret = frame[0];
    fr.frame = frame;
    fr.iface = (BYTE)k;
    fr.slot = (BYTE)slot;
    fr.nwords = (BYTE)(nwords > 16 ? 16 : nwords);
    fr.emit = emit_on_return;
    memcpy(fr.w, w, fr.nwords * 4);
    return (void *)post_tramp;
}

// Called from post_tramp when a post-hooked method returns; returns the real return address.
extern "C" void *__stdcall trace_post(unsigned result) {
    ThreadState *t = thread_state();
    Frame fr = t->f[--t->depth];
    unsigned k = fr.iface, slot = fr.slot;
    if (!fr.emit) return fr.ret; // recorded on entry; the frame only existed to track nesting
    bool tracing = g_trace_on != 0;
    Emit e;
    e.nb = 0;
    if (tracing) {
        collect_inputs(k, slot, fr.w, e);
        EnterCriticalSection(&g_tlock);
    }
    collect_outputs(k, slot, result, fr.w, e, tracing, t->depth);
    if (tracing) {
        LeaveCriticalSection(&g_tlock);
        t_emit(k, slot, 1, (BYTE)t->depth, result, fr.w, fr.nwords, e.b, e.nb);
    }
    if (k == IFACE_D3D9 && slot == 16) { // CreateDevice(this, Adapter, Type, hFocus, Flags, pp, ppDevice)
        IDirect3DDevice9 **out = (IDirect3DDevice9 **)fr.w[6];
        event("CreateDevice -> hr=0x%08lx device=%p\n", (unsigned long)result, out ? (void *)*out : NULL);
        if (SUCCEEDED((HRESULT)result) && out && *out) {
            const D3DPRESENT_PARAMETERS *pp = (const D3DPRESENT_PARAMETERS *)fr.w[5];
            g_device = *out;
            if (!g_hwnd) {
                g_device_t0 = GetTickCount();
                g_hwnd = (pp && pp->hDeviceWindow) ? pp->hDeviceWindow : (HWND)fr.w[3];
                CloseHandle(CreateThread(NULL, 0, input_thread, NULL, 0, NULL));
                CloseHandle(CreateThread(NULL, 0, command_thread, NULL, 0, NULL));
            }
            // The game presents through the implicit swap chain, not IDirect3DDevice9::Present.
            typedef HRESULT(__stdcall *GetSwapChain_t)(IDirect3DDevice9 *, UINT, IDirect3DSwapChain9 **);
            IDirect3DSwapChain9 *sc = NULL;
            if (SUCCEEDED(((GetSwapChain_t)orig_device[14])(*out, 0, &sc)) && sc) {
                hook_iface(IFACE_SWAPCHAIN, sc);
                ((ULONG(__stdcall *)(IDirect3DSwapChain9 *))orig_swapchain[2])(sc); // Release, unrecorded
            }
        }
    }
    return fr.ret;
}

// ---- process diagnostics: IAT hooks on the exe (thread creation, message boxes) ---------
static void *patch_iat(const char *dll, const char *func, void *replacement, WORD ordinal = 0) {
    BYTE *base = (BYTE *)GetModuleHandleA(NULL);
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
    IMAGE_DATA_DIRECTORY dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    for (IMAGE_IMPORT_DESCRIPTOR *d = (IMAGE_IMPORT_DESCRIPTOR *)(base + dir.VirtualAddress); d->Name; d++) {
        if (_stricmp((const char *)(base + d->Name), dll) != 0 || !d->OriginalFirstThunk) continue;
        IMAGE_THUNK_DATA *names = (IMAGE_THUNK_DATA *)(base + d->OriginalFirstThunk);
        IMAGE_THUNK_DATA *iat = (IMAGE_THUNK_DATA *)(base + d->FirstThunk);
        for (; names->u1.AddressOfData; names++, iat++) {
            if (names->u1.Ordinal & IMAGE_ORDINAL_FLAG) { // imported by ordinal (e.g. WSOCK32)
                if (!ordinal || (WORD)(names->u1.Ordinal & 0xFFFF) != ordinal) continue;
            } else {
                IMAGE_IMPORT_BY_NAME *n = (IMAGE_IMPORT_BY_NAME *)(base + names->u1.AddressOfData);
                if (strcmp((const char *)n->Name, func) != 0) continue;
            }
            void *old_fn = (void *)iat->u1.Function;
            DWORD old;
            VirtualProtect(&iat->u1.Function, sizeof(void *), PAGE_READWRITE, &old);
            iat->u1.Function = (DWORD_PTR)replacement;
            VirtualProtect(&iat->u1.Function, sizeof(void *), old, &old);
            return old_fn;
        }
    }
    event("patch_iat: %s!%s not found\n", dll, func);
    return NULL;
}

// ---- import census: which of the exe's imports are actually called, and how often ----------
// Every IAT slot is redirected to a 13-byte stub `lock inc [counter] ; jmp [original]`, which
// touches no register and nothing on the stack, so it is transparent for any calling convention.
#define IMP_MAX 512
static struct {
    const char *dll, *name;
    WORD ordinal;
    void *orig;
    volatile LONG calls;
} g_imp[IMP_MAX];
static int g_imp_count;
static char g_imp_path[MAX_PATH];
static void write_import_census(void) {
    if (!g_imp_count) return;
    FILE *f = fopen(g_imp_path, "w");
    if (!f) return;
    fprintf(f, "dll,function,ordinal,calls\n");
    for (int i = 0; i < g_imp_count; i++)
        fprintf(f, "%s,%s,%u,%ld\n", g_imp[i].dll, g_imp[i].name ? g_imp[i].name : "", g_imp[i].ordinal, g_imp[i].calls);
    fclose(f);
}
static void install_import_census(void) {
    if (!GetEnvironmentVariableA("BO1_IMPORT_CENSUS", g_imp_path, sizeof(g_imp_path)) || !g_imp_path[0]) return;
    BYTE *base = (BYTE *)GetModuleHandleA(NULL);
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
    IMAGE_DATA_DIRECTORY dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    BYTE *stubs = (BYTE *)VirtualAlloc(NULL, IMP_MAX * 16, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!stubs) return;
    for (IMAGE_IMPORT_DESCRIPTOR *d = (IMAGE_IMPORT_DESCRIPTOR *)(base + dir.VirtualAddress); d->Name; d++) {
        if (!d->OriginalFirstThunk) continue;
        IMAGE_THUNK_DATA *names = (IMAGE_THUNK_DATA *)(base + d->OriginalFirstThunk);
        IMAGE_THUNK_DATA *iat = (IMAGE_THUNK_DATA *)(base + d->FirstThunk);
        for (; names->u1.AddressOfData && g_imp_count < IMP_MAX; names++, iat++) {
            int i = g_imp_count++;
            g_imp[i].dll = (const char *)(base + d->Name);
            if (names->u1.Ordinal & IMAGE_ORDINAL_FLAG)
                g_imp[i].ordinal = (WORD)(names->u1.Ordinal & 0xFFFF);
            else
                g_imp[i].name = (const char *)((IMAGE_IMPORT_BY_NAME *)(base + names->u1.AddressOfData))->Name;
            g_imp[i].orig = (void *)iat->u1.Function;
            BYTE *st = stubs + i * 16; // 13 bytes used
            st[0] = 0xF0; st[1] = 0xFF; st[2] = 0x05; // lock inc dword ptr [imm32]
            *(DWORD *)(st + 3) = (DWORD)(DWORD_PTR)&g_imp[i].calls;
            st[7] = 0xFF; st[8] = 0x25;               // jmp dword ptr [imm32]
            *(DWORD *)(st + 9) = (DWORD)(DWORD_PTR)&g_imp[i].orig;
            DWORD old;
            VirtualProtect(&iat->u1.Function, sizeof(void *), PAGE_READWRITE, &old);
            iat->u1.Function = (DWORD_PTR)st;
            VirtualProtect(&iat->u1.Function, sizeof(void *), old, &old);
        }
    }
    FlushInstructionCache(GetCurrentProcess(), stubs, IMP_MAX * 16);
    event("import census: %d import slots redirected, writing %s\n", g_imp_count, g_imp_path);
}

typedef HANDLE(WINAPI *CreateThread_t)(LPSECURITY_ATTRIBUTES, SIZE_T, LPTHREAD_START_ROUTINE, LPVOID, DWORD, LPDWORD);
static CreateThread_t g_real_CreateThread;
static HANDLE WINAPI hooked_CreateThread(LPSECURITY_ATTRIBUTES sa, SIZE_T stack, LPTHREAD_START_ROUTINE start,
                                         LPVOID param, DWORD flags, LPDWORD tid_out) {
    DWORD tid = 0;
    HANDLE h = g_real_CreateThread(sa, stack, start, param, flags, &tid);
    if (tid_out) *tid_out = tid;
    event("CreateThread: start=%p param=%p stackSize=0x%lx flags=0x%lx -> tid=0x%lx (caller ret=%p)\n",
          (void *)start, param, (unsigned long)stack, flags, tid, __builtin_return_address(0));
    return h;
}

// Heuristic guest backtrace: stack words that point just after a call instruction in the
// exe's .text. Polluted by stale slots — a hint, not an unwinder.
static void log_backtrace(const char *tag) {
    NT_TIB *tib = (NT_TIB *)NtCurrentTeb();
    DWORD *sp = (DWORD *)__builtin_frame_address(0), *top = (DWORD *)tib->StackBase;
    char line[1024];
    int n = snprintf(line, sizeof(line), "backtrace[%s] tid=0x%lx:", tag, GetCurrentThreadId()), found = 0;
    for (; sp < top && found < 40 && n < (int)sizeof(line) - 16; sp++) {
        DWORD v = *sp;
        if (v < 0x00401005 || v >= 0x009A2000) continue;
        const BYTE *c = (const BYTE *)v;
        bool is_ret = c[-5] == 0xE8 || (c[-6] == 0xFF && c[-5] == 0x15) || (c[-2] == 0xFF && (c[-1] & 0xF8) == 0xD0) ||
                      (c[-3] == 0xFF && (c[-2] & 0xF8) == 0x50) || (c[-6] == 0xFF && (c[-5] & 0xF8) == 0x90);
        if (!is_ret) continue;
        n += snprintf(line + n, sizeof(line) - n, " %08lx", v);
        found++;
    }
    event("%s\n", line);
}

typedef LPVOID(WINAPI *VirtualAlloc_t)(LPVOID, SIZE_T, DWORD, DWORD);
static VirtualAlloc_t g_real_VirtualAlloc;
static LPVOID WINAPI hooked_VirtualAlloc(LPVOID addr, SIZE_T size, DWORD type, DWORD protect) {
    LPVOID r = g_real_VirtualAlloc(addr, size, type, protect);
    if (size >= 0x2000000 || !r) { // >= 32 MiB or failed
        DWORD err = r ? 0 : GetLastError();
        event("VirtualAlloc: addr=%p size=0x%lx (%lu MiB) type=0x%lx prot=0x%lx -> %p err=%lu (caller ret=%p)\n", addr,
              (unsigned long)size, (unsigned long)(size >> 20), type, protect, r, err, __builtin_return_address(0));
        if (!r) {
            log_backtrace("VirtualAlloc failed");
            SetLastError(err);
        }
    }
    return r;
}

typedef LPVOID(WINAPI *HeapAlloc_t)(HANDLE, DWORD, SIZE_T);
static HeapAlloc_t g_real_HeapAlloc;
static LPVOID WINAPI hooked_HeapAlloc(HANDLE heap, DWORD flags, SIZE_T size) {
    LPVOID r = g_real_HeapAlloc(heap, flags, size);
    if (size >= 0x10000000) { // >= 256 MiB: almost certainly a bogus size
        event("HeapAlloc: size=0x%lx (%lu MiB) flags=0x%lx -> %p (caller ret=%p)\n", (unsigned long)size,
              (unsigned long)(size >> 20), flags, r, __builtin_return_address(0));
        log_backtrace("huge HeapAlloc");
    }
    return r;
}

typedef void(WINAPI *GlobalMemoryStatus_t)(LPMEMORYSTATUS);
static GlobalMemoryStatus_t g_real_GlobalMemoryStatus;
static void WINAPI hooked_GlobalMemoryStatus(LPMEMORYSTATUS ms) {
    g_real_GlobalMemoryStatus(ms);
    event("GlobalMemoryStatus: load=%lu totalPhys=0x%lx availPhys=0x%lx totalPage=0x%lx availPage=0x%lx "
          "totalVirt=0x%lx availVirt=0x%lx (caller ret=%p)\n",
          ms->dwMemoryLoad, (unsigned long)ms->dwTotalPhys, (unsigned long)ms->dwAvailPhys,
          (unsigned long)ms->dwTotalPageFile, (unsigned long)ms->dwAvailPageFile, (unsigned long)ms->dwTotalVirtual,
          (unsigned long)ms->dwAvailVirtual, __builtin_return_address(0));
}

// Dynamic imports: what does the game look up by name at runtime? (BO1_IAT_HOOKS=1)
typedef HMODULE(WINAPI *LoadLibraryA_t)(LPCSTR);
typedef FARPROC(WINAPI *GetProcAddress_t)(HMODULE, LPCSTR);
static LoadLibraryA_t g_real_LoadLibraryA;
static GetProcAddress_t g_real_GetProcAddress;
static LONG g_dyn_logged;
static HMODULE WINAPI hooked_LoadLibraryA(LPCSTR name) {
    HMODULE h = g_real_LoadLibraryA(name);
    DWORD err = GetLastError();
    if (InterlockedIncrement(&g_dyn_logged) <= 200)
        event("LoadLibraryA(\"%s\") -> %p (caller ret=%p)\n", name ? name : "(null)", (void *)h, __builtin_return_address(0));
    SetLastError(err);
    return h;
}
static FARPROC WINAPI hooked_GetProcAddress(HMODULE mod, LPCSTR name) {
    FARPROC p = g_real_GetProcAddress(mod, name);
    DWORD err = GetLastError();
    if (InterlockedIncrement(&g_dyn_logged) <= 200) {
        char modname[MAX_PATH] = "?";
        GetModuleFileNameA(mod, modname, sizeof(modname));
        const char *base = strrchr(modname, '\\');
        if ((DWORD_PTR)name < 0x10000)
            event("GetProcAddress(%s, #%lu) -> %p (caller ret=%p)\n", base ? base + 1 : modname, (unsigned long)(DWORD_PTR)name,
                  (void *)p, __builtin_return_address(0));
        else
            event("GetProcAddress(%s, \"%s\") -> %p (caller ret=%p)\n", base ? base + 1 : modname, name, (void *)p,
                  __builtin_return_address(0));
    }
    SetLastError(err);
    return p;
}

typedef int(WINAPI *gethostname_t)(char *, int);
static gethostname_t g_real_gethostname;
static char g_fake_hostname[32];
static int WINAPI hooked_gethostname(char *name, int len) {
    char real[256] = "";
    int r = g_real_gethostname(real, sizeof(real));
    event("gethostname(len=%d) real=\"%s\" (%d chars, r=%d) -> returning \"%s\" (caller ret=%p)\n", len, real,
          (int)strlen(real), r, g_fake_hostname, __builtin_return_address(0));
    if (len <= (int)strlen(g_fake_hostname)) return -1;
    strcpy(name, g_fake_hostname);
    return 0;
}

// Does the game ever compile HLSL at runtime? (ADR-005 input.) Counts every call and logs the
// first few; installed whenever the message-box hook is.
typedef HRESULT(WINAPI *D3DXCompileShader_t)(LPCSTR, UINT, const void *, void *, LPCSTR, LPCSTR, DWORD, void **,
                                             void **, void **);
static D3DXCompileShader_t g_real_D3DXCompileShader;
static HRESULT WINAPI hooked_D3DXCompileShader(LPCSTR src, UINT len, const void *defines, void *include, LPCSTR func,
                                               LPCSTR profile, DWORD flags, void **shader, void **errors,
                                               void **ctab) {
    HRESULT hr = g_real_D3DXCompileShader(src, len, defines, include, func, profile, flags, shader, errors, ctab);
    LONG n = InterlockedIncrement(&g_d3dx_compiles);
    if (n <= 8)
        event("D3DXCompileShader #%ld: profile=%s entry=%s source=%u bytes flags=0x%lx -> hr=0x%08lx (frame %ld, "
              "caller ret=%p)\n",
              n, profile ? profile : "?", func ? func : "?", len, flags, (unsigned long)hr, g_frames,
              __builtin_return_address(0));
    return hr;
}

typedef int(WINAPI *MessageBoxA_t)(HWND, LPCSTR, LPCSTR, UINT);
static MessageBoxA_t g_real_MessageBoxA;
static bool g_msgbox_auto;
static int WINAPI hooked_MessageBoxA(HWND wnd, LPCSTR text, LPCSTR caption, UINT type) {
    event("MessageBoxA: caption=\"%.80s\" type=0x%x text=\"%.600s\" (caller ret=%p)\n", caption ? caption : "",
          type, text ? text : "", __builtin_return_address(0));
    log_backtrace("MessageBoxA");
    // Unattended baseline: never block on a dialog. Questions ("Run in safe mode?", "configure
    // for new hardware?") are answered No; everything else is acknowledged.
    if (g_msgbox_auto) {
        UINT kind = type & MB_TYPEMASK;
        int answer = (kind == MB_YESNO || kind == MB_YESNOCANCEL) ? IDNO : (kind == MB_OKCANCEL ? IDCANCEL : IDOK);
        event("MessageBoxA auto-answered %d\n", answer);
        return answer;
    }
    return g_real_MessageBoxA(wnd, text, caption, type);
}

static void note_stack(void) {
    NT_TIB *tib = (NT_TIB *)NtCurrentTeb();
    void *dealloc = *(void **)((BYTE *)tib + 0xE0C); // TEB.DeallocationStack
    event("stack tid=0x%lx base=%p limit=%p dealloc=%p reserve=%lu KiB\n", GetCurrentThreadId(), tib->StackBase,
          tib->StackLimit, dealloc,
          (unsigned long)(((DWORD)(DWORD_PTR)tib->StackBase - (DWORD)(DWORD_PTR)dealloc) >> 10));
}

// ---- exports ---------------------------------------------------------------------------
static void *real_proc(const char *name) {
    if (!g_real) {
        char path[MAX_PATH];
        // BO1_REAL_D3D9 selects another implementation (e.g. DXVK's d3d9.dll under any file name).
        if (!GetEnvironmentVariableA("BO1_REAL_D3D9", path, sizeof(path))) {
            GetSystemDirectoryA(path, sizeof(path));
            strcat(path, "\\d3d9.dll");
        }
        g_real = LoadLibraryA(path);
        event("real d3d9: %s -> %p (err=%lu)\n", path, (void *)g_real, g_real ? 0 : GetLastError());
    }
    return g_real ? (void *)GetProcAddress(g_real, name) : NULL;
}

extern "C" IDirect3D9 *WINAPI proxy_Direct3DCreate9(UINT sdk) {
    typedef IDirect3D9 *(WINAPI *fn_t)(UINT);
    fn_t fn = (fn_t)real_proc("Direct3DCreate9");
    IDirect3D9 *d3d = fn ? fn(sdk) : NULL;
    static LONG started;
    if (InterlockedExchange(&started, 1) == 0) CloseHandle(CreateThread(NULL, 0, flush_thread, NULL, 0, NULL));
    event("Direct3DCreate9(sdk=%u) -> %p (caller ret=%p)\n", sdk, (void *)d3d, __builtin_return_address(0));
    if (d3d) hook_iface(IFACE_D3D9, d3d);
    return d3d;
}

extern "C" int WINAPI proxy_D3DPERF_BeginEvent(D3DCOLOR col, LPCWSTR name) {
    typedef int(WINAPI *fn_t)(D3DCOLOR, LPCWSTR);
    static fn_t fn;
    if (!fn) fn = (fn_t)real_proc("D3DPERF_BeginEvent");
    return fn ? fn(col, name) : 0;
}

extern "C" int WINAPI proxy_D3DPERF_EndEvent(void) {
    typedef int(WINAPI *fn_t)(void);
    static fn_t fn;
    if (!fn) fn = (fn_t)real_proc("D3DPERF_EndEvent");
    return fn ? fn() : 0;
}

static void open_trace(void) {
    char path[MAX_PATH], num[32];
    if (!GetEnvironmentVariableA("BO1_TRACE_FILE", path, sizeof(path)) || !path[0]) return;
    if (GetEnvironmentVariableA("BO1_TRACE_MAX_MB", num, sizeof(num))) g_trace_max = (unsigned long long)atol(num) << 20;
    g_trace_video = GetEnvironmentVariableA("BO1_TRACE_VIDEO", num, sizeof(num)) && num[0] == '1';
    char win[64];
    if (GetEnvironmentVariableA("BO1_TRACE_FRAMES", win, sizeof(win)) && strchr(win, '-')) {
        g_win_first = atol(win);
        g_win_last = atol(strchr(win, '-') + 1);
    }
    g_tbuf = (BYTE *)VirtualAlloc(NULL, TBUF_SIZE, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    g_trace = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (g_trace == INVALID_HANDLE_VALUE || !g_tbuf) {
        event("trace: cannot start (file=%s err=%lu)\n", path, GetLastError());
        g_trace = INVALID_HANDLE_VALUE;
        return;
    }
    struct {
        char magic[8];
        DWORD version, header_size, pointer_size, rec_hdr_size, iface_count, reserved[3];
    } hdr = {{'B', 'O', '1', 'D', '3', 'D', '9', 'T'}, TRACE_VERSION, 40, 4, sizeof(RecHdr), IFACE_COUNT, {0, 0, 0}};
    t_write(&hdr, sizeof(hdr));
    g_trace_on = 1;
    event("trace: writing %s (limit %lu MiB, full uploads for frames %ld-%ld, video=%d)\n", path,
          (unsigned long)(g_trace_max >> 20), g_win_first, g_win_last, (int)g_trace_video);
}

extern "C" BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(inst);
        InitializeCriticalSection(&g_lock);
        InitializeCriticalSection(&g_tlock);
        g_t0 = GetTickCount();
        init_tables();
        for (int i = 0; i < IDIRECT3DDEVICE9_COUNT; i++) g_first_frame_device[i] = -1;
        if (!GetEnvironmentVariableA("BO1_D3D9_LOG", g_log_path, sizeof(g_log_path)))
            strcpy(g_log_path, "d3d9_census.txt");
        char num[32];
        if (GetEnvironmentVariableA("BO1_CAPTURE_EVERY", num, sizeof(num))) g_capture_every = atol(num);
        if (!GetEnvironmentVariableA("BO1_CAPTURE_DIR", g_capture_dir, sizeof(g_capture_dir)))
            strcpy(g_capture_dir, ".");
        event("proxy loaded, log=%s capture_every=%ld dir=%s\n", g_log_path, g_capture_every, g_capture_dir);
        if (GetEnvironmentVariableA("BO1_FRAMETIMES", g_ft_path, sizeof(g_ft_path)) && g_ft_path[0]) {
            g_ft = (DWORD *)VirtualAlloc(NULL, FT_MAX * sizeof(DWORD), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
            QueryPerformanceFrequency(&g_qpf);
            QueryPerformanceCounter(&g_qpc0);
        }
        g_tls = TlsAlloc(); // the shadow stack reads TEB.TlsSlots[] directly: needs an index < 64
        if (g_tls >= 64) {
            event("FATAL: TLS index %lu unusable; proxy disabled\n", g_tls);
            return FALSE;
        }
        open_trace();
        install_import_census(); // first, so the specific hooks below chain through the counters
        // Message boxes block an unattended run ("Run in safe mode?", "hardware changed?", fatal
        // errors), so that one hook is on whenever BO1_MSGBOX_AUTO=1. The other import hooks are
        // diagnostics and stay off unless BO1_IAT_HOOKS=1.
        g_msgbox_auto = GetEnvironmentVariableA("BO1_MSGBOX_AUTO", num, sizeof(num)) && num[0] == '1';
        bool diag = GetEnvironmentVariableA("BO1_IAT_HOOKS", num, sizeof(num)) && num[0] == '1';
        if (g_msgbox_auto || diag) {
            g_real_MessageBoxA = (MessageBoxA_t)patch_iat("USER32.dll", "MessageBoxA", (void *)hooked_MessageBoxA);
            g_real_D3DXCompileShader = (D3DXCompileShader_t)patch_iat("d3dx9_43.dll", "D3DXCompileShader",
                                                                      (void *)hooked_D3DXCompileShader);
        }
        if (GetEnvironmentVariableA("BO1_HOSTNAME", g_fake_hostname, sizeof(g_fake_hostname)) && g_fake_hostname[0])
            g_real_gethostname = (gethostname_t)patch_iat("WSOCK32.dll", "gethostname", (void *)hooked_gethostname, 57);
        if (diag) {
            g_real_CreateThread = (CreateThread_t)patch_iat("KERNEL32.dll", "CreateThread", (void *)hooked_CreateThread);
            g_real_LoadLibraryA = (LoadLibraryA_t)patch_iat("KERNEL32.dll", "LoadLibraryA", (void *)hooked_LoadLibraryA);
            g_real_GetProcAddress = (GetProcAddress_t)patch_iat("KERNEL32.dll", "GetProcAddress", (void *)hooked_GetProcAddress);
            g_real_VirtualAlloc = (VirtualAlloc_t)patch_iat("KERNEL32.dll", "VirtualAlloc", (void *)hooked_VirtualAlloc);
            g_real_GlobalMemoryStatus =
                (GlobalMemoryStatus_t)patch_iat("KERNEL32.dll", "GlobalMemoryStatus", (void *)hooked_GlobalMemoryStatus);
            if (GetEnvironmentVariableA("BO1_HOOK_HEAP", num, sizeof(num)) && num[0] == '1') // hot path
                g_real_HeapAlloc = (HeapAlloc_t)patch_iat("KERNEL32.dll", "HeapAlloc", (void *)hooked_HeapAlloc);
            note_stack();
        }
    } else if (reason == DLL_PROCESS_DETACH) {
        if (g_trace != INVALID_HANDLE_VALUE) {
            EnterCriticalSection(&g_tlock);
            t_flush();
            LeaveCriticalSection(&g_tlock);
        }
        write_census();
    }
    return TRUE;
}
