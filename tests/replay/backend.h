// Replay backends (T-031). The rendering backend is the shared Metal renderer (src/rhi, src/metal);
// the null backend only validates the call stream.
#pragma once
#include <memory>

#include "renderer.h"
#include "trace.h"

namespace bo1::replay {

using Options = rhi::Options;
using Backend = rhi::Renderer;

std::unique_ptr<Backend> make_null_backend(const Options &opts);
inline std::unique_ptr<Backend> make_metal_backend(const Options &opts) { return rhi::make_metal_renderer(opts); }

} // namespace bo1::replay
