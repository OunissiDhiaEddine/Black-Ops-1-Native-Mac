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

void WindowService::cursor(int32_t *x, int32_t *y) {
    std::lock_guard<std::mutex> lk(mu_);
    *x = input_.x;
    *y = input_.y;
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
