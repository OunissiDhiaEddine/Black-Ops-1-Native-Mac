// binkw32.dll for the native runtime: the 18 Bink functions the game imports, on FFmpeg's Bink decoders
// (ADR-008, T-054). Sound tracks are decoded as their packets come by and played on the game's own sound
// engine through host-side stream voices of the XAudio2 shim (T-053) — not through DirectSound.
//
// The contract comes from the game's own code (docs/RE_NOTES.md "Bink as the game uses it",
// tools/bink_sites.py), not from the original DLL:
//   - the handle is a guest block of which the game reads +0x08 Frames, +0x0C FrameNum (1-based),
//     +0x14 FrameRate, +0x18 FrameRateDiv (a divisor: never 0);
//   - the game owns the picture planes: BinkGetFrameBuffersInfo reports sizes, the game allocates,
//     BinkRegisterFrameBuffers hands the block over, and before EVERY BinkDoFrame the game rewrites the
//     plane pointers in that block, so they are read from guest memory at decode time;
//   - BinkDoFrame stores the index of the frame set it filled in the block's FrameNum (+0x14);
//   - BinkGetRealtime: +0 FrameNum, +4 FrameRate (a divisor), +8 FrameRateDiv, +0x2C / +0x30 read-buffer
//     size / fill — the game pauses a movie whose buffer is below 20 %, so a full buffer is reported.
// BO1_BINK=off restores the old behaviour (BinkOpen fails and the game skips every movie).
#include <chrono>
#include <cstdlib>
#include <mutex>
#include <unordered_map>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/pixdesc.h>
}

#include "win32_util.h"

namespace bo1 {

// host-side stream voices on the game's sound engine (xaudio2_faudio.cpp); null = no engine, play silently
struct HostStream;
HostStream *xaudio2_stream_open(uint32_t channels, uint32_t rate);
bool xaudio2_stream_queue(HostStream *h, const int16_t *pcm, size_t samples);
void xaudio2_stream_volume(HostStream *h, float volume);
uint64_t xaudio2_stream_played(HostStream *h);
void xaudio2_stream_play(HostStream *h, bool play);
void xaudio2_stream_close(HostStream *h);

namespace {

// guest structure offsets (RE_NOTES; names per RAD's public header)
enum : uint32_t {
    BINK_WIDTH = 0x00, BINK_HEIGHT = 0x04, BINK_FRAMES = 0x08, BINK_FRAMENUM = 0x0C, BINK_LASTFRAMENUM = 0x10,
    BINK_FRAMERATE = 0x14, BINK_FRAMERATEDIV = 0x18, BINK_BLOCK_SIZE = 0x400,
    FB_TOTALFRAMES = 0x00, FB_YA_WIDTH = 0x04, FB_YA_HEIGHT = 0x08, FB_CRCB_WIDTH = 0x0C, FB_CRCB_HEIGHT = 0x10,
    FB_FRAMENUM = 0x14, FB_FRAMES = 0x18, FB_SET_SIZE = 0x30,
    PLANE_ALLOCATE = 0, PLANE_BUFFER = 4, PLANE_PITCH = 8, PLANE_SIZE = 12, // order in a set: Y, cR, cB, A
    RT_FRAMENUM = 0x00, RT_FRAMERATE = 0x04, RT_FRAMERATEDIV = 0x08, RT_FRAMES = 0x0C, RT_READBUFFERSIZE = 0x2C,
    RT_READBUFFERUSED = 0x30, RT_SIZE = 0x38,
    BINKFROMMEMORY = 0x04000000u,
};
constexpr uint32_t kFrameSets = 2; // the game's own tables hold two (loop bound 0x03E4CB50 at 0x006D8A75)

using Clock = std::chrono::steady_clock;

// One sound track of a movie. The game addresses tracks by the id stored in the file (BinkSetVolume): the
// shipped movies carry 0 = stereo, 1 = mono, 3 = stereo, and the game plays 0 and 1 and mutes 2..4.
struct Track {
    int stream = -1;
    uint32_t id = 0;
    AVCodecContext *dec = nullptr;
    HostStream *out = nullptr;
    float volume = 1.0f;
    uint64_t samples = 0, dropped = 0;
};

struct Movie {
    std::string name;
    AVFormatContext *fmt = nullptr;
    AVIOContext *io = nullptr;      // only for movies opened from guest memory
    uint32_t mem = 0, mem_size = 0; // guest address / size of such a movie
    int64_t mem_pos = 0;
    AVCodecContext *dec = nullptr;
    AVFrame *frame = nullptr;
    AVPacket *pkt = nullptr;
    int stream = -1;
    uint32_t width = 0, height = 0, frames = 0, rate = 30, rate_div = 1;
    bool alpha = false;
    uint32_t decoded = 0;  // 1-based number of the frame held in `frame`; 0 = none
    uint32_t fb = 0;       // guest BINKFRAMEBUFFERS registered by the game
    int set = -1;          // frame set filled last
    // presentation clock: frame number `advanced` (0-based count of BinkNextFrame calls) is due at
    // start + advanced * frame time; pausing stops the clock
    Clock::time_point start, pause_begin;
    bool paused = false;
    uint64_t advanced = 0;
    uint32_t shown = 0, skipped = 0;
    std::vector<Track> tracks;
    // sound against picture (T-053 check): at every shown frame, seconds of sound already played minus the
    // frame's own time. Positive = sound ahead of the picture.
    double sync_sum = 0, sync_min = 1e9, sync_max = -1e9;
    uint32_t sync_n = 0;
    AVFrame *aframe = nullptr;
    std::vector<int16_t> pcm;
};

std::mutex g_lock;
std::unordered_map<uint32_t, Movie *> g_movies; // guest BINK block -> movie
std::string g_error = "no error";
uint32_t g_error_guest = 0;

Movie *find(uint32_t bink) {
    auto it = g_movies.find(bink);
    return it == g_movies.end() ? nullptr : it->second;
}

double frame_seconds(const Movie *m) { return (double)m->rate_div / (double)m->rate; }
double elapsed(const Movie *m) {
    Clock::time_point now = m->paused ? m->pause_begin : Clock::now();
    return std::chrono::duration<double>(now - m->start).count();
}
// seconds by which the current frame is overdue (negative: not yet time)
double lateness(const Movie *m) { return elapsed(m) - (double)m->advanced * frame_seconds(m); }

int mem_read(void *opaque, uint8_t *buf, int size) {
    Movie *m = static_cast<Movie *>(opaque);
    int64_t left = (int64_t)m->mem_size - m->mem_pos;
    if (left <= 0) return AVERROR_EOF;
    if (size > left) size = (int)left;
    memcpy(buf, g_mem + m->mem + m->mem_pos, (size_t)size);
    m->mem_pos += size;
    return size;
}
int64_t mem_seek(void *opaque, int64_t offset, int whence) {
    Movie *m = static_cast<Movie *>(opaque);
    if (whence == AVSEEK_SIZE) return m->mem_size;
    int64_t pos = whence == SEEK_SET ? offset : whence == SEEK_CUR ? m->mem_pos + offset : whence == SEEK_END ? (int64_t)m->mem_size + offset : -1;
    if (pos < 0 || pos > (int64_t)m->mem_size) return AVERROR(EINVAL);
    return m->mem_pos = pos;
}

void movie_free(Movie *m) {
    if (!m) return;
    for (Track &t : m->tracks) {
        xaudio2_stream_close(t.out);
        avcodec_free_context(&t.dec);
    }
    av_frame_free(&m->aframe);
    av_frame_free(&m->frame);
    av_packet_free(&m->pkt);
    avcodec_free_context(&m->dec);
    avformat_close_input(&m->fmt);
    if (m->io) {
        av_freep(&m->io->buffer);
        avio_context_free(&m->io);
    }
    delete m;
}

std::string av_message(int err) {
    char buf[128];
    av_strerror(err, buf, sizeof buf);
    return buf;
}

// Opens the container and the video decoder. On failure `why` says what went wrong.
Movie *movie_open(uint32_t source, uint32_t flags, std::string *why) {
    auto *m = new Movie;
    int err;
    if (flags & BINKFROMMEMORY) {
        // a whole .bik in guest memory (rawfile from a fastfile). The file's length is in its own header:
        // bytes 4..7 = size of everything after the first 8 bytes.
        m->name = "<memory>";
        m->mem = source;
        m->mem_size = rd32(source + 4) + 8;
        constexpr int kBuf = 64 * 1024;
        m->io = avio_alloc_context(static_cast<uint8_t *>(av_malloc(kBuf)), kBuf, 0, m, mem_read, nullptr, mem_seek);
        m->fmt = avformat_alloc_context();
        m->fmt->pb = m->io;
        err = avformat_open_input(&m->fmt, nullptr, nullptr, nullptr);
    } else {
        m->name = gstr(source);
        err = avformat_open_input(&m->fmt, host_path(m->name).c_str(), nullptr, nullptr);
    }
    if (err < 0) {
        *why = "Error opening file: " + av_message(err);
        movie_free(m);
        return nullptr;
    }
    if ((err = avformat_find_stream_info(m->fmt, nullptr)) < 0 ||
        (m->stream = av_find_best_stream(m->fmt, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0)) < 0) {
        *why = "Not a Bink file (no video stream)";
        movie_free(m);
        return nullptr;
    }
    AVStream *st = m->fmt->streams[m->stream];
    const AVCodec *codec = avcodec_find_decoder(st->codecpar->codec_id);
    m->dec = codec ? avcodec_alloc_context3(codec) : nullptr;
    if (!m->dec || avcodec_parameters_to_context(m->dec, st->codecpar) < 0 || (err = avcodec_open2(m->dec, codec, nullptr)) < 0) {
        *why = std::string("No decoder for ") + avcodec_get_name(st->codecpar->codec_id);
        movie_free(m);
        return nullptr;
    }
    for (unsigned i = 0; i < m->fmt->nb_streams; i++) {
        AVStream *as = m->fmt->streams[i];
        if ((int)i == m->stream) continue;
        Track t;
        const AVCodec *ac = as->codecpar->codec_type == AVMEDIA_TYPE_AUDIO ? avcodec_find_decoder(as->codecpar->codec_id) : nullptr;
        if (ac && (t.dec = avcodec_alloc_context3(ac)) && avcodec_parameters_to_context(t.dec, as->codecpar) >= 0 && avcodec_open2(t.dec, ac, nullptr) >= 0) {
            t.stream = (int)i;
            t.id = (uint32_t)as->id;
            t.out = xaudio2_stream_open((uint32_t)as->codecpar->ch_layout.nb_channels, (uint32_t)as->codecpar->sample_rate);
            m->tracks.push_back(t);
        } else {
            avcodec_free_context(&t.dec);
            as->discard = AVDISCARD_ALL;
        }
    }
    m->aframe = av_frame_alloc();
    m->frame = av_frame_alloc();
    m->pkt = av_packet_alloc();
    m->width = (uint32_t)st->codecpar->width;
    m->height = (uint32_t)st->codecpar->height;
    // the Bink demuxer's time base is one frame: den / num = frames per second, duration = frame count
    if (st->time_base.num > 0 && st->time_base.den > 0) {
        m->rate = (uint32_t)st->time_base.den;
        m->rate_div = (uint32_t)st->time_base.num;
    }
    m->frames = st->nb_frames > 0 ? (uint32_t)st->nb_frames : st->duration > 0 ? (uint32_t)st->duration : 1;
    const AVPixFmtDescriptor *pf = av_pix_fmt_desc_get((AVPixelFormat)st->codecpar->format);
    m->alpha = pf && (pf->flags & AV_PIX_FMT_FLAG_ALPHA);
    m->start = Clock::now();
    return m;
}

// A sound-track packet: decode and queue it on the track's voice (interleaved 16-bit). Muted tracks are skipped.
void movie_audio(Movie *m, Track &t) {
    if (!t.out || t.volume <= 0.0f) return;
    if (avcodec_send_packet(t.dec, m->pkt) < 0) return;
    while (avcodec_receive_frame(t.dec, m->aframe) >= 0) {
        const AVFrame *f = m->aframe;
        int ch = f->ch_layout.nb_channels, n = f->nb_samples;
        m->pcm.resize((size_t)n * (size_t)ch);
        auto put = [&](int i, int c, float v) {
            v *= 32767.0f;
            m->pcm[(size_t)i * ch + c] = (int16_t)(v > 32767.0f ? 32767.0f : v < -32768.0f ? -32768.0f : v);
        };
        if (f->format == AV_SAMPLE_FMT_FLTP) {
            for (int c = 0; c < ch; c++) {
                const float *src = reinterpret_cast<const float *>(f->extended_data[c]);
                for (int i = 0; i < n; i++) put(i, c, src[i]);
            }
        } else if (f->format == AV_SAMPLE_FMT_FLT) {
            const float *src = reinterpret_cast<const float *>(f->extended_data[0]);
            for (int i = 0; i < n * ch; i++) put(i / ch, i % ch, src[i]);
        } else {
            m->pcm.assign(m->pcm.size(), 0);
        }
        if (xaudio2_stream_queue(t.out, m->pcm.data(), m->pcm.size())) t.samples += (uint64_t)n;
        else t.dropped += (uint64_t)n;
        av_frame_unref(m->aframe);
    }
}

// Makes `frame` hold frame number `want` (1-based). Bink frames depend on their predecessors, so going
// back (a looping movie wraps to frame 1) restarts the stream.
bool movie_decode(Movie *m, uint32_t want) {
    if (m->decoded == want) return true;
    if (want < m->decoded || m->decoded == 0) {
        if (m->decoded != 0) {
            if (av_seek_frame(m->fmt, m->stream, 0, AVSEEK_FLAG_BACKWARD) < 0) {
                log("bink: %s: cannot rewind for frame %u", m->name.c_str(), want);
                return false;
            }
            avcodec_flush_buffers(m->dec);
            for (Track &t : m->tracks) avcodec_flush_buffers(t.dec);
        }
        m->decoded = 0;
    }
    while (m->decoded < want) {
        int err = av_read_frame(m->fmt, m->pkt);
        if (err < 0) {
            if (g_verbose) log("bink: %s: end of stream at frame %u (wanted %u): %s", m->name.c_str(), m->decoded, want, av_message(err).c_str());
            return false;
        }
        if (m->pkt->stream_index == m->stream) {
            if (avcodec_send_packet(m->dec, m->pkt) >= 0 && avcodec_receive_frame(m->dec, m->frame) >= 0) m->decoded++;
        } else {
            for (Track &t : m->tracks)
                if (t.stream == m->pkt->stream_index) movie_audio(m, t);
        }
        av_packet_unref(m->pkt);
    }
    return true;
}

// One decoded plane -> the game's plane memory (tight rows of `w` bytes at the pitch the game set).
void copy_plane(uint32_t plane, const uint8_t *src, int src_pitch, uint32_t w, uint32_t h, uint32_t src_w, uint32_t src_h) {
    uint32_t dst = rd32(plane + PLANE_BUFFER), pitch = rd32(plane + PLANE_PITCH);
    if (!dst || !src) return;
    if (pitch < w) pitch = w;
    uint32_t cw = src_w < w ? src_w : w, ch = src_h < h ? src_h : h;
    for (uint32_t y = 0; y < ch; y++) memcpy(g_mem + dst + y * pitch, src + (size_t)y * (size_t)src_pitch, cw);
}

void movie_present(Movie *m) {
    if (!m->fb || m->decoded == 0) return;
    uint32_t total = rd32(m->fb + FB_TOTALFRAMES);
    if (total == 0 || total > kFrameSets) total = kFrameSets;
    m->set = (m->set + 1) % (int)total;
    uint32_t set = m->fb + FB_FRAMES + (uint32_t)m->set * FB_SET_SIZE;
    uint32_t yw = rd32(m->fb + FB_YA_WIDTH), yh = rd32(m->fb + FB_YA_HEIGHT);
    uint32_t cw = rd32(m->fb + FB_CRCB_WIDTH), ch = rd32(m->fb + FB_CRCB_HEIGHT);
    const AVFrame *f = m->frame;
    uint32_t fw = (uint32_t)f->width, fh = (uint32_t)f->height, fcw = (fw + 1) / 2, fch = (fh + 1) / 2;
    copy_plane(set + 0 * PLANE_SIZE, f->data[0], f->linesize[0], yw, yh, fw, fh);     // Y
    copy_plane(set + 1 * PLANE_SIZE, f->data[2], f->linesize[2], cw, ch, fcw, fch);   // cR = V
    copy_plane(set + 2 * PLANE_SIZE, f->data[1], f->linesize[1], cw, ch, fcw, fch);   // cB = U
    if (m->alpha) copy_plane(set + 3 * PLANE_SIZE, f->data[3], f->linesize[3], yw, yh, fw, fh);
    wr32(m->fb + FB_FRAMENUM, (uint32_t)m->set);
    m->shown++;
    for (const Track &t : m->tracks) {
        if (!t.out || t.volume <= 0.0f || !t.dec || t.dec->sample_rate <= 0) continue;
        double d = (double)xaudio2_stream_played(t.out) / t.dec->sample_rate - (double)(m->decoded - 1) * frame_seconds(m);
        m->sync_sum += d, m->sync_n++;
        if (d < m->sync_min) m->sync_min = d;
        if (d > m->sync_max) m->sync_max = d;
        break; // one track is enough: they are queued together
    }
}

bool bink_enabled() {
    static const bool on = [] {
        const char *v = getenv("BO1_BINK");
        return !(v && (!strcmp(v, "off") || !strcmp(v, "0")));
    }();
    return on;
}

} // namespace

SHIM_NAMED(binkw32, BinkOpen, "_BinkOpen@8", 2) {
    // (name or memory, flags) -> HBINK or 0
    std::lock_guard<std::mutex> g(g_lock);
    if (!bink_enabled()) {
        g_error = "Bink video is switched off (BO1_BINK=off)";
        return 0;
    }
    std::string why;
    Movie *m = movie_open(a[0], a[1], &why);
    if (!m) {
        g_error = why;
        log("BinkOpen(%s, %#x) failed: %s", (a[1] & BINKFROMMEMORY) ? "<memory>" : gstr(a[0]).c_str(), a[1], why.c_str());
        return 0;
    }
    uint32_t bink = heap_alloc(BINK_BLOCK_SIZE, true);
    wr32(bink + BINK_WIDTH, m->width);
    wr32(bink + BINK_HEIGHT, m->height);
    wr32(bink + BINK_FRAMES, m->frames);
    wr32(bink + BINK_FRAMENUM, 1);
    wr32(bink + BINK_FRAMERATE, m->rate);
    wr32(bink + BINK_FRAMERATEDIV, m->rate_div);
    g_movies[bink] = m;
    std::string tr;
    for (const Track &t : m->tracks) tr += " " + std::to_string(t.id) + (t.out ? "" : "(no engine)");
    log("BinkOpen(%s, %#x): %ux%u, %u frames at %u/%u fps%s; sound tracks:%s", m->name.c_str(), a[1], m->width, m->height, m->frames, m->rate,
        m->rate_div, m->alpha ? ", alpha" : "", tr.empty() ? " none" : tr.c_str());
    return bink;
}

SHIM_NAMED(binkw32, BinkClose, "_BinkClose@4", 1) {
    std::lock_guard<std::mutex> g(g_lock);
    if (Movie *m = find(a[0])) {
        std::string tr;
        for (const Track &t : m->tracks)
            if (t.samples || t.dropped) tr += " track " + std::to_string(t.id) + ": " + std::to_string(t.samples) + " samples (" + std::to_string(t.dropped) + " dropped)";
        log("BinkClose(%s): %u frames shown, %u skipped;%s", m->name.c_str(), m->shown, m->skipped, tr.empty() ? " no sound played" : tr.c_str());
        if (m->sync_n)
            log("BinkClose(%s): sound minus picture at %u frames: avg %+.0f ms, min %+.0f ms, max %+.0f ms", m->name.c_str(), m->sync_n,
                1000 * m->sync_sum / m->sync_n, 1000 * m->sync_min, 1000 * m->sync_max);
        g_movies.erase(a[0]);
        movie_free(m);
        heap_free(a[0]);
    }
    return 0;
}

SHIM_NAMED(binkw32, BinkGetError, "_BinkGetError@0", 0) {
    std::lock_guard<std::mutex> g(g_lock);
    if (g_error_guest) heap_free(g_error_guest);
    return g_error_guest = heap_strdup(g_error);
}

SHIM_NAMED(binkw32, BinkGetFrameBuffersInfo, "_BinkGetFrameBuffersInfo@8", 2) {
    // (bink, BINKFRAMEBUFFERS *): sizes and which planes exist; the game then allocates them
    std::lock_guard<std::mutex> g(g_lock);
    Movie *m = find(a[0]);
    if (!m || !a[1]) return 0;
    uint32_t fb = a[1];
    wr32(fb + FB_TOTALFRAMES, kFrameSets);
    wr32(fb + FB_YA_WIDTH, m->width);
    wr32(fb + FB_YA_HEIGHT, m->height);
    wr32(fb + FB_CRCB_WIDTH, (m->width + 1) / 2);
    wr32(fb + FB_CRCB_HEIGHT, (m->height + 1) / 2);
    wr32(fb + FB_FRAMENUM, 0);
    for (uint32_t i = 0; i < kFrameSets; i++)
        for (uint32_t p = 0; p < 4; p++) wr32(fb + FB_FRAMES + i * FB_SET_SIZE + p * PLANE_SIZE + PLANE_ALLOCATE, p < 3 || m->alpha ? 1 : 0);
    return 1;
}

SHIM_NAMED(binkw32, BinkRegisterFrameBuffers, "_BinkRegisterFrameBuffers@8", 2) {
    std::lock_guard<std::mutex> g(g_lock);
    if (Movie *m = find(a[0])) m->fb = a[1];
    return 0;
}

SHIM_NAMED(binkw32, BinkDoFrame, "_BinkDoFrame@4", 1) {
    // decodes frame bink->FrameNum into the next frame set of the registered buffers
    std::lock_guard<std::mutex> g(g_lock);
    Movie *m = find(a[0]);
    if (!m) return 0;
    uint32_t want = rd32(a[0] + BINK_FRAMENUM);
    bool fresh = m->decoded != want;
    if (movie_decode(m, want) && (fresh || m->set < 0)) movie_present(m);
    if (!m->paused)
        for (Track &t : m->tracks) xaudio2_stream_play(t.out, true);
    return 0;
}

SHIM_NAMED(binkw32, BinkNextFrame, "_BinkNextFrame@4", 1) {
    std::lock_guard<std::mutex> g(g_lock);
    Movie *m = find(a[0]);
    if (!m) return 0;
    uint32_t n = rd32(a[0] + BINK_FRAMENUM);
    wr32(a[0] + BINK_LASTFRAMENUM, n);
    wr32(a[0] + BINK_FRAMENUM, n >= m->frames ? 1 : n + 1); // a movie that is advanced past its end starts over
    m->advanced++;
    return 0;
}

SHIM_NAMED(binkw32, BinkWait, "_BinkWait@4", 1) {
    // non-zero while it is too early for the current frame
    std::lock_guard<std::mutex> g(g_lock);
    Movie *m = find(a[0]);
    if (!m) return 0;
    double late = lateness(m);
    if (late > 0.5) // the game was busy (loading, a stall): carry on from here instead of racing to catch up
        m->start += std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(late));
    return late < 0 ? 1 : 0;
}

SHIM_NAMED(binkw32, BinkShouldSkip, "_BinkShouldSkip@4", 1) {
    // non-zero when playback is more than a frame behind: the game then decodes the next frame without showing this one
    std::lock_guard<std::mutex> g(g_lock);
    Movie *m = find(a[0]);
    if (!m || m->paused) return 0;
    double late = lateness(m);
    if (late > 2 * frame_seconds(m) && late <= 0.5) {
        m->skipped++;
        return 1;
    }
    return 0;
}

SHIM_NAMED(binkw32, BinkPause, "_BinkPause@8", 2) {
    // (bink, pause) -> pause state
    std::lock_guard<std::mutex> g(g_lock);
    Movie *m = find(a[0]);
    if (!m) return 0;
    bool pause = a[1] != 0;
    if (pause && !m->paused) {
        m->pause_begin = Clock::now();
    } else if (!pause && m->paused) {
        m->start += Clock::now() - m->pause_begin;
    }
    m->paused = pause;
    for (Track &t : m->tracks) xaudio2_stream_play(t.out, !pause && m->decoded != 0);
    return pause;
}

SHIM_NAMED(binkw32, BinkGetRealtime, "_BinkGetRealtime@12", 3) {
    // (bink, BINKREALTIME *, window)
    std::lock_guard<std::mutex> g(g_lock);
    Movie *m = find(a[0]);
    if (!a[1]) return 0;
    memset(g_mem + a[1], 0, RT_SIZE);
    wr32(a[1] + RT_FRAMENUM, m ? rd32(a[0] + BINK_FRAMENUM) : 1);
    wr32(a[1] + RT_FRAMERATE, m ? m->rate : 30);
    wr32(a[1] + RT_FRAMERATEDIV, m ? m->rate_div : 1);
    wr32(a[1] + RT_FRAMES, m ? m->frames : 1);
    wr32(a[1] + RT_READBUFFERSIZE, 0x100000); // "buffer full": the decoder reads the file itself
    wr32(a[1] + RT_READBUFFERUSED, 0x100000);
    return 0;
}

// Sound output is ours (see the top of the file), so the DirectSound hand-over and the track selection need no
// action: every track of the file is opened and BinkSetVolume decides which ones are heard.
SHIM_NAMED(binkw32, BinkSetSoundSystem, "_BinkSetSoundSystem@8", 2) { return 1; }
SHIM_NAMED(binkw32, BinkOpenDirectSound, "_BinkOpenDirectSound@4", 1) { return 0; }
SHIM_NAMED(binkw32, BinkSetSoundTrack, "_BinkSetSoundTrack@8", 2) { return 0; }
SHIM_NAMED(binkw32, BinkSetVolume, "_BinkSetVolume@12", 3) {
    // (bink, track id, volume 0..32768)
    std::lock_guard<std::mutex> g(g_lock);
    if (Movie *m = find(a[0]))
        for (Track &t : m->tracks)
            if (t.id == a[1]) {
                t.volume = (float)a[2] / 32768.0f;
                xaudio2_stream_volume(t.out, t.volume);
            }
    return 0;
}
// Memory and I/O tuning of the original library: the host decoder allocates and reads by itself.
SHIM_NAMED(binkw32, BinkSetMemory, "_BinkSetMemory@8", 2) { return 0; }
SHIM_NAMED(binkw32, BinkSetIOSize, "_BinkSetIOSize@4", 1) { return 0; }
SHIM_NAMED(binkw32, BinkControlBackgroundIO, "_BinkControlBackgroundIO@8", 2) { return 0; }

} // namespace bo1
