// Null replay backend: renders nothing, checks that the trace is self-consistent — every object a
// call refers to was created earlier and not yet destroyed, locks pair with unlocks, uploads
// inside the recording window carry data. This is the acceptance test for the trace format.
#include <algorithm>
#include <cinttypes>
#include <cstdio>
#include <map>
#include <unordered_map>
#include <vector>

#include "backend.h"

namespace bo1::replay {
using namespace trace;

namespace {
enum Kind : uint8_t {
    kUnknown, kDevice, kSwapChain, kSurface, kTexture, kVolume, kCube, kVB, kIB, kStateBlock, kVDecl,
    kVShader, kPShader, kQuery, kKindCount
};
const char *const kKindName[kKindCount] = {"unknown", "device", "swapchain", "surface", "texture",
                                           "volume", "cube", "vertexbuffer", "indexbuffer",
                                           "stateblock", "vertexdecl", "vertexshader", "pixelshader", "query"};
// Calls that return a new interface pointer through an out parameter: {iface, slot, word, kind}.
struct OutSpec { uint8_t iface, slot, word; Kind kind; };
const OutSpec kOuts[] = {
    {IFACE_D3D9, 16, 6, kDevice},     {IFACE_DEVICE, 13, 2, kSwapChain}, {IFACE_DEVICE, 14, 2, kSwapChain},
    {IFACE_DEVICE, 18, 4, kSurface},  {IFACE_DEVICE, 23, 7, kTexture},   {IFACE_DEVICE, 24, 8, kVolume},
    {IFACE_DEVICE, 25, 6, kCube},     {IFACE_DEVICE, 26, 5, kVB},        {IFACE_DEVICE, 27, 5, kIB},
    {IFACE_DEVICE, 28, 7, kSurface},  {IFACE_DEVICE, 29, 7, kSurface},   {IFACE_DEVICE, 36, 5, kSurface},
    {IFACE_DEVICE, 38, 2, kSurface},  {IFACE_DEVICE, 40, 1, kSurface},   {IFACE_DEVICE, 59, 2, kStateBlock},
    {IFACE_DEVICE, 61, 1, kStateBlock}, {IFACE_DEVICE, 64, 2, kUnknown}, {IFACE_DEVICE, 86, 2, kVDecl},
    {IFACE_DEVICE, 91, 2, kVShader},  {IFACE_DEVICE, 106, 2, kPShader},  {IFACE_DEVICE, 118, 2, kQuery},
    {IFACE_SWAPCHAIN, 5, 3, kSurface}, {IFACE_TEXTURE, 18, 2, kSurface}, {IFACE_CUBE, 18, 3, kSurface},
    {IFACE_VOLUME, 18, 2, kUnknown},
};
// Device calls that take objects: {slot, word, expected kind (kUnknown = any texture type)}.
struct UseSpec { uint8_t slot, word; Kind kind; };
const UseSpec kUses[] = {
    {65, 2, kUnknown}, {100, 2, kVB},    {104, 1, kIB},      {87, 1, kVDecl},   {92, 1, kVShader},
    {107, 1, kPShader}, {37, 2, kSurface}, {39, 1, kSurface}, {34, 1, kSurface}, {34, 3, kSurface},
};
const Kind kIfaceKind[IFACE_COUNT] = {kUnknown, kDevice,  kSwapChain, kTexture, kCube,  kVolume, kVB,
                                      kIB,      kSurface, kQuery,     kVShader, kPShader, kVDecl};

bool failed(uint32_t hr) { return (hr & 0x80000000u) != 0; }

class NullBackend final : public Backend {
public:
    explicit NullBackend(const Options &o) : opts_(o) {}

    void call(const Record &r, int frame) override {
        calls_++;
        frame_calls_++;
        // 1. `this` must be a live object of the right kind (the IDirect3D9 factory is implicit).
        if (r.iface != IFACE_D3D9) check_use(r, r.arg(0), kIfaceKind[r.iface], "this");
        // 2. object arguments
        if (r.iface == IFACE_DEVICE) {
            for (const UseSpec &u : kUses)
                if (u.slot == r.slot && r.arg(u.word)) check_use(r, r.arg(u.word), u.kind, "argument");
            if (r.slot == 82) draws_++, frame_draws_++, prims_ += r.arg(6);
        }
        // 3. new objects
        for (const OutSpec &o : kOuts) {
            if (o.iface != r.iface || o.slot != r.slot) continue;
            const Blob *b = r.blob(kOut);
            if (!b || b->data.size() != 4) {
                if (r.has_result() && !failed(r.result) && r.arg(o.word)) problem(r, "successful call without an OUT blob");
                continue;
            }
            uint32_t id;
            std::memcpy(&id, b->data.data(), 4);
            if (!id) continue;
            Kind k = o.kind;
            auto it = live_.find(id);
            if (it != live_.end() && o.kind == kUnknown) k = it->second; // GetTexture: keep what we know
            if (it == live_.end()) created_[k]++;
            live_[id] = k;
            if (r.iface == IFACE_DEVICE && (r.slot == 91 || r.slot == 106)) {
                const Blob *code = r.blob(kBytecode);
                if (!code || code->data.size() < 8) problem(r, "shader created without bytecode");
                else shader_bytes_ += code->data.size();
            }
            if (r.iface == IFACE_DEVICE && r.slot == 86 && !r.blob(kElements)) problem(r, "vertex declaration without elements");
        }
        // 4. destruction
        if (r.slot == 2 && r.iface >= IFACE_DEVICE && r.has_result() && r.result == 0) {
            if (live_.erase(r.arg(0))) destroyed_[kIfaceKind[r.iface]]++;
        }
        // 5. locks and uploads
        bool lock = (r.iface == IFACE_TEXTURE || r.iface == IFACE_CUBE || r.iface == IFACE_VOLUME) ? r.slot == 19
                    : (r.iface == IFACE_VB || r.iface == IFACE_IB) ? r.slot == 11
                    : r.iface == IFACE_SURFACE ? r.slot == 13 : false;
        bool unlock = (r.iface == IFACE_TEXTURE || r.iface == IFACE_CUBE || r.iface == IFACE_VOLUME) ? r.slot == 20
                      : (r.iface == IFACE_VB || r.iface == IFACE_IB) ? r.slot == 12
                      : r.iface == IFACE_SURFACE ? r.slot == 14 : false;
        if (lock && r.has_result() && !failed(r.result)) open_locks_[r.arg(0)]++;
        if (unlock) {
            auto it = open_locks_.find(r.arg(0));
            if (it == open_locks_.end() || it->second == 0) unmatched_unlocks_++;
            else if (--it->second == 0) open_locks_.erase(it);
            if (const Blob *lb = r.blob(kLock); lb && lb->data.size() == sizeof(LockDesc)) {
                LockDesc d;
                std::memcpy(&d, lb->data.data(), sizeof(d));
                const Blob *data = r.blob(kData);
                uploads_[d.dumped & 3]++;
                if (d.dumped == 1 || d.dumped == 2) {
                    if (!data || data->data.size() != d.size) problem(r, "upload size does not match its lock description");
                    else upload_bytes_ += d.size;
                } else if (d.dumped == 3 && frame + 1 >= opts_.first_frame && frame + 1 <= opts_.last_frame) {
                    in_window_missing_++;
                }
            }
        }
    }

    void end_frame(int frame) override {
        frames_ = frame;
        if (frame >= opts_.first_frame && frame <= opts_.last_frame) {
            win_frames_++;
            win_calls_ += frame_calls_;
            win_draws_ += frame_draws_;
        }
        max_frame_calls_ = std::max(max_frame_calls_, frame_calls_);
        frame_calls_ = frame_draws_ = 0;
    }

    int report() override {
        std::printf("frames %d   game calls %" PRIu64 "   draws %" PRIu64 "   triangles %" PRIu64 "\n", frames_, calls_, draws_, prims_);
        if (win_frames_)
            std::printf("window: %d frames, %.0f calls/frame, %.0f draws/frame (max calls in any frame: %" PRIu64 ")\n",
                        win_frames_, (double)win_calls_ / win_frames_, (double)win_draws_ / win_frames_, max_frame_calls_);
        std::printf("objects   created / destroyed / live at end\n");
        std::map<Kind, int> live;
        for (auto &[id, k] : live_) live[k]++;
        for (int k = 0; k < kKindCount; k++)
            if (created_[k] || destroyed_[k])
                std::printf("  %-13s %6d / %6d / %6d\n", kKindName[k], created_[k], destroyed_[k], live[(Kind)k]);
        std::printf("shader bytecode %.1f MiB   uploads: %" PRIu64 " with data (%.1f MiB), %" PRIu64 " trimmed, %" PRIu64
                    " without data (read-only/video), %" PRIu64 " skipped outside the window\n",
                    shader_bytes_ / 1048576.0, uploads_[1], upload_bytes_ / 1048576.0, uploads_[2], uploads_[0], uploads_[3]);
        std::printf("checks: %" PRIu64 " unknown-object references, %" PRIu64 " wrong-kind references, %" PRIu64
                    " unmatched unlocks, %" PRIu64 " uploads missing inside the window, %" PRIu64 " other\n",
                    unknown_refs_, wrong_kind_, unmatched_unlocks_, in_window_missing_, other_);
        for (auto &m : first_problems_) std::printf("  %s\n", m.c_str());
        return (int)std::min<uint64_t>(unknown_refs_ + wrong_kind_ + in_window_missing_ + other_, 1 << 30);
    }

private:
    void note(const Record &r, const char *what, uint32_t id) {
        if (first_problems_.size() >= 12) return;
        char buf[256];
        std::snprintf(buf, sizeof(buf), "%s::%s at offset %" PRIu64 " (frame %d): %s 0x%08x", iface_name(r.iface),
                      method_name(r.iface, r.slot), r.offset, frames_ + 1, what, id);
        first_problems_.push_back(buf);
    }
    void problem(const Record &r, const char *what) {
        other_++;
        note(r, what, 0);
    }
    void check_use(const Record &r, uint32_t id, Kind want, const char *role) {
        auto it = live_.find(id);
        if (it == live_.end()) {
            // The implicit swap chain's back buffer and objects that predate the trace are legal
            // only for interfaces the proxy hooked before their creation was recorded.
            unknown_refs_++;
            note(r, role, id);
            live_[id] = want; // report each unknown object once
            return;
        }
        Kind have = it->second;
        bool tex_any = want == kUnknown && (have == kTexture || have == kCube || have == kVolume || have == kUnknown);
        if (want != kUnknown && have != kUnknown && have != want) {
            wrong_kind_++;
            note(r, "wrong kind for", id);
        } else if (want == kUnknown && !tex_any) {
            wrong_kind_++;
            note(r, "not a texture:", id);
        }
    }

    Options opts_;
    std::unordered_map<uint32_t, Kind> live_;
    std::unordered_map<uint32_t, int> open_locks_;
    int created_[kKindCount] = {}, destroyed_[kKindCount] = {};
    int frames_ = 0, win_frames_ = 0;
    uint64_t calls_ = 0, draws_ = 0, prims_ = 0, frame_calls_ = 0, frame_draws_ = 0, win_calls_ = 0, win_draws_ = 0;
    uint64_t max_frame_calls_ = 0, shader_bytes_ = 0, upload_bytes_ = 0, uploads_[4] = {};
    uint64_t unknown_refs_ = 0, wrong_kind_ = 0, unmatched_unlocks_ = 0, in_window_missing_ = 0, other_ = 0;
    std::vector<std::string> first_problems_;
};
} // namespace

std::unique_ptr<Backend> make_null_backend(const Options &opts) { return std::make_unique<NullBackend>(opts); }

} // namespace bo1::replay
