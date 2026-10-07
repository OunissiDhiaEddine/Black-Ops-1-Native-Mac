// The game's window on macOS (T-052 part 2, ADR-006). AppKit requires windows and the event pump on the
// process main thread, while the game's "main" thread is an ordinary guest thread. So the process main
// thread runs WindowService::run() and every other thread talks to it through this interface:
// window creation is a blocking request, input arrives as Win32 messages in a thread-safe queue.
#pragma once
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <mutex>
#include <string>

#include "win_messages.h"

namespace bo1::win {

class WindowService {
public:
    static WindowService &get();

    // ---- process main thread ----
    // Initialises SDL video and serves requests / pumps events until `done` becomes true. False if SDL failed.
    bool run(const std::atomic<bool> &done);

    // ---- any thread ----
    // Creates the game window (one per process) with a w x h client area; blocks until it exists.
    bool create(uint32_t hwnd, const std::string &title, int w, int h);
    void set_title(const std::string &title);
    // New client-area size (the game's SetWindowPos after a display-mode change); applied by the main thread.
    void resize(int w, int h);
    uint32_t hwnd() const { return hwnd_; } // the guest window the real one stands for (0 = none)
    void *metal_layer() const { return layer_; } // CAMetalLayer*, valid after create()
    bool has_window() const { return window_ != nullptr; }
    // enable(): the game's message queue is fed from here (set before the guest starts).
    // enable(true): in addition a main thread is going to call run(), so create() can be served.
    void enable(bool with_window = false) { enabled_ = true, wants_window_ = with_window; }
    bool enabled() const { return enabled_; }
    bool wants_window() const { return wants_window_; }
    bool focused() const { return focused_; }
    void cursor(int32_t *x, int32_t *y);
    // SetCursorPos in client coordinates. The game does this every frame while it uses the mouse to look
    // around, so it also switches the window to relative mouse mode (pointer hidden and held, movement
    // reported as device deltas); the mode ends when the calls stop for kRelativeHoldMs.
    void warp(int32_t x, int32_t y);
    bool relative_mouse() const { return relative_on_; }
    static constexpr int kRelativeHoldMs = 500;
    // PeekMessage semantics: copies the oldest message; removes it when `remove` is set.
    bool peek(Msg *out, bool remove);
    void post(const Msg &m);
    size_t pending();

private:
    void serve_requests();
    struct CreateRequest {
        uint32_t hwnd;
        std::string title;
        int w, h;
        bool done, ok;
    };
    std::mutex mu_;
    std::condition_variable cv_;
    CreateRequest *create_ = nullptr;
    std::string new_title_;
    bool title_pending_ = false;
    int new_w_ = 0, new_h_ = 0; // pending resize() (0 = none)
    std::deque<Msg> queue_;
    SDL_Window *window_ = nullptr;
    void *layer_ = nullptr;
    uint32_t hwnd_ = 0;
    InputState input_;
    std::chrono::steady_clock::time_point last_warp_{};
    std::atomic<bool> relative_on_{false}; // what SDL was last told (main thread writes)
    // BO1_MOUSE_LOG=file: every mouse motion event and every SetCursorPos, for judging mouse look from a real
    // hand ("ms kind a b ..." per line; kinds: motion, warp, relative)
    FILE *mouse_log_ = nullptr;
    uint64_t look_events_ = 0, look_warps_ = 0;
    double look_dx_ = 0, look_dy_ = 0;
    std::atomic<bool> focused_{false}, running_{false}, enabled_{false}, wants_window_{false};
};

} // namespace bo1::win
