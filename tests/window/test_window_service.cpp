// Integration test for the window service (T-052 part 2): the process main thread runs the service, a
// worker thread plays the game's role — creates the window, receives input as Win32 messages. Input is
// synthetic (SDL_PushEvent), so the test needs a desktop session but no user. Opens a window for ~1 s.
#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>

#include "window_service.h"

using namespace bo1::win;

int main() {
    std::atomic<bool> done{false};
    int fail = 0;
    std::thread game([&] {
        WindowService &ws = WindowService::get();
        auto check = [&](bool ok, const char *what) {
            if (!ok) std::printf("FAIL: %s\n", what), fail++;
        };
        check(ws.create(0x10100, "BO1 window service test", 640, 480), "window + Metal layer created from a non-main thread");
        check(ws.metal_layer() != nullptr, "CAMetalLayer available");
        Msg m;
        while (ws.peek(&m, true)) {} // drop the focus/size messages of window creation
        SDL_Event e;
        std::memset(&e, 0, sizeof(e));
        e.type = SDL_EVENT_KEY_DOWN;
        e.key.scancode = SDL_SCANCODE_SPACE;
        SDL_PushEvent(&e);
        e.type = SDL_EVENT_KEY_UP;
        SDL_PushEvent(&e);
        std::memset(&e, 0, sizeof(e));
        e.type = SDL_EVENT_MOUSE_MOTION;
        e.motion.x = 100, e.motion.y = 50;
        SDL_PushEvent(&e);
        // wait (bounded) for the three messages to come through the main thread
        bool down = false, up = false, move = false;
        for (int i = 0; i < 500 && !(down && up && move); i++) {
            while (ws.peek(&m, true)) {
                if (m.message == WM_KEYDOWN && m.wparam == 0x20 && m.hwnd == 0x10100) down = true;
                if (m.message == WM_KEYUP && m.wparam == 0x20) up = true;
                if (m.message == WM_MOUSEMOVE && (m.lparam & 0xFFFF) == 100 && (m.lparam >> 16) == 50) move = true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        check(down, "WM_KEYDOWN VK_SPACE delivered to the worker thread");
        check(up, "WM_KEYUP VK_SPACE delivered");
        check(move, "WM_MOUSEMOVE (100, 50) delivered");
        int32_t x, y;
        ws.cursor(&x, &y);
        check(x == 100 && y == 50, "cursor position tracked");
        ws.set_title("BO1 window service test — done");
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        done = true;
    });
    bool ran = WindowService::get().run(done);
    game.join();
    if (!ran) std::printf("FAIL: SDL video did not initialise\n"), fail++;
    std::printf("%s\n", fail ? "FAILED" : "all checks passed");
    SDL_Quit();
    return fail ? 1 : 0;
}
