#include "window_service.h"

#include <SDL3/SDL_metal.h>

#include <vector>

namespace bo1::win {

WindowService &WindowService::get() {
    static WindowService s;
    return s;
}

bool WindowService::run(const std::atomic<bool> &done) {
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) {
        SDL_Log("SDL_Init failed: %s", SDL_GetError());
        std::lock_guard<std::mutex> lk(mu_);
        if (create_) create_->done = true, create_->ok = false;
        cv_.notify_all();
        return false;
    }
    running_ = true;
    if (const char *path = getenv("BO1_MOUSE_LOG")) mouse_log_ = fopen(path, "w");
    std::vector<Msg> batch;
    while (!done.load()) {
        serve_requests();
        SDL_Event ev;
        if (!SDL_WaitEventTimeout(&ev, 2)) continue;
        do {
            if (ev.type == SDL_EVENT_WINDOW_FOCUS_GAINED) focused_ = true;
            if (ev.type == SDL_EVENT_WINDOW_FOCUS_LOST) focused_ = false;
            // game controller: the first one that shows up is "controller 0"; a later one takes over when it leaves
            if (ev.type == SDL_EVENT_GAMEPAD_ADDED && !pad_.load()) {
                SDL_Gamepad *g = SDL_OpenGamepad(ev.gdevice.which);
                pad_ = g;
                SDL_Log("gamepad: %s connected", g ? SDL_GetGamepadName(g) : "(could not be opened)");
            } else if (ev.type == SDL_EVENT_GAMEPAD_REMOVED && pad_.load() && SDL_GetGamepadID(pad_.load()) == ev.gdevice.which) {
                SDL_Log("gamepad: disconnected");
                SDL_CloseGamepad(pad_.exchange(nullptr));
            } else if (ev.type >= SDL_EVENT_GAMEPAD_AXIS_MOTION && ev.type <= SDL_EVENT_GAMEPAD_BUTTON_UP) {
                pad_packet_++;
            }
            batch.clear();
            {
                std::lock_guard<std::mutex> lk(mu_);
                if (ev.type == SDL_EVENT_MOUSE_MOTION) {
                    if (input_.relative) look_events_++, look_dx_ += ev.motion.xrel, look_dy_ += ev.motion.yrel;
                    if (mouse_log_)
                        fprintf(mouse_log_, "%llu motion %.3f %.3f %.1f %.1f %d\n", (unsigned long long)SDL_GetTicks(), ev.motion.xrel, ev.motion.yrel,
                                ev.motion.x, ev.motion.y, input_.relative ? 1 : 0);
                }
                if (ev.type == SDL_EVENT_MOUSE_MOTION) to_game(&ev.motion.x, &ev.motion.y);
                else if (ev.type == SDL_EVENT_MOUSE_BUTTON_DOWN || ev.type == SDL_EVENT_MOUSE_BUTTON_UP) to_game(&ev.button.x, &ev.button.y);
                else if (ev.type == SDL_EVENT_MOUSE_WHEEL) to_game(&ev.wheel.mouse_x, &ev.wheel.mouse_y);
                translate(ev, hwnd_, input_, batch);
                for (const Msg &m : batch) queue_.push_back(m);
            }
        } while (SDL_PollEvent(&ev));
    }
    running_ = false;
    return true;
}

void WindowService::serve_requests() {
    std::unique_lock<std::mutex> lk(mu_);
    if (title_pending_ && window_) {
        SDL_SetWindowTitle(window_, new_title_.c_str());
        title_pending_ = false;
    }
    if (disp_pending_ && window_) {
        SDL_Window *w = window_;
        int dw = disp_w_, dh = disp_h_;
        bool full = disp_full_;
        disp_pending_ = false;
        lk.unlock();
        bool was = (SDL_GetWindowFlags(w) & SDL_WINDOW_FULLSCREEN) != 0;
        if (was != full) {
            SDL_SetWindowFullscreen(w, full); // no exclusive mode: the desktop's own full screen
            SDL_SyncWindow(w);                // the switch is animated; the size below needs it finished
        }
        if (!full) SDL_SetWindowSize(w, dw, dh);
        SDL_Log("window: display mode %dx%d %s", dw, dh, full ? "full screen" : "windowed");
        lk.lock();
    }
    if (window_) { // mouse look: follow what the game's SetCursorPos calls ask for
        bool want = input_.relative && focused_ &&
                    std::chrono::steady_clock::now() - last_warp_ < std::chrono::milliseconds(kRelativeHoldMs);
        if (!want) input_.relative = false;
        if (want != relative_on_) {
            SDL_Window *w = window_;
            float x = (float)input_.x, y = (float)input_.y;
            int ww = 0, wh = 0;
            if (disp_w_ && SDL_GetWindowSize(w, &ww, &wh) && ww > 0 && wh > 0) x = x * (float)ww / (float)disp_w_, y = y * (float)wh / (float)disp_h_;
            lk.unlock();
            bool ok = SDL_SetWindowRelativeMouseMode(w, want);
            if (!want) SDL_WarpMouseInWindow(w, x, y); // the pointer reappears where the game last put its cursor
            lk.lock();
            relative_on_ = want;
            // one line per switch in the run log: how much the hand moved while the game was looking around
            if (want) {
                SDL_Log("mouse look: relative mouse mode on%s", ok ? "" : " (SDL refused)");
                look_events_ = look_warps_ = 0, look_dx_ = look_dy_ = 0;
            } else {
                SDL_Log("mouse look: relative mouse mode off after %llu re-centre calls, %llu motion events, total movement %.0f / %.0f",
                        (unsigned long long)look_warps_, (unsigned long long)look_events_, look_dx_, look_dy_);
            }
            if (mouse_log_) fprintf(mouse_log_, "%llu relative %d\n", (unsigned long long)SDL_GetTicks(), want ? 1 : 0), fflush(mouse_log_);
        }
    }
    if (!create_ || create_->done) return;
    CreateRequest *rq = create_;
    lk.unlock();
    SDL_Window *w = SDL_CreateWindow(rq->title.c_str(), rq->w, rq->h, SDL_WINDOW_METAL);
    SDL_MetalView view = w ? SDL_Metal_CreateView(w) : nullptr;
    if (w) SDL_StartTextInput(w); // WM_CHAR comes from text-input events
    lk.lock();
    window_ = w;
    layer_ = view ? SDL_Metal_GetLayer(view) : nullptr;
    hwnd_ = rq->hwnd;
    rq->ok = layer_ != nullptr;
    rq->done = true;
    cv_.notify_all();
}

bool WindowService::create(uint32_t hwnd, const std::string &title, int w, int h) {
    CreateRequest rq{hwnd, title, w, h, false, false};
    std::unique_lock<std::mutex> lk(mu_);
    if (window_) return hwnd == hwnd_; // one game window
    create_ = &rq;
    cv_.wait(lk, [&] { return rq.done; });
    create_ = nullptr;
    return rq.ok;
}

void WindowService::set_title(const std::string &title) {
    std::lock_guard<std::mutex> lk(mu_);
    new_title_ = title;
    title_pending_ = true;
}

void WindowService::set_display(int w, int h, bool fullscreen) {
    if (w <= 0 || h <= 0) return;
    std::lock_guard<std::mutex> lk(mu_);
    disp_w_ = w, disp_h_ = h, disp_full_ = fullscreen, disp_pending_ = true;
}

void WindowService::to_game(float *x, float *y) {
    int ww = 0, wh = 0;
    if (!disp_w_ || !window_ || !SDL_GetWindowSize(window_, &ww, &wh) || ww <= 0 || wh <= 0) return;
    *x = *x * (float)disp_w_ / (float)ww;
    *y = *y * (float)disp_h_ / (float)wh;
}

void WindowService::cursor(int32_t *x, int32_t *y) {
    std::lock_guard<std::mutex> lk(mu_);
    *x = input_.x;
    *y = input_.y;
}

bool WindowService::gamepad(PadState *out) {
    SDL_Gamepad *g = pad_.load();
    if (!g) return false;
    static const struct { SDL_GamepadButton b; uint16_t bit; } kMap[] = {
        {SDL_GAMEPAD_BUTTON_DPAD_UP, 0x0001}, {SDL_GAMEPAD_BUTTON_DPAD_DOWN, 0x0002}, {SDL_GAMEPAD_BUTTON_DPAD_LEFT, 0x0004},
        {SDL_GAMEPAD_BUTTON_DPAD_RIGHT, 0x0008}, {SDL_GAMEPAD_BUTTON_START, 0x0010}, {SDL_GAMEPAD_BUTTON_BACK, 0x0020},
        {SDL_GAMEPAD_BUTTON_LEFT_STICK, 0x0040}, {SDL_GAMEPAD_BUTTON_RIGHT_STICK, 0x0080}, {SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, 0x0100},
        {SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, 0x0200}, {SDL_GAMEPAD_BUTTON_SOUTH, 0x1000}, {SDL_GAMEPAD_BUTTON_EAST, 0x2000},
        {SDL_GAMEPAD_BUTTON_WEST, 0x4000}, {SDL_GAMEPAD_BUTTON_NORTH, 0x8000},
    };
    uint16_t buttons = 0;
    for (const auto &m : kMap)
        if (SDL_GetGamepadButton(g, m.b)) buttons |= m.bit;
    auto flip = [](int16_t v) { return (int16_t)(v == -32768 ? 32767 : -v); }; // SDL: down is positive; XInput: up
    out->packet = pad_packet_.load();
    out->buttons = buttons;
    out->left_trigger = (uint8_t)(SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) >> 7);
    out->right_trigger = (uint8_t)(SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) >> 7);
    out->lx = SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_LEFTX);
    out->ly = flip(SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_LEFTY));
    out->rx = SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_RIGHTX);
    out->ry = flip(SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_RIGHTY));
    return true;
}

void WindowService::rumble(uint16_t low, uint16_t high) {
    if (SDL_Gamepad *g = pad_.load()) SDL_RumbleGamepad(g, low, high, 1000);
}

void WindowService::warp(int32_t x, int32_t y) {
    std::lock_guard<std::mutex> lk(mu_);
    input_.x = x;
    input_.y = y; // the fractions in fx / fy stay: the game calls this every frame and slow movement lives in them
    look_warps_++;
    if (mouse_log_) fprintf(mouse_log_, "%llu warp %d %d\n", (unsigned long long)SDL_GetTicks(), x, y);
    last_warp_ = std::chrono::steady_clock::now();
    if (focused_ || !window_) input_.relative = true; // without a window (tests, scripted runs) the cursor is always virtual
}

bool WindowService::peek(Msg *out, bool remove) {
    std::lock_guard<std::mutex> lk(mu_);
    if (queue_.empty()) return false;
    *out = queue_.front();
    if (remove) queue_.pop_front();
    return true;
}

void WindowService::post(const Msg &m) {
    std::lock_guard<std::mutex> lk(mu_);
    queue_.push_back(m);
}

size_t WindowService::pending() {
    std::lock_guard<std::mutex> lk(mu_);
    return queue_.size();
}

} // namespace bo1::win
