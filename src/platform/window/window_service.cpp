#include "window_service.h"

#include <SDL3/SDL_metal.h>

#include <vector>

namespace bo1::win {

WindowService &WindowService::get() {
    static WindowService s;
    return s;
}

bool WindowService::run(const std::atomic<bool> &done) {
    if (!SDL_Init(SDL_INIT_VIDEO)) {
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
            batch.clear();
            {
                std::lock_guard<std::mutex> lk(mu_);
                if (ev.type == SDL_EVENT_MOUSE_MOTION) {
                    if (input_.relative) look_events_++, look_dx_ += ev.motion.xrel, look_dy_ += ev.motion.yrel;
                    if (mouse_log_)
                        fprintf(mouse_log_, "%llu motion %.3f %.3f %.1f %.1f %d\n", (unsigned long long)SDL_GetTicks(), ev.motion.xrel, ev.motion.yrel,
                                ev.motion.x, ev.motion.y, input_.relative ? 1 : 0);
                }
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
    if (new_w_ && window_) {
        SDL_Window *w = window_;
        int nw = new_w_, nh = new_h_;
        new_w_ = new_h_ = 0;
        lk.unlock();
        SDL_SetWindowSize(w, nw, nh);
        lk.lock();
    }
    if (window_) { // mouse look: follow what the game's SetCursorPos calls ask for
        bool want = input_.relative && focused_ &&
                    std::chrono::steady_clock::now() - last_warp_ < std::chrono::milliseconds(kRelativeHoldMs);
        if (!want) input_.relative = false;
        if (want != relative_on_) {
            SDL_Window *w = window_;
            float x = (float)input_.x, y = (float)input_.y;
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

void WindowService::resize(int w, int h) {
    std::lock_guard<std::mutex> lk(mu_);
    new_w_ = w, new_h_ = h;
}

void WindowService::cursor(int32_t *x, int32_t *y) {
    std::lock_guard<std::mutex> lk(mu_);
    *x = input_.x;
    *y = input_.y;
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
