// Metal renderer (T-032 / T-034, library since T-051): executes the game's D3D9 calls on Metal and dumps
// the back buffer for frames inside the window. D3D9 semantics live here (ADR-004 proposal a):
// state is tracked exactly as the device would, and translated to Metal objects at draw time.
//
// Draw submission (P6): the calls a frame consists of arrive typed (renderer.h, "draw submission"), device
// state lives in flat arrays, and a draw tells the encoder only what differs from what it was last told
// (`Applied`). Hazards (a resource rewritten after a draw used it) are still resolved by flushing.
#import <CommonCrypto/CommonDigest.h>
#import <CoreGraphics/CoreGraphics.h>
#import <Foundation/Foundation.h>
#import <ImageIO/ImageIO.h>
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#include <initializer_list>
#include <SDL3/SDL.h>
#include <SDL3/SDL_metal.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <array>
#include <atomic>
#include <cstdarg>
#include <cstring>
#include <condition_variable>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

#include "renderer.h"
#include "sm3.h"

namespace bo1::rhi {
using namespace trace;

namespace {

constexpr uint32_t FOURCC(char a, char b, char c, char d) {
    return (uint32_t)(uint8_t)a | ((uint32_t)(uint8_t)b << 8) | ((uint32_t)(uint8_t)c << 16) | ((uint32_t)(uint8_t)d << 24);
}
enum : uint32_t {
    FMT_R8G8B8 = 20, FMT_A8R8G8B8 = 21, FMT_X8R8G8B8 = 22, FMT_R5G6B5 = 23, FMT_A8 = 28, FMT_G16R16 = 34,
    FMT_A16B16G16R16 = 36, FMT_L8 = 50, FMT_A8L8 = 51, FMT_D24S8 = 75, FMT_D24X8 = 77, FMT_D16 = 80,
    FMT_D24FS8 = 83, FMT_INDEX16 = 101, FMT_INDEX32 = 102, FMT_R32F = 114, FMT_A16B16G16R16F = 113,
    FMT_DXT1 = FOURCC('D', 'X', 'T', '1'), FMT_DXT2 = FOURCC('D', 'X', 'T', '2'), FMT_DXT3 = FOURCC('D', 'X', 'T', '3'),
    FMT_DXT4 = FOURCC('D', 'X', 'T', '4'), FMT_DXT5 = FOURCC('D', 'X', 'T', '5'), FMT_NULL = FOURCC('N', 'U', 'L', 'L'),
};
bool is_depth(uint32_t f) { return f == FMT_D24S8 || f == FMT_D24X8 || f == FMT_D16 || f == FMT_D24FS8 || f == 71 || f == 82; }

struct FormatInfo {
    MTLPixelFormat mtl = MTLPixelFormatInvalid;
    MTLTextureSwizzleChannels swizzle = {MTLTextureSwizzleRed, MTLTextureSwizzleGreen, MTLTextureSwizzleBlue, MTLTextureSwizzleAlpha};
    bool has_swizzle = false;
};
FormatInfo format_info(uint32_t f) {
    FormatInfo i;
    auto sw = [&](MTLTextureSwizzle r, MTLTextureSwizzle g, MTLTextureSwizzle b, MTLTextureSwizzle a) {
        i.swizzle = {r, g, b, a};
        i.has_swizzle = true;
    };
    switch (f) {
    case FMT_A8R8G8B8: i.mtl = MTLPixelFormatBGRA8Unorm; break;
    case FMT_X8R8G8B8: i.mtl = MTLPixelFormatBGRA8Unorm; sw(MTLTextureSwizzleRed, MTLTextureSwizzleGreen, MTLTextureSwizzleBlue, MTLTextureSwizzleOne); break;
    case FMT_R5G6B5: i.mtl = MTLPixelFormatB5G6R5Unorm; break;
    case FMT_A8: i.mtl = MTLPixelFormatA8Unorm; break;
    case FMT_L8: i.mtl = MTLPixelFormatR8Unorm; sw(MTLTextureSwizzleRed, MTLTextureSwizzleRed, MTLTextureSwizzleRed, MTLTextureSwizzleOne); break;
    case FMT_A8L8: i.mtl = MTLPixelFormatRG8Unorm; sw(MTLTextureSwizzleRed, MTLTextureSwizzleRed, MTLTextureSwizzleRed, MTLTextureSwizzleGreen); break;
    case FMT_G16R16: i.mtl = MTLPixelFormatRG16Unorm; sw(MTLTextureSwizzleRed, MTLTextureSwizzleGreen, MTLTextureSwizzleOne, MTLTextureSwizzleOne); break;
    case FMT_A16B16G16R16: i.mtl = MTLPixelFormatRGBA16Unorm; break;
    case FMT_A16B16G16R16F: i.mtl = MTLPixelFormatRGBA16Float; break;
    case FMT_R32F: i.mtl = MTLPixelFormatR32Float; sw(MTLTextureSwizzleRed, MTLTextureSwizzleOne, MTLTextureSwizzleOne, MTLTextureSwizzleOne); break;
    case FMT_DXT1: i.mtl = MTLPixelFormatBC1_RGBA; break;
    case FMT_DXT2: case FMT_DXT3: i.mtl = MTLPixelFormatBC2_RGBA; break;
    case FMT_DXT4: case FMT_DXT5: i.mtl = MTLPixelFormatBC3_RGBA; break;
    case FMT_D24S8: case FMT_D24X8: case FMT_D24FS8: case FMT_D16: case 71: case 82: i.mtl = MTLPixelFormatDepth32Float_Stencil8; break;
    default: break;
    }
    return i;
}

struct Tex {
    id<MTLTexture> tex = nil;
    id<MTLTexture> view = nil; // what shaders sample: `tex`, or a swizzled view of a render target
    uint32_t fmt = 0, w = 0, h = 0, d = 1, levels = 1;
    bool cube = false, volume = false, depth = false, null_target = false;
    MTLTextureType type() const { return cube ? MTLTextureTypeCube : volume ? MTLTextureType3D : MTLTextureType2D; }
    bool unrecorded = false; // the game's latest write to it is not in the trace (LockDesc.dumped == 0 or 3)
    uint64_t used_gen = 0;
};
struct Surface { // a 2D slice of a texture (or the whole of a standalone surface)
    uint32_t tex = 0, level = 0, face = 0;
};
struct Buf {
    id<MTLBuffer> buf = nil;
    uint32_t size = 0, index_fmt = FMT_INDEX16;
    uint64_t used_gen = 0;
    // Byte ranges the game wrote but the trace did not record (LockDesc.dumped == 3), sorted, disjoint.
    std::vector<std::pair<uint32_t, uint32_t>> unrecorded;
    void mark(uint32_t a, uint32_t b, bool missing) { // [a, b) becomes missing / present
        std::vector<std::pair<uint32_t, uint32_t>> out;
        for (auto [x, y] : unrecorded) {
            if (y <= a || x >= b) { out.push_back({x, y}); continue; }
            if (x < a) out.push_back({x, a});
            if (y > b) out.push_back({b, y});
        }
        if (missing && b > a) out.push_back({a, b});
        std::sort(out.begin(), out.end());
        unrecorded = std::move(out);
    }
    bool missing(uint64_t a, uint64_t b) const {
        for (auto [x, y] : unrecorded)
            if (x < b && y > a) return true;
        return false;
    }
};
struct Shader {
    int created_frame = 0;
    std::vector<uint32_t> code;
    bool vs = false, failed = false;
    sm3::Result meta; // from the first translation (inputs, samplers)
    bool have_meta = false;
    std::map<uint32_t, id<MTLFunction>> fn; // by shadow-sampler mask
};
struct Elem {
    uint16_t stream, offset;
    uint8_t type, method, usage, uidx;
};
struct Stream {
    uint32_t vb = 0, offset = 0, stride = 0;
};
// Key of the state-object caches (pipelines, depth-stencil states, samplers): the values themselves, hashed in
// place (text keys were 18% of the render thread in the first profile, heap-allocated ones ~5% later; T-058, P6).
template <size_t N> struct Key {
    uint64_t v[N];
    bool operator==(const Key &o) const { return !memcmp(v, o.v, sizeof(v)); }
};
template <size_t N> struct KeyHash {
    size_t operator()(const Key<N> &k) const {
        uint64_t h = 0x9E3779B97F4A7C15ull;
        for (uint64_t x : k.v) h = (h ^ x) * 0x100000001B3ull, h ^= h >> 29;
        return (size_t)h;
    }
};

const char *kInternalShaders = R"(
#include <metal_stdlib>
using namespace metal;
struct ClearU { float4 color; float depth; };
struct VO { float4 pos [[position]]; float2 uv; };
vertex VO fs_vs(uint vid [[vertex_id]], constant ClearU &u [[buffer(0)]]) {
    float2 p = float2((vid << 1) & 2, vid & 2);
    VO o; o.pos = float4(p * 2.0 - 1.0, u.depth, 1.0); o.uv = float2(p.x, 1.0 - p.y); return o;
}
fragment float4 clear_fs(VO in [[stage_in]], constant ClearU &u [[buffer(0)]]) { return u.color; }
fragment float4 blit_fs(VO in [[stage_in]], texture2d<float> t [[texture(0)]], sampler s [[sampler(0)]]) { return t.sample(s, in.uv); }
)";

struct Consts { // must match the translator's `Consts`
    float c[sm3::kMaxFloatConsts][4];
    int32_t i[16][4];
    uint32_t b[16];
    float extra[4];
};

class MetalRenderer final : public Renderer {
public:
    explicit MetalRenderer(const Options &o) : opts_(o) {
        dev_ = MTLCreateSystemDefaultDevice();
        if (!dev_) throw std::runtime_error("no Metal device");
        queue_ = [dev_ newCommandQueue];
        NSError *err = nil;
        internal_ = [dev_ newLibraryWithSource:@(kInternalShaders) options:nil error:&err];
        if (!internal_) throw std::runtime_error(std::string("internal shaders: ") + err.localizedDescription.UTF8String);
        dummy_vb_ = [dev_ newBufferWithLength:64 options:MTLResourceStorageModeShared];
        float one[4] = {0, 0, 0, 1};
        memcpy(dummy_vb_.contents, one, sizeof(one));
        for (unsigned st : {27, 19, 20, 171, 206, 207, 208, 209, 168}) rs_dirty_[st] |= kDirtyPipeline;        // blending, write mask
        for (unsigned st : {7, 14, 23, 52, 53, 54, 55, 56, 58, 59, 185, 186, 187, 188, 189}) rs_dirty_[st] |= kDirtyDepth; // depth / stencil
        rs_dirty_[174] |= kDirtyViewport;                                                                       // SCISSORTESTENABLE
        reset_state();
        // Shader cache (P7): BO1_SHADER_CACHE=dir | off; by default artifacts/shader_cache when run from the repository.
        const char *sc = getenv("BO1_SHADER_CACHE");
        if (sc ? strcmp(sc, "off") != 0 : access("artifacts", W_OK) == 0) {
            cache_dir_ = sc ? sc : "artifacts/shader_cache";
            [NSFileManager.defaultManager createDirectoryAtPath:@(cache_dir_.c_str()) withIntermediateDirectories:YES attributes:nil error:nil];
        }
        if (!opts_.dump_dir.empty())
            [NSFileManager.defaultManager createDirectoryAtPath:@(opts_.dump_dir.c_str()) withIntermediateDirectories:YES attributes:nil error:nil];
        // Debugging aid (T-056): BO1_DEBUG_FRAME=N BO1_DEBUG_DIR=dir dumps every pass of frame N (see debug_dump).
        const char *df = getenv("BO1_DEBUG_FRAME"), *dd = getenv("BO1_DEBUG_DIR");
        if (df && dd) {
            debug_frame_ = atoi(df);
            debug_dir_ = dd;
            [NSFileManager.defaultManager createDirectoryAtPath:@(dd) withIntermediateDirectories:YES attributes:nil error:nil];
            debug_log_ = fopen((debug_dir_ + "/passes.txt").c_str(), "w");
        }
    }
    ~MetalRenderer() override {
        if (debug_log_) fclose(debug_log_);
    }

    void call(const Record &r, int frame) override {
        frame_ = frame + 1; // frame being built
        @autoreleasepool {
            switch (r.iface) {
            case IFACE_D3D9: if (r.slot == 16) create_device(r); break;
            case IFACE_DEVICE: device_call(r); break;
            case IFACE_SWAPCHAIN: if (r.slot == 5) out_surface(r, kBackBufferTex, 0, 0); break;
            case IFACE_TEXTURE:
                if (r.slot == 18) out_surface(r, r.arg(0), r.arg(1), 0);
                else if (r.slot == 20) upload_texture(r, r.arg(0), r.arg(1), 0);
                else if (r.slot == 2 && released(r, textures_)) tex_dirty_ = 0xFFFF;
                break;
            case IFACE_CUBE:
                if (r.slot == 18) out_surface(r, r.arg(0), r.arg(2), r.arg(1));
                else if (r.slot == 20) upload_texture(r, r.arg(0), r.arg(2), r.arg(1));
                else if (r.slot == 2 && released(r, textures_)) tex_dirty_ = 0xFFFF;
                break;
            case IFACE_VOLUME:
                if (r.slot == 20) upload_texture(r, r.arg(0), r.arg(1), 0);
                else if (r.slot == 2 && released(r, textures_)) tex_dirty_ = 0xFFFF;
                break;
            case IFACE_VB: case IFACE_IB:
                if (r.slot == 12) upload_buffer(r);
                else if (r.slot == 2 && released(r, buffers_)) buffers_changed();
                break;
            case IFACE_SURFACE:
                if (r.slot == 14) {
                    auto it = surfaces_.find(r.arg(0));
                    if (it != surfaces_.end()) upload_texture(r, it->second.tex, it->second.level, it->second.face);
                } else if (r.slot == 2 && r.has_result() && r.result == 0) {
                    auto it = surfaces_.find(r.arg(0));
                    if (it != surfaces_.end() && it->second.tex == r.arg(0)) textures_.erase(r.arg(0)), tex_dirty_ = 0xFFFF; // standalone
                    surfaces_.erase(r.arg(0));
                }
                break;
            case IFACE_VSHADER: case IFACE_PSHADER: if (r.slot == 2 && released(r, shaders_)) drop_pipelines(r.arg(0)); break;
            case IFACE_VDECL: if (r.slot == 2 && released(r, decls_)) drop_pipelines(r.arg(0)); break;
            default: break;
            }
        }
    }

    void end_frame(int frame) override {
        frame_ = frame + 1; // typed calls carry no frame number: they belong to the frame after the last Present
        @autoreleasepool {
            if (rendering(frame)) {
                // The frame's work is submitted; the CPU only waits for the GPU when it reads the picture back
                // (a dump), otherwise for the PREVIOUS frame, so that at most one frame is queued (waiting for
                // this one cost 21% of the render thread in the first profile, T-058).
                bool dump = !opts_.dump_dir.empty() && frame >= opts_.first_frame && frame % 20 == 0;
                id<MTLCommandBuffer> mine = cmd_;
                flush(dump);
                if (!dump && prev_frame_cmd_) {
                    auto t0 = std::chrono::steady_clock::now();
                    [prev_frame_cmd_ waitUntilCompleted];
                    ns_gpu_wait_ += since(t0);
                }
                prev_frame_cmd_ = dump ? nil : mine;
                // Every command buffer of the frame before this one has completed now (they complete in the order
                // they were committed, and the last of them was just waited for, or this frame's was): its
                // constant chunks are free to be written again. This frame's wait one more frame.
                for (id<MTLBuffer> b : chunks_prev_)
                    if (chunks_free_.size() < kMaxFreeChunks) chunks_free_.push_back(b);
                chunks_prev_ = std::move(chunks_frame_);
                chunks_frame_.clear();
                frames_rendered_++;
                if (dump) dump_backbuffer(frame);
                if (opts_.window || opts_.layer) present_to_window(frame);
            }
        }
    }
    bool wants_quit() override { return quit_; }
    Work work() override {
        return {ns_translate_, ns_library_, ns_pipeline_, ns_gpu_wait_, ns_upload_, ns_drawable_, ns_gpu_.load(), functions_compiled_, pipelines_created_, uploads_, upload_bytes_, precompiled_used_};
    }

    int report() override {
        flush(true);
        if (opts_.window) printf("window: %d frames shown\n", frames_shown_);
        printf("shader work: translate %.1f ms, MSL compile %.1f ms, pipeline creation %.1f ms\n", ns_translate_ / 1e6, ns_library_ / 1e6,
               ns_pipeline_ / 1e6);
        if (!cache_dir_.empty()) printf("shader functions loaded from the cache: %llu of %zu\n", (unsigned long long)precompiled_used_, functions_compiled_);
        printf("metal backend: %d frames rendered, %llu draws executed, %llu skipped; %zu pipelines, %zu shader functions compiled\n",
               frames_rendered_, (unsigned long long)draws_, (unsigned long long)draws_skipped_, pipelines_.size() + internal_pipelines_.size(), functions_compiled_);
        for (auto &[k, n] : problems_) printf("  %8d  %s\n", n, k.c_str());
        for (auto &[kind, ids] : unrecorded_ids_) { // object id, number of frames affected, first and last such frame
            printf("  unrecorded %s (%zu):", kind.c_str(), ids.size());
            int shown = 0;
            for (auto &[id, frames] : ids)
                if (shown++ < 12) printf(" %08x[%zu frames %d-%d]", id, frames.size(), *frames.begin(), *frames.rbegin());
            printf("%s\n", ids.size() > 12 ? " ..." : "");
        }
        return 0;
    }

private:
    static constexpr uint32_t kBackBufferTex = 0xBBBB0001u;

    bool rendering(int frame) const { return frame >= opts_.first_frame - 1 && frame <= opts_.last_frame; }
    void problem(const std::string &what) { problems_[what]++; }
    // A draw reads data that the game wrote but the trace does not contain (docs/TRACE_FORMAT.md,
    // recording window): the replay then draws with zeros / stale contents — a capture gap, not a renderer fault.
    void unrecorded(const char *kind, uint32_t id) {
        if (frame_ < opts_.first_frame) return; // the lead-in frame is drawn for state only and lies before the recording window
        problem(std::string("draw reads an unrecorded ") + kind);
        unrecorded_ids_[kind][id].insert(frame_);
    }
    // Release that destroyed the object: forget it. Its id (the object's address) may come back as a new object.
    template <class M> bool released(const Record &r, M &map) {
        if (!r.has_result() || r.result != 0) return false;
        map.erase(r.arg(0));
        return true;
    }
    // Pipelines are cached under the ids of their shaders and vertex declaration: they go with any of the three.
    // (Called whenever a shader or declaration is destroyed: what the bound ids mean has to be looked up again.)
    void drop_pipelines(uint32_t id) {
        dirty_ |= kDirtyObjects | kDirtyFunctions | kDirtyPipeline;
        auto it = pipeline_users_.find(id);
        if (it == pipeline_users_.end()) return;
        for (const Key<19> &key : it->second) pipelines_.erase(key);
        last_pipeline_.value = nil;
        pipeline_users_.erase(it);
    }
    // A buffer was created or destroyed: the buffers remembered for the bound streams / indices may be stale.
    void buffers_changed() {
        stream_dirty_ = 0xFFFF;
        ib_dirty_ = true;
    }
    static uint32_t out_id(const Record &r) {
        const Blob *b = r.blob(kOut);
        uint32_t id = 0;
        if (b && b->data.size() == 4 && !(r.result & 0x80000000u)) memcpy(&id, b->data.data(), 4);
        return id;
    }
    void out_surface(const Record &r, uint32_t tex, uint32_t level, uint32_t face) {
        if (uint32_t id = out_id(r)) surfaces_[id] = {tex, level, face};
    }

    void reset_state() {
        rs_.fill(0);
        dirty_ = kDirtyAll;
        tex_dirty_ = samp_dirty_ = stream_dirty_ = 0xFFFF;
        ib_dirty_ = true;
        rs_[7] = 0; rs_[14] = 1; rs_[23] = 4; rs_[15] = 0; rs_[25] = 8; rs_[24] = 0; rs_[19] = 2; rs_[20] = 1;
        rs_[22] = 3; rs_[27] = 0; rs_[52] = 0; rs_[53] = 1; rs_[54] = 1; rs_[55] = 1; rs_[56] = 8; rs_[57] = 0;
        rs_[58] = 0xFFFFFFFF; rs_[59] = 0xFFFFFFFF; rs_[168] = 0xF; rs_[171] = 1; rs_[174] = 0; rs_[175] = 0;
        rs_[185] = 0; rs_[186] = 1; rs_[187] = 1; rs_[188] = 1; rs_[189] = 8; rs_[195] = 0; rs_[206] = 0;
        rs_[207] = 2; rs_[208] = 1; rs_[209] = 1;
        for (auto &s : samp_) s = {0, 1, 1, 1, 0, 1, 1, 0, 0, 0, 1, 0, 0, 0};
    }

    // ---- object creation -----------------------------------------------------------------
    void create_device(const Record &r) {
        const Blob *pp = r.blob(kPP);
        uint32_t w = 800, h = 600;
        if (pp && pp->data.size() >= 8) {
            memcpy(&w, pp->data.data(), 4);
            memcpy(&h, pp->data.data() + 4, 4);
        }
        Tex t;
        t.fmt = FMT_A8R8G8B8, t.w = w, t.h = h;
        MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:w height:h mipmapped:NO];
        d.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
        d.storageMode = MTLStorageModeShared;
        t.tex = [dev_ newTextureWithDescriptor:d];
        t.view = t.tex;
        textures_[kBackBufferTex] = t;
        tex_dirty_ = 0xFFFF;
        dirty_ |= kDirtyViewport;
        rt_ = {kBackBufferTex, 0, 0};
        have_rt_ = true;
        ds_ = {};
        have_ds_ = false;
        viewport_ = {0, 0, (double)w, (double)h, 0, 1};
        scissor_ = {0, 0, w, h};
    }

    void create_texture(uint32_t id, MTLTextureType type, uint32_t w, uint32_t h, uint32_t depth, uint32_t levels,
                        uint32_t usage, uint32_t fmt) {
        if (!id) return;
        tex_dirty_ = 0xFFFF; // the id may be one a stage already has bound
        Tex t;
        t.fmt = fmt, t.w = w, t.h = h, t.d = depth;
        t.cube = type == MTLTextureTypeCube, t.volume = type == MTLTextureType3D, t.depth = is_depth(fmt);
        if (fmt == FMT_NULL) {
            t.null_target = true;
            textures_[id] = t;
            return;
        }
        FormatInfo fi = format_info(fmt);
        if (fi.mtl == MTLPixelFormatInvalid) {
            problem("unsupported texture format " + std::to_string(fmt));
            textures_[id] = t;
            return;
        }
        uint32_t full = 1;
        for (uint32_t m = std::max(w, std::max(h, depth)); m > 1; m >>= 1) full++;
        t.levels = levels ? std::min(levels, full) : full;
        MTLTextureDescriptor *d = [MTLTextureDescriptor new];
        d.textureType = type;
        d.pixelFormat = fi.mtl;
        d.width = w;
        d.height = h;
        d.depth = t.volume ? depth : 1;
        d.mipmapLevelCount = t.levels;
        bool target = (usage & 3) != 0; // D3DUSAGE_RENDERTARGET | DEPTHSTENCIL
        d.usage = MTLTextureUsageShaderRead | (target ? MTLTextureUsageRenderTarget : 0);
        d.storageMode = t.depth ? MTLStorageModePrivate : MTLStorageModeShared;
        // Metal forbids swizzled render targets: render to the plain texture, sample through a view.
        if (fi.has_swizzle && !target) d.swizzle = fi.swizzle;
        if (fi.has_swizzle && target) d.usage |= MTLTextureUsagePixelFormatView;
        t.tex = [dev_ newTextureWithDescriptor:d];
        if (!t.tex) problem("texture creation failed");
        t.view = t.tex;
        if (t.tex && fi.has_swizzle && target)
            t.view = [t.tex newTextureViewWithPixelFormat:fi.mtl textureType:type levels:NSMakeRange(0, t.levels)
                                                   slices:NSMakeRange(0, t.cube ? 6 : 1) swizzle:fi.swizzle];
        textures_[id] = t;
    }

    void device_call(const Record &r) {
        switch (r.slot) {
        case 2: break; // device Release
        case 16: // Reset: back buffer of the new size, default state and no bindings
            if (r.has_result() && (r.result & 0x80000000u)) break;
            flush(true);
            create_device(r);
            reset_state();
            bound_tex_.fill(0);
            streams_.fill({});
            stream_mask_ = 0;
            cur_vs_ = cur_ps_ = cur_decl_ = cur_ib_ = 0;
            break;
        case 18: out_surface(r, kBackBufferTex, 0, 0); break;
        case 23: create_texture(out_id(r), MTLTextureType2D, r.arg(1), r.arg(2), 1, r.arg(3), r.arg(4), r.arg(5)); break;
        case 24: create_texture(out_id(r), MTLTextureType3D, r.arg(1), r.arg(2), r.arg(3), r.arg(4), r.arg(5), r.arg(6)); break;
        case 25: create_texture(out_id(r), MTLTextureTypeCube, r.arg(1), r.arg(1), 1, r.arg(2), r.arg(3), r.arg(4)); break;
        case 26: case 27:
            if (uint32_t id = out_id(r)) {
                Buf b;
                b.size = r.arg(1);
                if (r.slot == 27) b.index_fmt = r.arg(3);
                b.buf = [dev_ newBufferWithLength:std::max<uint32_t>(b.size, 16) options:MTLResourceStorageModeShared];
                buffers_[id] = b;
                buffers_changed();
            }
            break;
        case 28: case 29: // CreateRenderTarget / CreateDepthStencilSurface: a standalone surface
            if (uint32_t id = out_id(r)) {
                create_texture(id, MTLTextureType2D, r.arg(1), r.arg(2), 1, 1, r.slot == 28 ? 1 : 2, r.arg(3));
                surfaces_[id] = {id, 0, 0};
            }
            break;
        case 34: stretch_rect(r); break;
        case 37:
            if (r.arg(1) == 0) {
                end_pass();
                auto it = surfaces_.find(r.arg(2));
                have_rt_ = it != surfaces_.end();
                if (have_rt_) {
                    rt_ = it->second;
                    // D3D9 resets the viewport to the full target when the render target changes.
                    if (const Tex *t = tex(rt_.tex)) {
                        uint32_t w = std::max(1u, t->w >> rt_.level), h = std::max(1u, t->h >> rt_.level);
                        viewport_ = {0, 0, (double)w, (double)h, 0, 1};
                        scissor_ = {0, 0, w, h};
                        dirty_ |= kDirtyViewport;
                    }
                } else if (r.arg(2))
                    problem("SetRenderTarget: unknown surface");
            } else if (r.arg(2))
                problem("multiple render targets");
            break;
        case 39: {
            end_pass();
            auto it = surfaces_.find(r.arg(1));
            have_ds_ = it != surfaces_.end();
            if (have_ds_) ds_ = it->second;
            break;
        }
        case 43: clear(r); break;
        case 47:
            if (const Blob *b = r.blob(kStruct); b && b->data.size() >= 24) {
                uint32_t v[4];
                float z[2];
                memcpy(v, b->data.data(), 16);
                memcpy(z, b->data.data() + 16, 8);
                viewport_ = {(double)v[0], (double)v[1], (double)v[2], (double)v[3], z[0], z[1]};
                dirty_ |= kDirtyViewport;
            }
            break;
        case 75:
            if (const Blob *b = r.blob(kStruct); b && b->data.size() >= 16) {
                int32_t v[4];
                memcpy(v, b->data.data(), 16);
                scissor_ = {(NSUInteger)std::max(v[0], 0), (NSUInteger)std::max(v[1], 0), (NSUInteger)std::max(v[2] - v[0], 0), (NSUInteger)std::max(v[3] - v[1], 0)};
                dirty_ |= kDirtyViewport;
            }
            break;
        case 57: set_render_state(r.arg(1), r.arg(2)); break;
        case 65: set_texture(r.arg(1), r.arg(2)); break;
        case 69: set_sampler_state(r.arg(1), r.arg(2), r.arg(3)); break;
        case 82: draw_indexed(r.arg(1), (int32_t)r.arg(2), r.arg(3), r.arg(4), r.arg(5), r.arg(6)); break;
        case 86:
            if (uint32_t id = out_id(r)) {
                std::vector<Elem> el;
                if (const Blob *b = r.blob(kElements))
                    for (size_t o = 0; o + 8 <= b->data.size(); o += 8) {
                        Elem e;
                        memcpy(&e, b->data.data() + o, 8);
                        if (e.stream == 0xFF) break;
                        el.push_back(e);
                    }
                decls_[id] = std::move(el);
                dirty_ |= kDirtyObjects | kDirtyPipeline;
            }
            break;
        case 87: set_vertex_declaration(r.arg(1)); break;
        case 91: case 106:
            if (uint32_t id = out_id(r)) {
                Shader s;
                s.vs = r.slot == 91;
                if (const Blob *b = r.blob(kBytecode)) {
                    s.code.resize(b->data.size() / 4);
                    memcpy(s.code.data(), b->data.data(), s.code.size() * 4);
                }
                s.created_frame = frame_;
                if (opts_.cache_created && !cache_dir_.empty() && !s.code.empty()) note_created(s);
                shaders_[id] = std::move(s);
                dirty_ |= kDirtyObjects | kDirtyFunctions | kDirtyPipeline;
            }
            break;
        case 92: set_shader(true, r.arg(1)); break;
        case 107: set_shader(false, r.arg(1)); break;
        case 94: case 109:
            if (const Blob *b = r.blob(kData)) set_shader_constants(r.slot == 94, r.arg(1), b->data.data(), (uint32_t)(b->data.size() / 16));
            break;
        case 100: set_stream_source(r.arg(1), r.arg(2), r.arg(3), r.arg(4)); break;
        case 104: set_indices(r.arg(1)); break;
        default: break;
        }
    }

    Tex *tex(uint32_t id) {
        auto it = textures_.find(id);
        return it == textures_.end() ? nullptr : &it->second;
    }

    // ---- uploads ---------------------------------------------------------------------------
    void upload_buffer(const Record &r) {
        const Blob *lb = r.blob(kLock), *data = r.blob(kData);
        if (!lb || lb->data.size() != sizeof(LockDesc)) return;
        LockDesc d;
        memcpy(&d, lb->data.data(), sizeof(d));
        if (debug_log_ && getenv("BO1_DEBUG_UPLOADS") && (uint32_t)strtoul(getenv("BO1_DEBUG_UPLOADS"), nullptr, 16) == r.arg(0))
            fprintf(debug_log_, "upload frame=%d buf=%08x flags=%x offset=%u size=%u dumped=%u data=%zu\n", frame_, r.arg(0), d.lock_flags, d.offset, d.size, d.dumped, data ? data->data.size() : 0);
        buffer_written(r.arg(0), d.lock_flags, d.offset, d.size, d.dumped, data ? data->data.data() : nullptr, data ? data->data.size() : 0);
    }
    // The game unlocked a buffer: `n` bytes at `data` are what it wrote at `offset` (none: nothing was handed
    // over). `lock_size` / `dumped` describe the lock as the trace does (LockDesc; dumped 3 = written, not recorded).
    void buffer_written(uint32_t id, uint32_t lock_flags, uint32_t offset, uint32_t lock_size, uint32_t dumped, const void *data, size_t n) {
        auto it = buffers_.find(id);
        if (it == buffers_.end()) return;
        Buf &b = it->second;
        bool discard = lock_flags & 0x2000, nooverwrite = lock_flags & 0x1000;
        if (discard) { // fresh storage: earlier draws keep the old contents
            b.buf = [dev_ newBufferWithLength:std::max<uint32_t>(b.size, 16) options:MTLResourceStorageModeShared];
            b.used_gen = 0;
            b.unrecorded.clear();
        } else if (!nooverwrite && b.used_gen == gen_ && data) {
            @autoreleasepool {
                flush(true);
            }
        }
        if (dumped == 3) b.mark(offset, lock_size ? std::min<uint64_t>((uint64_t)offset + lock_size, b.size) : b.size, true);
        if (!data || !n) return;
        if ((uint64_t)offset + n > b.size) {
            problem("buffer upload out of range");
            return;
        }
        if (!b.unrecorded.empty()) b.mark(offset, offset + (uint32_t)n, false);
        memcpy((uint8_t *)b.buf.contents + offset, data, n);
    }

    void upload_texture(const Record &r, uint32_t tex_id, uint32_t level, uint32_t face) {
        const Blob *lb = r.blob(kLock), *data = r.blob(kData);
        if (!lb || lb->data.size() != sizeof(LockDesc)) return;
        Tex *t = tex(tex_id);
        if (!t || !t->tex) return;
        LockDesc d;
        memcpy(&d, lb->data.data(), sizeof(d));
        bool readonly = d.lock_flags & 0x10; // D3DLOCK_READONLY
        if (!data || data->data.empty()) {
            if (!readonly && level == 0) t->unrecorded = true;
            return;
        }
        if (level == 0) t->unrecorded = false;
        tex_dirty_ = 0xFFFF; // `unrecorded` is remembered per stage
        if (t->used_gen == gen_) flush(true); // an earlier draw in this command buffer samples it
        MTLRegion region;
        if (d.has_rect) region = MTLRegionMake3D(d.left, d.top, t->volume ? d.front : 0, d.right - d.left, d.bottom - d.top, t->volume ? d.back - d.front : 1);
        else region = MTLRegionMake3D(0, 0, 0, d.width, d.height, t->volume ? d.depth : 1);
        if (level >= t->levels) return;
        auto t0 = std::chrono::steady_clock::now();
        [t->tex replaceRegion:region mipmapLevel:level slice:t->cube ? face : 0 withBytes:data->data.data()
                  bytesPerRow:d.pitch bytesPerImage:t->volume ? d.slice_pitch : 0];
        ns_upload_ += since(t0);
        uploads_++, upload_bytes_ += data->data.size();
    }

    // ---- command buffer / pass management ---------------------------------------------------
    void ensure_cmd() {
        if (!cmd_) cmd_ = [queue_ commandBuffer];
    }
    void end_pass() {
        if (enc_) {
            [enc_ endEncoding];
            enc_ = nil;
            if (debugging() && pass_touched_) debug_dump(have_rt_ ? &rt_ : nullptr, have_ds_ ? &ds_ : nullptr, "pass");
            pass_touched_ = false;
            gpu_pass_report("pass");
        }
    }

    // ---- debugging aid (T-056) ---------------------------------------------------------------
    // For one frame, every pass (run of draws/clears on one target pair) and every StretchRect is
    // listed in <dir>/passes.txt and its targets are written as raw files that tools/pass_dump.py
    // turns into images. Shaders used in that frame are written as MSL + bytecode.
    bool debugging() const { return debug_log_ && frame_ == debug_frame_; }
    void debug_desc(const char *what, const Surface *s) {
        const Tex *t = s ? tex(s->tex) : nullptr;
        if (t) fprintf(debug_log_, " %s=%08x fmt=%u %ux%u level=%u face=%u", what, s->tex, t->fmt, t->w, t->h, s->level, s->face);
        else fprintf(debug_log_, " %s=none", what);
    }
    void debug_dump(const Surface *color, const Surface *depth, const char *kind) {
        if (cmd_) {
            [cmd_ commit];
            [cmd_ waitUntilCompleted];
            cmd_ = nil;
        }
        gen_++;
        retire_const_chunks();
        const_copy_[0] = const_copy_[1] = {};
        int n = debug_pass_++;
        fprintf(debug_log_, "%s %d:", kind, n);
        debug_desc("color", color);
        debug_desc("depth", depth);
        fprintf(debug_log_, "\n");
        auto write = [&](const char *suffix, MTLPixelFormat fmt, uint32_t d3dfmt, NSUInteger w, NSUInteger h, NSUInteger bpp, const std::vector<uint8_t> &px) {
            std::string path = debug_dir_ + fmt_key("/pass_%04d_%s.raw", n, suffix);
            if (FILE *f = fopen(path.c_str(), "wb")) {
                fprintf(f, "BO1RAW %lu %lu %lu %lu %u\n", (unsigned long)w, (unsigned long)h, (unsigned long)bpp, (unsigned long)fmt, d3dfmt);
                fwrite(px.data(), 1, px.size(), f);
                fclose(f);
            }
        };
        if (Tex *t = color ? tex(color->tex) : nullptr; t && t->tex && !t->depth) {
            NSUInteger bpp = 0;
            switch (t->tex.pixelFormat) {
            case MTLPixelFormatBGRA8Unorm: case MTLPixelFormatR32Float: case MTLPixelFormatRG16Unorm: bpp = 4; break;
            case MTLPixelFormatRGBA16Unorm: case MTLPixelFormatRGBA16Float: bpp = 8; break;
            case MTLPixelFormatB5G6R5Unorm: case MTLPixelFormatRG8Unorm: bpp = 2; break;
            case MTLPixelFormatR8Unorm: case MTLPixelFormatA8Unorm: bpp = 1; break;
            default: break;
            }
            NSUInteger w = std::max(1u, t->w >> color->level), h = std::max(1u, t->h >> color->level);
            if (bpp) {
                std::vector<uint8_t> px(w * h * bpp);
                [t->tex getBytes:px.data() bytesPerRow:w * bpp bytesPerImage:0 fromRegion:MTLRegionMake2D(0, 0, w, h) mipmapLevel:color->level
                           slice:t->cube ? color->face : 0];
                write("c", t->tex.pixelFormat, t->fmt, w, h, bpp, px);
            }
        }
        if (Tex *t = depth ? tex(depth->tex) : nullptr; t && t->tex && t->depth) {
            NSUInteger w = std::max(1u, t->w >> depth->level), h = std::max(1u, t->h >> depth->level);
            id<MTLBuffer> buf = [dev_ newBufferWithLength:w * h * 4 options:MTLResourceStorageModeShared];
            id<MTLCommandBuffer> cb = [queue_ commandBuffer];
            id<MTLBlitCommandEncoder> b = [cb blitCommandEncoder];
            [b copyFromTexture:t->tex sourceSlice:0 sourceLevel:depth->level sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(w, h, 1)
                      toBuffer:buf destinationOffset:0 destinationBytesPerRow:w * 4 destinationBytesPerImage:w * h * 4
                       options:MTLBlitOptionDepthFromDepthStencil];
            [b endEncoding];
            [cb commit];
            [cb waitUntilCompleted];
            std::vector<uint8_t> px(w * h * 4);
            memcpy(px.data(), buf.contents, px.size());
            write("d", MTLPixelFormatDepth32Float, t->fmt, w, h, 4, px);
        }
        fflush(debug_log_);
    }
    void debug_shader(uint32_t id, Shader &s, uint32_t shadow) {
        if (!debug_shaders_.insert(((uint64_t)id << 16) | shadow).second) return;
        sm3::Options o;
        o.shadow_samplers = shadow;
        sm3::Result res = sm3::translate(s.code, o);
        std::string base = debug_dir_ + fmt_key("/%s_%08x", s.vs ? "vs" : "ps", id);
        if (FILE *f = fopen((base + (shadow ? fmt_key("_shadow%x.msl", shadow) : std::string(".msl"))).c_str(), "w")) {
            fputs(res.msl.c_str(), f);
            fclose(f);
        }
        if (FILE *f = fopen((base + ".bin").c_str(), "wb")) {
            fwrite(s.code.data(), 4, s.code.size(), f);
            fclose(f);
        }
    }
    void debug_draw(uint32_t prim_count, int32_t base_vertex, uint32_t start_index, Shader &vs, Shader &ps, uint32_t shadow) {
        debug_shader(cur_vs_, vs, 0);
        debug_shader(cur_ps_, ps, shadow);
        fprintf(debug_log_, "  draw vs=%08x ps=%08x shadow=%x prims=%u vp=%g,%g,%gx%g z=%g..%g zen=%u zw=%u zf=%u ab=%u src=%u dst=%u sep=%u srca=%u dsta=%u at=%u/%u/%u cw=%x cull=%u st=%u sc=%u", cur_vs_, cur_ps_, shadow,
                prim_count, viewport_.originX, viewport_.originY, viewport_.width, viewport_.height, viewport_.znear, viewport_.zfar, rs_[7], rs_[14], rs_[23], rs_[27], rs_[19], rs_[20],
                rs_[206], rs_[207], rs_[208], rs_[15], rs_[25], rs_[24], rs_[168], rs_[22], rs_[52], rs_[174]);
        for (const sm3::SamplerDecl &s : ps.meta.samplers) {
            const Tex *t = tex(bound_tex_[s.reg]);
            const auto &st = samp_[s.reg];
            fprintf(debug_log_, " s%u=%08x", s.reg, bound_tex_[s.reg]);
            if (t) fprintf(debug_log_, "(f%u %ux%u a%u%u m%u%u%u)", t->fmt, t->w, t->h, st[1], st[2], st[5], st[6], st[7]);
        }
        if (rs_[52])
            fprintf(debug_log_, " stencil[func=%u ref=%u mask=%x wmask=%x fail=%u zfail=%u pass=%u two=%u ccw=%u/%u/%u/%u]", rs_[56], rs_[57], rs_[58], rs_[59], rs_[53], rs_[54], rs_[55],
                    rs_[185], rs_[189], rs_[186], rs_[187], rs_[188]);
        fprintf(debug_log_, "\n");
        if (prim_count > 2) return; // quads: also show where the vertices are and the constants that place them
        auto ib = buffers_.find(cur_ib_);
        auto di = decls_.find(cur_decl_);
        if (ib == buffers_.end() || di == decls_.end()) return;
        bool i32 = ib->second.index_fmt == FMT_INDEX32;
        fprintf(debug_log_, "    scissor=%lu,%lu,%lux%lu base=%d decl:", (unsigned long)scissor_.x, (unsigned long)scissor_.y, (unsigned long)scissor_.width, (unsigned long)scissor_.height, base_vertex);
        for (const Elem &e : di->second) fprintf(debug_log_, " [s%u+%u t%u u%u.%u]", e.stream, e.offset, e.type, e.usage, e.uidx);
        fprintf(debug_log_, "\n");
        for (uint32_t k = 0; k < prim_count * 3; k++) {
            size_t off = (size_t)(start_index + k) * (i32 ? 4 : 2);
            if (off + 4 > ib->second.size) break;
            uint32_t idx = i32 ? *(const uint32_t *)((const uint8_t *)ib->second.buf.contents + off) : *(const uint16_t *)((const uint8_t *)ib->second.buf.contents + off);
            idx += (uint32_t)base_vertex;
            fprintf(debug_log_, "    i%u=%u", k, idx);
            for (const Elem &e : di->second) {
                if (e.usage != 0 || e.stream >= 16) continue;
                auto vb = buffers_.find(streams_[e.stream].vb);
                if (vb == buffers_.end()) continue;
                size_t vo = (size_t)streams_[e.stream].offset + (size_t)idx * streams_[e.stream].stride + e.offset;
                if (vo + 16 > vb->second.size) continue;
                float f[4];
                memcpy(f, (const uint8_t *)vb->second.buf.contents + vo, 16);
                fprintf(debug_log_, " pos=(%g %g %g %g) [vb %08x off %u stride %u]", f[0], f[1], f[2], f[3], streams_[e.stream].vb, streams_[e.stream].offset, streams_[e.stream].stride);
            }
            fprintf(debug_log_, "\n");
        }
        for (int c = 0; c < 8; c++) fprintf(debug_log_, "    vs c%d = %g %g %g %g\n", c, vsc_.c[c][0], vsc_.c[c][1], vsc_.c[c][2], vsc_.c[c][3]);
    }
    // Constant chunks (bind_consts) are kept and used again instead of being allocated for every command buffer:
    // a fight needs ~20 of them (2.3 MiB each) per frame, and fresh memory costs a page fault per 16 KiB.
    // A chunk may be written again once the GPU is done with the command buffers that read it; end_frame
    // knows when that is.
    void retire_const_chunks() {
        for (id<MTLBuffer> b : const_chunks_) chunks_frame_.push_back(b);
        const_chunks_.clear();
        const_off_ = 0;
    }
    // The GPU's own time for a command buffer, added up when it completes (Work::ns_gpu).
    void commit(id<MTLCommandBuffer> cb) {
        std::atomic<uint64_t> *sum = &ns_gpu_;
        [cb addCompletedHandler:^(id<MTLCommandBuffer> done) {
            double s = done.GPUEndTime - done.GPUStartTime;
            if (s > 0) sum->fetch_add((uint64_t)(s * 1e9), std::memory_order_relaxed);
        }];
        [cb commit];
    }
    // BO1_GPU_PASSES=N: in frame N every pass gets a command buffer of its own and one line on stderr with the
    // GPU's time for it (P7: which passes of a frame cost what at a given resolution).
    void gpu_pass_report(const char *kind) {
        if (frame_ != gpu_passes_frame_ || !cmd_) return;
        id<MTLCommandBuffer> cb = cmd_;
        [cb commit];
        [cb waitUntilCompleted];
        cmd_ = nil;
        gen_++;
        retire_const_chunks();
        const_copy_[0] = const_copy_[1] = {};
        const Tex *ct = have_rt_ ? tex(rt_.tex) : nullptr, *dt = have_ds_ ? tex(ds_.tex) : nullptr;
        fprintf(stderr, "gpu pass %3d %-8s %7.3f ms  %4u draws  colour %s fmt %u %ux%u  depth %s\n", gpu_pass_n_++, kind, (cb.GPUEndTime - cb.GPUStartTime) * 1e3,
                pass_draws_, ct ? "yes" : "no", ct ? ct->fmt : 0, ct ? std::max(1u, ct->w >> rt_.level) : 0, ct ? std::max(1u, ct->h >> rt_.level) : 0, dt ? "yes" : "no");
        pass_draws_ = 0;
    }
    void flush(bool wait) {
        end_pass();
        if (cmd_) {
            commit(cmd_);
            if (wait) {
                auto t0 = std::chrono::steady_clock::now();
                [cmd_ waitUntilCompleted];
                ns_gpu_wait_ += since(t0);
            }
            cmd_ = nil;
        }
        gen_++;
        retire_const_chunks();
        const_copy_[0] = const_copy_[1] = {};
    }
    id<MTLTexture> target_texture(const Surface &s, Tex **out = nullptr) {
        Tex *t = tex(s.tex);
        if (out) *out = t;
        return t ? t->tex : nil;
    }
    // Begin (or continue) a pass on the current targets. Returns false if there is nothing to draw to.
    // A Clear that opens a pass and covers the whole target becomes the pass's load action (P7): the GPU then
    // neither loads the old contents of a tile nor shades a clearing triangle over it — at 2560x1600 that is
    // 4 M pixels of colour, depth and stencil per cleared pass.
    struct PassClear {
        uint32_t flags, color, stencil; // D3DCLEAR_TARGET = 1, ZBUFFER = 2, STENCIL = 4; D3DCOLOR
        float z;
        bool done = false;              // set by begin_pass when the load actions did it
    };
    bool begin_pass(PassClear *pc = nullptr) {
        if (enc_) return true;
        Tex *ct = nullptr, *dt = nullptr;
        id<MTLTexture> color = have_rt_ ? target_texture(rt_, &ct) : nil;
        id<MTLTexture> depth = have_ds_ ? target_texture(ds_, &dt) : nil;
        if (!color && !depth) return false;
        pass_w_ = color ? std::max<NSUInteger>(1, color.width >> rt_.level) : std::max<NSUInteger>(1, depth.width >> ds_.level);
        pass_h_ = color ? std::max<NSUInteger>(1, color.height >> rt_.level) : std::max<NSUInteger>(1, depth.height >> ds_.level);
        bool clear_color = false, clear_depth = false, clear_stencil = false;
        if (pc && !debugging()) {
            // Direct3D clears the viewport (and scissor) area: only when that is the whole target, and for
            // depth / stencil only when the depth buffer is exactly as large as the pass (a larger one keeps
            // what lies outside).
            MTLScissorRect area = viewport_rect();
            bool whole = area.x == 0 && area.y == 0 && area.width == pass_w_ && area.height == pass_h_ && !rs_[174];
            bool depth_fits = depth && std::max<NSUInteger>(1, depth.width >> ds_.level) == pass_w_ && std::max<NSUInteger>(1, depth.height >> ds_.level) == pass_h_;
            bool want_ds = depth && (pc->flags & 6);
            if (whole && (!want_ds || depth_fits)) {
                clear_color = color && (pc->flags & 1);
                clear_depth = depth && (pc->flags & 2);
                clear_stencil = depth && (pc->flags & 4);
                pc->done = true;
            }
        }
        MTLRenderPassDescriptor *p = [MTLRenderPassDescriptor new];
        if (color) {
            p.colorAttachments[0].texture = color;
            p.colorAttachments[0].level = rt_.level;
            p.colorAttachments[0].slice = ct->cube ? rt_.face : 0;
            p.colorAttachments[0].loadAction = clear_color ? MTLLoadActionClear : MTLLoadActionLoad;
            p.colorAttachments[0].storeAction = MTLStoreActionStore;
            if (clear_color)
                p.colorAttachments[0].clearColor = MTLClearColorMake(((pc->color >> 16) & 0xFF) / 255.f, ((pc->color >> 8) & 0xFF) / 255.f, (pc->color & 0xFF) / 255.f, (pc->color >> 24) / 255.f);
        }
        if (depth) {
            p.depthAttachment.texture = depth;
            p.depthAttachment.level = ds_.level;
            p.depthAttachment.loadAction = clear_depth ? MTLLoadActionClear : MTLLoadActionLoad;
            p.depthAttachment.storeAction = MTLStoreActionStore;
            if (clear_depth) p.depthAttachment.clearDepth = pc->z;
            p.stencilAttachment.texture = depth;
            p.stencilAttachment.level = ds_.level;
            p.stencilAttachment.loadAction = clear_stencil ? MTLLoadActionClear : MTLLoadActionLoad;
            p.stencilAttachment.storeAction = MTLStoreActionStore;
            if (clear_stencil) p.stencilAttachment.clearStencil = pc->stencil & 0xFF;
        }
        ensure_cmd();
        enc_ = [cmd_ renderCommandEncoderWithDescriptor:p];
        const_copy_[0].on_encoder = const_copy_[1].on_encoder = false;
        // A new encoder knows nothing: every stage is looked at again (which also marks its texture as used by
        // this command buffer), the pipeline depends on the pass's formats, the scissor on its size.
        ap_ = {};
        tex_dirty_ = samp_dirty_ = 0xFFFF;
        dirty_ |= kDirtyPipeline | kDirtyViewport;
        [enc_ setFrontFacingWinding:MTLWindingClockwise];
        [enc_ setVertexBuffer:dummy_vb_ offset:0 atIndex:16];
        pass_color_ = color ? color.pixelFormat : MTLPixelFormatInvalid;
        pass_depth_ = depth ? depth.pixelFormat : MTLPixelFormatInvalid;
        if (ct) ct->used_gen = gen_;
        if (dt) dt->used_gen = gen_;
        return true;
    }

    id<MTLRenderPipelineState> internal_pipeline(const char *fs, uint32_t write_mask, bool blit) {
        std::string key = fmt_key("int:%s:%lu:%lu:%u", fs, (unsigned long)pass_color_, (unsigned long)pass_depth_, write_mask);
        auto it = internal_pipelines_.find(key);
        if (it != internal_pipelines_.end()) return it->second;
        MTLRenderPipelineDescriptor *d = [MTLRenderPipelineDescriptor new];
        d.vertexFunction = [internal_ newFunctionWithName:@"fs_vs"];
        d.fragmentFunction = [internal_ newFunctionWithName:@(fs)];
        d.colorAttachments[0].pixelFormat = pass_color_;
        d.colorAttachments[0].writeMask = mtl_write_mask(write_mask);
        if (!blit) {
            d.depthAttachmentPixelFormat = pass_depth_;
            d.stencilAttachmentPixelFormat = pass_depth_;
        }
        NSError *err = nil;
        id<MTLRenderPipelineState> p = [dev_ newRenderPipelineStateWithDescriptor:d error:&err];
        if (!p) problem(std::string("internal pipeline: ") + (err ? err.localizedDescription.UTF8String : "?"));
        internal_pipelines_[key] = p;
        return p;
    }
    static MTLColorWriteMask mtl_write_mask(uint32_t d3d) { // D3DCOLORWRITEENABLE: R=1 G=2 B=4 A=8
        MTLColorWriteMask m = MTLColorWriteMaskNone;
        if (d3d & 1) m |= MTLColorWriteMaskRed;
        if (d3d & 2) m |= MTLColorWriteMaskGreen;
        if (d3d & 4) m |= MTLColorWriteMaskBlue;
        if (d3d & 8) m |= MTLColorWriteMaskAlpha;
        return m;
    }
    // The last key a state lookup was made with and what it returned: consecutive lookups mostly ask for the same
    // pipeline / depth state / sampler (T-058).
    template <size_t N> struct Last {
        Key<N> key;
        id value = nil;
        bool same(const Key<N> &k) const { return value && key == k; }
        id set(const Key<N> &k, id v) {
            key = k;
            return value = v;
        }
    };
    static std::string fmt_key(const char *f, ...) {
        char buf[512];
        va_list ap;
        va_start(ap, f);
        vsnprintf(buf, sizeof(buf), f, ap);
        va_end(ap);
        return buf;
    }

    MTLScissorRect clamp_scissor(MTLScissorRect s) const {
        if (s.x > pass_w_) s.x = pass_w_;
        if (s.y > pass_h_) s.y = pass_h_;
        if (s.x + s.width > pass_w_) s.width = pass_w_ - s.x;
        if (s.y + s.height > pass_h_) s.height = pass_h_ - s.y;
        return s;
    }
    MTLScissorRect viewport_rect() const {
        return clamp_scissor({(NSUInteger)std::max(0.0, viewport_.originX), (NSUInteger)std::max(0.0, viewport_.originY),
                              (NSUInteger)std::max(0.0, viewport_.width), (NSUInteger)std::max(0.0, viewport_.height)});
    }

    // Clear(Count, pRects, Flags, Color, Z, Stencil): clipped to the viewport (and scissor).
    void clear(const Record &r) {
        if (!rendering(frame_)) return;
        uint32_t flags = r.arg(3), color = r.arg(4), stencil = r.arg(6);
        float z;
        uint32_t zbits = r.arg(5);
        memcpy(&z, &zbits, 4);
        PassClear pc{flags, color, stencil, z};
        if (!begin_pass(r.arg(1) ? nullptr : &pc)) return; // (a rect list is drawn)
        if (pc.done) {
            pass_touched_ = true;
            return;
        }
        MTLScissorRect area = viewport_rect();
        if (rs_[174]) { // intersect with the scissor rect
            MTLScissorRect s = clamp_scissor(scissor_);
            NSUInteger x0 = std::max(area.x, s.x), y0 = std::max(area.y, s.y);
            NSUInteger x1 = std::min(area.x + area.width, s.x + s.width), y1 = std::min(area.y + area.height, s.y + s.height);
            area = {x0, y0, x1 > x0 ? x1 - x0 : 0, y1 > y0 ? y1 - y0 : 0};
        }
        if (!area.width || !area.height) return;
        pass_touched_ = true;
        if (debugging()) fprintf(debug_log_, "  clear flags=%x color=%08x z=%g stencil=%u area=%lu,%lu,%lux%lu\n", flags, color, z, stencil, (unsigned long)area.x, (unsigned long)area.y, (unsigned long)area.width, (unsigned long)area.height);
        bool want_color = (flags & 1) && pass_color_ != MTLPixelFormatInvalid;
        bool want_depth = (flags & 2) && pass_depth_ != MTLPixelFormatInvalid, want_stencil = (flags & 4) && pass_depth_ != MTLPixelFormatInvalid;
        id<MTLRenderPipelineState> p = internal_pipeline("clear_fs", want_color ? 0xF : 0, false);
        if (!p) return;
        id<MTLDepthStencilState> dss = clear_depth_[want_depth][want_stencil];
        if (!dss) {
            MTLDepthStencilDescriptor *d = [MTLDepthStencilDescriptor new];
            d.depthCompareFunction = MTLCompareFunctionAlways;
            d.depthWriteEnabled = want_depth;
            if (want_stencil) {
                MTLStencilDescriptor *s = [MTLStencilDescriptor new];
                s.stencilCompareFunction = MTLCompareFunctionAlways;
                s.depthStencilPassOperation = MTLStencilOperationReplace;
                d.frontFaceStencil = s;
                d.backFaceStencil = s;
            }
            dss = clear_depth_[want_depth][want_stencil] = [dev_ newDepthStencilStateWithDescriptor:d];
        }
        struct { float color[4]; float depth; float pad[3]; } u = {{((color >> 16) & 0xFF) / 255.f, ((color >> 8) & 0xFF) / 255.f, (color & 0xFF) / 255.f, (color >> 24) / 255.f}, z, {0, 0, 0}};
        [enc_ setRenderPipelineState:p];
        if (pass_depth_ != MTLPixelFormatInvalid) [enc_ setDepthStencilState:dss];
        [enc_ setStencilReferenceValue:stencil & 0xFF];
        [enc_ setCullMode:MTLCullModeNone];
        MTLViewport full = {0, 0, (double)pass_w_, (double)pass_h_, 0, 1};
        [enc_ setViewport:full];
        [enc_ setVertexBytes:&u length:sizeof(u) atIndex:0];
        [enc_ setFragmentBytes:&u length:sizeof(u) atIndex:0];
        // The next draw finds the encoder changed: pipeline, depth state, stencil reference, cull mode, viewport,
        // scissor and vertex buffer 0 are no longer what it was last told.
        ap_.pipeline = ap_.depth = ap_.vb[0] = nullptr;
        ap_.raster = false;
        dirty_ |= kDirtyViewport;
        // Optional rect list (D3DRECT[Count]): each rect is clipped to the viewport/scissor area.
        const Blob *rects = r.blob(kRectA);
        size_t nrects = rects ? rects->data.size() / 16 : 0;
        if (r.arg(1) && !nrects) problem("Clear with a rect list that was not recorded");
        for (size_t i = 0; i < std::max<size_t>(nrects, 1); i++) {
            MTLScissorRect a = area;
            if (nrects) {
                int32_t q[4];
                memcpy(q, rects->data.data() + i * 16, 16);
                NSUInteger x0 = std::max<NSUInteger>(area.x, (NSUInteger)std::max(q[0], 0)), y0 = std::max<NSUInteger>(area.y, (NSUInteger)std::max(q[1], 0));
                NSUInteger x1 = std::min<NSUInteger>(area.x + area.width, (NSUInteger)std::max(q[2], 0)), y1 = std::min<NSUInteger>(area.y + area.height, (NSUInteger)std::max(q[3], 0));
                if (x1 <= x0 || y1 <= y0) continue;
                a = {x0, y0, x1 - x0, y1 - y0};
            }
            [enc_ setScissorRect:a];
            [enc_ drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
        }
    }

    // StretchRect(src, srcRect, dst, dstRect, filter) — only whole-surface copies occur.
    void stretch_rect(const Record &r) {
        if (!rendering(frame_)) return;
        auto si = surfaces_.find(r.arg(1)), di = surfaces_.find(r.arg(3));
        if (si == surfaces_.end() || di == surfaces_.end()) {
            problem("StretchRect: unknown surface");
            return;
        }
        Tex *st = tex(si->second.tex), *dt = tex(di->second.tex);
        if (!st || !dt || !st->tex || !dt->tex) return;
        if (r.arg(2) || r.arg(4)) problem("StretchRect with rects");
        end_pass();
        ensure_cmd();
        uint32_t sl = si->second.level, dl = di->second.level;
        NSUInteger sw = std::max(1u, st->w >> sl), sh = std::max(1u, st->h >> sl), dw = std::max(1u, dt->w >> dl), dh = std::max(1u, dt->h >> dl);
        st->used_gen = dt->used_gen = gen_;
        if (sw == dw && sh == dh && st->tex.pixelFormat == dt->tex.pixelFormat) {
            id<MTLBlitCommandEncoder> b = [cmd_ blitCommandEncoder];
            [b copyFromTexture:st->tex sourceSlice:st->cube ? si->second.face : 0 sourceLevel:sl sourceOrigin:MTLOriginMake(0, 0, 0)
                    sourceSize:MTLSizeMake(sw, sh, 1) toTexture:dt->tex destinationSlice:dt->cube ? di->second.face : 0
              destinationLevel:dl destinationOrigin:MTLOriginMake(0, 0, 0)];
            [b endEncoding];
            gpu_pass_report("copy");
            if (debugging()) debug_dump(&di->second, nullptr, fmt_key("stretchrect(copy) src=%08x", si->second.tex).c_str());
            return;
        }
        if (st->depth || dt->depth) {
            problem("StretchRect between depth surfaces of different size");
            return;
        }
        MTLRenderPassDescriptor *p = [MTLRenderPassDescriptor new];
        p.colorAttachments[0].texture = dt->tex;
        p.colorAttachments[0].level = dl;
        p.colorAttachments[0].slice = dt->cube ? di->second.face : 0;
        p.colorAttachments[0].loadAction = MTLLoadActionDontCare;
        p.colorAttachments[0].storeAction = MTLStoreActionStore;
        id<MTLRenderCommandEncoder> e = [cmd_ renderCommandEncoderWithDescriptor:p];
        MTLPixelFormat saved_c = pass_color_, saved_d = pass_depth_;
        pass_color_ = dt->tex.pixelFormat, pass_depth_ = MTLPixelFormatInvalid;
        id<MTLRenderPipelineState> pipe = internal_pipeline("blit_fs", 0xF, true);
        pass_color_ = saved_c, pass_depth_ = saved_d;
        if (pipe) {
            struct { float color[4]; float depth; float pad[3]; } u = {{0, 0, 0, 0}, 0, {0, 0, 0}};
            id<MTLTexture> view = st->tex;
            if (sl || st->cube)
                view = [st->tex newTextureViewWithPixelFormat:st->tex.pixelFormat textureType:MTLTextureType2D levels:NSMakeRange(sl, 1)
                                                       slices:NSMakeRange(st->cube ? si->second.face : 0, 1)];
            [e setRenderPipelineState:pipe];
            [e setVertexBytes:&u length:sizeof(u) atIndex:0];
            [e setFragmentTexture:view atIndex:0];
            [e setFragmentSamplerState:sampler(1, 1, 3, 3, 3, 0, 1, false) atIndex:0];
            [e drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
        }
        [e endEncoding];
        gpu_pass_report("stretch");
        if (debugging()) debug_dump(&di->second, nullptr, fmt_key("stretchrect(scale) src=%08x", si->second.tex).c_str());
    }

    // ---- shaders, pipelines, states ------------------------------------------------------------
    // ---- shader cache (P7) -----------------------------------------------------------------------------------
    // Compiling a shader's MSL is what stalls a frame at the shader's first use (130-340 ms each when Metal has
    // never seen it; Metal's own memory of them is per folder and not ours to manage). The cache is a directory
    // of Metal libraries compiled OUTSIDE the game by Apple's offline compiler (scripts/shader_cache.sh), named
    // by the hash of the MSL text: loading one takes well under a millisecond. The game only reads libraries and
    // leaves the source of what it lacks (<key>.metal) for the script; nothing is compiled in the background
    // while playing, and a draw is never left out (the user's decision of 2026-10-07 stands).
    static std::string cache_key(const std::string &msl) {
        unsigned char d[CC_SHA256_DIGEST_LENGTH];
        CC_SHA256(msl.data(), (CC_LONG)msl.size(), d);
        char hex[33];
        for (int i = 0; i < 16; i++) snprintf(hex + 2 * i, 3, "%02x", d[i]);
        return hex;
    }
    id<MTLLibrary> cached_library(const std::string &key) {
        std::string path = cache_dir_ + "/" + key + ".metallib";
        if (access(path.c_str(), R_OK) != 0) return nil;
        NSError *err = nil;
        id<MTLLibrary> lib = [dev_ newLibraryWithURL:[NSURL fileURLWithPath:@(path.c_str())] error:&err];
        if (!lib) problem("shader cache: a library could not be loaded");
        return lib;
    }
    void leave_source(const std::string &key, const std::string &msl) {
        std::string base = cache_dir_ + "/" + key;
        if (access((base + ".metal").c_str(), F_OK) == 0 || access((base + ".metallib").c_str(), F_OK) == 0) return;
        std::string tmp = base + fmt_key(".tmp%d", (int)getpid());
        if (FILE *f = fopen(tmp.c_str(), "w")) {
            bool ok = fwrite(msl.data(), 1, msl.size(), f) == msl.size();
            ok = fclose(f) == 0 && ok;
            if (!ok || rename(tmp.c_str(), (base + ".metal").c_str()) != 0) remove(tmp.c_str());
        }
    }
    // The game created a shader (Options::cache_created): its source goes to the cache directory unless a library
    // or the source is there already. Translation only (~0.04 ms); nothing is compiled.
    void note_created(Shader &s) {
        auto t0 = std::chrono::steady_clock::now();
        sm3::Result res = sm3::translate(s.code, {});
        ns_translate_ += since(t0);
        if (res.ok) leave_source(cache_key(res.msl), res.msl);
    }

    id<MTLFunction> function(Shader &s, uint32_t shadow_mask) {
        if (s.failed) return nil;
        auto it = s.fn.find(shadow_mask);
        if (it != s.fn.end()) return it->second;
        sm3::Options o;
        o.shadow_samplers = shadow_mask;
        auto t0 = std::chrono::steady_clock::now();
        sm3::Result res = sm3::translate(s.code, o);
        ns_translate_ += since(t0);
        if (!res.ok) {
            s.failed = true;
            problem("shader translation: " + res.error);
            return nil;
        }
        NSError *err = nil;
        t0 = std::chrono::steady_clock::now();
        std::string key = cache_dir_.empty() ? std::string() : cache_key(res.msl);
        id<MTLLibrary> lib = key.empty() ? nil : cached_library(key);
        const bool cached = lib != nil;
        if (cached) precompiled_used_++;
        else {
            lib = [dev_ newLibraryWithSource:@(res.msl.c_str()) options:nil error:&err];
            if (lib && !key.empty()) leave_source(key, res.msl);
        }
        id<MTLFunction> f = lib ? [lib newFunctionWithName:@"main0"] : nil;
        ns_library_ += since(t0);
        if (!f) {
            s.failed = true;
            problem(std::string("shader compile: ") + (err ? [[err.localizedDescription componentsSeparatedByString:@"\n"][0] UTF8String] : "?"));
            return nil;
        }
        if (!s.have_meta) {
            s.meta = std::move(res);
            s.meta.msl.clear();
            s.have_meta = true;
        }
        functions_compiled_++;
        // BO1_SHADER_LOG=1: every compile that happened at first use, with how long the shader had existed
        static const bool shader_log = getenv("BO1_SHADER_LOG") != nullptr;
        if (shader_log && !cached)
            fprintf(stderr, "shader compiled at first use: %s, shadow mask %x, created in frame %d, used in frame %d, %.1f ms\n", s.vs ? "vs" : "ps", shadow_mask,
                    s.created_frame, frame_, (double)since(t0) / 1e6);
        return s.fn[shadow_mask] = f;
    }

    static MTLVertexFormat vertex_format(uint8_t type) {
        switch (type) {
        case 0: return MTLVertexFormatFloat;
        case 1: return MTLVertexFormatFloat2;
        case 2: return MTLVertexFormatFloat3;
        case 3: return MTLVertexFormatFloat4;
        case 4: return MTLVertexFormatUChar4Normalized_BGRA;
        case 5: return MTLVertexFormatUChar4;
        case 6: return MTLVertexFormatShort2;
        case 7: return MTLVertexFormatShort4;
        case 8: return MTLVertexFormatUChar4Normalized;
        case 9: return MTLVertexFormatShort2Normalized;
        case 10: return MTLVertexFormatShort4Normalized;
        case 11: return MTLVertexFormatUShort2Normalized;
        case 12: return MTLVertexFormatUShort4Normalized;
        case 14: return MTLVertexFormatInt1010102Normalized;
        case 15: return MTLVertexFormatHalf2;
        case 16: return MTLVertexFormatHalf4;
        default: return MTLVertexFormatInvalid;
        }
    }
    static MTLBlendFactor blend_factor(uint32_t f) {
        switch (f) {
        case 1: return MTLBlendFactorZero;
        case 2: return MTLBlendFactorOne;
        case 3: return MTLBlendFactorSourceColor;
        case 4: return MTLBlendFactorOneMinusSourceColor;
        case 5: return MTLBlendFactorSourceAlpha;
        case 6: return MTLBlendFactorOneMinusSourceAlpha;
        case 7: return MTLBlendFactorDestinationAlpha;
        case 8: return MTLBlendFactorOneMinusDestinationAlpha;
        case 9: return MTLBlendFactorDestinationColor;
        case 10: return MTLBlendFactorOneMinusDestinationColor;
        case 11: return MTLBlendFactorSourceAlphaSaturated;
        case 14: return MTLBlendFactorBlendColor;
        case 15: return MTLBlendFactorOneMinusBlendColor;
        default: return MTLBlendFactorOne;
        }
    }
    static MTLBlendOperation blend_op(uint32_t o) {
        switch (o) {
        case 2: return MTLBlendOperationSubtract;
        case 3: return MTLBlendOperationReverseSubtract;
        case 4: return MTLBlendOperationMin;
        case 5: return MTLBlendOperationMax;
        default: return MTLBlendOperationAdd;
        }
    }

    id<MTLRenderPipelineState> pipeline(Shader &vs, Shader &ps, id<MTLFunction> vf, id<MTLFunction> pf, const std::vector<Elem> &decl) {
        bool blend = rs_[27] != 0, sep = rs_[206] != 0;
        const Key<19> key = {{cur_vs_, cur_ps_, (uint64_t)(uintptr_t)(__bridge void *)pf, cur_decl_, (uint64_t)pass_color_, (uint64_t)pass_depth_, blend,
                              rs_[19], rs_[20], rs_[171], sep, rs_[207], rs_[208], rs_[209], rs_[168], streams_[0].stride, streams_[1].stride,
                              streams_[2].stride, streams_[3].stride}};
        if (last_pipeline_.same(key)) return last_pipeline_.value;
        auto it = pipelines_.find(key);
        if (it != pipelines_.end()) return last_pipeline_.set(key, it->second);
        (void)ps;
        MTLRenderPipelineDescriptor *d = [MTLRenderPipelineDescriptor new];
        d.vertexFunction = vf;
        d.fragmentFunction = pf;
        MTLVertexDescriptor *vd = [MTLVertexDescriptor vertexDescriptor];
        bool need_dummy = false;
        for (const sm3::Semantic &in : vs.meta.inputs) {
            const Elem *found = nullptr;
            for (const Elem &e : decl)
                if (e.usage == in.usage && e.uidx == in.index) found = &e;
            MTLVertexFormat f = found ? vertex_format(found->type) : MTLVertexFormatInvalid;
            if (found && f != MTLVertexFormatInvalid && found->stream < 16) {
                vd.attributes[in.reg].format = f;
                vd.attributes[in.reg].offset = found->offset;
                vd.attributes[in.reg].bufferIndex = found->stream;
                vd.layouts[found->stream].stride = std::max<uint32_t>(streams_[found->stream].stride, 4);
                vd.layouts[found->stream].stepFunction = MTLVertexStepFunctionPerVertex;
            } else { // the declaration has no such element: D3D reads (0,0,0,1)
                vd.attributes[in.reg].format = MTLVertexFormatFloat4;
                vd.attributes[in.reg].offset = 0;
                vd.attributes[in.reg].bufferIndex = 16;
                need_dummy = true;
            }
        }
        if (need_dummy) {
            vd.layouts[16].stride = 16;
            vd.layouts[16].stepFunction = MTLVertexStepFunctionConstant;
            vd.layouts[16].stepRate = 0;
        }
        d.vertexDescriptor = vd;
        if (pass_color_ != MTLPixelFormatInvalid) {
            MTLRenderPipelineColorAttachmentDescriptor *c = d.colorAttachments[0];
            c.pixelFormat = pass_color_;
            c.writeMask = mtl_write_mask(rs_[168]);
            c.blendingEnabled = blend;
            if (blend) {
                c.sourceRGBBlendFactor = blend_factor(rs_[19]);
                c.destinationRGBBlendFactor = blend_factor(rs_[20]);
                c.rgbBlendOperation = blend_op(rs_[171]);
                c.sourceAlphaBlendFactor = blend_factor(sep ? rs_[207] : rs_[19]);
                c.destinationAlphaBlendFactor = blend_factor(sep ? rs_[208] : rs_[20]);
                c.alphaBlendOperation = blend_op(sep ? rs_[209] : rs_[171]);
            }
        }
        if (pass_depth_ != MTLPixelFormatInvalid) {
            d.depthAttachmentPixelFormat = pass_depth_;
            d.stencilAttachmentPixelFormat = pass_depth_;
        }
        NSError *err = nil;
        auto t0 = std::chrono::steady_clock::now();
        id<MTLRenderPipelineState> p = [dev_ newRenderPipelineStateWithDescriptor:d error:&err];
        ns_pipeline_ += since(t0);
        pipelines_created_++;
        if (!p) problem(std::string("pipeline: ") + (err ? [[err.localizedDescription componentsSeparatedByString:@"\n"][0] UTF8String] : "?"));
        for (uint32_t id : {cur_vs_, cur_ps_, cur_decl_}) pipeline_users_[id].push_back(key);
        return pipelines_[key] = p;
    }

    id<MTLDepthStencilState> depth_state() {
        bool z = rs_[7] != 0, stencil = rs_[52] != 0, two = rs_[185] != 0;
        const Key<15> key = {{z, rs_[14], rs_[23], stencil, rs_[53], rs_[54], rs_[55], rs_[56], rs_[58], rs_[59], two, rs_[186], rs_[187], rs_[188], rs_[189]}};
        if (last_depth_.same(key)) return last_depth_.value;
        auto it = depth_states_.find(key);
        if (it != depth_states_.end()) return last_depth_.set(key, it->second);
        MTLDepthStencilDescriptor *d = [MTLDepthStencilDescriptor new];
        d.depthCompareFunction = z ? (MTLCompareFunction)(std::clamp<uint32_t>(rs_[23], 1, 8) - 1) : MTLCompareFunctionAlways;
        d.depthWriteEnabled = z && rs_[14];
        if (stencil) {
            auto face = [&](uint32_t fail, uint32_t zfail, uint32_t pass, uint32_t func) {
                MTLStencilDescriptor *s = [MTLStencilDescriptor new];
                s.stencilFailureOperation = (MTLStencilOperation)(std::clamp<uint32_t>(fail, 1, 8) - 1);
                s.depthFailureOperation = (MTLStencilOperation)(std::clamp<uint32_t>(zfail, 1, 8) - 1);
                s.depthStencilPassOperation = (MTLStencilOperation)(std::clamp<uint32_t>(pass, 1, 8) - 1);
                s.stencilCompareFunction = (MTLCompareFunction)(std::clamp<uint32_t>(func, 1, 8) - 1);
                s.readMask = rs_[58] & 0xFF;
                s.writeMask = rs_[59] & 0xFF;
                return s;
            };
            d.frontFaceStencil = face(rs_[53], rs_[54], rs_[55], rs_[56]); // clockwise = D3D "front"
            d.backFaceStencil = two ? face(rs_[186], rs_[187], rs_[188], rs_[189]) : d.frontFaceStencil;
        }
        return depth_states_[key] = [dev_ newDepthStencilStateWithDescriptor:d];
    }

    id<MTLSamplerState> sampler(uint32_t u, uint32_t v, uint32_t w, uint32_t mag, uint32_t min, uint32_t mip, uint32_t aniso, bool compare) {
        const Key<8> key = {{u, v, w, mag, min, mip, aniso, compare}};
        Last<8> &last = last_sampler_[(u * 31 + v * 7 + mag * 5 + min * 3 + mip + aniso + compare) & 7];
        if (last.same(key)) return last.value;
        auto it = samplers_.find(key);
        if (it != samplers_.end()) return last.set(key, it->second);
        auto addr = [](uint32_t a) {
            switch (a) {
            case 2: return MTLSamplerAddressModeMirrorRepeat;
            case 3: return MTLSamplerAddressModeClampToEdge;
            case 4: return MTLSamplerAddressModeClampToBorderColor;
            case 5: return MTLSamplerAddressModeMirrorClampToEdge;
            default: return MTLSamplerAddressModeRepeat;
            }
        };
        MTLSamplerDescriptor *d = [MTLSamplerDescriptor new];
        d.sAddressMode = addr(u);
        d.tAddressMode = addr(v);
        d.rAddressMode = addr(w);
        d.magFilter = mag >= 2 ? MTLSamplerMinMagFilterLinear : MTLSamplerMinMagFilterNearest;
        d.minFilter = min >= 2 ? MTLSamplerMinMagFilterLinear : MTLSamplerMinMagFilterNearest;
        d.mipFilter = mip == 0 ? MTLSamplerMipFilterNotMipmapped : mip == 1 ? MTLSamplerMipFilterNearest : MTLSamplerMipFilterLinear;
        d.maxAnisotropy = (min == 3 || mag == 3) ? std::clamp<uint32_t>(aniso, 1, 16) : 1;
        if (compare) d.compareFunction = MTLCompareFunctionLessEqual;
        return samplers_[key] = [dev_ newSamplerStateWithDescriptor:d];
    }

    // Per-draw constants: bump-allocated from chunks that live until the command buffer is done.
    // `used` = how many float registers the shader can read (sm3::Result::float_consts_used; 0 = all): only
    // those and the small tail (integer / bool registers, extras) are copied — the full block is 4.4 KiB and
    // copying it twice per draw was the largest single cost on the render thread (T-058 profile).
    void bind_consts(Consts &c, bool vertex, uint32_t used) {
        if (!used || used > sm3::kMaxFloatConsts) used = sm3::kMaxFloatConsts;
        ConstCopy &last = const_copy_[vertex ? 0 : 1];
        // The copy made for an earlier draw still holds what this shader reads: nothing was set since, and it
        // covered at least as many registers. Then only the encoder needs to know (once per pass).
        if (!last.buf || last.dirty || used > last.used) {
            const NSUInteger need = (sizeof(Consts) + 255) & ~(NSUInteger)255;
            if (const_chunks_.empty() || const_off_ + need > const_chunks_.back().length) {
                if (chunks_free_.empty()) const_chunks_.push_back([dev_ newBufferWithLength:need * 512 options:MTLResourceStorageModeShared]);
                else {
                    const_chunks_.push_back(chunks_free_.back());
                    chunks_free_.pop_back();
                }
                const_off_ = 0;
            }
            id<MTLBuffer> b = const_chunks_.back();
            uint8_t *dst = (uint8_t *)b.contents + const_off_;
            if (used == sm3::kMaxFloatConsts) memcpy(dst, &c, sizeof(Consts));
            else {
                memcpy(dst, &c, (size_t)used * 16);
                memcpy(dst + offsetof(Consts, i), &c.i, sizeof(Consts) - offsetof(Consts, i));
            }
            last = {b, const_off_, used, false, false};
            const_off_ += need;
        }
        if (last.on_encoder) return;
        if (vertex) [enc_ setVertexBuffer:last.buf offset:last.off atIndex:sm3::kConstBufferIndex];
        else [enc_ setFragmentBuffer:last.buf offset:last.off atIndex:sm3::kConstBufferIndex];
        last.on_encoder = true;
    }

    // ---- draw submission (P6) -------------------------------------------------------------------------------
    // The state setters only record; whatever is expensive to work out is flagged and worked out by the next draw.
public:
    void set_render_state(uint32_t state, uint32_t value) override {
        if (state >= rs_.size() || rs_[state] == value) return;
        rs_[state] = value;
        dirty_ |= rs_dirty_[state];
    }
    void set_texture(uint32_t stage, uint32_t texture) override {
        if (stage >= 16 || bound_tex_[stage] == texture) return;
        bound_tex_[stage] = texture;
        tex_dirty_ |= (uint16_t)(1u << stage);
    }
    void set_sampler_state(uint32_t stage, uint32_t type, uint32_t value) override {
        if (stage >= 16 || type >= 14 || samp_[stage][type] == value) return;
        samp_[stage][type] = value;
        samp_dirty_ |= (uint16_t)(1u << stage);
    }
    void set_vertex_declaration(uint32_t decl) override {
        if (cur_decl_ == decl) return;
        cur_decl_ = decl;
        dirty_ |= kDirtyObjects | kDirtyPipeline;
    }
    void set_shader(bool vertex, uint32_t shader) override {
        uint32_t &cur = vertex ? cur_vs_ : cur_ps_;
        if (cur == shader) return;
        cur = shader;
        dirty_ |= kDirtyObjects | kDirtyFunctions | kDirtyPipeline;
    }
    void set_shader_constants(bool vertex, uint32_t start, const void *data, uint32_t count) override {
        if (start >= sm3::kMaxFloatConsts || !data) return;
        Consts &c = vertex ? vsc_ : psc_;
        memcpy(c.c[start], data, (size_t)std::min<uint32_t>(count, sm3::kMaxFloatConsts - start) * 16);
        const_copy_[vertex ? 0 : 1].dirty = true;
    }
    void set_stream_source(uint32_t stream, uint32_t buffer, uint32_t offset, uint32_t stride) override {
        if (stream >= 16) return;
        Stream &s = streams_[stream];
        if (s.vb != buffer) stream_dirty_ |= (uint16_t)(1u << stream);
        if (s.stride != stride) dirty_ |= kDirtyPipeline; // the vertex layout is part of the pipeline
        s = {buffer, offset, stride};
        stream_mask_ = buffer ? stream_mask_ | (uint16_t)(1u << stream) : stream_mask_ & (uint16_t)~(1u << stream);
    }
    void set_indices(uint32_t buffer) override {
        if (cur_ib_ == buffer) return;
        cur_ib_ = buffer;
        ib_dirty_ = true;
    }
    void write_buffer(uint32_t buffer, uint32_t lock_flags, uint32_t offset, const void *data, uint32_t size) override {
        buffer_written(buffer, lock_flags, offset, size, data ? 1 : 0, data, data ? size : 0);
    }
    void draw_indexed(uint32_t type, int32_t base_vertex, uint32_t min_index, uint32_t num_vertices, uint32_t start_index, uint32_t prim_count) override {
        if (!rendering(frame_)) return;
        const char *why;
        // Only a draw that starts a pass or needs a state object looked up (or made) can leave autoreleased
        // objects behind; the others, most of a frame, do without a pool.
        if (!enc_ || (dirty_ & (kDirtyObjects | kDirtyFunctions | kDirtyPipeline | kDirtyDepth))) {
            @autoreleasepool {
                why = draw(type, base_vertex, min_index, num_vertices, start_index, prim_count);
            }
        } else
            why = draw(type, base_vertex, min_index, num_vertices, start_index, prim_count);
        if (why) {
            draws_skipped_++;
            problem(std::string("draw skipped: ") + why);
        }
    }

private:
    Buf *find_buffer(uint32_t id) {
        auto it = buffers_.find(id);
        return it == buffers_.end() ? nullptr : &it->second;
    }
    // Which objects the bound shader / declaration ids mean.
    void resolve_objects() {
        dirty_ &= ~kDirtyObjects;
        auto vi = shaders_.find(cur_vs_), pi = shaders_.find(cur_ps_);
        auto di = decls_.find(cur_decl_);
        vs_p_ = vi == shaders_.end() ? nullptr : &vi->second;
        ps_p_ = pi == shaders_.end() ? nullptr : &pi->second;
        decl_p_ = di == decls_.end() ? nullptr : &di->second;
        objects_missing_ = !vs_p_ || !ps_p_ ? "no shader bound" : !decl_p_ ? "no vertex declaration" : nullptr;
    }
    // The bound shaders' Metal functions (compiled at first use) and the stages the pixel shader samples.
    void resolve_functions() {
        dirty_ &= ~kDirtyFunctions;
        dirty_ |= kDirtyPipeline;
        functions_missing_ = nullptr;
        vf_ = pf_ = nil;
        pf0_ = function(*ps_p_, 0);
        if (!pf0_) {
            functions_missing_ = "pixel shader unavailable";
            return;
        }
        ps_samplers_ = 0;
        for (const sm3::SamplerDecl &s : ps_p_->meta.samplers)
            if (s.reg < 16) ps_samplers_ |= (uint16_t)(1u << s.reg);
        // every stage is looked at again: what a stage needs (texture kind, comparison) depends on the shader
        tex_dirty_ = 0xFFFF;
        shadow_ = 0;
        pf_ = pf0_;
        vf_ = function(*vs_p_, 0);
    }
    // The stages in `todo` (sampled by the bound pixel shader, texture or sampler state changed): which Metal
    // texture and sampler each means, told to the encoder if it differs from what it has. Samplers bound to
    // depth textures are sampled with comparison (hardware shadow maps): that selects the shader variant.
    void apply_stages(Shader &ps, uint16_t todo) {
        uint32_t shadow = shadow_;
        for (const sm3::SamplerDecl &s : ps.meta.samplers) {
            const uint32_t reg = s.reg, bit = 1u << reg;
            if (reg >= 16 || !(todo & bit)) continue;
            Tex *t = (tex_dirty_ & bit) ? (tex_p_[reg] = tex(bound_tex_[reg])) : tex_p_[reg];
            bool is_shadow = t && t->depth && s.type == 2;
            shadow = is_shadow ? shadow | bit : shadow & ~bit;
            MTLTextureType want = s.type == 3 ? MTLTextureTypeCube : s.type == 4 ? MTLTextureType3D : MTLTextureType2D;
            bool usable = t && t->view && t->type() == want;
            id<MTLTexture> mt = usable ? t->view : fallback_texture(want, is_shadow);
            if (usable) t->used_gen = gen_;
            bad_tex_ = !usable && bound_tex_[reg] ? bad_tex_ | (uint16_t)bit : bad_tex_ & (uint16_t)~bit;
            unrec_tex_ = t && t->unrecorded ? unrec_tex_ | (uint16_t)bit : unrec_tex_ & (uint16_t)~bit;
            if (void *p = (__bridge void *)mt; ap_.tex[reg] != p) {
                [enc_ setFragmentTexture:mt atIndex:reg];
                ap_.tex[reg] = p;
            }
            const auto &st = samp_[reg];
            id<MTLSamplerState> ss = sampler(st[1], st[2], st[3], st[5], st[6], st[7], st[10], is_shadow);
            if (void *p = (__bridge void *)ss; ap_.samp[reg] != p) {
                [enc_ setFragmentSamplerState:ss atIndex:reg];
                ap_.samp[reg] = p;
            }
        }
        tex_dirty_ &= (uint16_t)~todo;
        samp_dirty_ &= (uint16_t)~todo;
        if (shadow != shadow_) {
            shadow_ = shadow;
            pf_ = shadow ? function(ps, shadow) : pf0_;
            dirty_ |= kDirtyPipeline;
        }
    }

    // DrawIndexedPrimitive(Type, BaseVertexIndex, MinVertexIndex, NumVertices, StartIndex, PrimCount).
    // Returns why the draw was skipped, or null.
    const char *draw(uint32_t type, int32_t base_vertex, uint32_t min_index, uint32_t num_vertices, uint32_t start_index, uint32_t prim_count) {
        if (type != 4) return "primitive type";
        if (dirty_ & kDirtyObjects) resolve_objects();
        if (objects_missing_) return objects_missing_;
        if (ib_dirty_) ib_p_ = find_buffer(cur_ib_), ib_dirty_ = false;
        if (!ib_p_) return "no index buffer";
        if (!begin_pass()) return "no render target";
        if (dirty_ & kDirtyFunctions) resolve_functions();
        if (functions_missing_) return functions_missing_;
        Shader &vs = *vs_p_, &ps = *ps_p_;
        if (uint16_t todo = (tex_dirty_ | samp_dirty_) & ps_samplers_) apply_stages(ps, todo);
        if (!vf_ || !pf_) return "shader unavailable";
        if (pass_color_ == MTLPixelFormatInvalid && pass_depth_ == MTLPixelFormatInvalid) return "no attachments";
        if (dirty_ & kDirtyPipeline) {
            cur_pipeline_ = pipeline(vs, ps, vf_, pf_, *decl_p_);
            dirty_ &= ~kDirtyPipeline;
        }
        if (!cur_pipeline_) return "pipeline creation failed";

        if (void *p = (__bridge void *)cur_pipeline_; ap_.pipeline != p) {
            [enc_ setRenderPipelineState:cur_pipeline_];
            ap_.pipeline = p;
        }
        if (pass_depth_ != MTLPixelFormatInvalid) {
            if (dirty_ & kDirtyDepth) {
                cur_depth_ = depth_state();
                dirty_ &= ~kDirtyDepth;
            }
            if (void *p = (__bridge void *)cur_depth_; ap_.depth != p) {
                [enc_ setDepthStencilState:cur_depth_];
                ap_.depth = p;
            }
        }
        float bias, slope;
        memcpy(&bias, &rs_[195], 4);
        memcpy(&slope, &rs_[175], 4);
        const uint32_t ref = rs_[57] & 0xFF, cull = rs_[22];
        if (!ap_.raster || ap_.stencil_ref != ref) [enc_ setStencilReferenceValue:ref];
        if (!ap_.raster || ap_.cull != cull) [enc_ setCullMode:cull == 3 ? MTLCullModeBack : cull == 2 ? MTLCullModeFront : MTLCullModeNone];
        if (!ap_.raster || memcmp(&ap_.bias, &bias, 4) || memcmp(&ap_.slope, &slope, 4)) [enc_ setDepthBias:bias * 16777216.0f slopeScale:slope clamp:0];
        ap_.stencil_ref = ref, ap_.cull = cull, ap_.bias = bias, ap_.slope = slope, ap_.raster = true;
        if (dirty_ & kDirtyViewport) { // stays flagged while there is nothing to draw into
            viewport_bad_ = nullptr;
            if (viewport_.width <= 0 || viewport_.height <= 0) viewport_bad_ = "empty viewport";
            else {
                cur_scissor_ = rs_[174] ? clamp_scissor(scissor_) : viewport_rect();
                if (!cur_scissor_.width || !cur_scissor_.height) viewport_bad_ = "empty scissor";
            }
            if (!viewport_bad_) {
                [enc_ setViewport:viewport_];
                [enc_ setScissorRect:cur_scissor_];
                dirty_ &= ~kDirtyViewport;
            }
        }
        if (viewport_bad_) return viewport_bad_;

        for (uint32_t m = stream_mask_; m; m &= m - 1) {
            const uint32_t s = (uint32_t)__builtin_ctz(m);
            if (stream_dirty_ & (1u << s)) stream_buf_[s] = find_buffer(streams_[s].vb);
            Buf *b = stream_buf_[s];
            if (!b) continue;
            b->used_gen = gen_;
            if (!b->unrecorded.empty()) {
                // DrawIndexedPrimitive reads vertices [Base + MinIndex, Base + MinIndex + NumVertices).
                uint64_t first = (uint64_t)streams_[s].offset + (uint64_t)(int64_t)(base_vertex + (int64_t)min_index) * streams_[s].stride;
                if (b->missing(first, first + (uint64_t)num_vertices * streams_[s].stride)) unrecorded("vertex buffer", streams_[s].vb);
            }
            const NSUInteger off = std::min(streams_[s].offset, b->size);
            if (void *p = (__bridge void *)b->buf; ap_.vb[s] != p) {
                [enc_ setVertexBuffer:b->buf offset:off atIndex:s];
                ap_.vb[s] = p, ap_.vb_off[s] = off;
            } else if (ap_.vb_off[s] != off) {
                [enc_ setVertexBufferOffset:off atIndex:s];
                ap_.vb_off[s] = off;
            }
        }
        stream_dirty_ = 0;
        if (uint16_t m = (uint16_t)((bad_tex_ | unrec_tex_) & ps_samplers_)) // diagnostics, counted per draw
            for (uint32_t reg = 0; reg < 16; reg++) {
                if ((bad_tex_ & m) >> reg & 1) problem("sampler bound to a texture of the wrong type or unknown texture");
                if ((unrec_tex_ & m) >> reg & 1) unrecorded("texture", bound_tex_[reg]);
            }
        const float ve[4] = {(float)(1.0 / viewport_.width), (float)(-1.0 / viewport_.height), 0, 0};
        const float pe[4] = {(rs_[24] & 0xFF) / 255.0f, rs_[15] ? (float)rs_[25] : 0.0f, 0, 0};
        if (memcmp(vsc_.extra, ve, sizeof(ve))) memcpy(vsc_.extra, ve, sizeof(ve)), const_copy_[0].dirty = true;
        if (memcmp(psc_.extra, pe, sizeof(pe))) memcpy(psc_.extra, pe, sizeof(pe)), const_copy_[1].dirty = true;
        bind_consts(vsc_, true, vs.meta.uses_relative_consts ? 0 : vs.meta.float_consts_used);
        bind_consts(psc_, false, ps.meta.uses_relative_consts ? 0 : ps.meta.float_consts_used);

        Buf &index = *ib_p_;
        index.used_gen = gen_;
        bool i32 = index.index_fmt == FMT_INDEX32;
        NSUInteger isz = i32 ? 4 : 2, count = (NSUInteger)prim_count * 3, off = (NSUInteger)start_index * isz;
        if (off + count * isz > index.size) return "index range out of buffer";
        if (!index.unrecorded.empty() && index.missing(off, off + count * isz)) unrecorded("index buffer", cur_ib_);
        [enc_ drawIndexedPrimitives:MTLPrimitiveTypeTriangle indexCount:count indexType:i32 ? MTLIndexTypeUInt32 : MTLIndexTypeUInt16
                        indexBuffer:index.buf indexBufferOffset:off instanceCount:1 baseVertex:base_vertex baseInstance:0];
        draws_++;
        pass_draws_++;
        pass_touched_ = true;
        if (debugging()) debug_draw(prim_count, base_vertex, start_index, vs, ps, shadow_);
        return nullptr;
    }

    id<MTLTexture> fallback_texture(MTLTextureType type, bool depth) {
        int key = (int)type * 2 + (depth ? 1 : 0);
        if (fallback_[key]) return fallback_[key];
        MTLTextureDescriptor *d = [MTLTextureDescriptor new];
        d.textureType = type;
        d.pixelFormat = depth ? MTLPixelFormatDepth32Float : MTLPixelFormatRGBA8Unorm;
        d.width = d.height = 1;
        d.usage = MTLTextureUsageShaderRead | (depth ? MTLTextureUsageRenderTarget : 0);
        d.storageMode = depth ? MTLStorageModePrivate : MTLStorageModeShared;
        return fallback_[key] = [dev_ newTextureWithDescriptor:d];
    }

    // ---- live window (P5/T-050): the game's back buffer is copied to a CAMetalLayer drawable ----------
    void present_to_window(int frame) {
        Tex *t = tex(kBackBufferTex);
        if (!t || !t->tex) return;
        if (opts_.layer && !layer_) { // the runtime's window (src/platform/window): only the layer is ours to use
            layer_ = (__bridge CAMetalLayer *)opts_.layer;
            layer_.device = dev_;
            layer_.pixelFormat = t->tex.pixelFormat;
            layer_.framebufferOnly = NO;
            layer_.drawableSize = CGSizeMake(t->w, t->h);
        }
        if (!opts_.layer && !window_) {
            if (!SDL_Init(SDL_INIT_VIDEO)) {
                fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
                opts_.window = false;
                return;
            }
            window_ = SDL_CreateWindow("BO1 Metal replay", (int)t->w, (int)t->h, SDL_WINDOW_METAL);
            view_ = window_ ? SDL_Metal_CreateView(window_) : nullptr;
            if (!view_) {
                fprintf(stderr, "cannot create the window: %s\n", SDL_GetError());
                opts_.window = false;
                return;
            }
            layer_ = (__bridge CAMetalLayer *)SDL_Metal_GetLayer(view_);
            layer_.device = dev_;
            layer_.pixelFormat = t->tex.pixelFormat;
            layer_.framebufferOnly = NO; // the drawable is a blit destination
            layer_.drawableSize = CGSizeMake(t->w, t->h);
            next_present_ = SDL_GetTicksNS();
        }
        if (layer_ && ((uint32_t)layer_.drawableSize.width != t->w || (uint32_t)layer_.drawableSize.height != t->h))
            layer_.drawableSize = CGSizeMake(t->w, t->h); // the back buffer changed size (Reset / a new device)
        SDL_Event ev;
        while (!opts_.layer && SDL_PollEvent(&ev))
            if (ev.type == SDL_EVENT_QUIT || (ev.type == SDL_EVENT_KEY_DOWN && ev.key.key == SDLK_ESCAPE)) quit_ = true;
        auto t_drawable = std::chrono::steady_clock::now();
        id<CAMetalDrawable> drawable = [layer_ nextDrawable];
        ns_drawable_ += since(t_drawable);
        if (drawable && drawable.texture.pixelFormat == t->tex.pixelFormat && drawable.texture.width == t->w && drawable.texture.height == t->h) {
            id<MTLCommandBuffer> cb = [queue_ commandBuffer];
            id<MTLBlitCommandEncoder> blit = [cb blitCommandEncoder];
            [blit copyFromTexture:t->tex sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(t->w, t->h, 1)
                        toTexture:drawable.texture destinationSlice:0 destinationLevel:0 destinationOrigin:MTLOriginMake(0, 0, 0)];
            [blit endEncoding];
            [cb presentDrawable:drawable];
            commit(cb);
            frames_shown_++;
        } else if (!warned_window_) {
            warned_window_ = true;
            fprintf(stderr, "window: drawable does not match the back buffer (format %lu vs %lu); frame not shown\n",
                    (unsigned long)(drawable ? drawable.texture.pixelFormat : 0), (unsigned long)t->tex.pixelFormat);
        }
        if (opts_.layer) return; // window title and pacing belong to the game
        char title[96];
        snprintf(title, sizeof(title), "BO1 Metal replay — frame %d", frame);
        SDL_SetWindowTitle(window_, title);
        if (opts_.fps > 0) { // pace the replay like the game's presents
            next_present_ += 1000000000ull / (uint64_t)opts_.fps;
            uint64_t now = SDL_GetTicksNS();
            if (next_present_ > now) SDL_DelayNS(next_present_ - now);
            else next_present_ = now;
        }
    }

    void dump_backbuffer(int frame) {
        Tex *t = tex(kBackBufferTex);
        if (!t || !t->tex) return;
        std::vector<uint8_t> px((size_t)t->w * t->h * 4);
        [t->tex getBytes:px.data() bytesPerRow:t->w * 4 fromRegion:MTLRegionMake2D(0, 0, t->w, t->h) mipmapLevel:0];
        for (size_t i = 3; i < px.size(); i += 4) px[i] = 255; // the window shows colour only
        CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
        CGContextRef ctx = CGBitmapContextCreate(px.data(), t->w, t->h, 8, t->w * 4, cs, (CGBitmapInfo)kCGImageAlphaNoneSkipFirst | kCGBitmapByteOrder32Little);
        CGImageRef img = CGBitmapContextCreateImage(ctx);
        NSString *path = [NSString stringWithFormat:@"%s/frame_%06d.png", opts_.dump_dir.c_str(), frame];
        CGImageDestinationRef dst = CGImageDestinationCreateWithURL((__bridge CFURLRef)[NSURL fileURLWithPath:path], CFSTR("public.png"), 1, nullptr);
        if (dst) {
            CGImageDestinationAddImage(dst, img, nullptr);
            CGImageDestinationFinalize(dst);
            CFRelease(dst);
        }
        CGImageRelease(img);
        CGContextRelease(ctx);
        CGColorSpaceRelease(cs);
        printf("dumped %s\n", path.UTF8String);
    }

    Options opts_;
    int debug_frame_ = -1, debug_pass_ = 0;
    bool pass_touched_ = false;
    std::string debug_dir_;
    FILE *debug_log_ = nullptr;
    std::set<uint64_t> debug_shaders_;
    SDL_Window *window_ = nullptr;
    SDL_MetalView view_ = nullptr;
    CAMetalLayer *layer_ = nil;
    bool quit_ = false, warned_window_ = false;
    uint64_t next_present_ = 0;
    int frames_shown_ = 0;
    id<MTLDevice> dev_ = nil;
    id<MTLCommandQueue> queue_ = nil;
    id<MTLLibrary> internal_ = nil;
    id<MTLCommandBuffer> cmd_ = nil;
    id<MTLRenderCommandEncoder> enc_ = nil;
    id<MTLBuffer> dummy_vb_ = nil;
    MTLPixelFormat pass_color_ = MTLPixelFormatInvalid, pass_depth_ = MTLPixelFormatInvalid;
    NSUInteger pass_w_ = 1, pass_h_ = 1;
    uint64_t gen_ = 1;
    int frame_ = 0, frames_rendered_ = 0;
    uint64_t draws_ = 0, draws_skipped_ = 0;
    size_t functions_compiled_ = 0;
    uint64_t ns_translate_ = 0, ns_library_ = 0, ns_pipeline_ = 0; // where shader start-up time goes (T-036)
    uint64_t precompiled_used_ = 0, ns_drawable_ = 0, ns_gpu_wait_ = 0, ns_upload_ = 0, pipelines_created_ = 0, uploads_ = 0, upload_bytes_ = 0; // Work (T-058)
    std::string cache_dir_; // shader cache directory; empty = off
    std::atomic<uint64_t> ns_gpu_{0};
    int gpu_passes_frame_ = getenv("BO1_GPU_PASSES") ? atoi(getenv("BO1_GPU_PASSES")) : -1, gpu_pass_n_ = 0;
    uint32_t pass_draws_ = 0;
    static uint64_t since(std::chrono::steady_clock::time_point t0) {
        return (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count();
    }

    std::unordered_map<uint32_t, Tex> textures_;
    std::unordered_map<uint32_t, Surface> surfaces_;
    std::unordered_map<uint32_t, Buf> buffers_;
    std::unordered_map<uint32_t, Shader> shaders_;
    std::unordered_map<uint32_t, std::vector<Elem>> decls_;
    id<MTLCommandBuffer> prev_frame_cmd_ = nil;
    std::unordered_map<Key<19>, id<MTLRenderPipelineState>, KeyHash<19>> pipelines_;
    std::unordered_map<uint32_t, std::vector<Key<19>>> pipeline_users_; // shader / declaration id -> keys in pipelines_
    std::map<std::string, id<MTLRenderPipelineState>> internal_pipelines_; // clear / blit
    std::unordered_map<Key<15>, id<MTLDepthStencilState>, KeyHash<15>> depth_states_;
    id<MTLDepthStencilState> clear_depth_[2][2] = {}; // Clear: [writes depth][writes stencil]
    std::unordered_map<Key<8>, id<MTLSamplerState>, KeyHash<8>> samplers_;
    std::map<int, id<MTLTexture>> fallback_;
    std::vector<id<MTLBuffer>> const_chunks_;                         // read by the command buffer being built
    std::vector<id<MTLBuffer>> chunks_frame_, chunks_prev_, chunks_free_; // by committed ones of this frame / the frame before; reusable
    static constexpr size_t kMaxFreeChunks = 32;
    NSUInteger const_off_ = 0;
    // The last copy of each stage's constants (0 = vertex, 1 = pixel): where it is, how many float registers it
    // holds, whether the game set a constant since, and whether the current pass's encoder already points at it.
    struct ConstCopy {
        id<MTLBuffer> buf = nil;
        NSUInteger off = 0;
        uint32_t used = 0;
        bool dirty = true, on_encoder = false;
    };
    ConstCopy const_copy_[2];
    Last<19> last_pipeline_;
    Last<15> last_depth_;
    Last<8> last_sampler_[8];
    std::map<std::string, int> problems_;
    std::map<std::string, std::map<uint32_t, std::set<int>>> unrecorded_ids_;

    // D3D9 device state
    std::array<uint32_t, 256> rs_{}; // D3DRS_* -> value (the highest state Direct3D 9 defines is 209)
    std::array<std::array<uint32_t, 14>, 16> samp_{};
    std::array<uint32_t, 16> bound_tex_{};
    std::array<Stream, 16> streams_{};
    uint32_t cur_vs_ = 0, cur_ps_ = 0, cur_decl_ = 0, cur_ib_ = 0;
    Surface rt_{}, ds_{};
    bool have_rt_ = false, have_ds_ = false;
    MTLViewport viewport_ = {0, 0, 1, 1, 0, 1};
    MTLScissorRect scissor_ = {0, 0, 1, 1};
    Consts vsc_{}, psc_{};

    // Draw submission (P6): what has to be worked out again before the next draw ...
    enum : uint32_t {
        kDirtyObjects = 1,   // which shaders / declaration the bound ids mean
        kDirtyFunctions = 2, // the bound shaders' Metal functions and what the pixel shader samples
        kDirtyPipeline = 4,  // something the pipeline is made from
        kDirtyDepth = 8,     // a depth / stencil render state
        kDirtyViewport = 16, // viewport, scissor rect, SCISSORTESTENABLE, or the size of the pass
        kDirtyAll = 31,
    };
    uint32_t dirty_ = kDirtyAll;
    uint8_t rs_dirty_[256] = {};                      // render state -> dirty bits it sets when its value changes
    uint16_t tex_dirty_ = 0xFFFF, samp_dirty_ = 0xFFFF; // stages whose texture / sampler state has to be looked at again
    uint16_t stream_dirty_ = 0xFFFF, stream_mask_ = 0;  // streams whose buffer has to be looked up again / that have one
    bool ib_dirty_ = true;
    // ... and what was worked out last time
    Shader *vs_p_ = nullptr, *ps_p_ = nullptr;
    const std::vector<Elem> *decl_p_ = nullptr;
    const char *objects_missing_ = nullptr, *functions_missing_ = nullptr, *viewport_bad_ = nullptr; // why a draw is skipped
    id<MTLFunction> vf_ = nil, pf0_ = nil, pf_ = nil;
    uint16_t ps_samplers_ = 0;               // stages the bound pixel shader samples
    uint16_t bad_tex_ = 0, unrec_tex_ = 0;   // stages bound to a texture of the wrong kind / whose contents the trace lacks
    uint32_t shadow_ = 0;                    // stages sampled with comparison (depth textures)
    Tex *tex_p_[16] = {};
    Buf *stream_buf_[16] = {}, *ib_p_ = nullptr;
    id<MTLRenderPipelineState> cur_pipeline_ = nil;
    id<MTLDepthStencilState> cur_depth_ = nil;
    MTLScissorRect cur_scissor_ = {0, 0, 1, 1};
    // What the current pass's encoder was last told (identities only: the encoder keeps alive what it was
    // given, so an address cannot come back as another object while it is compared here).
    struct Applied {
        void *pipeline, *depth;
        void *vb[16], *tex[16], *samp[16];
        NSUInteger vb_off[16];
        uint32_t stencil_ref, cull;
        float bias, slope;
        bool raster; // stencil_ref / cull / bias / slope are valid
    };
    Applied ap_{};
};

} // namespace

std::unique_ptr<Renderer> make_metal_renderer(const Options &opts) { return std::make_unique<MetalRenderer>(opts); }

} // namespace bo1::rhi
