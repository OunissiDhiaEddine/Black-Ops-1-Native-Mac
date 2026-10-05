// T-012: d3d9.dll proxy logger for the reference baseline (32-bit PE, runs under Wine).
//
// Drop the built d3d9.dll next to BlackOps.exe and force the native override
// (WINEDLLOVERRIDES=d3d9=n,b). It loads the real d3d9 from the system directory, forwards
// Direct3DCreate9, and patches the IDirect3D9 / IDirect3DDevice9 vtables IN PLACE with the
// thunks from thunks.S. Objects are never wrapped, so pointers the game hands back to D3D
// are the real ones (Wine's d3d9 asserts on vtable identity for some arguments).
//
// This stage logs call NAMES and COUNTS only (plus the CreateDevice/Reset presentation
// parameters). Output: a text census rewritten every CENSUS_PERIOD frames and at unload.
//   BO1_D3D9_LOG   path of the census file (default: d3d9_census.txt in the cwd)
#define WIN32_LEAN_AND_MEAN
#define CINTERFACE
#define COBJMACROS
#include <windows.h>
#include <d3d9.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "d3d9_methods.h"

#define CENSUS_PERIOD 120

// ---- slot tables, verified against the SDK header ------------------------------------
#define CHECK_D3D9(i, name) \
    static_assert(offsetof(IDirect3D9Vtbl, name) == (i) * sizeof(void *), "IDirect3D9 slot " #name);
#define CHECK_DEVICE(i, name) \
    static_assert(offsetof(IDirect3DDevice9Vtbl, name) == (i) * sizeof(void *), "IDirect3DDevice9 slot " #name);
IDIRECT3D9_METHODS(CHECK_D3D9)
IDIRECT3DDEVICE9_METHODS(CHECK_DEVICE)
static_assert(sizeof(IDirect3D9Vtbl) == IDIRECT3D9_COUNT * sizeof(void *), "IDirect3D9 size");
static_assert(sizeof(IDirect3DDevice9Vtbl) == IDIRECT3DDEVICE9_COUNT * sizeof(void *), "IDirect3DDevice9 size");

#define NAME(i, name) #name,
static const char *const g_names_d3d9[IDIRECT3D9_COUNT] = {IDIRECT3D9_METHODS(NAME)};
static const char *const g_names_device[IDIRECT3DDEVICE9_COUNT] = {IDIRECT3DDEVICE9_METHODS(NAME)};

extern "C" {
// Originals, read by the thunks' tail jump.
void *orig_d3d9[IDIRECT3D9_COUNT];
void *orig_device[IDIRECT3DDEVICE9_COUNT];
// Thunk address tables from thunks.S.
extern void *const thunks_d3d9[IDIRECT3D9_COUNT];
extern void *const thunks_device[IDIRECT3DDEVICE9_COUNT];
void __stdcall trace_d3d9(unsigned slot, void **args);
void __stdcall trace_device(unsigned slot, void **args);
}

// ---- state ---------------------------------------------------------------------------
static HMODULE g_real;
static CRITICAL_SECTION g_lock;
static volatile LONG g_count_d3d9[IDIRECT3D9_COUNT];
static volatile LONG g_count_device[IDIRECT3DDEVICE9_COUNT];
static LONG g_first_frame_device[IDIRECT3DDEVICE9_COUNT]; // frame index of first use, -1 = never
static volatile LONG g_frames;
static void **g_vtbl_d3d9, **g_vtbl_device;
static DWORD g_render_thread;
static LONG g_other_thread_calls;
static char g_log_path[MAX_PATH];
static char g_events[16384]; // CreateDevice / Reset parameter dumps
static size_t g_events_len;
static DWORD g_t0;

static void event(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    EnterCriticalSection(&g_lock);
    if (g_events_len < sizeof(g_events) - 512)
        g_events_len += vsnprintf(g_events + g_events_len, sizeof(g_events) - g_events_len, fmt, ap);
    LeaveCriticalSection(&g_lock);
    va_end(ap);
}

static void write_census(void) {
    EnterCriticalSection(&g_lock);
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

extern "C" void __stdcall trace_device(unsigned slot, void **args) {
    LONG n = InterlockedIncrement(&g_count_device[slot]);
    if (n == 1) g_first_frame_device[slot] = g_frames;
    DWORD tid = GetCurrentThreadId();
    if (slot == 17) { // Present
        if (!g_render_thread) g_render_thread = tid;
        LONG fr = InterlockedIncrement(&g_frames);
        if (fr % CENSUS_PERIOD == 0) write_census();
    } else if (slot == 16) { // Reset(this, pp)
        dump_pp("Reset", (const D3DPRESENT_PARAMETERS *)args[2]);
    }
    if (g_render_thread && tid != g_render_thread) InterlockedIncrement(&g_other_thread_calls);
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
    if (SUCCEEDED(hr) && out && *out)
        hook_vtable((void **)(*out)->lpVtbl, orig_device, thunks_device, IDIRECT3DDEVICE9_COUNT, &g_vtbl_device);
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
        event("proxy loaded, log=%s\n", g_log_path);
        write_census();
    } else if (reason == DLL_PROCESS_DETACH) {
        write_census();
    }
    return TRUE;
}
