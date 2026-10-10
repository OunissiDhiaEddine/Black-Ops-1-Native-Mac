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
    bool precompile = false; // compile a shader in the background when the game creates it (level load), so that
                             // its first use in a frame does not wait for the compiler (T-058: ~250 ms each, cold)
    bool cache_created = false; // shader cache: also note the source of every shader the game CREATES (a level's
                                // ~6,000 at load), so that scripts/shader_cache.sh can compile them before their first use
    int fps = 60;            // presentation rate of the window (0 = as fast as the calls arrive)
};

class Renderer {
public:
    virtual ~Renderer() = default;
    // One game-made call (not nested inside another D3D9 call). `frame` counts completed Presents.
    virtual void call(const trace::Record &r, int frame) = 0;
    // After the Present that ends `frame` (1-based).
    virtual void end_frame(int frame) = 0;

    // ---- draw submission (P6) ----------------------------------------------------------------------------
    // The calls a frame consists of (20,000-31,000 per frame in a fight), typed: each means what the Direct3D 9
    // device method of the same name means, without a record around it. `call()` decodes a record of one of
    // these into the same function, so the trace replay and the live game run the same code. Object arguments
    // are object ids, as in a record. A renderer that does not draw may leave them empty.
    virtual void set_render_state(uint32_t /*state*/, uint32_t /*value*/) {}
    virtual void set_texture(uint32_t /*stage*/, uint32_t /*texture*/) {}
    virtual void set_sampler_state(uint32_t /*stage*/, uint32_t /*type*/, uint32_t /*value*/) {}
    virtual void set_vertex_declaration(uint32_t /*decl*/) {}
    virtual void set_shader(bool /*vertex*/, uint32_t /*shader*/) {}
    // `count` float4 registers from register `start`.
    virtual void set_shader_constants(bool /*vertex*/, uint32_t /*start*/, const void * /*data*/, uint32_t /*count*/) {}
    virtual void set_stream_source(uint32_t /*stream*/, uint32_t /*buffer*/, uint32_t /*offset*/, uint32_t /*stride*/) {}
    virtual void set_indices(uint32_t /*buffer*/) {}
    // DrawIndexedPrimitive(Type, BaseVertexIndex, MinVertexIndex, NumVertices, StartIndex, PrimitiveCount)
    virtual void draw_indexed(uint32_t /*type*/, int32_t /*base_vertex*/, uint32_t /*min_index*/, uint32_t /*num_vertices*/,
                              uint32_t /*start_index*/, uint32_t /*prim_count*/) {}
    // Unlock of a vertex / index buffer that was locked with `lock_flags` at `offset`: `size` bytes the game
    // wrote there (`data` may be null: a read-only lock, or nothing to hand over).
    virtual void write_buffer(uint32_t /*buffer*/, uint32_t /*lock_flags*/, uint32_t /*offset*/, const void * /*data*/, uint32_t /*size*/) {}

    // Human-readable summary at the end of the run; returns the number of problems found.
    virtual int report() = 0;
    // Running totals of the work that makes single frames slow (T-058): nanoseconds spent translating shaders,
    // compiling MSL, creating pipelines, waiting for the GPU, uploading textures, waiting for a drawable; then counts of shader
    // functions compiled, pipelines created, texture uploads and bytes uploaded.
    struct Work {
        uint64_t ns_translate = 0, ns_compile = 0, ns_pipeline = 0, ns_gpu_wait = 0, ns_upload = 0;
        uint64_t ns_drawable = 0; // waiting for the window's next drawable (vsync / compositor)
        uint64_t ns_gpu = 0;      // time the GPU spent executing the command buffers that have completed (P7)
        uint64_t functions = 0, pipelines = 0, uploads = 0, upload_bytes = 0;
        uint64_t precompiled = 0; // of `functions`: compiled in the background before their first use
    };
    virtual Work work() { return {}; }
    // True once the user closed the window (or pressed Escape).
    virtual bool wants_quit() { return false; }
};

std::unique_ptr<Renderer> make_metal_renderer(const Options &opts); // src/metal

} // namespace bo1::rhi
