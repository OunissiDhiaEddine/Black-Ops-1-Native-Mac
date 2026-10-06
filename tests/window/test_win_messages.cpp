// Unit test for the SDL3 -> Win32 message translation (T-052). No window is opened: synthetic SDL
// events go straight into bo1::win::translate and the resulting messages are checked field by field.
#include <cstdio>
#include <cstring>

#include "win_messages.h"

using namespace bo1::win;

static int g_fail = 0;
#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            std::printf("FAIL line %d: %s\n", __LINE__, #cond);                  \
            g_fail++;                                                            \
        }                                                                        \
    } while (0)

static SDL_Event key(SDL_Scancode sc, bool down, bool repeat = false) {
    SDL_Event e;
    std::memset(&e, 0, sizeof(e));
    e.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
    e.key.scancode = sc;
    e.key.down = down;
    e.key.repeat = repeat;
    return e;
}

int main() {
    const uint32_t hwnd = 0x10100;
    InputState st;
    std::vector<Msg> m;

    // W down / repeat / up: virtual key 'W', scan code 0x11
    translate(key(SDL_SCANCODE_W, true), hwnd, st, m);
    CHECK(m.size() == 1 && m[0].message == WM_KEYDOWN && m[0].wparam == 'W' && m[0].lparam == 0x00110001u && m[0].hwnd == hwnd);
    translate(key(SDL_SCANCODE_W, true, true), hwnd, st, m);
    CHECK(m.size() == 2 && m[1].lparam == 0x40110001u);
    translate(key(SDL_SCANCODE_W, false), hwnd, st, m);
    CHECK(m.size() == 3 && m[2].message == WM_KEYUP && m[2].lparam == 0xC0110001u);
    m.clear();

    // extended key: Up arrow = VK_UP, scan 0x48 with bit 24
    translate(key(SDL_SCANCODE_UP, true), hwnd, st, m);
    CHECK(m.size() == 1 && m[0].wparam == 0x26 && m[0].lparam == 0x01480001u);
    m.clear();

    // Enter also yields WM_CHAR 13 (what TranslateMessage would post); Escape likewise
    translate(key(SDL_SCANCODE_RETURN, true), hwnd, st, m);
    CHECK(m.size() == 2 && m[0].message == WM_KEYDOWN && m[0].wparam == 0x0D && m[1].message == WM_CHAR && m[1].wparam == 0x0D);
    m.clear();

    // Alt+Enter arrives as WM_SYSKEYDOWN with the context bit, no WM_CHAR
    translate(key(SDL_SCANCODE_LALT, true), hwnd, st, m);
    translate(key(SDL_SCANCODE_RETURN, true), hwnd, st, m);
    CHECK(m.size() == 2 && m[0].message == WM_SYSKEYDOWN && m[0].wparam == 0x12);
    CHECK(m[1].message == WM_SYSKEYDOWN && m[1].wparam == 0x0D && (m[1].lparam & (1u << 29)));
    translate(key(SDL_SCANCODE_RETURN, false), hwnd, st, m);
    translate(key(SDL_SCANCODE_LALT, false), hwnd, st, m);
    CHECK(m.size() == 4 && m[3].message == WM_KEYUP && !st.alt);
    m.clear();

    // text input -> one WM_CHAR per ASCII character
    SDL_Event t;
    std::memset(&t, 0, sizeof(t));
    t.type = SDL_EVENT_TEXT_INPUT;
    t.text.text = "ab~";
    translate(t, hwnd, st, m);
    CHECK(m.size() == 3 && m[0].message == WM_CHAR && m[0].wparam == 'a' && m[2].wparam == '~');
    m.clear();

    // mouse: move, left down (MK_LBUTTON in wParam), move while held, left up, wheel up one notch
    SDL_Event mv;
    std::memset(&mv, 0, sizeof(mv));
    mv.type = SDL_EVENT_MOUSE_MOTION;
    mv.motion.x = 320, mv.motion.y = 200;
    translate(mv, hwnd, st, m);
    CHECK(m.size() == 1 && m[0].message == WM_MOUSEMOVE && m[0].wparam == 0 && m[0].lparam == ((200u << 16) | 320u));
    SDL_Event b;
    std::memset(&b, 0, sizeof(b));
    b.type = SDL_EVENT_MOUSE_BUTTON_DOWN;
    b.button.button = SDL_BUTTON_LEFT;
    b.button.x = 320, b.button.y = 200;
    translate(b, hwnd, st, m);
    CHECK(m.size() == 2 && m[1].message == WM_LBUTTONDOWN && m[1].wparam == MK_LBUTTON);
    mv.motion.x = 321;
    translate(mv, hwnd, st, m);
    CHECK(m.size() == 3 && m[2].wparam == MK_LBUTTON && (m[2].lparam & 0xFFFF) == 321);
    b.type = SDL_EVENT_MOUSE_BUTTON_UP;
    translate(b, hwnd, st, m);
    CHECK(m.size() == 4 && m[3].message == WM_LBUTTONUP && m[3].wparam == 0);
    SDL_Event w;
    std::memset(&w, 0, sizeof(w));
    w.type = SDL_EVENT_MOUSE_WHEEL;
    w.wheel.y = 1.0f;
    translate(w, hwnd, st, m);
    CHECK(m.size() == 5 && m[4].message == WM_MOUSEWHEEL && (m[4].wparam >> 16) == 120);
    w.wheel.y = -1.0f;
    translate(w, hwnd, st, m);
    CHECK(m.size() == 6 && (int16_t)(m[5].wparam >> 16) == -120);
    m.clear();

    // focus and close
    SDL_Event f;
    std::memset(&f, 0, sizeof(f));
    f.type = SDL_EVENT_WINDOW_FOCUS_LOST;
    translate(f, hwnd, st, m);
    CHECK(m.size() == 3 && m[0].message == WM_KILLFOCUS && m[1].message == WM_ACTIVATE && m[1].wparam == 0);
    f.type = SDL_EVENT_WINDOW_FOCUS_GAINED;
    translate(f, hwnd, st, m);
    CHECK(m.size() == 6 && m[4].message == WM_ACTIVATE && m[4].wparam == 1 && m[5].message == WM_SETFOCUS);
    f.type = SDL_EVENT_WINDOW_CLOSE_REQUESTED;
    translate(f, hwnd, st, m);
    CHECK(m.size() == 7 && m[6].message == WM_CLOSE);

    // every mapped key has a unique (scan, extended) pair and a non-zero virtual key
    int mapped = 0;
    bool seen[2][256] = {};
    for (int sc = 0; sc < SDL_SCANCODE_COUNT; sc++) {
        KeyInfo k = key_info((SDL_Scancode)sc);
        if (!k.vk) continue;
        mapped++;
        CHECK(!seen[k.extended][k.scan]);
        seen[k.extended][k.scan] = true;
    }
    std::printf("%d keys mapped; %s\n", mapped, g_fail ? "FAILED" : "all checks passed");
    return g_fail ? 1 : 0;
}
