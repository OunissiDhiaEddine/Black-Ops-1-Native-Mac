#include "win_messages.h"

namespace bo1::win {

namespace {
struct Row {
    SDL_Scancode sdl;
    uint8_t vk, scan;
    bool ext;
};
// Scan codes: IBM PS/2 set 1 (what Windows puts in lParam bits 16-23). Virtual keys: winuser.h.
const Row kKeys[] = {
    {SDL_SCANCODE_ESCAPE, 0x1B, 0x01, false}, {SDL_SCANCODE_1, '1', 0x02, false}, {SDL_SCANCODE_2, '2', 0x03, false},
    {SDL_SCANCODE_3, '3', 0x04, false}, {SDL_SCANCODE_4, '4', 0x05, false}, {SDL_SCANCODE_5, '5', 0x06, false},
    {SDL_SCANCODE_6, '6', 0x07, false}, {SDL_SCANCODE_7, '7', 0x08, false}, {SDL_SCANCODE_8, '8', 0x09, false},
    {SDL_SCANCODE_9, '9', 0x0A, false}, {SDL_SCANCODE_0, '0', 0x0B, false}, {SDL_SCANCODE_MINUS, 0xBD, 0x0C, false},
    {SDL_SCANCODE_EQUALS, 0xBB, 0x0D, false}, {SDL_SCANCODE_BACKSPACE, 0x08, 0x0E, false}, {SDL_SCANCODE_TAB, 0x09, 0x0F, false},
    {SDL_SCANCODE_Q, 'Q', 0x10, false}, {SDL_SCANCODE_W, 'W', 0x11, false}, {SDL_SCANCODE_E, 'E', 0x12, false},
    {SDL_SCANCODE_R, 'R', 0x13, false}, {SDL_SCANCODE_T, 'T', 0x14, false}, {SDL_SCANCODE_Y, 'Y', 0x15, false},
    {SDL_SCANCODE_U, 'U', 0x16, false}, {SDL_SCANCODE_I, 'I', 0x17, false}, {SDL_SCANCODE_O, 'O', 0x18, false},
    {SDL_SCANCODE_P, 'P', 0x19, false}, {SDL_SCANCODE_LEFTBRACKET, 0xDB, 0x1A, false}, {SDL_SCANCODE_RIGHTBRACKET, 0xDD, 0x1B, false},
    {SDL_SCANCODE_RETURN, 0x0D, 0x1C, false}, {SDL_SCANCODE_LCTRL, 0x11, 0x1D, false}, {SDL_SCANCODE_A, 'A', 0x1E, false},
    {SDL_SCANCODE_S, 'S', 0x1F, false}, {SDL_SCANCODE_D, 'D', 0x20, false}, {SDL_SCANCODE_F, 'F', 0x21, false},
    {SDL_SCANCODE_G, 'G', 0x22, false}, {SDL_SCANCODE_H, 'H', 0x23, false}, {SDL_SCANCODE_J, 'J', 0x24, false},
    {SDL_SCANCODE_K, 'K', 0x25, false}, {SDL_SCANCODE_L, 'L', 0x26, false}, {SDL_SCANCODE_SEMICOLON, 0xBA, 0x27, false},
    {SDL_SCANCODE_APOSTROPHE, 0xDE, 0x28, false}, {SDL_SCANCODE_GRAVE, 0xC0, 0x29, false}, {SDL_SCANCODE_LSHIFT, 0x10, 0x2A, false},
    {SDL_SCANCODE_BACKSLASH, 0xDC, 0x2B, false}, {SDL_SCANCODE_Z, 'Z', 0x2C, false}, {SDL_SCANCODE_X, 'X', 0x2D, false},
    {SDL_SCANCODE_C, 'C', 0x2E, false}, {SDL_SCANCODE_V, 'V', 0x2F, false}, {SDL_SCANCODE_B, 'B', 0x30, false},
    {SDL_SCANCODE_N, 'N', 0x31, false}, {SDL_SCANCODE_M, 'M', 0x32, false}, {SDL_SCANCODE_COMMA, 0xBC, 0x33, false},
    {SDL_SCANCODE_PERIOD, 0xBE, 0x34, false}, {SDL_SCANCODE_SLASH, 0xBF, 0x35, false}, {SDL_SCANCODE_RSHIFT, 0x10, 0x36, false},
    {SDL_SCANCODE_KP_MULTIPLY, 0x6A, 0x37, false}, {SDL_SCANCODE_LALT, 0x12, 0x38, false}, {SDL_SCANCODE_SPACE, 0x20, 0x39, false},
    {SDL_SCANCODE_CAPSLOCK, 0x14, 0x3A, false}, {SDL_SCANCODE_F1, 0x70, 0x3B, false}, {SDL_SCANCODE_F2, 0x71, 0x3C, false},
    {SDL_SCANCODE_F3, 0x72, 0x3D, false}, {SDL_SCANCODE_F4, 0x73, 0x3E, false}, {SDL_SCANCODE_F5, 0x74, 0x3F, false},
    {SDL_SCANCODE_F6, 0x75, 0x40, false}, {SDL_SCANCODE_F7, 0x76, 0x41, false}, {SDL_SCANCODE_F8, 0x77, 0x42, false},
    {SDL_SCANCODE_F9, 0x78, 0x43, false}, {SDL_SCANCODE_F10, 0x79, 0x44, false}, {SDL_SCANCODE_NUMLOCKCLEAR, 0x90, 0x45, true},
    {SDL_SCANCODE_SCROLLLOCK, 0x91, 0x46, false}, {SDL_SCANCODE_KP_7, 0x67, 0x47, false}, {SDL_SCANCODE_KP_8, 0x68, 0x48, false},
    {SDL_SCANCODE_KP_9, 0x69, 0x49, false}, {SDL_SCANCODE_KP_MINUS, 0x6D, 0x4A, false}, {SDL_SCANCODE_KP_4, 0x64, 0x4B, false},
    {SDL_SCANCODE_KP_5, 0x65, 0x4C, false}, {SDL_SCANCODE_KP_6, 0x66, 0x4D, false}, {SDL_SCANCODE_KP_PLUS, 0x6B, 0x4E, false},
    {SDL_SCANCODE_KP_1, 0x61, 0x4F, false}, {SDL_SCANCODE_KP_2, 0x62, 0x50, false}, {SDL_SCANCODE_KP_3, 0x63, 0x51, false},
    {SDL_SCANCODE_KP_0, 0x60, 0x52, false}, {SDL_SCANCODE_KP_PERIOD, 0x6E, 0x53, false}, {SDL_SCANCODE_F11, 0x7A, 0x57, false},
    {SDL_SCANCODE_F12, 0x7B, 0x58, false},
    // extended keys (E0 prefix)
    {SDL_SCANCODE_KP_ENTER, 0x0D, 0x1C, true}, {SDL_SCANCODE_RCTRL, 0x11, 0x1D, true}, {SDL_SCANCODE_KP_DIVIDE, 0x6F, 0x35, true},
    {SDL_SCANCODE_RALT, 0x12, 0x38, true}, {SDL_SCANCODE_HOME, 0x24, 0x47, true}, {SDL_SCANCODE_UP, 0x26, 0x48, true},
    {SDL_SCANCODE_PAGEUP, 0x21, 0x49, true}, {SDL_SCANCODE_LEFT, 0x25, 0x4B, true}, {SDL_SCANCODE_RIGHT, 0x27, 0x4D, true},
    {SDL_SCANCODE_END, 0x23, 0x4F, true}, {SDL_SCANCODE_DOWN, 0x28, 0x50, true}, {SDL_SCANCODE_PAGEDOWN, 0x22, 0x51, true},
    {SDL_SCANCODE_INSERT, 0x2D, 0x52, true}, {SDL_SCANCODE_DELETE, 0x2E, 0x53, true},
};

uint32_t pack_xy(int32_t x, int32_t y) { return ((uint32_t)(uint16_t)y << 16) | (uint16_t)x; }
} // namespace

KeyInfo key_info(SDL_Scancode sc) {
    for (const Row &r : kKeys)
        if (r.sdl == sc) return {r.vk, r.scan, r.ext};
    return {0, 0, false};
}

size_t translate(const SDL_Event &ev, uint32_t hwnd, InputState &st, std::vector<Msg> &out) {
    size_t before = out.size();
    uint32_t time = (uint32_t)(ev.common.timestamp / 1000000ull);
    auto push = [&](uint32_t message, uint32_t wp, uint32_t lp) { out.push_back({hwnd, message, wp, lp, time, st.x, st.y}); };
    switch (ev.type) {
    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_KEY_UP: {
        KeyInfo k = key_info(ev.key.scancode);
        if (!k.vk) break;
        bool down = ev.type == SDL_EVENT_KEY_DOWN;
        if (k.vk == 0x12) st.alt = down;
        // lParam: repeat count 1, scan code, extended, context (Alt held), previous state, transition
        uint32_t lp = 1u | ((uint32_t)k.scan << 16) | (k.extended ? 1u << 24 : 0) | (st.alt ? 1u << 29 : 0);
        if (down && ev.key.repeat) lp |= 1u << 30;
        if (!down) lp |= (1u << 30) | (1u << 31);
        bool sys = st.alt && k.vk != 0x11 && k.vk != 0x10; // Alt combinations arrive as WM_SYSKEY*
        push(down ? (sys ? WM_SYSKEYDOWN : WM_KEYDOWN) : (sys ? WM_SYSKEYUP : WM_KEYUP), k.vk, lp);
        // control characters that TranslateMessage would turn into WM_CHAR (printable text comes from TEXT_INPUT)
        if (down && !sys && (k.vk == 0x0D || k.vk == 0x08 || k.vk == 0x09 || k.vk == 0x1B)) push(WM_CHAR, k.vk, lp);
        break;
    }
    case SDL_EVENT_TEXT_INPUT:
        for (const unsigned char *p = (const unsigned char *)ev.text.text; p && *p; p++)
            if (*p >= 0x20 && *p < 0x7F) push(WM_CHAR, *p, 1); // ASCII only: the game's console and menus are ASCII
        break;
    case SDL_EVENT_MOUSE_MOTION:
        if (st.relative) {
            st.fx += ev.motion.xrel;
            st.fy += ev.motion.yrel;
            int32_t dx = (int32_t)st.fx, dy = (int32_t)st.fy; // whole steps now, the rest with the next event
            st.fx -= (float)dx;
            st.fy -= (float)dy;
            if (!dx && !dy) break;
            st.x += dx;
            st.y += dy;
        } else {
            st.x = (int32_t)ev.motion.x;
            st.y = (int32_t)ev.motion.y;
        }
        push(WM_MOUSEMOVE, st.mouse_keys, pack_xy(st.x, st.y));
        break;
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP: {
        bool down = ev.type == SDL_EVENT_MOUSE_BUTTON_DOWN;
        if (!st.relative) { // in mouse look the real pointer is parked; the virtual cursor stays where it is
            st.x = (int32_t)ev.button.x;
            st.y = (int32_t)ev.button.y;
        }
        uint32_t bit = 0, msg = 0, hi = 0;
        switch (ev.button.button) {
        case SDL_BUTTON_LEFT: bit = MK_LBUTTON, msg = down ? WM_LBUTTONDOWN : WM_LBUTTONUP; break;
        case SDL_BUTTON_RIGHT: bit = MK_RBUTTON, msg = down ? WM_RBUTTONDOWN : WM_RBUTTONUP; break;
        case SDL_BUTTON_MIDDLE: bit = MK_MBUTTON, msg = down ? WM_MBUTTONDOWN : WM_MBUTTONUP; break;
        case SDL_BUTTON_X1: bit = MK_XBUTTON1, msg = down ? WM_XBUTTONDOWN : WM_XBUTTONUP, hi = 1; break;
        case SDL_BUTTON_X2: bit = MK_XBUTTON2, msg = down ? WM_XBUTTONDOWN : WM_XBUTTONUP, hi = 2; break;
        default: break;
        }
        if (!msg) break;
        st.mouse_keys = down ? (st.mouse_keys | bit) : (st.mouse_keys & ~bit);
        push(msg, st.mouse_keys | (hi << 16), pack_xy(st.x, st.y));
        break;
    }
    case SDL_EVENT_MOUSE_WHEEL: {
        int32_t delta = (int32_t)(ev.wheel.y * 120.0f); // WHEEL_DELTA per notch
        if (delta) push(WM_MOUSEWHEEL, ((uint32_t)(uint16_t)(int16_t)delta << 16) | st.mouse_keys, pack_xy(st.x, st.y));
        break;
    }
    case SDL_EVENT_WINDOW_FOCUS_GAINED:
        push(WM_ACTIVATEAPP, 1, 0);
        push(WM_ACTIVATE, 1, 0); // WA_ACTIVE
        push(WM_SETFOCUS, 0, 0);
        break;
    case SDL_EVENT_WINDOW_FOCUS_LOST:
        st.mouse_keys = 0;
        st.alt = false;
        st.relative = false;
        push(WM_KILLFOCUS, 0, 0);
        push(WM_ACTIVATE, 0, 0); // WA_INACTIVE
        push(WM_ACTIVATEAPP, 0, 0);
        break;
    case SDL_EVENT_WINDOW_RESIZED:
        push(WM_SIZE, 0, pack_xy(ev.window.data1, ev.window.data2));
        break;
    case SDL_EVENT_WINDOW_MINIMIZED:
        push(WM_SIZE, 1, 0); // SIZE_MINIMIZED
        break;
    case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
    case SDL_EVENT_QUIT:
        push(WM_CLOSE, 0, 0);
        break;
    default:
        break;
    }
    return out.size() - before;
}

} // namespace bo1::win
