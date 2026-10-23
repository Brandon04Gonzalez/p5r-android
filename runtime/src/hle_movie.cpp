// nn::movie at the API level. The library is the game's subsdk0, which is not recompiled: its imports land here.
// P5R's CRI Mana Switch backend (CriVpdSwitch, main+1276000..127a000) drives its VP9 decoder like Android's
// MediaCodec in asynchronous mode, from three threads:
//   - setup: Decoder(type, format, mode, priority), CreateVp9DecoderConfigurationData(w, h, fps * 1000, md) (true
//     on success), Configure(md) (0), RegisterClientEvents(events), Start();
//   - an event thread waits on DecoderEvents {input available, output available, format changed, error} (four
//     nn::os::EventType*), then takes indices with AcquireInputBufferIndex(&index, &more) and
//     AcquireOutputBufferIndex(&index, &pts, &flags, &more) (flag 4: end of stream) until they fail;
//   - a feeder fills GetInputBuffer(index) and calls SendInputBufferForDecode(index, offset, size, pts, flags); a
//     consumer passes its own frame memory to GetOutputBuffer(index, buffer), which the decoder fills, reads the
//     NV12 layout with GetOutputBufferFormat (format 4) and gives the index back with ReleaseOutputBufferIndex.
// Objects: Decoder (0x10, the game's operator new) holds our vtable and an id; Buffer (0x40) +0 offset, +4 size,
// +0x18 capacity, +0x28 base; MediaData (8 bytes) points at a host-side map of values.
//
// Decoding: on Android the device's own VP9 decoder (AMediaCodec, ByteBuffer mode, polled from the calls above: the
// feeder's sends, the event thread's acquires and the consumer's releases, and every 2 ms by a guest thread of ours
// that signals the output event), its pictures converted to tight NV12.
// Elsewhere, or when the device has no VP9 decoder, each input frame comes out as a black frame (NV12 Y 16, UV 128)
// of the same timestamp, so a video plays black for its length and the game goes on.
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#if defined(__ANDROID__)
#include <media/NdkMediaCodec.h>
#include <media/NdkMediaFormat.h>
#endif

#include <nx/runtime.h>

#include "platform.h"

namespace nx {

namespace {

// ---- MediaData ---------------------------------------------------------------------------------------------------

struct MediaValue {
    int32_t i = 0;
    int32_t rect[4] = {};
    std::string s;
    bool is_rect = false;
};
using MediaMap = std::map<std::string, MediaValue>;

MediaMap* media(uint64_t md) { return md ? reinterpret_cast<MediaMap*>(ld<uint64_t>(md)) : nullptr; }

void md_set(uint64_t md, const char* key, int32_t v) {
    if (MediaMap* m = media(md)) (*m)[key].i = v;
}

void md_set_rect(uint64_t md, const char* key, int32_t l, int32_t t, int32_t r, int32_t b) {
    if (MediaMap* m = media(md)) {
        MediaValue& v = (*m)[key];
        v.is_rect = true, v.rect[0] = l, v.rect[1] = t, v.rect[2] = r, v.rect[3] = b;
    }
}

bool md_get(uint64_t md, const char* key, int32_t& out) {
    MediaMap* m = media(md);
    auto it = m ? m->find(key) : MediaMap::iterator{};
    if (!m || it == m->end() || it->second.is_rect) return false;
    out = it->second.i;
    return true;
}

void MediaDataCtor(CPU& c) { st<uint64_t>(c.x[0], reinterpret_cast<uint64_t>(new MediaMap)); }
void MediaDataDtor(CPU& c) {
    delete media(c.x[0]);
    st<uint64_t>(c.x[0], 0);
}
void MediaDataClear(CPU& c) {
    if (MediaMap* m = media(c.x[0])) m->clear();
}
// bool FindInt32(const char* key, int32_t* out) const
void MediaDataFindInt32(CPU& c) {
    int32_t v = 0;
    bool found = md_get(c.x[0], reinterpret_cast<const char*>(c.x[1]), v);
    if (found && c.x[2]) st<int32_t>(c.x[2], v);
    c.x[0] = found;
}
// bool FindRect(const char* key, int32_t* left, int32_t* top, int32_t* right, int32_t* bottom) const
void MediaDataFindRect(CPU& c) {
    MediaMap* m = media(c.x[0]);
    auto it = m ? m->find(reinterpret_cast<const char*>(c.x[1])) : MediaMap::iterator{};
    bool found = m && it != m->end() && it->second.is_rect;
    for (int k = 0; k < 4 && found; k++)
        if (c.x[2 + k]) st<int32_t>(c.x[2 + k], it->second.rect[k]);
    c.x[0] = found;
}

// ---- Buffer ------------------------------------------------------------------------------------------------------

constexpr uint64_t kBufOffset = 0x0, kBufSize = 0x4, kBufCapacity = 0x18, kBufBase = 0x28;

void BufferCtor(CPU& c) { std::memset(reinterpret_cast<void*>(c.x[0]), 0, 0x40); }
void BufferDtor(CPU&) {}
void BufferSetDataAndCapacity(CPU& c) {  // (void* data, size_t capacity)
    st<uint64_t>(c.x[0] + kBufBase, c.x[1]);
    st<uint64_t>(c.x[0] + kBufCapacity, c.x[2]);
    st<int32_t>(c.x[0] + kBufOffset, 0);
    st<int32_t>(c.x[0] + kBufSize, 0);
}
void BufferSetRange(CPU& c) {  // (int offset, int size)
    st<int32_t>(c.x[0] + kBufOffset, int32_t(c.x[1]));
    st<int32_t>(c.x[0] + kBufSize, int32_t(c.x[2]));
}
void BufferBase(CPU& c) { c.x[0] = ld<uint64_t>(c.x[0] + kBufBase); }
void BufferCapacity(CPU& c) { c.x[0] = ld<uint64_t>(c.x[0] + kBufCapacity); }

// ---- Decoder -----------------------------------------------------------------------------------------------------

enum Event { kInputAvailable, kOutputAvailable, kFormatChanged, kError };
constexpr int kInputs = 4, kMaxOutputs = 32;
// Output slots offered to the game (GetOutputBufferSize). CriVpdSwitch keeps the pictures it took until it shows
// them, and feeds no more input while it holds nearly all: with 8, P5R's opening film (after New Game) played ~4 s
// and froze ~4 s over and over, 7 of the 8 held and no input sent. With 16 it plays through at 30 pictures a second.
// NX_MOVIE_OUTPUTS=<n> (2..32): another count.
int output_slots() {
    static const int n = [] {
        const char* e = std::getenv("NX_MOVIE_OUTPUTS");
        return e ? std::clamp(std::atoi(e), 2, kMaxOutputs) : 16;
    }();
    return n;
}
constexpr uint32_t kEndOfStream = 4;
constexpr uint32_t kTryAgain = 0x1001;  // any non-zero status: CriVpdSwitch waits for the next event

struct Decoder {
    uint32_t type = 0, format = 0, mode = 0;
    uint64_t events[4] = {};
    int32_t width = 0, height = 0, max_input = 0;
    std::vector<uint8_t> inputs[kInputs];
    std::deque<int> free_inputs;
    struct Output {
        int64_t pts = 0;
        uint32_t flags = 0;
        bool busy = false;
    } outputs[kMaxOutputs];
    std::deque<int> ready;                              // outputs to acquire
    std::deque<std::pair<int64_t, uint32_t>> pending;  // black frames made with every output slot busy
    std::vector<uint8_t> frames[kMaxOutputs];              // a slot's decoded picture (tight NV12); empty: black
    bool started = false, format_sent = false;
    uint64_t frames_in = 0, frames_out = 0, decoded = 0;
    // Per-window counts for the movie: line (pump thread, every 2 s while the decoder runs).
    struct Stats {
        uint64_t sent = 0, decoded = 0, acquired = 0, got = 0, released = 0, empty_acquires = 0;
        uint64_t convert_ns = 0, copy_ns = 0, send_wait_ns = 0, max_got_gap_ns = 0;
    } win;
    uint64_t last_got_ns = 0, win_from_ns = 0, last_sent_ns = 0;
    int64_t last_sent_pts = 0, last_got_pts = 0;
    bool starve_logged = false;  // NX_MOVIE_DUMP: threads logged for this pause in the input
#if defined(__ANDROID__)
    AMediaCodec* codec = nullptr;
    struct Input {
        std::vector<uint8_t> data;
        int64_t pts;
        uint32_t flags;
    };
    std::deque<Input> to_codec;  // sent by the game, not yet taken by the codec
    int32_t color = 0, codec_stride = 0, codec_rows = 0, crop_left = 0, crop_top = 0;
    bool eos_sent = false;   // the game's end of stream went to the codec
    bool codec_eos = false;  // the end-of-stream picture came out
#endif

    // CriVpdSwitch puts its UV texture right after width x height bytes of Y, whatever offsets we report: rows of
    // exactly the width, no padding.
    int32_t stride() const { return width; }
    int32_t rows() const { return height; }
    uint64_t nv12_size() const { return uint64_t(stride()) * rows() * 3 / 2; }

    int free_slot() const {
        for (int k = 0; k < output_slots(); k++)
            if (!outputs[k].busy) return k;
        return -1;
    }
    // A black frame into a free output slot (or the pending queue when all are taken).
    void emit(int64_t pts, uint32_t flags) {
        if (int k = free_slot(); k >= 0) {
            outputs[k] = {pts, flags, true};
            frames[k].clear();
            ready.push_back(k);
            return;
        }
        pending.emplace_back(pts, flags);
    }
    void reset_queues() {
        free_inputs.clear();
        for (int k = 0; k < kInputs; k++) free_inputs.push_back(k);
        for (auto& o : outputs) o = {};
        ready.clear(), pending.clear();
        win = {}, win_from_ns = last_got_ns = last_sent_ns = 0;
#if defined(__ANDROID__)
        to_codec.clear();
        eos_sent = codec_eos = false;
#endif
    }
};

#if defined(__ANDROID__)
constexpr int32_t kColorI420 = 19, kColorQcom32m = 0x7FA30C04;

// The codec's picture (color format `color`, rows of `stride` bytes, `rows` rows per plane) into tight NV12 of the
// video's size, from the crop origin. I420 is interleaved; every other format is taken as semi-planar (NV12 and the
// Qualcomm/MediaTek variants with aligned strides, which is what ByteBuffer decoders hand out).
void to_nv12(const Decoder& d, const uint8_t* src, size_t size, std::vector<uint8_t>& out) {
    const int32_t w = d.width, h = d.height;
    int32_t stride = d.codec_stride > 0 ? d.codec_stride : w, rows = d.codec_rows > 0 ? d.codec_rows : h;
    if (d.color == kColorQcom32m && d.codec_stride <= 0) stride = (w + 127) & ~127, rows = (h + 31) & ~31;
    out.resize(size_t(w) * h * 3 / 2);
    const size_t y0 = size_t(d.crop_top) * stride + d.crop_left;
    for (int32_t r = 0; r < h; r++) {
        size_t at = y0 + size_t(r) * stride;
        if (at + w > size) return;
        std::memcpy(out.data() + size_t(r) * w, src + at, size_t(w));
    }
    uint8_t* uv = out.data() + size_t(w) * h;
    const size_t chroma = size_t(stride) * rows;
    if (d.color == kColorI420) {
        const size_t cs = size_t(stride) / 2, plane = cs * (size_t(rows) / 2);
        for (int32_t r = 0; r < h / 2; r++) {
            size_t at = chroma + size_t(d.crop_top / 2 + r) * cs + d.crop_left / 2;
            if (at + plane + w / 2 > size) return;
            for (int32_t x = 0; x < w / 2; x++)
                uv[size_t(r) * w + 2 * x] = src[at + x], uv[size_t(r) * w + 2 * x + 1] = src[at + plane + x];
        }
        return;
    }
    for (int32_t r = 0; r < h / 2; r++) {
        size_t at = chroma + size_t(d.crop_top / 2 + r) * stride + (d.crop_left & ~1);
        if (at + w > size) return;
        std::memcpy(uv + size_t(r) * w, src + at, size_t(w));
    }
}

void read_codec_format(Decoder& d) {
    AMediaFormat* f = AMediaCodec_getOutputFormat(d.codec);
    if (!f) return;
    AMediaFormat_getInt32(f, "color-format", &d.color);
    AMediaFormat_getInt32(f, "stride", &d.codec_stride);
    AMediaFormat_getInt32(f, "slice-height", &d.codec_rows);
    int32_t l = 0, t = 0, r = 0, b = 0;
    if (AMediaFormat_getRect(f, "crop", &l, &t, &r, &b)) d.crop_left = l, d.crop_top = t;
    log("movie: codec output: color format %#x, stride %d, slice height %d, crop at %d,%d (%s)", d.color,
        d.codec_stride, d.codec_rows, d.crop_left, d.crop_top, AMediaFormat_toString(f));
    AMediaFormat_delete(f);
}

// Feeds the codec what it will take, then takes its pictures into free output slots (waiting up to `wait_us` for the
// first). True when an output became ready.
bool pump(Decoder& d, int64_t wait_us) {
    if (!d.codec) return false;
    while (!d.to_codec.empty()) {
        ssize_t i = AMediaCodec_dequeueInputBuffer(d.codec, 0);
        if (i < 0) break;
        size_t cap = 0;
        uint8_t* buf = AMediaCodec_getInputBuffer(d.codec, size_t(i), &cap);
        Decoder::Input& in = d.to_codec.front();
        size_t n = buf ? std::min(cap, in.data.size()) : 0;
        if (n) std::memcpy(buf, in.data.data(), n);
        AMediaCodec_queueInputBuffer(d.codec, size_t(i), 0, n, uint64_t(in.pts),
                                     (in.flags & kEndOfStream) ? AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM : 0);
        d.to_codec.pop_front();
    }
    bool any = false;
    for (int slot; (slot = d.free_slot()) >= 0;) {
        AMediaCodecBufferInfo info{};
        ssize_t o = AMediaCodec_dequeueOutputBuffer(d.codec, &info, any ? 0 : wait_us);
        if (o == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED) {
            read_codec_format(d);
            continue;
        }
        if (o == AMEDIACODEC_INFO_OUTPUT_BUFFERS_CHANGED) continue;
        if (o < 0) break;
        size_t size = 0;
        uint8_t* buf = AMediaCodec_getOutputBuffer(d.codec, size_t(o), &size);
        const uint64_t t0 = plat::monotonic_ns();
        if (buf && info.size > 0)
            to_nv12(d, buf + info.offset, size_t(info.size), d.frames[slot]);
        else
            d.frames[slot].clear();
        d.win.convert_ns += plat::monotonic_ns() - t0;
        d.win.decoded++;
        AMediaCodec_releaseOutputBuffer(d.codec, size_t(o), false);
        const bool eos = info.flags & AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM;
        d.codec_eos |= eos;
        d.outputs[slot] = {info.presentationTimeUs, eos ? kEndOfStream : 0u, true};
        d.ready.push_back(slot);
        d.decoded += info.size > 0;
        any = true;
    }
    return any;
}

// The device's VP9 decoder, configured for the video (null: none, black frames).
AMediaCodec* make_codec(const Decoder& d) {
    AMediaCodec* c = AMediaCodec_createDecoderByType("video/x-vnd.on2.vp9");
    if (!c) return nullptr;
    AMediaFormat* f = AMediaFormat_new();
    AMediaFormat_setString(f, AMEDIAFORMAT_KEY_MIME, "video/x-vnd.on2.vp9");
    AMediaFormat_setInt32(f, AMEDIAFORMAT_KEY_WIDTH, d.width);
    AMediaFormat_setInt32(f, AMEDIAFORMAT_KEY_HEIGHT, d.height);
    AMediaFormat_setInt32(f, AMEDIAFORMAT_KEY_MAX_INPUT_SIZE, d.max_input);
    AMediaFormat_setInt32(f, AMEDIAFORMAT_KEY_COLOR_FORMAT, 0x7F420888);  // COLOR_FormatYUV420Flexible
    media_status_t st = AMediaCodec_configure(c, f, nullptr, nullptr, 0);
    AMediaFormat_delete(f);
    if (st != AMEDIA_OK) {
        log("movie: the VP9 decoder refused %dx%d (%d): black frames", d.width, d.height, int(st));
        AMediaCodec_delete(c);
        return nullptr;
    }
    char* name = nullptr;
    if (AMediaCodec_getName(c, &name) == AMEDIA_OK && name) {
        log("movie: VP9 decoder %s", name);
        AMediaCodec_releaseName(c, name);
    }
    return c;
}
#endif

}  // namespace

// For the per-present film trace (NX_TRACE_VIDEO, nvn_queue.cpp): pictures the game took, and the last one's pts.
std::atomic<uint64_t> g_movie_gots{0};
std::atomic<int64_t> g_movie_last_pts{0};

namespace {

// NX_TRACE_VIDEO: each decoder event (acquired, taken, released) with its time, for the film's cadence.
void ev_trace(const char* what, int index, int extra = -1) {
    static long left = std::getenv("NX_TRACE_VIDEO") ? 3 * std::atol(std::getenv("NX_TRACE_VIDEO")) : 0;
    if (left <= 0) return;
    left--;
    log("movie trace: %s %d (%d) at %.1f ms, thread %lld", what, index, extra, double(plat::monotonic_ns() % 1000000000000ull) / 1e6,
        (long long)plat::current_thread_id());
}

std::mutex g_lock;
std::unordered_map<uint64_t, std::unique_ptr<Decoder>> g_decoders;
uint64_t g_next_id = 1;

Decoder* decoder(uint64_t self) {
    auto it = g_decoders.find(ld<uint64_t>(self + 8));
    return it == g_decoders.end() ? nullptr : it->second.get();
}

// The SDK's own nn::os::SignalEvent, outside our lock (the woken threads call back in).
void signal(CPU& c, uint64_t event) {
    static const uint64_t fn = find_symbol("_ZN2nn2os11SignalEventEPNS0_9EventTypeE");
    if (!event || !fn) return;
    c.x[0] = event;
    call(c, fn);
}

void signal_all(CPU& c, const std::vector<uint64_t>& events) {
    for (uint64_t e : events) signal(c, e);
}

// D2 (+0x00) and D0 (+0x08): the deleting one frees the object with the game's operator delete.
void DecoderDtor(CPU& c) {
    std::lock_guard lk(g_lock);
    auto it = g_decoders.find(ld<uint64_t>(c.x[0] + 8));
    if (it == g_decoders.end()) return;
#if defined(__ANDROID__)
    if (it->second->codec) AMediaCodec_delete(it->second->codec);
#endif
    g_decoders.erase(it);
}
void DecoderDeletingDtor(CPU& c) {
    const uint64_t self = c.x[0];
    DecoderDtor(c);
    static const uint64_t del = find_symbol("_ZdlPv");
    if (del) {
        c.x[0] = self;
        call(c, del);
    }
}

// Status Configure(MediaData*)
void DecoderConfigure(CPU& c) {
    std::lock_guard lk(g_lock);
    Decoder* d = decoder(c.x[0]);
    if (!d) return void(c.x[0] = kTryAgain);
    md_get(c.x[1], "width", d->width);
    md_get(c.x[1], "height", d->height);
    md_get(c.x[1], "max-input-size", d->max_input);
    if (d->max_input <= 0) d->max_input = 2 << 20;
    log("movie: decoder configured %dx%d, input buffers of %d bytes", d->width, d->height, d->max_input);
#if defined(__ANDROID__)
    if (!d->codec && d->width > 0 && d->height > 0) d->codec = make_codec(*d);
#endif
    c.x[0] = 0;
}

// Status RegisterClientEvents(DecoderEvents*)
void DecoderRegisterClientEvents(CPU& c) {
    std::lock_guard lk(g_lock);
    if (Decoder* d = decoder(c.x[0]))
        for (int k = 0; k < 4; k++) d->events[k] = c.x[1] ? ld<uint64_t>(c.x[1] + 8ull * k) : 0;
    c.x[0] = 0;
}

void put_format(const Decoder& d, uint64_t md) {
    md_set(md, "width", d.width);
    md_set(md, "height", d.height);
    md_set(md, "stride", d.stride());
    md_set(md, "slice-height", d.rows());
    md_set_rect(md, "crop", 0, 0, d.width - 1, d.height - 1);
    md_set(md, "nv12-y-stride", d.stride());
    md_set(md, "nv12-y-offset", 0);
    md_set(md, "nv12-uv-stride", d.stride());
    md_set(md, "nv12-uv-offset", d.stride() * d.rows());
}

// Status GetOutputFormat(MediaData*)
void DecoderGetOutputFormat(CPU& c) {
    std::lock_guard lk(g_lock);
    Decoder* d = decoder(c.x[0]);
    if (d) put_format(*d, c.x[1]);
    c.x[0] = d ? 0 : kTryAgain;
}

#if defined(__ANDROID__)
// The codec's pictures come out on their own time, a few to tens of milliseconds after their input. The calls above
// only take them when the game makes one, and CriVpdSwitch waits for the output event before it feeds much more: a
// picture that came out after the last call stalled the video for good (the casino opening after New Game, 424
// frames in, at ~15 fps until then). This guest thread drains every started codec every 2 ms and signals the event.
void movie_pump_thread(CPU& c) {
    log("movie: pump thread running");
    for (;;) {
        plat::sleep_ns(2'000'000);
        std::vector<uint64_t> events;
        bool dump = false;
        {
            std::lock_guard lk(g_lock);
            const uint64_t now = plat::monotonic_ns();
            for (auto& [id, d] : g_decoders) {
                if (d->started && d->codec && pump(*d, 0)) events.push_back(d->events[kOutputAvailable]);
                if (!d->started) continue;
                // NX_MOVIE_DUMP=<n>: the first n times a playing film gets no input for a second, where every guest
                // thread is (what the game's reader waits for).
                static int dumps_left = std::getenv("NX_MOVIE_DUMP") ? std::atoi(std::getenv("NX_MOVIE_DUMP")) : 0;
                if (dumps_left > 0 && d->last_sent_ns && !d->eos_sent && !d->starve_logged &&
                    now - d->last_sent_ns > 1'000'000'000ull) {
                    int busy = 0;
                    std::string held;
                    for (const auto& o : d->outputs)
                        if (o.busy) busy++, held += " " + std::to_string(o.pts / 1000);
                    log("movie: no input for %.0f ms (%llu frames in, last pts %lld ms, last taken %lld ms; ready %zu, "
                        "slots busy %d, their pts in ms:%s): threads follow", double(now - d->last_sent_ns) / 1e6,
                        (unsigned long long)d->frames_in, (long long)(d->last_sent_pts / 1000),
                        (long long)(d->last_got_pts / 1000), d->ready.size(), busy, held.c_str());
                    d->starve_logged = true, dumps_left--, dump = true;
                }
                if (!d->win_from_ns) d->win_from_ns = now;
                if (now - d->win_from_ns < 2'000'000'000ull) continue;
                // What the video did over the window: pictures the game sent, the codec decoded, the game took
                // (got = copied into its memory, about one per shown frame) and the longest wait between two.
                const auto& w = d->win;
                const double s = double(now - d->win_from_ns) / 1e9;
                int busy = 0;
                for (const auto& o : d->outputs) busy += o.busy;
                log("movie: per s: sent %.1f, decoded %.1f, acquired %.1f (+%.1f empty), got %.1f, released %.1f; "
                    "longest gap between gots %.0f ms; convert %.2f ms, copy %.2f ms, send wait %.2f ms per picture; "
                    "queued for the codec %zu, ready %zu, slots busy %d",
                    w.sent / s, w.decoded / s, w.acquired / s, w.empty_acquires / s, w.got / s, w.released / s,
                    double(w.max_got_gap_ns) / 1e6, w.decoded ? double(w.convert_ns) / 1e6 / double(w.decoded) : 0.0,
                    w.got ? double(w.copy_ns) / 1e6 / double(w.got) : 0.0,
                    w.sent ? double(w.send_wait_ns) / 1e6 / double(w.sent) : 0.0, d->to_codec.size(), d->ready.size(), busy);
                d->win = {};
                d->win_from_ns = now;
            }
        }
        if (dump) log_guest_threads();
        signal_all(c, events);
    }
}

void start_pump_thread(CPU& c) {
    static bool started = false;
    if (started) return;
    started = true;
    uint64_t create = find_symbol("_ZN2nn2os12CreateThreadEPNS0_10ThreadTypeEPFvPvES3_S3_mi");
    uint64_t start = find_symbol("_ZN2nn2os11StartThreadEPNS0_10ThreadTypeE");
    if (!create || !start) return log("movie: nn::os::CreateThread/StartThread not found, no pump thread");
    uint64_t thread = kernel_alloc(0x1000), stack = kernel_alloc(0x20000);
    uint64_t saved[31];
    std::memcpy(saved, c.x, sizeof saved);
    c.x[0] = thread, c.x[1] = hle_address("__movie_pump_thread"), c.x[2] = 0, c.x[3] = stack, c.x[4] = 0x20000;
    c.x[5] = 16;  // high priority: it only polls
    call(c, create);
    uint64_t rc = c.x[0];
    c.x[0] = thread;
    if (rc == 0) call(c, start);
    std::memcpy(c.x, saved, sizeof saved);
    if (rc) log("movie: CreateThread failed (%llx)", (unsigned long long)rc);
}
#endif

// Status Start(), Stop(), Reset(), Flush()
void DecoderStart(CPU& c) {
    uint64_t input_event = 0;
    {
        std::lock_guard lk(g_lock);
        if (Decoder* d = decoder(c.x[0])) {
            d->reset_queues();
            d->started = true;
            input_event = d->events[kInputAvailable];
#if defined(__ANDROID__)
            if (d->codec && AMediaCodec_start(d->codec) != AMEDIA_OK) {
                log("movie: the VP9 decoder did not start: black frames");
                AMediaCodec_delete(d->codec);
                d->codec = nullptr;
            }
#endif
        }
    }
#if defined(__ANDROID__)
    start_pump_thread(c);
#endif
    signal(c, input_event);
    c.x[0] = 0;
}
void DecoderStop(CPU& c) {
    std::lock_guard lk(g_lock);
    if (Decoder* d = decoder(c.x[0])) {
        log("movie: decoder stopped at present %llu, %.1f s, after %llu frames in, %llu out (%llu decoded)",
            (unsigned long long)g_frame.load(), double(plat::monotonic_ns()) / 1e9, (unsigned long long)d->frames_in,
            (unsigned long long)d->frames_out, (unsigned long long)d->decoded);
#if defined(__ANDROID__)
        if (d->codec && d->started) AMediaCodec_stop(d->codec);
#endif
        d->started = false;
        d->reset_queues();
    }
    c.x[0] = 0;
}
void DecoderFlush(CPU& c) {
    uint64_t input_event = 0;
    {
        std::lock_guard lk(g_lock);
        if (Decoder* d = decoder(c.x[0])) {
#if defined(__ANDROID__)
            if (d->codec && d->started) AMediaCodec_flush(d->codec);
#endif
            d->reset_queues();
            d->format_sent = false;
            input_event = d->started ? d->events[kInputAvailable] : 0;
        }
    }
    signal(c, input_event);
    c.x[0] = 0;
}

// Status AcquireInputBufferIndex(int* index, int* more)
void DecoderAcquireInputBufferIndex(CPU& c) {
    std::lock_guard lk(g_lock);
    Decoder* d = decoder(c.x[0]);
    if (!d || !d->started || d->free_inputs.empty()) return void(c.x[0] = kTryAgain);
    int index = d->free_inputs.front();
    d->free_inputs.pop_front();
    if (c.x[1]) st<int32_t>(c.x[1], index);
    if (c.x[2]) st<int32_t>(c.x[2], int32_t(d->free_inputs.size()));
    c.x[0] = 0;
}

// Status GetInputBuffer(int index, Buffer*)
void DecoderGetInputBuffer(CPU& c) {
    std::lock_guard lk(g_lock);
    Decoder* d = decoder(c.x[0]);
    int index = int(c.x[1]);
    if (!d || index < 0 || index >= kInputs || !c.x[2]) return void(c.x[0] = kTryAgain);
    auto& mem = d->inputs[index];
    if (mem.size() < size_t(d->max_input)) mem.assign(size_t(d->max_input), 0);
    st<uint64_t>(c.x[2] + kBufBase, reinterpret_cast<uint64_t>(mem.data()));
    st<uint64_t>(c.x[2] + kBufCapacity, mem.size());
    st<int32_t>(c.x[2] + kBufOffset, 0);
    st<int32_t>(c.x[2] + kBufSize, 0);
    c.x[0] = 0;
}

// Status SendInputBufferForDecode(int index, int offset, int size, int64_t pts, uint32_t flags)
void DecoderSendInputBufferForDecode(CPU& c) {
    std::vector<uint64_t> events;
    {
        std::lock_guard lk(g_lock);
        Decoder* d = decoder(c.x[0]);
        int index = int(c.x[1]);
        if (!d || index < 0 || index >= kInputs) return void(c.x[0] = kTryAgain);
        const int64_t pts = int64_t(c.x[4]);
        const uint32_t flags = uint32_t(c.x[5]);
        if (d->frames_in++ == 0)
            log("movie: first input frame, %d bytes, pts %lld, flags %#x", int(c.x[3]), (long long)pts, flags);
        bool output = true;
#if defined(__ANDROID__)
        if (d->codec) {
            // The bytes are the game's (its input buffer goes back at once): copied for the codec. A picture usually
            // comes out a frame or two later; at the end of the stream everything still inside is waited for.
            const uint8_t* src = d->inputs[index].data() + std::max<int64_t>(0, int64_t(c.x[2]));
            size_t n = std::min<size_t>(size_t(std::max<int64_t>(0, int64_t(c.x[3]))),
                                        d->inputs[index].size() - std::min<size_t>(d->inputs[index].size(), size_t(c.x[2])));
            d->to_codec.push_back({std::vector<uint8_t>(src, src + n), pts, flags & kEndOfStream});
            d->eos_sent |= (flags & kEndOfStream) != 0;
            const uint64_t t0 = plat::monotonic_ns();
            output = pump(*d, 4000);
            for (int k = 0; (flags & kEndOfStream) && !d->codec_eos && k < 100 && d->free_slot() >= 0; k++)
                output |= pump(*d, 20000);
            d->win.send_wait_ns += plat::monotonic_ns() - t0;
            d->win.sent++;
            d->last_sent_ns = plat::monotonic_ns(), d->starve_logged = false, d->last_sent_pts = pts;
        } else
#endif
            d->emit(pts, flags & kEndOfStream);
        d->free_inputs.push_back(index);
        if (!d->format_sent) d->format_sent = true, events.push_back(d->events[kFormatChanged]);
        if (output) events.push_back(d->events[kOutputAvailable]);
        events.push_back(d->events[kInputAvailable]);
    }
    signal_all(c, events);
    c.x[0] = 0;
}

// Status AcquireOutputBufferIndex(int* index, int64_t* pts, uint32_t* flags, int* more)
void DecoderAcquireOutputBufferIndex(CPU& c) {
    std::lock_guard lk(g_lock);
    Decoder* d = decoder(c.x[0]);
#if defined(__ANDROID__)
    if (d && d->ready.empty()) pump(*d, 0);
#endif
    if (d && d->ready.empty()) d->win.empty_acquires++;
    if (!d || d->ready.empty()) return void(c.x[0] = kTryAgain);
    int index = d->ready.front();
    d->ready.pop_front();
    ev_trace("acquired", index, int(d->ready.size()));
    d->win.acquired++;
    if (c.x[1]) st<int32_t>(c.x[1], index);
    if (c.x[2]) st<int64_t>(c.x[2], d->outputs[index].pts);
    if (c.x[3]) st<uint32_t>(c.x[3], d->outputs[index].flags);
    if (c.x[4]) st<int32_t>(c.x[4], int32_t(d->ready.size()));
    c.x[0] = 0;
}

// The frame into the client's memory: black NV12 at our layout (GetOutputBufferFormat).
void fill_black(const Decoder& d, uint64_t buffer) {
    uint8_t* base = reinterpret_cast<uint8_t*>(ld<uint64_t>(buffer + kBufBase));
    uint64_t cap = ld<uint64_t>(buffer + kBufCapacity);
    if (!base || !cap) return;
    uint64_t y = std::min<uint64_t>(cap, uint64_t(d.stride()) * d.rows());
    std::memset(base, 16, y);
    std::memset(base + y, 128, std::min<uint64_t>(cap - y, d.nv12_size() - y));
    st<int32_t>(buffer + kBufOffset, 0);
    st<int32_t>(buffer + kBufSize, int32_t(std::min<uint64_t>(cap, d.nv12_size())));
}

// Status GetOutputBuffer(int index, Buffer*) and GetOutputBuffer(int index, Buffer*, MediaData*)
void DecoderGetOutputBuffer(CPU& c) {
    std::lock_guard lk(g_lock);
    Decoder* d = decoder(c.x[0]);
    int index = int(c.x[1]);
    if (!d || index < 0 || index >= output_slots() || !d->outputs[index].busy || !c.x[2]) return void(c.x[0] = kTryAgain);
    static bool said = false;
    if (!said) said = true, log("movie: output format %u (4: NV12), frame %dx%d", d->format, d->width, d->height);
    const std::vector<uint8_t>& frame = d->frames[index];
    uint8_t* base = reinterpret_cast<uint8_t*>(ld<uint64_t>(c.x[2] + kBufBase));
    uint64_t cap = ld<uint64_t>(c.x[2] + kBufCapacity);
    const uint64_t t0 = plat::monotonic_ns();
    // The game's textures over this memory are write-watched (a fault per 4 KB page otherwise: 760 per 1080p frame).
    if (base && cap) plat::write_watch_will_write(reinterpret_cast<uint64_t>(base), std::min<uint64_t>(cap, d->nv12_size()));
    if (!frame.empty() && base && cap) {  // a decoded picture
        std::memcpy(base, frame.data(), std::min<uint64_t>(cap, frame.size()));
        st<int32_t>(c.x[2] + kBufOffset, 0);
        st<int32_t>(c.x[2] + kBufSize, int32_t(std::min<uint64_t>(cap, frame.size())));
    } else {
        fill_black(*d, c.x[2]);
    }
    const uint64_t t1 = plat::monotonic_ns();
    d->win.copy_ns += t1 - t0;
    d->win.got++;
    d->last_got_pts = d->outputs[index].pts;
    g_movie_last_pts = d->last_got_pts, g_movie_gots++;
    ev_trace("taken", index, int(reinterpret_cast<uint64_t>(base) & 0xfffffff));
    if (d->last_got_ns) d->win.max_got_gap_ns = std::max(d->win.max_got_gap_ns, t1 - d->last_got_ns);
    d->last_got_ns = t1;
    d->frames_out++;
    c.x[0] = 0;
}
void DecoderGetOutputBufferWithFormat(CPU& c) {
    const uint64_t self = c.x[0], md = c.x[3];
    DecoderGetOutputBuffer(c);
    std::lock_guard lk(g_lock);
    if (Decoder* d = decoder(self); d && c.x[0] == 0) put_format(*d, md);
}

// Status GetOutputBufferSize(int* size, int* ?)
void DecoderGetOutputBufferSize(CPU& c) {
    std::lock_guard lk(g_lock);
    Decoder* d = decoder(c.x[0]);
    if (!d) return void(c.x[0] = kTryAgain);
    if (c.x[1]) st<int32_t>(c.x[1], int32_t(d->nv12_size()));
    if (c.x[2]) st<int32_t>(c.x[2], output_slots());
    c.x[0] = 0;
}

// Status ReleaseOutputBufferIndex(int index)
void DecoderReleaseOutputBufferIndex(CPU& c) {
    uint64_t output_event = 0;
    {
        std::lock_guard lk(g_lock);
        Decoder* d = decoder(c.x[0]);
        int index = int(c.x[1]);
        if (d && index >= 0 && index < output_slots()) {
            d->outputs[index].busy = false;
            d->win.released++;
            ev_trace("released", index);
            if (!d->pending.empty()) {  // a slot is free again for a frame that waited
                auto [pts, flags] = d->pending.front();
                d->pending.pop_front();
                d->emit(pts, flags);
                output_event = d->events[kOutputAvailable];
            }
#if defined(__ANDROID__)
            // A picture the codec held for want of a slot; after the end of stream, the last ones are waited for.
            for (int k = 0; k < (d->eos_sent && !d->codec_eos ? 50 : 1) && d->codec; k++)
                if (pump(*d, d->eos_sent && !d->codec_eos ? 20000 : 0)) {
                    output_event = d->events[kOutputAvailable];
                    break;
                }
#endif
        }
    }
    signal(c, output_event);
    c.x[0] = 0;
}

// Status GetOutputBufferFormat(int index, MediaData*)
void DecoderGetOutputBufferFormat(CPU& c) {
    std::lock_guard lk(g_lock);
    Decoder* d = decoder(c.x[0]);
    if (d) put_format(*d, c.x[2]);
    c.x[0] = d ? 0 : kTryAgain;
}

void DecoderGetLastError(CPU& c) { c.x[0] = 0; }

const std::array<const char*, 19> kVtable = {
    "movie::Decoder::~Decoder", "movie::Decoder::~Decoder (deleting)", "movie::Decoder::Configure",
    "movie::Decoder::RegisterClientEvents", "movie::Decoder::GetOutputFormat", "movie::Decoder::Start",
    "movie::Decoder::Stop", "movie::Decoder::Reset", "movie::Decoder::Flush",
    "movie::Decoder::AcquireInputBufferIndex", "movie::Decoder::GetInputBuffer",
    "movie::Decoder::SendInputBufferForDecode", "movie::Decoder::AcquireOutputBufferIndex",
    "movie::Decoder::GetOutputBuffer", "movie::Decoder::GetOutputBufferSize",
    "movie::Decoder::ReleaseOutputBufferIndex", "movie::Decoder::GetLastError",
    "movie::Decoder::GetOutputBuffer (format)", "movie::Decoder::GetOutputBufferFormat"};

// Decoder::Decoder(DecoderType, DecoderOutputFormat, DecoderMode, int priority)
void DecoderCtor(CPU& c) {
    static uint64_t vtable[kVtable.size()];
    static std::once_flag once;
    std::call_once(once, [] {
        for (size_t k = 0; k < kVtable.size(); k++) vtable[k] = hle_address(kVtable[k]);
    });
    const uint64_t self = c.x[0];
    auto d = std::make_unique<Decoder>();
    d->type = uint32_t(c.x[1]), d->format = uint32_t(c.x[2]), d->mode = uint32_t(c.x[3]);
    log("movie: Decoder(type %u, output format %u, mode %u, priority %d) at present %llu, %.1f s", d->type, d->format,
        d->mode, int(c.x[4]), (unsigned long long)g_frame.load(), double(plat::monotonic_ns()) / 1e9);
    std::lock_guard lk(g_lock);
    const uint64_t id = g_next_id++;
    g_decoders[id] = std::move(d);
    st<uint64_t>(self, reinterpret_cast<uint64_t>(vtable));
    st<uint64_t>(self + 8, id);
}

// bool CreateVp9DecoderConfigurationData(int width, int height, int frame rate x 1000, MediaData*)
void CreateVp9DecoderConfigurationData(CPU& c) {
    const int32_t w = int32_t(c.x[0]), h = int32_t(c.x[1]);
    const uint64_t md = c.x[3];
    if (MediaMap* m = media(md)) (*m)["mime"].s = "video/x-vnd.on2.vp9";
    md_set(md, "width", w);
    md_set(md, "height", h);
    md_set(md, "frame-rate", int32_t(c.x[2]));
    md_set(md, "max-input-size", std::max(1 << 20, w * h * 3 / 2));
    c.x[0] = 1;
}

int32_t g_priority = 16;
void SetAllocator(CPU& c) { c.x[0] = 0; }  // our buffers are host memory
void SetCoreMask(CPU& c) { c.x[0] = 0; }
void SetThreadPriority(CPU& c) {
    g_priority = int32_t(c.x[0]);
    c.x[0] = 0;
}
void GetThreadPriority(CPU& c) { c.x[0] = uint32_t(g_priority); }

}  // namespace

NX_HLE("_ZN5movie12SetAllocatorEPFPvmmS0_EPFvS0_S0_EPFS0_S0_mS0_ES0_", SetAllocator);
NX_HLE("_ZN5movie11SetCoreMaskEm", SetCoreMask);
NX_HLE("_ZN5movie17SetThreadPriorityEi", SetThreadPriority);
NX_HLE("_ZN5movie17GetThreadPriorityEv", GetThreadPriority);
NX_HLE("_ZN5movie33CreateVp9DecoderConfigurationDataEiiiPNS_9MediaDataE", CreateVp9DecoderConfigurationData);
NX_HLE("_ZN5movie9MediaDataC1Ev", MediaDataCtor);
NX_HLE("_ZN5movie9MediaDataD1Ev", MediaDataDtor);
NX_HLE("_ZN5movie9MediaData5ClearEv", MediaDataClear);
NX_HLE("_ZNK5movie9MediaData9FindInt32EPKcPi", MediaDataFindInt32);
NX_HLE("_ZNK5movie9MediaData8FindRectEPKcPiS3_S3_S3_", MediaDataFindRect);
NX_HLE("_ZN5movie6BufferC1Ev", BufferCtor);
NX_HLE("_ZN5movie6BufferD1Ev", BufferDtor);
NX_HLE("_ZN5movie6Buffer18SetDataAndCapacityEPvm", BufferSetDataAndCapacity);
NX_HLE("_ZN5movie6Buffer8SetRangeEii", BufferSetRange);
NX_HLE("_ZNK5movie6Buffer4BaseEv", BufferBase);
NX_HLE("_ZNK5movie6Buffer8CapacityEv", BufferCapacity);
NX_HLE("_ZN5movie7DecoderC1ENS_11DecoderTypeENS_19DecoderOutputFormatENS_11DecoderModeEi", DecoderCtor);
// The vtable's entries (kVtable), in its order: subsdk0's _ZTVN5movie7DecoderE.
NX_HLE("movie::Decoder::~Decoder", DecoderDtor);
NX_HLE("movie::Decoder::~Decoder (deleting)", DecoderDeletingDtor);
NX_HLE("movie::Decoder::Configure", DecoderConfigure);
NX_HLE("movie::Decoder::RegisterClientEvents", DecoderRegisterClientEvents);
NX_HLE("movie::Decoder::GetOutputFormat", DecoderGetOutputFormat);
NX_HLE("movie::Decoder::Start", DecoderStart);
NX_HLE("movie::Decoder::Stop", DecoderStop);
NX_HLE("movie::Decoder::Reset", DecoderStop);
NX_HLE("movie::Decoder::Flush", DecoderFlush);
NX_HLE("movie::Decoder::AcquireInputBufferIndex", DecoderAcquireInputBufferIndex);
NX_HLE("movie::Decoder::GetInputBuffer", DecoderGetInputBuffer);
NX_HLE("movie::Decoder::SendInputBufferForDecode", DecoderSendInputBufferForDecode);
NX_HLE("movie::Decoder::AcquireOutputBufferIndex", DecoderAcquireOutputBufferIndex);
NX_HLE("movie::Decoder::GetOutputBuffer", DecoderGetOutputBuffer);
NX_HLE("movie::Decoder::GetOutputBufferSize", DecoderGetOutputBufferSize);
NX_HLE("movie::Decoder::ReleaseOutputBufferIndex", DecoderReleaseOutputBufferIndex);
NX_HLE("movie::Decoder::GetLastError", DecoderGetLastError);
NX_HLE("movie::Decoder::GetOutputBuffer (format)", DecoderGetOutputBufferWithFormat);
NX_HLE("movie::Decoder::GetOutputBufferFormat", DecoderGetOutputBufferFormat);
#if defined(__ANDROID__)
NX_HLE("__movie_pump_thread", movie_pump_thread);
#endif

}  // namespace nx
