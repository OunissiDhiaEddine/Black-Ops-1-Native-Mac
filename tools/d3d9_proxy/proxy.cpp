// T-012: d3d9.dll proxy logger for the reference baseline (32-bit PE, runs under Wine).
//
// Drop the built d3d9.dll next to BlackOps.exe and force the native override
// (WINEDLLOVERRIDES=d3d9=n,b). It loads the real d3d9 from the system directory, forwards
// Direct3DCreate9, and patches the IDirect3D9 / IDirect3DDevice9 vtables IN PLACE with the
// thunks from thunks.S. Objects are never wrapped, so pointers the game hands back to D3D
// are the real ones (Wine's d3d9 asserts on vtable identity for some arguments).
//
// This stage logs call NAMES and COUNTS only (plus the CreateDevice/Reset presentation
// parameters). Output: a text census rewritten every 2 s, on every event and at unload.
//   BO1_D3D9_LOG        path of the census file (default: d3d9_census.txt in the cwd)
//   BO1_CAPTURE_EVERY   N > 0: dump the back buffer to a BMP every N presented frames (T-014)
//   BO1_CAPTURE_DIR     directory for frame_NNNNNN.bmp (default: cwd)
//   BO1_IAT_HOOKS       1: also hook CreateThread/MessageBoxA/VirtualAlloc/HeapAlloc/GlobalMemoryStatus
//                       in the exe import table and log them (diagnostics)
//   BO1_MSGBOX_AUTO     1 (with BO1_IAT_HOOKS): answer message boxes without showing them
//   BO1_KEYS            "ms:vk,ms:vk": post key presses to the game window (ms after device creation)
//   BO1_HOSTNAME        name: make WSOCK32!gethostname return this short name (experiment)
//   BO1_HOOK_HEAP       1 (with BO1_IAT_HOOKS): also log HeapAlloc requests >= 256 MiB
#define WIN32_LEAN_AND_MEAN
#define CINTERFACE
#define COBJMACROS
#include <windows.h>
#include <d3d9.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "d3d9_methods.h"


// ---- slot tables, verified against the SDK header ------------------------------------
#define CHECK_D3D9(i, name) \
    static_assert(offsetof(IDirect3D9Vtbl, name) == (i) * sizeof(void *), "IDirect3D9 slot " #name);
#define CHECK_DEVICE(i, name) \
    static_assert(offsetof(IDirect3DDevice9Vtbl, name) == (i) * sizeof(void *), "IDirect3DDevice9 slot " #name);
IDIRECT3D9_METHODS(CHECK_D3D9)
IDIRECT3DDEVICE9_METHODS(CHECK_DEVICE)
#define CHECK_SWAPCHAIN(i, name) \
    static_assert(offsetof(IDirect3DSwapChain9Vtbl, name) == (i) * sizeof(void *), "IDirect3DSwapChain9 slot " #name);
IDIRECT3DSWAPCHAIN9_METHODS(CHECK_SWAPCHAIN)
static_assert(sizeof(IDirect3DSwapChain9Vtbl) == IDIRECT3DSWAPCHAIN9_COUNT * sizeof(void *), "IDirect3DSwapChain9 size");
static_assert(sizeof(IDirect3D9Vtbl) == IDIRECT3D9_COUNT * sizeof(void *), "IDirect3D9 size");
static_assert(sizeof(IDirect3DDevice9Vtbl) == IDIRECT3DDEVICE9_COUNT * sizeof(void *), "IDirect3DDevice9 size");

#define NAME(i, name) #name,
static const char *const g_names_d3d9[IDIRECT3D9_COUNT] = {IDIRECT3D9_METHODS(NAME)};
static const char *const g_names_device[IDIRECT3DDEVICE9_COUNT] = {IDIRECT3DDEVICE9_METHODS(NAME)};
static const char *const g_names_swapchain[IDIRECT3DSWAPCHAIN9_COUNT] = {IDIRECT3DSWAPCHAIN9_METHODS(NAME)};

extern "C" {
// Originals, read by the thunks' tail jump.
void *orig_d3d9[IDIRECT3D9_COUNT];
void *orig_device[IDIRECT3DDEVICE9_COUNT];
void *orig_swapchain[IDIRECT3DSWAPCHAIN9_COUNT];
// Thunk address tables from thunks.S.
extern void *const thunks_d3d9[IDIRECT3D9_COUNT];
extern void *const thunks_device[IDIRECT3DDEVICE9_COUNT];
extern void *const thunks_swapchain[IDIRECT3DSWAPCHAIN9_COUNT];
void __stdcall trace_swapchain(unsigned slot, void **args);
void __stdcall trace_d3d9(unsigned slot, void **args);
void __stdcall trace_device(unsigned slot, void **args);
}

// ---- state ---------------------------------------------------------------------------
static HMODULE g_real;
static CRITICAL_SECTION g_lock;
static volatile LONG g_count_d3d9[IDIRECT3D9_COUNT];
static volatile LONG g_count_device[IDIRECT3DDEVICE9_COUNT];
static volatile LONG g_count_swapchain[IDIRECT3DSWAPCHAIN9_COUNT];
static IDirect3DDevice9 *g_device;
static LONG g_first_frame_device[IDIRECT3DDEVICE9_COUNT]; // frame index of first use, -1 = never
static volatile LONG g_frames;
static void **g_vtbl_d3d9, **g_vtbl_device, **g_vtbl_swapchain;
static DWORD g_render_thread;
static LONG g_other_thread_calls;
static char g_log_path[MAX_PATH];
static char g_events[65536]; // CreateDevice / Reset parameter dumps
static size_t g_events_len;
static DWORD g_t0;

static void write_census(void);
static void note_stack(void);
static volatile LONG g_last_flush_ms;

// Events are rare (create/reset/capture), and the process is usually killed rather than
// unloaded, so every event flushes the census file.
static void event(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    EnterCriticalSection(&g_lock);
    if (g_events_len < sizeof(g_events) - 512)
        g_events_len += vsnprintf(g_events + g_events_len, sizeof(g_events) - g_events_len, fmt, ap);
    LeaveCriticalSection(&g_lock);
    va_end(ap);
    write_census();
}

static void write_census(void) {
    EnterCriticalSection(&g_lock);
    g_last_flush_ms = (LONG)(GetTickCount() - g_t0);
    FILE *f = fopen(g_log_path, "w");
    if (f) {
        LONG frames = g_frames;
        fprintf(f, "# bo1 d3d9 proxy census (call names + counts)\n");
        fprintf(f, "frames_presented %ld\nelapsed_ms %lu\nrender_thread 0x%lx\ncalls_from_other_threads %ld\n",
                frames, GetTickCount() - g_t0, g_render_thread, g_other_thread_calls);
        fprintf(f, "\n[events]\n%s", g_events);
        fprintf(f, "\n[IDirect3D9]  slot name calls\n");
        for (int i = 0; i < IDIRECT3D9_COUNT; i++)
            if (g_count_d3d9[i]) fprintf(f, "%3d %-28s %ld\n", i, g_names_d3d9[i], g_count_d3d9[i]);
        fprintf(f, "\n[IDirect3DSwapChain9]  slot name calls\n");
        for (int i = 0; i < IDIRECT3DSWAPCHAIN9_COUNT; i++)
            if (g_count_swapchain[i]) fprintf(f, "%3d %-28s %ld\n", i, g_names_swapchain[i], g_count_swapchain[i]);
        fprintf(f, "\n[IDirect3DDevice9]  slot name calls calls_per_frame first_frame\n");
        int used = 0;
        for (int i = 0; i < IDIRECT3DDEVICE9_COUNT; i++) {
            if (!g_count_device[i]) continue;
            used++;
            fprintf(f, "%3d %-28s %10ld %10.1f %6ld\n", i, g_names_device[i], g_count_device[i],
                    frames ? (double)g_count_device[i] / frames : 0.0, g_first_frame_device[i]);
        }
        fprintf(f, "\ndevice_methods_used %d of %d\n[unused IDirect3DDevice9]\n", used, IDIRECT3DDEVICE9_COUNT);
        for (int i = 0; i < IDIRECT3DDEVICE9_COUNT; i++)
            if (!g_count_device[i]) fprintf(f, "%s ", g_names_device[i]);
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

// ---- golden-frame capture: back buffer -> 24-bit BMP, just before Present -------------
// All device calls go through orig_device[] so they do not show up in the census.
static LONG g_capture_every, g_captures;
static char g_capture_dir[MAX_PATH];
#define CAPTURE_MAX 400

static void capture_backbuffer(IDirect3DDevice9 *dev, LONG frame) {
    typedef HRESULT(__stdcall *GetBackBuffer_t)(IDirect3DDevice9 *, UINT, UINT, D3DBACKBUFFER_TYPE, IDirect3DSurface9 **);
    typedef HRESULT(__stdcall *CreateRT_t)(IDirect3DDevice9 *, UINT, UINT, D3DFORMAT, D3DMULTISAMPLE_TYPE, DWORD, BOOL, IDirect3DSurface9 **, HANDLE *);
    typedef HRESULT(__stdcall *CreatePlain_t)(IDirect3DDevice9 *, UINT, UINT, D3DFORMAT, D3DPOOL, IDirect3DSurface9 **, HANDLE *);
    typedef HRESULT(__stdcall *StretchRect_t)(IDirect3DDevice9 *, IDirect3DSurface9 *, const RECT *, IDirect3DSurface9 *, const RECT *, D3DTEXTUREFILTERTYPE);
    typedef HRESULT(__stdcall *GetRTData_t)(IDirect3DDevice9 *, IDirect3DSurface9 *, IDirect3DSurface9 *);
    IDirect3DSurface9 *bb = NULL, *resolved = NULL, *sys = NULL;
    HRESULT hr = ((GetBackBuffer_t)orig_device[18])(dev, 0, 0, D3DBACKBUFFER_TYPE_MONO, &bb);
    if (FAILED(hr) || !bb) {
        event("capture frame %ld: GetBackBuffer failed 0x%08lx\n", frame, (unsigned long)hr);
        return;
    }
    D3DSURFACE_DESC d;
    IDirect3DSurface9_GetDesc(bb, &d);
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
    if (SUCCEEDED(hr)) hr = IDirect3DSurface9_LockRect(sys, &lr, NULL, D3DLOCK_READONLY);
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
        IDirect3DSurface9_UnlockRect(sys);
    } else
        event("capture frame %ld: failed 0x%08lx (fmt=%d msaa=%d)\n", frame, (unsigned long)hr, (int)d.Format,
              (int)d.MultiSampleType);
    if (sys) IDirect3DSurface9_Release(sys);
    if (resolved) IDirect3DSurface9_Release(resolved);
    IDirect3DSurface9_Release(bb);
}

// Patch `count` entries of a live vtable with our thunks, saving the originals once.
static void hook_vtable(void **vtbl, void **orig, void *const *thunks, int count, void ***hooked) {
    EnterCriticalSection(&g_lock);
    if (*hooked != vtbl) {
        if (*hooked)
            event("WARNING: second distinct vtable %p for an interface already hooked at %p (ignored)\n",
                  (void *)vtbl, (void *)*hooked);
        else {
            DWORD old;
            if (VirtualProtect(vtbl, count * sizeof(void *), PAGE_EXECUTE_READWRITE, &old)) {
                for (int i = 0; i < count; i++) {
                    orig[i] = vtbl[i];
                    vtbl[i] = thunks[i];
                }
                VirtualProtect(vtbl, count * sizeof(void *), old, &old);
                *hooked = vtbl;
            } else
                event("ERROR: VirtualProtect failed on vtable %p (%lu)\n", (void *)vtbl, GetLastError());
        }
    }
    LeaveCriticalSection(&g_lock);
}

// ---- trace callbacks (called by the thunks before the original method runs) -----------
extern "C" void __stdcall trace_d3d9(unsigned slot, void **args) {
    InterlockedIncrement(&g_count_d3d9[slot]);
    if (slot == 16) { // CreateDevice(this, Adapter, DeviceType, hFocusWindow, BehaviorFlags, pp, ppDevice)
        event("CreateDevice: adapter=%u devtype=%d hwnd=%p behavior=0x%lx (caller ret=%p)\n",
              (unsigned)(UINT_PTR)args[2], (int)(INT_PTR)args[3], args[4], (DWORD)(DWORD_PTR)args[5], args[0]);
        dump_pp("CreateDevice", (const D3DPRESENT_PARAMETERS *)args[6]);
    }
}

// Frame boundary (device or swap-chain Present), called BEFORE the real Present.
static void on_present(IDirect3DDevice9 *dev) {
    if (!g_render_thread) g_render_thread = GetCurrentThreadId();
    LONG fr = InterlockedIncrement(&g_frames);
    if (g_capture_every > 0 && fr % g_capture_every == 0 && g_captures < CAPTURE_MAX)
        capture_backbuffer(dev, fr);
}

// ---- scripted input: BO1_KEYS="ms:vk,ms:vk,..." posts key presses to the game window --------
// Times are milliseconds after device creation; vk is a Windows virtual-key code (hex or dec).
// Lets an unattended run get past "Press Any Key" and through menus reproducibly.
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

// The game is normally killed, not unloaded, and may stop calling D3D entirely: flush on a timer.
static DWORD WINAPI flush_thread(LPVOID) {
    for (;;) {
        Sleep(2000);
        write_census();
    }
}

extern "C" void __stdcall trace_device(unsigned slot, void **args) {
    LONG n = InterlockedIncrement(&g_count_device[slot]);
    if (n == 1) g_first_frame_device[slot] = g_frames;
    if ((n & 63) == 1) note_stack();
    DWORD tid = GetCurrentThreadId();
    if (slot == 17) { // Present
        on_present((IDirect3DDevice9 *)args[1]);
    } else if (slot == 16) { // Reset(this, pp)
        dump_pp("Reset", (const D3DPRESENT_PARAMETERS *)args[2]);
    }
    if (g_render_thread && tid != g_render_thread) InterlockedIncrement(&g_other_thread_calls);
}

extern "C" void __stdcall trace_swapchain(unsigned slot, void **) {
    InterlockedIncrement(&g_count_swapchain[slot]);
    if (slot == 3 && g_device) on_present(g_device); // IDirect3DSwapChain9::Present = frame boundary
}

// ---- process diagnostics: IAT hooks on the exe (thread creation, message boxes) ---------
// Needed to understand baseline failures, and the thread inventory feeds the P2 platform map.
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
// exe's .text. Good enough to map a failure onto re/symbols.csv; not a real unwinder.
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

// Strings still referenced from the stack (format strings and messages passed down the failing
// call chain). Pointers may target the exe image or the stack itself.
static void log_stack_strings(const char *tag) {
    NT_TIB *tib = (NT_TIB *)NtCurrentTeb();
    DWORD *sp = (DWORD *)__builtin_frame_address(0), *top = (DWORD *)tib->StackBase;
    int found = 0;
    DWORD last = 0;
    for (; sp < top && found < 40; sp++) {
        DWORD v = *sp;
        bool in_image = v >= 0x009A3000 && v < 0x00BA6000;
        bool in_stack = v > (DWORD)(DWORD_PTR)sp && v < (DWORD)(DWORD_PTR)top - 16;
        if ((!in_image && !in_stack) || v == last) continue;
        const unsigned char *c = (const unsigned char *)v;
        int len = 0;
        while (len < 200 && (DWORD)(DWORD_PTR)(c + len) < (in_stack ? (DWORD)(DWORD_PTR)top : 0x00BA6000u) &&
               ((c[len] >= 32 && c[len] < 127) || c[len] == '\n' || c[len] == '\t'))
            len++;
        if (len < 10 || c[len] != 0) continue;
        last = v;
        found++;
        event("stackstr[%s] @%p -> %08lx \"%.160s\"\n", tag, (void *)sp, v, (const char *)c);
    }
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

// Experiment: Windows machine names are <= 15 chars; a macOS hostname such as
// "Somebodys-MacBook-Air.local" is longer than anything the game was tested with.
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

typedef int(WINAPI *MessageBoxA_t)(HWND, LPCSTR, LPCSTR, UINT);
static MessageBoxA_t g_real_MessageBoxA;
static bool g_msgbox_auto;
static int WINAPI hooked_MessageBoxA(HWND wnd, LPCSTR text, LPCSTR caption, UINT type) {
    event("MessageBoxA: caption=\"%.80s\" type=0x%x text=\"%.600s\" (caller ret=%p)\n", caption ? caption : "",
          type, text ? text : "", __builtin_return_address(0));
    log_backtrace("MessageBoxA");
    log_stack_strings("MessageBoxA");
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

// Per-thread stack facts, logged the first time a thread makes a D3D call and whenever it
// reaches a new low-water mark by more than 64 KiB.
struct ThreadStack { DWORD tid; DWORD low; };
static ThreadStack g_stacks[64];
static void note_stack(void) {
    NT_TIB *tib = (NT_TIB *)NtCurrentTeb();
    DWORD esp = (DWORD)(DWORD_PTR)__builtin_frame_address(0);
    DWORD tid = GetCurrentThreadId();
    for (int i = 0; i < 64; i++) {
        if (g_stacks[i].tid == tid) {
            if (esp + 0x10000 < g_stacks[i].low) {
                g_stacks[i].low = esp;
                event("stack tid=0x%lx new low esp=%08lx (base=%p limit=%p, used %lu KiB)\n", tid, esp, tib->StackBase,
                      tib->StackLimit, (unsigned long)(((DWORD)(DWORD_PTR)tib->StackBase - esp) >> 10));
            }
            return;
        }
        if (g_stacks[i].tid == 0 && InterlockedCompareExchange((LONG *)&g_stacks[i].tid, (LONG)tid, 0) == 0) {
            g_stacks[i].low = esp;
            void *dealloc = *(void **)((BYTE *)tib + 0xE0C); // TEB.DeallocationStack
            event("stack tid=0x%lx base=%p limit=%p dealloc=%p reserve=%lu KiB esp=%08lx (used %lu KiB)\n", tid,
                  tib->StackBase, tib->StackLimit, dealloc,
                  (unsigned long)(((DWORD)(DWORD_PTR)tib->StackBase - (DWORD)(DWORD_PTR)dealloc) >> 10), esp,
                  (unsigned long)(((DWORD)(DWORD_PTR)tib->StackBase - esp) >> 10));
            return;
        }
    }
}

// ---- CreateDevice needs a post-hook: the device vtable only exists after it returns ---
typedef HRESULT(__stdcall *CreateDevice_t)(IDirect3D9 *, UINT, D3DDEVTYPE, HWND, DWORD,
                                           D3DPRESENT_PARAMETERS *, IDirect3DDevice9 **);
static CreateDevice_t g_real_create_device;

static HRESULT __stdcall hooked_CreateDevice(IDirect3D9 *self, UINT adapter, D3DDEVTYPE type, HWND hwnd,
                                             DWORD behavior, D3DPRESENT_PARAMETERS *pp,
                                             IDirect3DDevice9 **out) {
    void *args[7] = {__builtin_return_address(0), self, (void *)(UINT_PTR)adapter, (void *)(INT_PTR)type,
                     hwnd, (void *)(DWORD_PTR)behavior, pp};
    trace_d3d9(16, args);
    HRESULT hr = g_real_create_device(self, adapter, type, hwnd, behavior, pp, out);
    event("CreateDevice -> hr=0x%08lx device=%p\n", (unsigned long)hr, out ? (void *)*out : NULL);
    if (SUCCEEDED(hr) && out && *out) {
        g_device = *out;
        if (!g_hwnd) {
            g_device_t0 = GetTickCount();
            g_hwnd = (pp && pp->hDeviceWindow) ? pp->hDeviceWindow : hwnd;
            CloseHandle(CreateThread(NULL, 0, input_thread, NULL, 0, NULL));
        }
        hook_vtable((void **)(*out)->lpVtbl, orig_device, thunks_device, IDIRECT3DDEVICE9_COUNT, &g_vtbl_device);
        // The game presents through the implicit swap chain, not IDirect3DDevice9::Present.
        typedef HRESULT(__stdcall *GetSwapChain_t)(IDirect3DDevice9 *, UINT, IDirect3DSwapChain9 **);
        IDirect3DSwapChain9 *sc = NULL;
        if (SUCCEEDED(((GetSwapChain_t)orig_device[14])(*out, 0, &sc)) && sc) {
            hook_vtable((void **)sc->lpVtbl, orig_swapchain, thunks_swapchain, IDIRECT3DSWAPCHAIN9_COUNT,
                        &g_vtbl_swapchain);
            ((ULONG(__stdcall *)(IDirect3DSwapChain9 *))orig_swapchain[2])(sc); // Release, uncounted
        }
    }
    return hr;
}

// ---- exports ---------------------------------------------------------------------------
static void *real_proc(const char *name) {
    if (!g_real) {
        char path[MAX_PATH];
        GetSystemDirectoryA(path, sizeof(path));
        strcat(path, "\\d3d9.dll");
        g_real = LoadLibraryA(path);
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
    if (d3d) {
        void **vtbl = (void **)d3d->lpVtbl;
        bool first = g_vtbl_d3d9 != vtbl;
        hook_vtable(vtbl, orig_d3d9, thunks_d3d9, IDIRECT3D9_COUNT, &g_vtbl_d3d9);
        if (first && g_vtbl_d3d9 == vtbl) { // swap the generic CreateDevice thunk for the post-hook
            DWORD old;
            g_real_create_device = (CreateDevice_t)orig_d3d9[16];
            VirtualProtect(&vtbl[16], sizeof(void *), PAGE_EXECUTE_READWRITE, &old);
            vtbl[16] = (void *)hooked_CreateDevice;
            VirtualProtect(&vtbl[16], sizeof(void *), old, &old);
        }
    }
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

extern "C" BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(inst);
        InitializeCriticalSection(&g_lock);
        g_t0 = GetTickCount();
        for (int i = 0; i < IDIRECT3DDEVICE9_COUNT; i++) g_first_frame_device[i] = -1;
        if (!GetEnvironmentVariableA("BO1_D3D9_LOG", g_log_path, sizeof(g_log_path)))
            strcpy(g_log_path, "d3d9_census.txt");
        char num[32];
        if (GetEnvironmentVariableA("BO1_CAPTURE_EVERY", num, sizeof(num))) g_capture_every = atol(num);
        if (!GetEnvironmentVariableA("BO1_CAPTURE_DIR", g_capture_dir, sizeof(g_capture_dir)))
            strcpy(g_capture_dir, ".");
        event("proxy loaded, log=%s capture_every=%ld dir=%s\n", g_log_path, g_capture_every, g_capture_dir);
        // Message boxes block an unattended run ("Run in safe mode?", "hardware changed?", fatal
        // errors), so that one hook is on whenever BO1_MSGBOX_AUTO=1. The other import hooks are
        // diagnostics and stay off unless BO1_IAT_HOOKS=1.
        g_msgbox_auto = GetEnvironmentVariableA("BO1_MSGBOX_AUTO", num, sizeof(num)) && num[0] == '1';
        bool diag = GetEnvironmentVariableA("BO1_IAT_HOOKS", num, sizeof(num)) && num[0] == '1';
        if (g_msgbox_auto || diag)
            g_real_MessageBoxA = (MessageBoxA_t)patch_iat("USER32.dll", "MessageBoxA", (void *)hooked_MessageBoxA);
        if (GetEnvironmentVariableA("BO1_HOSTNAME", g_fake_hostname, sizeof(g_fake_hostname)) && g_fake_hostname[0])
            g_real_gethostname = (gethostname_t)patch_iat("WSOCK32.dll", "gethostname", (void *)hooked_gethostname, 57);
        if (diag) {
            g_real_CreateThread = (CreateThread_t)patch_iat("KERNEL32.dll", "CreateThread", (void *)hooked_CreateThread);
            g_real_VirtualAlloc = (VirtualAlloc_t)patch_iat("KERNEL32.dll", "VirtualAlloc", (void *)hooked_VirtualAlloc);
            g_real_GlobalMemoryStatus =
                (GlobalMemoryStatus_t)patch_iat("KERNEL32.dll", "GlobalMemoryStatus", (void *)hooked_GlobalMemoryStatus);
            if (GetEnvironmentVariableA("BO1_HOOK_HEAP", num, sizeof(num)) && num[0] == '1') // hot path
                g_real_HeapAlloc = (HeapAlloc_t)patch_iat("KERNEL32.dll", "HeapAlloc", (void *)hooked_HeapAlloc);
        }
        note_stack();
        write_census();
    } else if (reason == DLL_PROCESS_DETACH) {
        write_census();
    }
    return TRUE;
}
