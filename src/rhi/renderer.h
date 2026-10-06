// Renderer interface (ADR-004): a renderer consumes the game's Direct3D 9 calls, one record at a time,
// in the order the game makes them. Producers: the trace replay tool and the native d3d9 shim.
#pragma once
#include <memory>
#include <string>

#include "d3d9_call.h"

namespace bo1::rhi {

struct Options {
    int first_frame = 0;     // frames before this are executed for state only (no draws needed)
    int last_frame = 1 << 30;
    std::string dump_dir;    // where a rendering backend writes frame images
    bool window = false;     // show every rendered frame in a window (SDL3 + CAMetalLayer)
    void *layer = nullptr;   // CAMetalLayer* of an existing window: frames are presented there and the renderer
                             // neither creates a window nor touches SDL (the native runtime owns window and events)
    int fps = 60;            // presentation rate of the window (0 = as fast as the calls arrive)
};

class Renderer {
public:
    virtual ~Renderer() = default;
    // One game-made call (not nested inside another D3D9 call). `frame` counts completed Presents.
    virtual void call(const trace::Record &r, int frame) = 0;
    // After the Present that ends `frame` (1-based).
    virtual void end_frame(int frame) = 0;
    // Human-readable summary at the end of the run; returns the number of problems found.
    virtual int report() = 0;
    // True once the user closed the window (or pressed Escape).
    virtual bool wants_quit() { return false; }
};

std::unique_ptr<Renderer> make_metal_renderer(const Options &opts); // src/metal

} // namespace bo1::rhi
