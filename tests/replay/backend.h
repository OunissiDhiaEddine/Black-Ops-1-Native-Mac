// Replay backend interface (T-031). A backend consumes the game's D3D9 call stream.
// The null backend only validates; the Metal backend (T-032..T-034) renders.
#pragma once
#include <cstdint>
#include <memory>
#include <string>

#include "trace.h"

namespace bo1::replay {

struct Options {
    int first_frame = 0;     // frames before this are replayed for state only (no draws needed)
    int last_frame = 1 << 30;
    std::string dump_dir;    // where a rendering backend writes frame images
    bool window = false;     // show every rendered frame in a window (SDL3 + CAMetalLayer)
    int fps = 60;            // presentation rate of the window (0 = as fast as the replay goes)
};

class Backend {
public:
    virtual ~Backend() = default;
    // One game-made call (depth == 0), in trace order. `frame` counts completed Presents.
    virtual void call(const trace::Record &r, int frame) = 0;
    // After the Present record that ends `frame` (1-based).
    virtual void end_frame(int frame) = 0;
    // Human-readable summary at the end of the run; returns the number of problems found.
    virtual int report() = 0;
    // True once the user closed the window (or pressed Escape); the replay loop stops.
    virtual bool wants_quit() { return false; }
};

std::unique_ptr<Backend> make_null_backend(const Options &opts);
std::unique_ptr<Backend> make_metal_backend(const Options &opts); // offscreen Metal renderer

} // namespace bo1::replay
