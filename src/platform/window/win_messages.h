// SDL3 events -> Win32 window messages (T-052, ADR-006). Pure translation, no window or guest state:
// the user32 shim feeds the result into the game's message queue. The game reads keys from
// WM_KEYDOWN/WM_KEYUP (virtual key in wParam, PS/2 set-1 scan code + extended bit in lParam), text
// from WM_CHAR and the mouse from WM_*BUTTON*/WM_MOUSEMOVE/WM_MOUSEWHEEL.
#pragma once
#include <SDL3/SDL.h>

#include <cstdint>
#include <vector>

namespace bo1::win {

enum : uint32_t {
    WM_SIZE = 0x0005, WM_ACTIVATE = 0x0006, WM_SETFOCUS = 0x0007, WM_KILLFOCUS = 0x0008, WM_CLOSE = 0x0010, WM_QUIT = 0x0012,
    WM_ACTIVATEAPP = 0x001C, WM_KEYDOWN = 0x0100, WM_KEYUP = 0x0101, WM_CHAR = 0x0102, WM_SYSKEYDOWN = 0x0104, WM_SYSKEYUP = 0x0105,
    WM_MOUSEMOVE = 0x0200, WM_LBUTTONDOWN = 0x0201, WM_LBUTTONUP = 0x0202, WM_RBUTTONDOWN = 0x0204, WM_RBUTTONUP = 0x0205,
    WM_MBUTTONDOWN = 0x0207, WM_MBUTTONUP = 0x0208, WM_MOUSEWHEEL = 0x020A, WM_XBUTTONDOWN = 0x020B, WM_XBUTTONUP = 0x020C,
};
enum : uint32_t { MK_LBUTTON = 1, MK_RBUTTON = 2, MK_SHIFT = 4, MK_CONTROL = 8, MK_MBUTTON = 0x10, MK_XBUTTON1 = 0x20, MK_XBUTTON2 = 0x40 };

struct Msg { // MSG without the handle width problem: all fields are 32-bit guest values
    uint32_t hwnd, message, wparam, lparam, time;
    int32_t x, y;
};

struct KeyInfo {
    uint8_t vk;       // Windows virtual-key code (0 = not mapped)
    uint8_t scan;     // PS/2 set-1 make code
    bool extended;    // E0-prefixed key (lParam bit 24)
};
KeyInfo key_info(SDL_Scancode sc);

// Tracks button/modifier state between events (wParam of mouse messages carries it).
struct InputState {
    uint32_t mouse_keys = 0; // MK_* bits
    int32_t x = 0, y = 0;
    bool alt = false;
};

// Appends the Win32 messages one SDL event produces for window `hwnd`. Returns how many were added.
size_t translate(const SDL_Event &ev, uint32_t hwnd, InputState &st, std::vector<Msg> &out);

} // namespace bo1::win
