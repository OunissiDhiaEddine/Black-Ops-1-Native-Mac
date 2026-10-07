// XAudio2 2.7 for the native runtime, on FAudio (ADR-009, T-053).
//
// The game creates one COM class, XAudio2 2.7 (both CoCreateInstance sites pass CLSID_XAudio2,
// re/com_steam_usage.json), and from then on talks to guest-visible objects with the vtable layout of the
// XAudio2 2.7 SDK header: IXAudio2 (16 methods), IXAudio2Voice (19), IXAudio2SourceVoice (+10). Slots and
// stack words were checked against the game's own call sites (docs/RE_NOTES.md "XAudio2 as the game uses
// it"). Every method forwards to the FAudio function of the same name; the work here is the 32-bit
// structure layouts and the three places where the audio engine calls back INTO the game:
//   - IXAudio2VoiceCallback / IXAudio2EngineCallback objects (game code),
//   - the game's own effects (XAPO objects, IXAPO + IXAPOParameters) in every effect chain,
// which are guest COM objects called with guest_call() — from the calling game thread when FAudio calls
// them synchronously, and from the mixer thread for everything that happens during a mix. The mix itself
// runs on a guest-capable thread of ours (thread_spawn_host): the audio device's own thread has neither a
// guest stack nor room for lifted frames, so it only hands each quantum over and waits.
// Two things this FAudio build does not do and the game needs:
//   - xWMA sounds (format tags 0x161 / 0x162; the engine has no decoder for them and would call a null
//     function): such voices are created as 16-bit PCM and every submitted buffer is decoded here with
//     FFmpeg's WMA decoders first;
//   - a 48 kHz engine: the game's effects write fixed 480-frame blocks, so the mastering voice always runs
//     at 48000 Hz whatever the output device prefers (SDL converts on the way out).
// BO1_AUDIO=off: no XAudio2 (CoCreateInstance fails as before and the game runs silent).
#include <FAPO.h>
#include <FAudio.h>
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
#include <libavutil/mem.h>
}

#include <atomic>
#include <condition_variable>
#include <cstdlib>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "win32_util.h"

namespace bo1 {
namespace {

enum : uint32_t { S_OK = 0, E_NOINTERFACE = 0x80004002u, E_POINTER = 0x80004003u, E_FAIL = 0x80004005u, REGDB_E_CLASSNOTREG = 0x80040154u };
enum { KIND_SOURCE, KIND_SUBMIX, KIND_MASTER };

// IIDs the game's effect objects answer to (both byte patterns are in the image: 0x005BEE40, 0x005AA858)
const uint8_t kIID_IXAPO[16] = {0x01, 0xC0, 0x0B, 0xA9, 0x97, 0xE8, 0x97, 0xE8, 0x55, 0xE4, 0x9E, 0x47, 0x00, 0x00, 0x00, 0x00};
const uint8_t kIID_IXAPOParameters[16] = {0x01, 0xC0, 0x0B, 0xA9, 0x97, 0xE8, 0x97, 0xE8, 0x55, 0xE4, 0x9E, 0x47, 0x00, 0x00, 0x00, 0x01};
const uint8_t kCLSID_XAudio2[16] = {0x85, 0x86, 0x50, 0x5A, 0x54, 0xA2, 0xBA, 0x4F, 0x9B, 0x82, 0x9A, 0x24, 0xB0, 0x03, 0x06, 0xAF};
const uint8_t kCLSID_XAudio2_Debug[16] = {0x35, 0xEA, 0x05, 0xDB, 0x29, 0x03, 0x4B, 0x4D, 0xA5, 0x3A, 0x6D, 0xEA, 0xD0, 0x3D, 0x38, 0x52};

Cpu *cur_cpu() {
    GuestThread *t = thread_current();
    if (!t) {
        log("xaudio2: guest callback on a thread without guest state — aborting");
        abort();
    }
    return thread_cpu(t);
}
// calls method `slot` of a guest COM object (stdcall, `this` first)
uint32_t gcall(uint32_t obj, uint32_t slot, std::initializer_list<uint32_t> args) {
    std::vector<uint32_t> v{obj};
    v.insert(v.end(), args);
    uint32_t fn = rd32(rd32(obj) + 4 * slot);
    Cpu *cpu = cur_cpu();
    uint32_t saved = cpu->esp;
    for (auto it = v.rbegin(); it != v.rend(); ++it) {
        cpu->esp -= 4;
        wr32(cpu->esp, *it);
    }
    cpu->esp -= 4;
    wr32(cpu->esp, 0xFFEF0000u); // same never-executed return address guest_call() uses
    rt_dispatch(cpu, fn);
    cpu->esp = saved;
    return cpu->eax;
}

// ---- the mixer thread ------------------------------------------------------------------------------------
struct Mixer {
    std::mutex mu;
    std::condition_variable cv;
    FAudioEngineCallEXT proc = nullptr;
    FAudio *audio = nullptr;
    float *out = nullptr;
    bool pending = false, started = false;
    std::atomic<uint64_t> quanta{0};
} g_mixer;

void FAUDIOCALL engine_proc(FAudioEngineCallEXT def, FAudio *audio, float *output, void *) {
    std::unique_lock<std::mutex> lk(g_mixer.mu);
    g_mixer.proc = def;
    g_mixer.audio = audio;
    g_mixer.out = output;
    g_mixer.pending = true;
    g_mixer.cv.notify_all();
    g_mixer.cv.wait(lk, [] { return !g_mixer.pending; });
}
void mixer_start() {
    std::lock_guard<std::mutex> lk(g_mixer.mu);
    if (g_mixer.started) return;
    g_mixer.started = true;
    thread_spawn_host(1u << 20, [](Cpu *) {
        std::unique_lock<std::mutex> lk(g_mixer.mu);
        for (;;) {
            g_mixer.cv.wait(lk, [] { return g_mixer.pending; });
            FAudioEngineCallEXT proc = g_mixer.proc;
            FAudio *audio = g_mixer.audio;
            float *out = g_mixer.out;
            lk.unlock();
            proc(audio, out); // voice callbacks and effect processing happen in here, on this thread
            g_mixer.quanta++;
            lk.lock();
            g_mixer.pending = false;
            g_mixer.cv.notify_all();
        }
    });
}

// ---- callbacks into the game ---------------------------------------------------------------------------------
// IXAudio2VoiceCallback: 0 OnVoiceProcessingPassStart(bytes) 1 OnVoiceProcessingPassEnd 2 OnStreamEnd
//                        3 OnBufferStart(ctx) 4 OnBufferEnd(ctx) 5 OnLoopEnd(ctx) 6 OnVoiceError(ctx, hr)
// A buffer of an xWMA voice as the engine sees it: decoded PCM owned by us, plus the game's own context value.
struct WmaBuffer {
    uint32_t guest_ctx = 0;
    std::vector<int16_t> pcm;
};
struct WmaState; // decoder of an xWMA voice (below)
struct VoiceCb {
    FAudioVoiceCallback cb; // first member: FAudio hands this pointer back
    uint32_t guest = 0;     // the game's callback object, 0 = none
    WmaState *wma = nullptr; // non-null: buffer contexts are WmaBuffer pointers
    std::mutex mu;
    std::vector<WmaBuffer *> live;
};
uint32_t ctx32(VoiceCb *v, void *p) { return v->wma ? (p ? static_cast<WmaBuffer *>(p)->guest_ctx : 0) : (uint32_t)(uintptr_t)p; }
VoiceCb *vcb(FAudioVoiceCallback *c) { return reinterpret_cast<VoiceCb *>(c); }
void FAUDIOCALL vcb_buffer_end(FAudioVoiceCallback *c, void *ctx) {
    VoiceCb *v = vcb(c);
    if (v->guest) gcall(v->guest, 4, {ctx32(v, ctx)});
    if (v->wma && ctx) { // the engine is done with the decoded samples
        auto *b = static_cast<WmaBuffer *>(ctx);
        std::lock_guard<std::mutex> lk(v->mu);
        std::erase(v->live, b);
        delete b;
    }
}
void FAUDIOCALL vcb_buffer_start(FAudioVoiceCallback *c, void *ctx) {
    if (vcb(c)->guest) gcall(vcb(c)->guest, 3, {ctx32(vcb(c), ctx)});
}
void FAUDIOCALL vcb_loop_end(FAudioVoiceCallback *c, void *ctx) {
    if (vcb(c)->guest) gcall(vcb(c)->guest, 5, {ctx32(vcb(c), ctx)});
}
void FAUDIOCALL vcb_stream_end(FAudioVoiceCallback *c) {
    if (vcb(c)->guest) gcall(vcb(c)->guest, 2, {});
}
void FAUDIOCALL vcb_error(FAudioVoiceCallback *c, void *ctx, uint32_t err) {
    if (vcb(c)->guest) gcall(vcb(c)->guest, 6, {ctx32(vcb(c), ctx), err});
}
void FAUDIOCALL vcb_pass_end(FAudioVoiceCallback *c) {
    if (vcb(c)->guest) gcall(vcb(c)->guest, 1, {});
}
void FAUDIOCALL vcb_pass_start(FAudioVoiceCallback *c, uint32_t bytes) {
    if (vcb(c)->guest) gcall(vcb(c)->guest, 0, {bytes});
}
VoiceCb *voice_cb_new(uint32_t guest, WmaState *wma) {
    if (!guest && !wma) return nullptr;
    auto *v = new VoiceCb;
    v->cb = {vcb_buffer_end, vcb_buffer_start, vcb_loop_end, vcb_stream_end, vcb_error, vcb_pass_end, vcb_pass_start};
    v->guest = guest;
    v->wma = wma;
    return v;
}

// ---- xWMA -> PCM ------------------------------------------------------------------------------------------------
// One decoder per voice: consecutive buffers of a voice continue the same bit stream until the game flushes
// the voice or marks a buffer as the end of the stream.
struct WmaState {
    AVCodecContext *dec = nullptr;
    AVPacket *pkt = nullptr;
    AVFrame *frame = nullptr;
    uint32_t block_align = 0, channels = 0;
    bool fresh = true;
    ~WmaState() {
        av_frame_free(&frame);
        av_packet_free(&pkt);
        avcodec_free_context(&dec);
    }
};
bool is_wma(const FAudioWaveFormatEx *f) { return f->wFormatTag == FAUDIO_FORMAT_WMAUDIO2 || f->wFormatTag == FAUDIO_FORMAT_WMAUDIO3; }
WmaState *wma_open(const FAudioWaveFormatEx *f) {
    auto *w = new WmaState;
    w->block_align = f->nBlockAlign;
    w->channels = f->nChannels;
    const AVCodec *codec = avcodec_find_decoder(f->wFormatTag == FAUDIO_FORMAT_WMAUDIO2 ? AV_CODEC_ID_WMAV2 : AV_CODEC_ID_WMAPRO);
    if (!codec || !(w->dec = avcodec_alloc_context3(codec))) return w; // no decoder: the voice plays silence
    av_channel_layout_default(&w->dec->ch_layout, f->nChannels);
    w->dec->sample_rate = (int)f->nSamplesPerSec;
    w->dec->bit_rate = (int64_t)f->nAvgBytesPerSec * 8;
    w->dec->block_align = f->nBlockAlign;
    w->dec->bits_per_coded_sample = f->wBitsPerSample;
    // codec set-up bytes follow the WAVEFORMATEX; xWMA usually has none for WMA 2, and the decoder then needs
    // the six bytes a plain .wma file would carry (flags: byte 4 = 31, the value FFmpeg's own xWMA reader uses)
    uint32_t extra = f->cbSize;
    w->dec->extradata_size = extra ? (int)extra : 6;
    w->dec->extradata = static_cast<uint8_t *>(av_mallocz((size_t)w->dec->extradata_size + AV_INPUT_BUFFER_PADDING_SIZE));
    if (extra) memcpy(w->dec->extradata, reinterpret_cast<const uint8_t *>(f) + 18, extra);
    else w->dec->extradata[4] = 31;
    if (avcodec_open2(w->dec, codec, nullptr) < 0) {
        log("xaudio2: cannot open the %s decoder (%u ch, %u Hz, block %u) — this voice will be silent", codec->name, f->nChannels, f->nSamplesPerSec,
            f->nBlockAlign);
        avcodec_free_context(&w->dec);
        return w;
    }
    w->pkt = av_packet_alloc();
    w->frame = av_frame_alloc();
    return w;
}
// Decodes `bytes` of xWMA packets into interleaved 16-bit samples. `expect_bytes` (0 = unknown) is the decoded
// size the game's packet table announces; the result is padded or cut to it so that play and loop positions fit.
void wma_decode(WmaState *w, const uint8_t *data, uint32_t bytes, uint32_t expect_bytes, std::vector<int16_t> &out) {
    if (w->dec && w->block_align) {
        if (w->fresh) avcodec_flush_buffers(w->dec);
        w->fresh = false;
        for (uint32_t off = 0; off + w->block_align <= bytes; off += w->block_align) {
            if (av_new_packet(w->pkt, (int)w->block_align) < 0) break;
            memcpy(w->pkt->data, data + off, w->block_align);
            int sent = avcodec_send_packet(w->dec, w->pkt);
            av_packet_unref(w->pkt);
            if (sent < 0) continue;
            while (avcodec_receive_frame(w->dec, w->frame) >= 0) {
                const AVFrame *f = w->frame;
                int ch = f->ch_layout.nb_channels;
                size_t base = out.size();
                out.resize(base + (size_t)f->nb_samples * (size_t)ch);
                for (int c = 0; c < ch; c++) {
                    if (f->format == AV_SAMPLE_FMT_FLTP) {
                        const float *src = reinterpret_cast<const float *>(f->extended_data[c]);
                        for (int i = 0; i < f->nb_samples; i++) {
                            float v = src[i] * 32767.0f;
                            out[base + (size_t)i * ch + c] = (int16_t)(v > 32767.0f ? 32767.0f : v < -32768.0f ? -32768.0f : v);
                        }
                    } else if (f->format == AV_SAMPLE_FMT_S16P) {
                        const int16_t *src = reinterpret_cast<const int16_t *>(f->extended_data[c]);
                        for (int i = 0; i < f->nb_samples; i++) out[base + (size_t)i * ch + c] = src[i];
                    }
                }
                av_frame_unref(w->frame);
            }
        }
    }
    if (expect_bytes) out.resize(expect_bytes / 2, 0);
    if (out.empty()) out.resize((size_t)w->channels * 64, 0);
}

// IXAudio2EngineCallback: 0 OnProcessingPassStart 1 OnProcessingPassEnd 2 OnCriticalError(hr)
struct EngineCb {
    FAudioEngineCallback cb;
    uint32_t guest;
};
void FAUDIOCALL ecb_error(FAudioEngineCallback *c, uint32_t err) { gcall(reinterpret_cast<EngineCb *>(c)->guest, 2, {err}); }
void FAUDIOCALL ecb_pass_end(FAudioEngineCallback *c) { gcall(reinterpret_cast<EngineCb *>(c)->guest, 1, {}); }
void FAUDIOCALL ecb_pass_start(FAudioEngineCallback *c) { gcall(reinterpret_cast<EngineCb *>(c)->guest, 0, {}); }

// ---- the game's effects (XAPO) as FAudio effects -----------------------------------------------------------
// IXAPO: 0-2 IUnknown, 3 GetRegistrationProperties, 4 IsInputFormatSupported, 5 IsOutputFormatSupported,
// 6 Initialize, 7 Reset, 8 LockForProcess, 9 UnlockForProcess, 10 Process, 11 CalcInputFrames,
// 12 CalcOutputFrames. IXAPOParameters: 0-2 IUnknown, 3 SetParameters, 4 GetParameters.
// The engine's buffers are host memory, the effect is guest code: samples are copied through guest scratch
// buffers sized at LockForProcess.
constexpr uint32_t kFmtBytes = 64;
struct Stream {
    uint32_t buf = 0, bytes = 0, block = 0; // guest buffer, its size, bytes per frame
};
struct GuestApo {
    FAPO apo; // first member
    std::atomic<int> refs{1};
    uint32_t xapo = 0, params = 0; // the game's IXAPO / IXAPOParameters interface pointers (0 = not offered)
    uint32_t scratch = 0;          // guest memory: 2 formats, an out-pointer, lock / process parameter arrays
    uint32_t lock_fmts = 0;        // guest copies of the formats named by the current LockForProcess
    std::vector<Stream> in, out;
    std::mutex param_mu;
    uint32_t param_buf = 0, param_cap = 0;
};
enum : uint32_t { SCR_FMT0 = 0, SCR_FMT1 = kFmtBytes, SCR_PTR = 2 * kFmtBytes, SCR_LOCK = SCR_PTR + 8, SCR_PROC = SCR_LOCK + 16 * 8, SCR_SIZE = SCR_PROC + 16 * 12 };
GuestApo *apo_of(void *p) { return static_cast<GuestApo *>(p); }
uint32_t put_format(uint32_t dst, const FAudioWaveFormatEx *f) {
    if (!f) return 0;
    uint32_t n = 18 + (f->wFormatTag == FAUDIO_FORMAT_PCM ? 0u : (uint32_t)f->cbSize);
    if (n > kFmtBytes) n = kFmtBytes;
    memcpy(g_mem + dst, f, n);
    return dst;
}


void streams_free(GuestApo *a) {
    for (std::vector<Stream> *v : {&a->in, &a->out}) {
        for (Stream &s : *v)
            if (s.buf) heap_free(s.buf);
        v->clear();
    }
    if (a->lock_fmts) heap_free(a->lock_fmts);
    a->lock_fmts = 0;
}

int32_t FAPOCALL apo_addref(void *p) { return ++apo_of(p)->refs; }
int32_t FAPOCALL apo_release(void *p) {
    GuestApo *a = apo_of(p);
    int n = --a->refs;
    if (n == 0) {
        if (a->params) gcall(a->params, 2, {});
        if (a->xapo) gcall(a->xapo, 2, {});
        streams_free(a);
        if (a->scratch) heap_free(a->scratch);
        if (a->param_buf) heap_free(a->param_buf);
        delete a;
    }
    return n;
}
uint32_t FAPOCALL apo_registration(void *p, FAPORegistrationProperties **pp) {
    GuestApo *a = apo_of(p);
    wr32(a->scratch + SCR_PTR, 0);
    uint32_t hr = gcall(a->xapo, 3, {a->scratch + SCR_PTR});
    uint32_t g = rd32(a->scratch + SCR_PTR);
    if ((int32_t)hr < 0 || !g) return (int32_t)hr < 0 ? hr : E_FAIL;
    // same packed layout on both sides; the engine frees its copy with the allocator it was created with (malloc)
    *pp = static_cast<FAPORegistrationProperties *>(malloc(sizeof(FAPORegistrationProperties)));
    memcpy(*pp, g_mem + g, sizeof(FAPORegistrationProperties));
    heap_free(g); // the effect allocated it with CoTaskMemAlloc
    return hr;
}
uint32_t apo_format_supported(GuestApo *a, uint32_t slot, const FAudioWaveFormatEx *f0, const FAudioWaveFormatEx *f1, FAudioWaveFormatEx **pp) {
    wr32(a->scratch + SCR_PTR, 0);
    uint32_t hr = gcall(a->xapo, slot, {put_format(a->scratch + SCR_FMT0, f0), put_format(a->scratch + SCR_FMT1, f1), pp ? a->scratch + SCR_PTR : 0});
    if (pp) {
        uint32_t g = rd32(a->scratch + SCR_PTR);
        *pp = nullptr;
        if (g) { // closest supported format, allocated by the effect: hand the engine a copy it can free
            *pp = static_cast<FAudioWaveFormatEx *>(malloc(kFmtBytes));
            memcpy(*pp, g_mem + g, kFmtBytes);
            heap_free(g);
        }
    }
    return hr;
}
uint32_t FAPOCALL apo_input_supported(void *p, const FAudioWaveFormatEx *out, const FAudioWaveFormatEx *req, FAudioWaveFormatEx **pp) {
    return apo_format_supported(apo_of(p), 4, out, req, pp);
}
uint32_t FAPOCALL apo_output_supported(void *p, const FAudioWaveFormatEx *in, const FAudioWaveFormatEx *req, FAudioWaveFormatEx **pp) {
    return apo_format_supported(apo_of(p), 5, in, req, pp);
}
uint32_t FAPOCALL apo_initialize(void *p, const void *data, uint32_t size) {
    GuestApo *a = apo_of(p);
    uint32_t g = 0;
    if (data && size) {
        g = heap_alloc(size);
        memcpy(g_mem + g, data, size);
    }
    uint32_t hr = gcall(a->xapo, 6, {g, size});
    if (g) heap_free(g);
    return hr;
}
void FAPOCALL apo_reset(void *p) { gcall(apo_of(p)->xapo, 7, {}); }
uint32_t FAPOCALL apo_lock(void *p, uint32_t in_n, const FAPOLockForProcessBufferParameters *in, uint32_t out_n, const FAPOLockForProcessBufferParameters *out) {
    GuestApo *a = apo_of(p);
    if (in_n > 8 || out_n > 8) return E_FAIL;
    streams_free(a);
    uint32_t fmts = a->lock_fmts = heap_alloc((in_n + out_n) * kFmtBytes + 4, true); // the effect may keep the pointers while locked
    auto fill = [&](std::vector<Stream> &v, uint32_t n, const FAPOLockForProcessBufferParameters *src, uint32_t table, uint32_t fmt_base) {
        for (uint32_t i = 0; i < n; i++) {
            Stream s;
            s.block = src[i].pFormat ? src[i].pFormat->nBlockAlign : 4;
            s.bytes = src[i].MaxFrameCount * s.block;
            s.buf = heap_alloc(s.bytes + 64, true);
            v.push_back(s);
            wr32(table + 8 * i, put_format(fmt_base + i * kFmtBytes, src[i].pFormat));
            wr32(table + 8 * i + 4, src[i].MaxFrameCount);
        }
    };
    uint32_t in_tab = a->scratch + SCR_LOCK, out_tab = in_tab + 8 * 8;
    fill(a->in, in_n, in, in_tab, fmts);
    fill(a->out, out_n, out, out_tab, fmts + in_n * kFmtBytes);
    return gcall(a->xapo, 8, {in_n, in_tab, out_n, out_tab});
}
void FAPOCALL apo_unlock(void *p) { gcall(apo_of(p)->xapo, 9, {}); }
void FAPOCALL apo_process(void *p, uint32_t in_n, const FAPOProcessBufferParameters *in, uint32_t out_n, FAPOProcessBufferParameters *out, int32_t enabled) {
    GuestApo *a = apo_of(p);
    if (in_n > a->in.size() || out_n > a->out.size()) return;
    uint32_t in_tab = a->scratch + SCR_PROC, out_tab = in_tab + 8 * 12;
    for (uint32_t i = 0; i < in_n; i++) {
        const Stream &s = a->in[i];
        uint32_t bytes = in[i].ValidFrameCount * s.block;
        if (bytes > s.bytes) bytes = s.bytes;
        if (in[i].pBuffer) memcpy(g_mem + s.buf, in[i].pBuffer, bytes);
        wr32(in_tab + 12 * i, s.buf);
        wr32(in_tab + 12 * i + 4, (uint32_t)in[i].BufferFlags);
        wr32(in_tab + 12 * i + 8, in[i].ValidFrameCount);
    }
    for (uint32_t i = 0; i < out_n; i++) {
        // an effect that works in place gets the same buffer on both sides, as the engine arranges it
        bool in_place = i < in_n && in[i].pBuffer == out[i].pBuffer;
        wr32(out_tab + 12 * i, in_place ? a->in[i].buf : a->out[i].buf);
        wr32(out_tab + 12 * i + 4, (uint32_t)out[i].BufferFlags);
        wr32(out_tab + 12 * i + 8, out[i].ValidFrameCount);
    }
    gcall(a->xapo, 10, {in_n, in_tab, out_n, out_tab, (uint32_t)enabled});
    for (uint32_t i = 0; i < out_n; i++) {
        const Stream &s = a->out[i];
        uint32_t frames = rd32(out_tab + 12 * i + 8), bytes = frames * s.block;
        if (bytes > s.bytes) bytes = s.bytes;
        out[i].BufferFlags = (FAPOBufferFlags)rd32(out_tab + 12 * i + 4);
        out[i].ValidFrameCount = frames;
        if (out[i].pBuffer) memcpy(out[i].pBuffer, g_mem + rd32(out_tab + 12 * i), bytes);
    }
}
uint32_t FAPOCALL apo_calc_input(void *p, uint32_t frames) { return gcall(apo_of(p)->xapo, 11, {frames}); }
uint32_t FAPOCALL apo_calc_output(void *p, uint32_t frames) { return gcall(apo_of(p)->xapo, 12, {frames}); }
uint32_t apo_param_scratch(GuestApo *a, uint32_t size) {
    if (size > a->param_cap) {
        if (a->param_buf) heap_free(a->param_buf);
        a->param_cap = size + 64;
        a->param_buf = heap_alloc(a->param_cap, true);
    }
    return a->param_buf;
}
void FAPOCALL apo_set_parameters(void *p, const void *data, uint32_t size) {
    GuestApo *a = apo_of(p);
    if (!a->params || !data) return;
    std::lock_guard<std::mutex> lk(a->param_mu);
    uint32_t g = apo_param_scratch(a, size);
    memcpy(g_mem + g, data, size);
    gcall(a->params, 3, {g, size});
}
void FAPOCALL apo_get_parameters(void *p, void *data, uint32_t size) {
    GuestApo *a = apo_of(p);
    if (!a->params || !data) return;
    std::lock_guard<std::mutex> lk(a->param_mu);
    uint32_t g = apo_param_scratch(a, size);
    gcall(a->params, 4, {g, size});
    memcpy(data, g_mem + g, size);
}

// Wraps the IUnknown the game put into an effect descriptor. Returns nullptr if it is not an IXAPO.
GuestApo *apo_wrap(uint32_t unk) {
    auto *a = new GuestApo;
    a->apo = {apo_addref, apo_release, apo_registration, apo_input_supported, apo_output_supported, apo_initialize, apo_reset,
              apo_lock, apo_unlock, apo_process, apo_calc_input, apo_calc_output, apo_set_parameters, apo_get_parameters};
    a->scratch = heap_alloc(SCR_SIZE + 32, true);
    uint32_t iid = a->scratch + SCR_FMT0, out = a->scratch + SCR_PTR;
    memcpy(g_mem + iid, kIID_IXAPO, 16);
    wr32(out, 0);
    if ((int32_t)gcall(unk, 0, {iid, out}) >= 0) a->xapo = rd32(out);
    memcpy(g_mem + iid, kIID_IXAPOParameters, 16);
    wr32(out, 0);
    if ((int32_t)gcall(unk, 0, {iid, out}) >= 0) a->params = rd32(out);
    if (!a->xapo) {
        log("xaudio2: effect object %08x does not offer IXAPO", unk);
        apo_release(a);
        return nullptr;
    }
    return a;
}

// ---- guest structures -> FAudio structures --------------------------------------------------------------------
struct VoiceRec {
    FAudioVoice *fv = nullptr;
    int kind = 0;
    VoiceCb *cb = nullptr;
    WmaState *wma = nullptr;
};
std::mutex g_mu;
std::unordered_map<uint32_t, VoiceRec> g_voices; // guest voice object -> record
std::unordered_map<uint32_t, FAudio *> g_engines; // guest IXAudio2 object -> engine
std::unordered_map<uint32_t, EngineCb *> g_engine_cbs;
std::atomic<uint32_t> g_wma_voices{0}, g_wma_buffers{0};
std::atomic<FAudio *> g_live_engine{nullptr}; // the engine that currently has a mastering voice (host streams play on it)

FAudioVoice *fv(uint32_t guest) {
    if (!guest) return nullptr;
    std::lock_guard<std::mutex> lk(g_mu);
    auto it = g_voices.find(guest);
    if (it == g_voices.end()) {
        log("xaudio2: %08x is not a live voice", guest);
        return nullptr;
    }
    return it->second.fv;
}
FAudio *engine(uint32_t guest) {
    std::lock_guard<std::mutex> lk(g_mu);
    auto it = g_engines.find(guest);
    return it == g_engines.end() ? nullptr : it->second;
}

// XAUDIO2_VOICE_SENDS { u32 SendCount; XAUDIO2_SEND_DESCRIPTOR *pSends } / { u32 Flags; IXAudio2Voice *pOutputVoice }
struct Sends {
    FAudioVoiceSends s{};
    std::vector<FAudioSendDescriptor> d;
    const FAudioVoiceSends *get(uint32_t g) {
        if (!g) return nullptr;
        uint32_t n = rd32(g), p = rd32(g + 4);
        d.resize(n);
        for (uint32_t i = 0; i < n; i++) {
            d[i].Flags = rd32(p + 8 * i);
            d[i].pOutputVoice = fv(rd32(p + 8 * i + 4));
        }
        s.SendCount = n;
        s.pSends = d.data();
        return &s;
    }
};
// XAUDIO2_EFFECT_CHAIN { u32 EffectCount; XAUDIO2_EFFECT_DESCRIPTOR *p } / { IUnknown *pEffect; BOOL InitialState; u32 OutputChannels }
struct Chain {
    FAudioEffectChain c{};
    std::vector<FAudioEffectDescriptor> d;
    std::vector<GuestApo *> apos;
    bool ok = true;
    const FAudioEffectChain *get(uint32_t g) {
        if (!g) return nullptr;
        uint32_t n = rd32(g), p = rd32(g + 4);
        d.resize(n);
        for (uint32_t i = 0; i < n; i++) {
            GuestApo *a = apo_wrap(rd32(p + 12 * i));
            if (!a) {
                ok = false;
                d.resize(i);
                break;
            }
            apos.push_back(a);
            d[i].pEffect = &a->apo;
            d[i].InitialState = (int32_t)rd32(p + 12 * i + 4);
            d[i].OutputChannels = rd32(p + 12 * i + 8);
        }
        c.EffectCount = (uint32_t)d.size();
        c.pEffectDescriptors = d.data();
        return &c;
    }
    ~Chain() { // the engine took its own references
        for (GuestApo *a : apos) apo_release(a);
    }
};

float f32(uint32_t bits) {
    float f;
    memcpy(&f, &bits, 4);
    return f;
}

uint32_t voice_vtable(int kind);
uint32_t voice_new(FAudioVoice *v, int kind, VoiceCb *cb, WmaState *wma = nullptr) {
    uint32_t o = com_new(voice_vtable(kind), 8);
    std::lock_guard<std::mutex> lk(g_mu);
    g_voices[o] = VoiceRec{v, kind, cb, wma};
    return o;
}
VoiceRec voice_rec(uint32_t guest) {
    std::lock_guard<std::mutex> lk(g_mu);
    auto it = g_voices.find(guest);
    return it == g_voices.end() ? VoiceRec{} : it->second;
}

// ---- IXAudio2 --------------------------------------------------------------------------------------------------
uint32_t x_query_interface(Cpu *, const uint32_t *a) {
    if (!a[2]) return E_POINTER;
    wr32(a[2], a[0]);
    wr32(a[0] + COM_REFS, rd32(a[0] + COM_REFS) + 1);
    return S_OK;
}
uint32_t x_addref(Cpu *, const uint32_t *a) {
    uint32_t n = rd32(a[0] + COM_REFS) + 1;
    wr32(a[0] + COM_REFS, n);
    return n;
}
uint32_t x_release(Cpu *, const uint32_t *a) {
    uint32_t n = rd32(a[0] + COM_REFS) - 1;
    wr32(a[0] + COM_REFS, n);
    if (n == 0) {
        if (g_verbose) log("IXAudio2::Release(%08x): last reference", a[0]);
        FAudio *e = engine(a[0]);
        {
            std::lock_guard<std::mutex> lk(g_mu);
            g_engines.erase(a[0]);
        }
        if (e) FAudio_Release(e);
        heap_free(a[0]);
    }
    return n;
}
uint32_t x_get_device_count(Cpu *, const uint32_t *a) {
    uint32_t n = 0, hr = FAudio_GetDeviceCount(engine(a[0]), &n);
    wr32(a[1], n);
    return hr;
}
uint32_t x_get_device_details(Cpu *, const uint32_t *a) {
    // XAUDIO2_DEVICE_DETAILS: WCHAR[256] id, WCHAR[256] name, role, WAVEFORMATEXTENSIBLE — packed, same layout
    FAudioDeviceDetails d{};
    uint32_t hr = FAudio_GetDeviceDetails(engine(a[0]), a[1], &d);
    if (hr == 0) memcpy(g_mem + a[2], &d, sizeof d);
    return hr;
}
uint32_t x_initialize(Cpu *, const uint32_t *a) {
    FAudio *e = engine(a[0]);
    uint32_t hr = FAudio_Initialize(e, a[1], FAUDIO_DEFAULT_PROCESSOR);
    if (hr == 0) {
        mixer_start();
        FAudio_SetEngineProcedureEXT(e, engine_proc, nullptr);
    }
    if (g_verbose) log("IXAudio2::Initialize(flags %#x) -> %#x", a[1], hr);
    return hr;
}
uint32_t x_register_callbacks(Cpu *, const uint32_t *a) {
    auto *cb = new EngineCb{{ecb_error, ecb_pass_end, ecb_pass_start}, a[1]};
    {
        std::lock_guard<std::mutex> lk(g_mu);
        g_engine_cbs[a[1]] = cb;
    }
    return FAudio_RegisterForCallbacks(engine(a[0]), &cb->cb);
}
uint32_t x_unregister_callbacks(Cpu *, const uint32_t *a) {
    EngineCb *cb = nullptr;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        auto it = g_engine_cbs.find(a[1]);
        if (it != g_engine_cbs.end()) {
            cb = it->second;
            g_engine_cbs.erase(it);
        }
    }
    if (cb) {
        FAudio_UnregisterForCallbacks(engine(a[0]), &cb->cb);
        delete cb;
    }
    return 0;
}
uint32_t x_create_source_voice(Cpu *, const uint32_t *a) {
    // (this, ppVoice, WAVEFORMATEX *, Flags, float MaxFrequencyRatio, pCallback, pSendList, pEffectChain)
    const auto *fmt = g2h<FAudioWaveFormatEx>(a[2]);
    Sends sends;
    Chain chain;
    const FAudioEffectChain *fc = chain.get(a[7]);
    if (!chain.ok) return E_FAIL;
    WmaState *wma = nullptr;
    FAudioWaveFormatEx pcm{};
    const FAudioWaveFormatEx *use = fmt;
    if (is_wma(fmt)) { // decoded here: the engine gets a 16-bit PCM voice of the same shape
        wma = wma_open(fmt);
        pcm.wFormatTag = FAUDIO_FORMAT_PCM;
        pcm.nChannels = fmt->nChannels;
        pcm.nSamplesPerSec = fmt->nSamplesPerSec;
        pcm.wBitsPerSample = 16;
        pcm.nBlockAlign = (uint16_t)(fmt->nChannels * 2);
        pcm.nAvgBytesPerSec = pcm.nSamplesPerSec * pcm.nBlockAlign;
        use = &pcm;
        if (g_wma_voices++ == 0)
            log("xaudio2: first xWMA voice (format %#x, %u ch, %u Hz, block %u, %u extra bytes) — decoded with FFmpeg%s", fmt->wFormatTag, fmt->nChannels,
                fmt->nSamplesPerSec, fmt->nBlockAlign, fmt->cbSize, wma->dec ? "" : " [NO DECODER: silent]");
    }
    VoiceCb *cb = voice_cb_new(a[5], wma);
    FAudioSourceVoice *v = nullptr;
    uint32_t hr = FAudio_CreateSourceVoice(engine(a[0]), &v, use, a[3], f32(a[4]), cb ? &cb->cb : nullptr, sends.get(a[6]), fc);
    if (hr != 0 || !v) {
        log("IXAudio2::CreateSourceVoice(format %#x, %u ch, %u Hz) failed: %#x", fmt->wFormatTag, fmt->nChannels, fmt->nSamplesPerSec, hr);
        delete cb;
        delete wma;
        wr32(a[1], 0);
        return hr ? hr : E_FAIL;
    }
    wr32(a[1], voice_new(v, KIND_SOURCE, cb, wma));
    return hr;
}
uint32_t x_create_submix_voice(Cpu *, const uint32_t *a) {
    // (this, ppVoice, InputChannels, InputSampleRate, Flags, ProcessingStage, pSendList, pEffectChain)
    Sends sends;
    Chain chain;
    const FAudioEffectChain *fc = chain.get(a[7]);
    if (!chain.ok) return E_FAIL;
    FAudioSubmixVoice *v = nullptr;
    uint32_t hr = FAudio_CreateSubmixVoice(engine(a[0]), &v, a[2], a[3], a[4], a[5], sends.get(a[6]), fc);
    if (g_verbose || hr) log("IXAudio2::CreateSubmixVoice(%u ch, %u Hz, flags %#x, stage %u, %u effects) -> %#x", a[2], a[3], a[4], a[5], fc ? fc->EffectCount : 0, hr);
    wr32(a[1], hr == 0 && v ? voice_new(v, KIND_SUBMIX, nullptr) : 0);
    return hr;
}
uint32_t x_create_mastering_voice(Cpu *, const uint32_t *a) {
    // 2.7: (this, ppVoice, InputChannels, InputSampleRate, Flags, DeviceIndex, pEffectChain)
    Chain chain;
    const FAudioEffectChain *fc = chain.get(a[6]);
    if (!chain.ok) return E_FAIL;
    FAudioMasteringVoice *v = nullptr;
    // 0 Hz = "the device's rate" — but the game's effects only work in 480-frame blocks (10 ms at 48 kHz; at
    // 44.1 kHz they overran their buffers into the game's own variables), so the engine always runs at 48 kHz
    uint32_t rate = a[3] ? a[3] : 48000;
    uint32_t hr = FAudio_CreateMasteringVoice(engine(a[0]), &v, a[2], rate, a[4], a[5], fc);
    FAudioVoiceDetails d{};
    if (hr == 0 && v) FAudioVoice_GetVoiceDetails(v, &d);
    log("IXAudio2::CreateMasteringVoice(%u ch, %u Hz, device %u) -> %#x (%u ch, %u Hz)", a[2], a[3], a[5], hr, d.InputChannels, d.InputSampleRate);
    wr32(a[1], hr == 0 && v ? voice_new(v, KIND_MASTER, nullptr) : 0);
    if (hr == 0 && v) g_live_engine = engine(a[0]);
    // BO1_AUDIO_MUTE=1: the real output device and its clock, but nothing audible (the game never sets the
    // mastering voice's volume itself, re/xaudio2_usage.csv). For timing checks that SDL's dummy / disk
    // devices cannot give: those consume samples slower than real time.
    if (hr == 0 && v && getenv("BO1_AUDIO_MUTE")) FAudioVoice_SetVolume(v, 0.0f, 0);
    return hr;
}
uint32_t x_start_engine(Cpu *, const uint32_t *a) { return FAudio_StartEngine(engine(a[0])); }
uint32_t x_stop_engine(Cpu *, const uint32_t *a) {
    if (g_verbose) log("IXAudio2::StopEngine");
    FAudio_StopEngine(engine(a[0]));
    return 0;
}
uint32_t x_commit_changes(Cpu *, const uint32_t *a) { return FAudio_CommitOperationSet(engine(a[0]), a[1]); }
uint32_t x_get_performance_data(Cpu *, const uint32_t *a) {
    FAudioPerformanceData d{};
    FAudio_GetPerformanceData(engine(a[0]), &d);
    memcpy(g_mem + a[1], &d, sizeof d); // 64 bytes, the 2.7 layout
    return 0;
}
uint32_t x_set_debug_configuration(Cpu *, const uint32_t *) { return 0; }

const ComMethod kEngineMethods[] = {
    {"QueryInterface", 3, x_query_interface}, {"AddRef", 1, x_addref}, {"Release", 1, x_release},
    {"GetDeviceCount", 2, x_get_device_count}, {"GetDeviceDetails", 3, x_get_device_details}, {"Initialize", 3, x_initialize},
    {"RegisterForCallbacks", 2, x_register_callbacks}, {"UnregisterForCallbacks", 2, x_unregister_callbacks},
    {"CreateSourceVoice", 8, x_create_source_voice}, {"CreateSubmixVoice", 8, x_create_submix_voice},
    {"CreateMasteringVoice", 7, x_create_mastering_voice}, {"StartEngine", 1, x_start_engine}, {"StopEngine", 1, x_stop_engine},
    {"CommitChanges", 2, x_commit_changes}, {"GetPerformanceData", 2, x_get_performance_data},
    {"SetDebugConfiguration", 3, x_set_debug_configuration},
};

// ---- IXAudio2Voice / IXAudio2SourceVoice ------------------------------------------------------------------------
uint32_t v_get_voice_details(Cpu *, const uint32_t *a) {
    // 2.7: { CreationFlags, InputChannels, InputSampleRate } (no ActiveFlags)
    FAudioVoiceDetails d{};
    FAudioVoice_GetVoiceDetails(fv(a[0]), &d);
    wr32(a[1], d.CreationFlags);
    wr32(a[1] + 4, d.InputChannels);
    wr32(a[1] + 8, d.InputSampleRate);
    return 0;
}
uint32_t v_set_output_voices(Cpu *, const uint32_t *a) {
    Sends s;
    return FAudioVoice_SetOutputVoices(fv(a[0]), s.get(a[1]));
}
uint32_t v_set_effect_chain(Cpu *, const uint32_t *a) {
    Chain c;
    const FAudioEffectChain *fc = c.get(a[1]);
    if (!c.ok) return E_FAIL;
    return FAudioVoice_SetEffectChain(fv(a[0]), fc);
}
uint32_t v_enable_effect(Cpu *, const uint32_t *a) { return FAudioVoice_EnableEffect(fv(a[0]), a[1], a[2]); }
uint32_t v_disable_effect(Cpu *, const uint32_t *a) { return FAudioVoice_DisableEffect(fv(a[0]), a[1], a[2]); }
uint32_t v_get_effect_state(Cpu *, const uint32_t *a) {
    int32_t on = 0;
    FAudioVoice_GetEffectState(fv(a[0]), a[1], &on);
    wr32(a[2], (uint32_t)on);
    return 0;
}
uint32_t v_set_effect_parameters(Cpu *, const uint32_t *a) { return FAudioVoice_SetEffectParameters(fv(a[0]), a[1], g_mem + a[2], a[3], a[4]); }
uint32_t v_get_effect_parameters(Cpu *, const uint32_t *a) { return FAudioVoice_GetEffectParameters(fv(a[0]), a[1], g_mem + a[2], a[3]); }
uint32_t v_set_filter_parameters(Cpu *, const uint32_t *a) { return FAudioVoice_SetFilterParameters(fv(a[0]), g2h<FAudioFilterParameters>(a[1]), a[2]); }
uint32_t v_get_filter_parameters(Cpu *, const uint32_t *a) {
    FAudioVoice_GetFilterParameters(fv(a[0]), g2h<FAudioFilterParameters>(a[1]));
    return 0;
}
uint32_t v_set_output_filter_parameters(Cpu *, const uint32_t *a) {
    return FAudioVoice_SetOutputFilterParameters(fv(a[0]), fv(a[1]), g2h<FAudioFilterParameters>(a[2]), a[3]);
}
uint32_t v_get_output_filter_parameters(Cpu *, const uint32_t *a) {
    FAudioVoice_GetOutputFilterParameters(fv(a[0]), fv(a[1]), g2h<FAudioFilterParameters>(a[2]));
    return 0;
}
uint32_t v_set_volume(Cpu *, const uint32_t *a) { return FAudioVoice_SetVolume(fv(a[0]), f32(a[1]), a[2]); }
uint32_t v_get_volume(Cpu *, const uint32_t *a) {
    FAudioVoice_GetVolume(fv(a[0]), g2h<float>(a[1]));
    return 0;
}
uint32_t v_set_channel_volumes(Cpu *, const uint32_t *a) { return FAudioVoice_SetChannelVolumes(fv(a[0]), a[1], g2h<float>(a[2]), a[3]); }
uint32_t v_get_channel_volumes(Cpu *, const uint32_t *a) {
    FAudioVoice_GetChannelVolumes(fv(a[0]), a[1], g2h<float>(a[2]));
    return 0;
}
uint32_t v_set_output_matrix(Cpu *, const uint32_t *a) { return FAudioVoice_SetOutputMatrix(fv(a[0]), fv(a[1]), a[2], a[3], g2h<float>(a[4]), a[5]); }
uint32_t v_get_output_matrix(Cpu *, const uint32_t *a) {
    FAudioVoice_GetOutputMatrix(fv(a[0]), fv(a[1]), a[2], a[3], g2h<float>(a[4]));
    return 0;
}
uint32_t v_destroy_voice(Cpu *, const uint32_t *a) {
    VoiceRec r;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        auto it = g_voices.find(a[0]);
        if (it == g_voices.end()) return 0;
        r = it->second;
        g_voices.erase(it);
    }
    if (g_verbose) log("IXAudio2Voice::DestroyVoice(%08x, %s)", a[0], r.kind == KIND_SOURCE ? "source" : r.kind == KIND_SUBMIX ? "submix" : "mastering");
    if (r.kind == KIND_MASTER) g_live_engine = nullptr;
    FAudioVoice_DestroyVoice(r.fv);
    if (r.cb)
        for (WmaBuffer *b : r.cb->live) delete b; // buffers the engine dropped without a callback
    delete r.cb;
    delete r.wma;
    heap_free(a[0]);
    return 0;
}
uint32_t s_start(Cpu *, const uint32_t *a) { return FAudioSourceVoice_Start(fv(a[0]), a[1], a[2]); }
uint32_t s_stop(Cpu *, const uint32_t *a) { return FAudioSourceVoice_Stop(fv(a[0]), a[1], a[2]); }
uint32_t s_submit_source_buffer(Cpu *, const uint32_t *a) {
    // XAUDIO2_BUFFER: Flags, AudioBytes, pAudioData, PlayBegin, PlayLength, LoopBegin, LoopLength, LoopCount, pContext
    uint32_t g = a[1];
    FAudioBuffer b{};
    b.Flags = rd32(g);
    b.AudioBytes = rd32(g + 4);
    b.pAudioData = g_mem + rd32(g + 8); // not copied by the engine: stays in guest memory until OnBufferEnd
    b.PlayBegin = rd32(g + 12);
    b.PlayLength = rd32(g + 16);
    b.LoopBegin = rd32(g + 20);
    b.LoopLength = rd32(g + 24);
    b.LoopCount = rd32(g + 28);
    b.pContext = (void *)(uintptr_t)rd32(g + 32);
    FAudioBufferWMA w{};
    if (a[2]) { // XAUDIO2_BUFFER_WMA { const u32 *pDecodedPacketCumulativeBytes; u32 PacketCount }
        w.pDecodedPacketCumulativeBytes = g2h<uint32_t>(rd32(a[2]));
        w.PacketCount = rd32(a[2] + 4);
    }
    VoiceRec r = voice_rec(a[0]);
    if (!r.fv) return E_FAIL;
    if (r.wma) { // xWMA: decode now and queue the samples; the game's context value travels in the record
        auto *wb = new WmaBuffer;
        wb->guest_ctx = rd32(g + 32);
        uint32_t expect = (w.pDecodedPacketCumulativeBytes && w.PacketCount) ? w.pDecodedPacketCumulativeBytes[w.PacketCount - 1] : 0;
        wma_decode(r.wma, b.pAudioData, b.AudioBytes, expect, wb->pcm);
        if (b.Flags & FAUDIO_END_OF_STREAM) r.wma->fresh = true;
        b.pAudioData = reinterpret_cast<const uint8_t *>(wb->pcm.data());
        b.AudioBytes = (uint32_t)(wb->pcm.size() * 2);
        b.pContext = wb;
        {
            std::lock_guard<std::mutex> lk(r.cb->mu);
            r.cb->live.push_back(wb);
        }
        g_wma_buffers++;
        uint32_t hr = FAudioSourceVoice_SubmitSourceBuffer(r.fv, &b, nullptr);
        if (hr != 0) {
            std::lock_guard<std::mutex> lk(r.cb->mu);
            std::erase(r.cb->live, wb);
            delete wb;
        }
        return hr;
    }
    return FAudioSourceVoice_SubmitSourceBuffer(r.fv, &b, a[2] ? &w : nullptr);
}
uint32_t s_flush_source_buffers(Cpu *, const uint32_t *a) {
    VoiceRec r = voice_rec(a[0]);
    if (r.wma) r.wma->fresh = true;
    return FAudioSourceVoice_FlushSourceBuffers(r.fv);
}
uint32_t s_discontinuity(Cpu *, const uint32_t *a) { return FAudioSourceVoice_Discontinuity(fv(a[0])); }
uint32_t s_exit_loop(Cpu *, const uint32_t *a) { return FAudioSourceVoice_ExitLoop(fv(a[0]), a[1]); }
uint32_t s_get_state(Cpu *, const uint32_t *a) {
    // 2.7: (this, XAUDIO2_VOICE_STATE *) with { void *pCurrentBufferContext; u32 BuffersQueued; u64 SamplesPlayed }, packed
    FAudioVoiceState st{};
    VoiceRec r = voice_rec(a[0]);
    FAudioSourceVoice_GetState(r.fv, &st, 0);
    uint32_t ctx = (uint32_t)(uintptr_t)st.pCurrentBufferContext;
    if (r.wma && r.cb) { // the context is one of our records, which the mixer may free at any moment
        std::lock_guard<std::mutex> lk(r.cb->mu);
        ctx = 0;
        for (WmaBuffer *b : r.cb->live)
            if (b == st.pCurrentBufferContext) ctx = b->guest_ctx;
    }
    wr32(a[1], ctx);
    wr32(a[1] + 4, st.BuffersQueued);
    wr64(a[1] + 8, st.SamplesPlayed);
    return 0;
}
uint32_t s_set_frequency_ratio(Cpu *, const uint32_t *a) { return FAudioSourceVoice_SetFrequencyRatio(fv(a[0]), f32(a[1]), a[2]); }
uint32_t s_get_frequency_ratio(Cpu *, const uint32_t *a) {
    FAudioSourceVoice_GetFrequencyRatio(fv(a[0]), g2h<float>(a[1]));
    return 0;
}
uint32_t s_set_source_sample_rate(Cpu *, const uint32_t *a) { return FAudioSourceVoice_SetSourceSampleRate(fv(a[0]), a[1]); }

const ComMethod kVoiceMethods[] = {
    {"GetVoiceDetails", 2, v_get_voice_details}, {"SetOutputVoices", 2, v_set_output_voices}, {"SetEffectChain", 2, v_set_effect_chain},
    {"EnableEffect", 3, v_enable_effect}, {"DisableEffect", 3, v_disable_effect}, {"GetEffectState", 3, v_get_effect_state},
    {"SetEffectParameters", 5, v_set_effect_parameters}, {"GetEffectParameters", 4, v_get_effect_parameters},
    {"SetFilterParameters", 3, v_set_filter_parameters}, {"GetFilterParameters", 2, v_get_filter_parameters},
    {"SetOutputFilterParameters", 4, v_set_output_filter_parameters}, {"GetOutputFilterParameters", 3, v_get_output_filter_parameters},
    {"SetVolume", 3, v_set_volume}, {"GetVolume", 2, v_get_volume}, {"SetChannelVolumes", 4, v_set_channel_volumes},
    {"GetChannelVolumes", 3, v_get_channel_volumes}, {"SetOutputMatrix", 6, v_set_output_matrix}, {"GetOutputMatrix", 5, v_get_output_matrix},
    {"DestroyVoice", 1, v_destroy_voice},
    // IXAudio2SourceVoice continues here
    {"Start", 3, s_start}, {"Stop", 3, s_stop}, {"SubmitSourceBuffer", 3, s_submit_source_buffer},
    {"FlushSourceBuffers", 1, s_flush_source_buffers}, {"Discontinuity", 1, s_discontinuity}, {"ExitLoop", 2, s_exit_loop},
    {"GetState", 2, s_get_state}, {"SetFrequencyRatio", 3, s_set_frequency_ratio}, {"GetFrequencyRatio", 2, s_get_frequency_ratio},
    {"SetSourceSampleRate", 2, s_set_source_sample_rate},
};
constexpr size_t kBaseVoiceMethods = 19;

uint32_t voice_vtable(int kind) {
    switch (kind) {
    case KIND_SOURCE: return com_vtable("IXAudio2SourceVoice", kVoiceMethods, sizeof kVoiceMethods / sizeof kVoiceMethods[0]);
    case KIND_SUBMIX: return com_vtable("IXAudio2SubmixVoice", kVoiceMethods, kBaseVoiceMethods);
    default: return com_vtable("IXAudio2MasteringVoice", kVoiceMethods, kBaseVoiceMethods);
    }
}

} // namespace

// ---- host-side stream voices (the movies' sound tracks, bink_ffmpeg.cpp) ---------------------------------------
// A 16-bit PCM source voice on the game's engine, sent straight to the mastering voice. Queued blocks are
// copied and freed when the engine has played them.
struct HostStream {
    FAudioVoiceCallback cb; // first member
    FAudioSourceVoice *voice = nullptr;
    std::atomic<int> queued{0};
    bool playing = false;
    std::vector<int16_t> pending; // samples waiting for a free slot in the engine's queue (64 buffers)
};
static void FAUDIOCALL hs_buffer_end(FAudioVoiceCallback *c, void *ctx) {
    free(ctx);
    reinterpret_cast<HostStream *>(c)->queued--;
}
static void FAUDIOCALL hs_nop_ctx(FAudioVoiceCallback *, void *) {}
static void FAUDIOCALL hs_nop(FAudioVoiceCallback *) {}
static void FAUDIOCALL hs_nop_err(FAudioVoiceCallback *, void *, uint32_t) {}
static void FAUDIOCALL hs_nop_bytes(FAudioVoiceCallback *, uint32_t) {}

HostStream *xaudio2_stream_open(uint32_t channels, uint32_t rate) {
    FAudio *e = g_live_engine.load();
    if (!e) return nullptr; // no sound engine (yet): the caller plays silently
    auto *h = new HostStream;
    h->cb = {hs_buffer_end, hs_nop_ctx, hs_nop_ctx, hs_nop, hs_nop_err, hs_nop, hs_nop_bytes};
    FAudioWaveFormatEx f{};
    f.wFormatTag = FAUDIO_FORMAT_PCM;
    f.nChannels = (uint16_t)channels;
    f.nSamplesPerSec = rate;
    f.wBitsPerSample = 16;
    f.nBlockAlign = (uint16_t)(channels * 2);
    f.nAvgBytesPerSec = rate * f.nBlockAlign;
    if (FAudio_CreateSourceVoice(e, &h->voice, &f, 0, 2.0f, &h->cb, nullptr, nullptr) != 0 || !h->voice) {
        delete h;
        return nullptr;
    }
    return h;
}
// Queues interleaved samples (copied). When the engine's queue is full (the decoder delivers in bursts while the
// game catches up after a stall) the samples wait here and go out with the next block; only if more than ten
// seconds pile up is anything dropped (returns false).
bool xaudio2_stream_queue(HostStream *h, const int16_t *pcm, size_t samples) {
    if (!h || !samples) return false;
    constexpr size_t kMaxPending = 48000 * 2 * 10;
    if (h->pending.size() + samples > kMaxPending) return false;
    h->pending.insert(h->pending.end(), pcm, pcm + samples);
    if (h->queued >= FAUDIO_MAX_QUEUED_BUFFERS - 2) return true;
    size_t bytes = h->pending.size() * 2;
    void *copy = malloc(bytes);
    memcpy(copy, h->pending.data(), bytes);
    h->pending.clear();
    FAudioBuffer b{};
    b.AudioBytes = (uint32_t)bytes;
    b.pAudioData = static_cast<const uint8_t *>(copy);
    b.pContext = copy;
    h->queued++;
    if (FAudioSourceVoice_SubmitSourceBuffer(h->voice, &b, nullptr) != 0) {
        h->queued--;
        free(copy);
        return false;
    }
    return true;
}
// Samples the engine has played from this stream so far (frames, not bytes).
uint64_t xaudio2_stream_played(HostStream *h) {
    if (!h) return 0;
    FAudioVoiceState st{};
    FAudioSourceVoice_GetState(h->voice, &st, 0);
    return st.SamplesPlayed;
}
void xaudio2_stream_volume(HostStream *h, float volume) {
    if (h) FAudioVoice_SetVolume(h->voice, volume, 0);
}
void xaudio2_stream_play(HostStream *h, bool play) {
    if (!h || h->playing == play) return;
    h->playing = play;
    if (play) FAudioSourceVoice_Start(h->voice, 0, 0);
    else FAudioSourceVoice_Stop(h->voice, 0, 0);
}
void xaudio2_stream_close(HostStream *h) {
    if (!h) return;
    FAudioVoice_DestroyVoice(h->voice);
    delete h; // blocks still queued at this point are few and are left to the allocator
}

// Called by CoCreateInstance (misc_dlls.cpp). Returns an HRESULT and stores the new IXAudio2 in *ppv.
uint32_t xaudio2_create(const uint8_t *clsid, uint32_t ppv) {
    if (memcmp(clsid, kCLSID_XAudio2, 16) != 0 && memcmp(clsid, kCLSID_XAudio2_Debug, 16) != 0) return REGDB_E_CLASSNOTREG;
    const char *sw = getenv("BO1_AUDIO");
    if (sw && (!strcmp(sw, "off") || !strcmp(sw, "0"))) {
        log("CoCreateInstance(XAudio2) -> REGDB_E_CLASSNOTREG (BO1_AUDIO=off)");
        return REGDB_E_CLASSNOTREG;
    }
    FAudio *e = nullptr;
    // version 7 = the XAudio2 2.7 behaviour (device enumeration, Initialize, 2.7 mastering voice)
    uint32_t hr = FAudioCOMConstructWithCustomAllocatorEXT(&e, 7, malloc, free, realloc);
    if (hr != 0 || !e) {
        log("CoCreateInstance(XAudio2): FAudio construction failed: %#x", hr);
        return hr ? hr : E_FAIL;
    }
    uint32_t o = com_new(com_vtable("IXAudio2", kEngineMethods, sizeof kEngineMethods / sizeof kEngineMethods[0]), 8);
    {
        std::lock_guard<std::mutex> lk(g_mu);
        g_engines[o] = e;
    }
    log("CoCreateInstance(XAudio2 2.7) -> %08x (FAudio %u.%02u)", o, FAudioLinkedVersion() / 100 % 100, FAudioLinkedVersion() % 100);
    wr32(ppv, o);
    return S_OK;
}

} // namespace bo1
