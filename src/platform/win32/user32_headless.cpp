// user32 / gdi32 for the native runtime (T-045, T-052). Window classes and windows are bookkeeping here;
// the window procedure receives the creation / activation messages a real window would get. When the
// window service runs (BO1_WINDOW=1, src/platform/window, ADR-006) the game's top-level window is a real
// SDL3 window and PeekMessageA / GetMessageA / GetCursorPos are served from it; without it the run is
// headless and the queue only carries scripted input (BO1_KEYS). The file name is historical.
// Message sequences are a simplified version of what Windows sends (LIKELY sufficient; to be compared
// with a message trace from the Wine baseline if the game misbehaves).
#include <unistd.h>

#include <chrono>
#include <string>
#include <vector>
#include <map>
#include <mutex>

#include "image_info.h"
#include "win32_util.h"
#include "window_service.h"

namespace bo1 {

enum : uint32_t { WM_CREATE = 0x01, WM_MOVE = 0x03, WM_SIZE = 0x05, WM_ACTIVATE = 0x06, WM_SETFOCUS = 0x07, WM_SHOWWINDOW = 0x18,
                  WM_ACTIVATEAPP = 0x1C, WM_NCCREATE = 0x81, WM_NCACTIVATE = 0x86 };

struct WndClass {
    uint32_t wndproc = 0, style = 0, cls_extra = 0, wnd_extra = 0;
};
struct Window {
    uint32_t wndproc = 0, style = 0, exstyle = 0, userdata = 0, id = 0;
    int32_t x = 0, y = 0, w = 0, h = 0;
    bool visible = false;
};
static std::mutex g_mu;
static std::map<std::string, WndClass> g_classes;
static std::map<uint32_t, Window> g_windows;
static uint32_t g_next_hwnd = 0x00010100, g_focus = 0, g_next_atom = 0xC000;
static const uint32_t kDesktop = 0x00010010;
static const int32_t kScreenW = 1440, kScreenH = 900; // headless "monitor": the desktop mode of the baseline (re/d3d9_adapter_probe.json)

static std::string class_key(uint32_t name_va) {
    if (name_va < 0x10000) return "#" + std::to_string(name_va);
    std::string s = gstr(name_va);
    for (char &c : s) c = (char)tolower((unsigned char)c);
    return s;
}

SHIM(user32, RegisterClassExA, 1) {
    // WNDCLASSEXA: cbSize, style, lpfnWndProc, cbClsExtra, cbWndExtra, hInstance, hIcon, hCursor, hbrBackground, lpszMenuName, lpszClassName, hIconSm
    WndClass c;
    c.style = rd32(a[0] + 4);
    c.wndproc = rd32(a[0] + 8);
    c.cls_extra = rd32(a[0] + 12);
    c.wnd_extra = rd32(a[0] + 16);
    std::lock_guard<std::mutex> lk(g_mu);
    g_classes[class_key(rd32(a[0] + 40))] = c;
    if (g_verbose) log("RegisterClassExA(\"%s\", wndproc %08x)", gstr(rd32(a[0] + 40)).c_str(), c.wndproc);
    return g_next_atom++;
}
SHIM(user32, RegisterClassA, 1) {
    WndClass c;
    c.style = rd32(a[0]);
    c.wndproc = rd32(a[0] + 4);
    std::lock_guard<std::mutex> lk(g_mu);
    g_classes[class_key(rd32(a[0] + 36))] = c;
    return g_next_atom++;
}

static uint32_t send(Cpu *cpu, uint32_t hwnd, uint32_t msg, uint32_t wp, uint32_t lp) {
    uint32_t proc;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        auto it = g_windows.find(hwnd);
        if (it == g_windows.end() || !it->second.wndproc) return 0;
        proc = it->second.wndproc;
    }
    return guest_call(cpu, proc, {hwnd, msg, wp, lp});
}

static void adjust_rect(uint32_t rect, uint32_t style, bool menu);
SHIM(user32, CreateWindowExA, 12) {
    // (dwExStyle, lpClassName, lpWindowName, dwStyle, X, Y, nWidth, nHeight, hWndParent, hMenu, hInstance, lpParam)
    uint32_t hwnd;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        auto it = g_classes.find(class_key(a[1]));
        if (it == g_classes.end()) {
            log("CreateWindowExA: class \"%s\" is not registered", gstr(a[1]).c_str());
            last_error(cpu) = 1407; // ERROR_CANNOT_FIND_WND_CLASS
            return 0;
        }
        hwnd = g_next_hwnd;
        g_next_hwnd += 4;
        Window w;
        w.wndproc = it->second.wndproc;
        w.exstyle = a[0];
        w.style = a[3];
        w.x = (int32_t)a[4] == (int32_t)0x80000000 ? 0 : (int32_t)a[4]; // CW_USEDEFAULT
        w.y = (int32_t)a[5] == (int32_t)0x80000000 ? 0 : (int32_t)a[5];
        w.w = (int32_t)a[6] == (int32_t)0x80000000 ? 640 : (int32_t)a[6];
        w.h = (int32_t)a[7] == (int32_t)0x80000000 ? 480 : (int32_t)a[7];
        g_windows[hwnd] = w;
    }
    if (g_verbose) log("CreateWindowExA(class \"%s\", \"%s\", style %08x, %dx%d) -> hwnd %08x", gstr(a[1]).c_str(), gstr(a[2]).c_str(), a[3], (int32_t)a[6], (int32_t)a[7], hwnd);
    // CREATESTRUCTA on the guest stack area below esp is not safe to use from a shim: allocate it
    uint32_t cs = heap_alloc(48, true);
    wr32(cs, a[11]);      // lpCreateParams
    wr32(cs + 4, a[10]);  // hInstance
    wr32(cs + 8, a[9]);   // hMenu
    wr32(cs + 12, a[8]);  // hwndParent
    wr32(cs + 16, a[7]);  // cy
    wr32(cs + 20, a[6]);  // cx
    wr32(cs + 24, a[5]);  // y
    wr32(cs + 28, a[4]);  // x
    wr32(cs + 32, a[3]);  // style
    wr32(cs + 36, a[2]);  // lpszName
    wr32(cs + 40, a[1]);  // lpszClass
    wr32(cs + 44, a[0]);  // dwExStyle
    bool ok = send(cpu, hwnd, WM_NCCREATE, 0, cs) != 0 && (int32_t)send(cpu, hwnd, WM_CREATE, 0, cs) != -1;
    heap_free(cs);
    if (!ok) {
        std::lock_guard<std::mutex> lk(g_mu);
        g_windows.erase(hwnd);
        return 0;
    }
    if (a[3] & 0x10000000u) { // WS_VISIBLE
        std::lock_guard<std::mutex> lk(g_mu);
        g_windows[hwnd].visible = true;
    }
    // The game's main window becomes a real one when the window service runs (BO1_WINDOW=1). Its client
    // area is what the game asked the outer size for: borders and caption are added back by AdjustWindowRect.
    win::WindowService &ws = win::WindowService::get();
    if (ws.wants_window() && !ws.has_window() && !a[8] /* top level */) {
        uint32_t rc = heap_alloc(16, true);
        adjust_rect(rc, a[3], false); // how much the style adds around a client area
        int cw = (int32_t)a[6] - ((int32_t)rd32(rc + 8) - (int32_t)rd32(rc)), ch = (int32_t)a[7] - ((int32_t)rd32(rc + 12) - (int32_t)rd32(rc + 4));
        heap_free(rc);
        if (cw < 64 || ch < 64) cw = (int32_t)a[6], ch = (int32_t)a[7];
        if (ws.create(hwnd, a[2] ? "Call of Duty: Black Ops (native)" : "bo1", cw, ch)) log("window: %dx%d client area for hwnd %08x", cw, ch, hwnd);
        else log("window: could not create the real window (continuing headless)");
    }
    return hwnd;
}
SHIM(user32, DestroyWindow, 1) {
    std::lock_guard<std::mutex> lk(g_mu);
    return g_windows.erase(a[0]) ? 1 : 0;
}
SHIM(user32, DefWindowProcA, 4) { return a[1] == WM_NCCREATE ? 1 : 0; }
SHIM(user32, ShowWindow, 2) {
    bool was, show = a[1] != 0; // SW_HIDE = 0
    int32_t w, h, x, y;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        auto it = g_windows.find(a[0]);
        if (it == g_windows.end()) return 0;
        was = it->second.visible;
        it->second.visible = show;
        w = it->second.w, h = it->second.h, x = it->second.x, y = it->second.y;
    }
    if (show && !was) {
        send(cpu, a[0], WM_SHOWWINDOW, 1, 0);
        send(cpu, a[0], WM_ACTIVATEAPP, 1, 0);
        send(cpu, a[0], WM_NCACTIVATE, 1, 0);
        send(cpu, a[0], WM_ACTIVATE, 1 /* WA_ACTIVE */, 0);
        g_focus = a[0];
        send(cpu, a[0], WM_SETFOCUS, 0, 0);
        send(cpu, a[0], WM_SIZE, 0, ((uint32_t)h << 16) | ((uint32_t)w & 0xFFFF));
        send(cpu, a[0], WM_MOVE, 0, ((uint32_t)y << 16) | ((uint32_t)x & 0xFFFF));
    }
    return was ? 1 : 0;
}
SHIM(user32, UpdateWindow, 1) { return 1; }
SHIM(user32, SetFocus, 1) {
    uint32_t old = g_focus;
    g_focus = a[0];
    if (a[0] && a[0] != old) send(cpu, a[0], WM_SETFOCUS, old, 0);
    return old;
}
SHIM(user32, GetForegroundWindow, 0) { return g_focus; }
SHIM(user32, GetActiveWindow, 0) { return g_focus; }
SHIM(user32, GetDesktopWindow, 0) { return kDesktop; }
SHIM(user32, IsWindow, 1) {
    std::lock_guard<std::mutex> lk(g_mu);
    return g_windows.count(a[0]) ? 1 : 0;
}
// The game gave its window a new outer size (display-mode change: R_ResetDevice, VA 0x006B87C9): the real
// window follows with the client area that outer size stands for.
static void resize_real_window(uint32_t hwnd, uint32_t style, int32_t w, int32_t h) {
    win::WindowService &ws = win::WindowService::get();
    if (!ws.has_window() || ws.hwnd() != hwnd) return;
    uint32_t rc = heap_alloc(16, true);
    adjust_rect(rc, style, false);
    int cw = w - ((int32_t)rd32(rc + 8) - (int32_t)rd32(rc)), ch = h - ((int32_t)rd32(rc + 12) - (int32_t)rd32(rc + 4));
    heap_free(rc);
    if (cw < 64 || ch < 64) return;
    ws.resize(cw, ch);
    log("window: resized to a %dx%d client area", cw, ch);
}
SHIM(user32, SetWindowPos, 7) {
    // (hWnd, hWndInsertAfter, X, Y, cx, cy, uFlags)
    uint32_t style;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        auto it = g_windows.find(a[0]);
        if (it == g_windows.end()) return 0;
        if (!(a[6] & 2)) it->second.x = (int32_t)a[2], it->second.y = (int32_t)a[3]; // !SWP_NOMOVE
        if (!(a[6] & 1)) it->second.w = (int32_t)a[4], it->second.h = (int32_t)a[5]; // !SWP_NOSIZE
        style = it->second.style;
    }
    if (!(a[6] & 1)) resize_real_window(a[0], style, (int32_t)a[4], (int32_t)a[5]);
    return 1;
}
SHIM(user32, MoveWindow, 6) {
    uint32_t style;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        auto it = g_windows.find(a[0]);
        if (it == g_windows.end()) return 0;
        it->second.x = (int32_t)a[1], it->second.y = (int32_t)a[2], it->second.w = (int32_t)a[3], it->second.h = (int32_t)a[4];
        style = it->second.style;
    }
    resize_real_window(a[0], style, (int32_t)a[3], (int32_t)a[4]);
    return 1;
}
SHIM(user32, GetWindowRect, 2) {
    std::lock_guard<std::mutex> lk(g_mu);
    auto it = g_windows.find(a[0]);
    if (it == g_windows.end()) return 0;
    wr32(a[1], (uint32_t)it->second.x);
    wr32(a[1] + 4, (uint32_t)it->second.y);
    wr32(a[1] + 8, (uint32_t)(it->second.x + it->second.w));
    wr32(a[1] + 12, (uint32_t)(it->second.y + it->second.h));
    return 1;
}
SHIM(user32, GetWindowLongA, 2) {
    std::lock_guard<std::mutex> lk(g_mu);
    auto it = g_windows.find(a[0]);
    if (it == g_windows.end()) return 0;
    switch ((int32_t)a[1]) {
    case -16: return it->second.style;
    case -20: return it->second.exstyle;
    case -4: return it->second.wndproc;
    case -21: return it->second.userdata;
    case -12: return it->second.id;
    default: return 0;
    }
}
SHIM(user32, SetWindowLongA, 3) {
    std::lock_guard<std::mutex> lk(g_mu);
    auto it = g_windows.find(a[0]);
    if (it == g_windows.end()) return 0;
    uint32_t *field = nullptr;
    switch ((int32_t)a[1]) {
    case -16: field = &it->second.style; break;
    case -20: field = &it->second.exstyle; break;
    case -4: field = &it->second.wndproc; break;
    case -21: field = &it->second.userdata; break;
    case -12: field = &it->second.id; break;
    default: return 0;
    }
    uint32_t old = *field;
    *field = a[2];
    return old;
}
SHIM(user32, SetWindowTextA, 2) { return 1; }

// Styles decide the border sizes; the headless runtime uses the classic metrics (caption 23, frame 8/4).
static void adjust_rect(uint32_t rect, uint32_t style, bool menu) {
    int32_t bx = 0, by = 0, cap = 0;
    if (style & 0x00C00000u) cap = 23;                         // WS_CAPTION
    if (style & 0x00040000u) bx = by = 8;                      // WS_THICKFRAME
    else if (style & (0x00800000u | 0x00400000u)) bx = by = 3; // WS_BORDER / WS_DLGFRAME
    wr32(rect, rd32(rect) - (uint32_t)bx);
    wr32(rect + 4, rd32(rect + 4) - (uint32_t)(by + cap + (menu ? 20 : 0)));
    wr32(rect + 8, rd32(rect + 8) + (uint32_t)bx);
    wr32(rect + 12, rd32(rect + 12) + (uint32_t)by);
}
SHIM(user32, AdjustWindowRect, 3) {
    adjust_rect(a[0], a[1], a[2] != 0);
    return 1;
}
SHIM(user32, AdjustWindowRectEx, 4) {
    adjust_rect(a[0], a[1], a[2] != 0);
    return 1;
}
SHIM(user32, GetSystemMetrics, 1) {
    switch (a[0]) {
    case 0: return (uint32_t)kScreenW;  // SM_CXSCREEN
    case 1: return (uint32_t)kScreenH;  // SM_CYSCREEN
    case 4: return 23;                  // SM_CYCAPTION
    case 5: case 6: return 1;           // SM_CXBORDER / SM_CYBORDER
    case 32: case 33: return 8;         // SM_CXFRAME / SM_CYFRAME
    case 76: case 77: return 0;         // SM_XVIRTUALSCREEN / SM_YVIRTUALSCREEN
    case 78: return (uint32_t)kScreenW; // SM_CXVIRTUALSCREEN
    case 79: return (uint32_t)kScreenH;
    case 80: return 1;                  // SM_CMONITORS
    case 0x1000: return 0;              // SM_REMOTESESSION
    default:
        if (g_verbose) log("GetSystemMetrics(%u) -> 0 (not modelled)", a[0]);
        return 0;
    }
}
static const uint32_t kMonitor = 0x00010001;
SHIM(user32, MonitorFromPoint, 3) { return kMonitor; }
SHIM(user32, MonitorFromWindow, 2) { return kMonitor; }
SHIM(user32, GetMonitorInfoA, 2) {
    uint32_t size = rd32(a[1]);
    for (int k = 0; k < 2; k++) { // rcMonitor, rcWork
        wr32(a[1] + 4 + 16 * k, 0);
        wr32(a[1] + 8 + 16 * k, 0);
        wr32(a[1] + 12 + 16 * k, (uint32_t)kScreenW);
        wr32(a[1] + 16 + 16 * k, (uint32_t)kScreenH);
    }
    wr32(a[1] + 36, 1); // MONITORINFOF_PRIMARY
    if (size >= 72) put_str(a[1] + 40, 32, "\\\\.\\DISPLAY1");
    return 1;
}

// ---- messages: the headless queue is always empty ----------------------------------------------------------
// BO1_CMDS="ms|command;ms|command": engine console commands, queued through the game's own
// Cbuf_AddText(localClientNum, text) (VA 0x0049B930, re/symbols.csv) on the thread that pumps messages —
// the same hook the tracing proxy uses for the baseline (e.g. "60000|devmap cuba").
static void run_scripted_commands(Cpu *cpu) {
    struct Cmd { long at; std::string text; };
    static std::vector<Cmd> cmds;
    static bool parsed = false;
    static auto t0 = std::chrono::steady_clock::now();
    if (!parsed) {
        parsed = true;
        if (const char *e = getenv("BO1_CMDS")) {
            std::string spec = e;
            size_t pos = 0;
            while (pos < spec.size()) {
                size_t end = spec.find(';', pos);
                if (end == std::string::npos) end = spec.size();
                std::string tok = spec.substr(pos, end - pos);
                pos = end + 1;
                size_t bar = tok.find('|');
                if (bar != std::string::npos) cmds.push_back({strtol(tok.c_str(), nullptr, 0), tok.substr(bar + 1)});
            }
        }
        static const uint8_t prologue[] = {0x6A, 0x35, 0xE8}; // push 0x35 ; call Sys_EnterCriticalSection
        if (!cmds.empty() && memcmp(g_mem + 0x0049B930, prologue, 3) != 0) {
            log("BO1_CMDS: this is not the executable the Cbuf_AddText address belongs to — ignored");
            cmds.clear();
        }
    }
    if (cmds.empty()) return;
    long now = (long)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
    for (size_t i = 0; i < cmds.size();) {
        if (cmds[i].at > now) { i++; continue; }
        uint32_t text = heap_strdup(cmds[i].text + "\n");
        log("scripted command: %s", cmds[i].text.c_str());
        guest_call(cpu, 0x0049B930, {0, text});
        heap_free(text);
        cmds.erase(cmds.begin() + (long)i);
    }
}

SHIM(user32, PeekMessageA, 5) { // (lpMsg, hWnd, wMsgFilterMin, wMsgFilterMax, wRemoveMsg)
    run_scripted_commands(cpu);
    win::WindowService &ws = win::WindowService::get();
    win::Msg m;
    if (!ws.enabled() || !ws.peek(&m, a[4] & 1)) return 0;
    wr32(a[0], m.hwnd), wr32(a[0] + 4, m.message), wr32(a[0] + 8, m.wparam), wr32(a[0] + 12, m.lparam);
    wr32(a[0] + 16, m.time), wr32(a[0] + 20, (uint32_t)m.x), wr32(a[0] + 24, (uint32_t)m.y);
    return 1;
}
SHIM(user32, GetMessageA, 4) { // (lpMsg, hWnd, wMsgFilterMin, wMsgFilterMax): blocks until a message arrives
    win::WindowService &ws = win::WindowService::get();
    if (!ws.enabled()) {
        log("GetMessageA: blocking forever (no message source in the headless runtime)");
        for (;;) usleep(1000000);
    }
    win::Msg m;
    while (!ws.peek(&m, true)) usleep(1000);
    wr32(a[0], m.hwnd), wr32(a[0] + 4, m.message), wr32(a[0] + 8, m.wparam), wr32(a[0] + 12, m.lparam);
    wr32(a[0] + 16, m.time), wr32(a[0] + 20, (uint32_t)m.x), wr32(a[0] + 24, (uint32_t)m.y);
    return m.message != 0x0012; // WM_QUIT ends the caller's loop
}
SHIM(user32, TranslateMessage, 1) { return 0; }
SHIM(user32, DispatchMessageA, 1) {
    // MSG: hwnd, message, wParam, lParam, time, pt
    return send(cpu, rd32(a[0]), rd32(a[0] + 4), rd32(a[0] + 8), rd32(a[0] + 12));
}
SHIM(user32, SendMessageA, 4) { return send(cpu, a[0], a[1], a[2], a[3]); }
SHIM(user32, PostMessageA, 4) { return 1; }
SHIM(user32, PostQuitMessage, 1) { return 0; }
SHIM(user32, CallWindowProcA, 5) { return a[0] ? guest_call(cpu, a[0], {a[1], a[2], a[3], a[4]}) : 0; }
SHIM(user32, RegisterWindowMessageA, 1) {
    static std::map<std::string, uint32_t> ids;
    std::lock_guard<std::mutex> lk(g_mu);
    auto it = ids.find(gstr(a[0]));
    if (it != ids.end()) return it->second;
    uint32_t id = 0xC000 + (uint32_t)ids.size();
    ids[gstr(a[0])] = id;
    return id;
}
SHIM(user32, SetWindowsHookExA, 4) { return 0x00020001; } // hook never fires without real input
SHIM(user32, CallNextHookEx, 4) { return 0; }

// ---- cursor / input -----------------------------------------------------------------------------------------
static int32_t g_cursor_count = 0;
SHIM(user32, ShowCursor, 1) { return (uint32_t)(a[0] ? ++g_cursor_count : --g_cursor_count); }
SHIM(user32, GetCursorPos, 1) {
    win::WindowService &ws = win::WindowService::get();
    if (ws.enabled() && ws.has_window()) { // screen position = window origin + position inside the client area
        int32_t x, y, ox = 0, oy = 0;
        ws.cursor(&x, &y);
        {
            std::lock_guard<std::mutex> lk(g_mu);
            if (!g_windows.empty()) ox = g_windows.begin()->second.x, oy = g_windows.begin()->second.y;
        }
        wr32(a[0], (uint32_t)(ox + x));
        wr32(a[0] + 4, (uint32_t)(oy + y));
        return 1;
    }
    wr32(a[0], (uint32_t)(kScreenW / 2));
    wr32(a[0] + 4, (uint32_t)(kScreenH / 2));
    return 1;
}
SHIM(user32, SetCursorPos, 2) {
    // (x, y) in screen coordinates. The game's mouse look (0x00867840) reads the cursor, takes the distance
    // from where it last put it and re-centres it with this call (0x005B7700): ignoring the call turns every
    // offset from the centre into a constant turn rate. The window service keeps the game's cursor.
    win::WindowService &ws = win::WindowService::get();
    if (ws.enabled()) {
        int32_t ox = 0, oy = 0;
        {
            std::lock_guard<std::mutex> lk(g_mu);
            if (!g_windows.empty()) ox = g_windows.begin()->second.x, oy = g_windows.begin()->second.y;
        }
        ws.warp((int32_t)a[0] - ox, (int32_t)a[1] - oy);
    }
    return 1;
}
SHIM(user32, ScreenToClient, 2) {
    std::lock_guard<std::mutex> lk(g_mu);
    auto it = g_windows.find(a[0]);
    if (it == g_windows.end()) return 0;
    wr32(a[1], rd32(a[1]) - (uint32_t)it->second.x);
    wr32(a[1] + 4, rd32(a[1] + 4) - (uint32_t)it->second.y);
    return 1;
}
SHIM(user32, ClientToScreen, 2) {
    std::lock_guard<std::mutex> lk(g_mu);
    auto it = g_windows.find(a[0]);
    if (it == g_windows.end()) return 0;
    wr32(a[1], rd32(a[1]) + (uint32_t)it->second.x);
    wr32(a[1] + 4, rd32(a[1] + 4) + (uint32_t)it->second.y);
    return 1;
}
SHIM(user32, GetAsyncKeyState, 1) { return 0; }
SHIM(user32, MapVirtualKeyA, 2) { return 0; }
SHIM(user32, LoadIconA, 2) { return 0x00030001; }
SHIM(user32, LoadCursorA, 2) { return 0x00030002; }
SHIM(user32, LoadImageA, 6) { return 0x00030003; }
SHIM(user32, MessageBoxA, 4) {
    // Never blocks. The text is the interesting part: the game reports fatal errors this way.
    log("MessageBoxA(\"%s\": \"%s\", type %x) -> IDNO/IDOK", gstr(a[2]).c_str(), gstr(a[1]).c_str(), a[3]);
    return (a[3] & 0xF) == 4 || (a[3] & 0xF) == 3 ? 7 /* IDNO */ : 1 /* IDOK */;
}

// ---- gdi32 -----------------------------------------------------------------------------------------------------
SHIM(gdi32, CreateSolidBrush, 1) { return 0x00040001; }
SHIM(gdi32, GetDeviceCaps, 2) {
    switch (a[1]) {
    case 8: return (uint32_t)kScreenW; // HORZRES
    case 10: return (uint32_t)kScreenH; // VERTRES
    case 12: return 32;                // BITSPIXEL
    case 116: return 60;               // VREFRESH
    default: return 0;
    }
}
SHIM(user32, GetDC, 1) { return 0x00050001; }
SHIM(user32, ReleaseDC, 2) { return 1; }
SHIM(gdi32, SetDeviceGammaRamp, 2) { return 1; }

} // namespace bo1
