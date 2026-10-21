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
// No VP9 decoding yet: each input frame comes out as an output frame of the same timestamp, black (NV12 Y 16,
// UV 128), so a video plays black for its length and the game goes on (games/p5r/PROGRESO.md: AMediaCodec next).
#include <algorithm>
#include <array>
#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include <nx/runtime.h>

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
constexpr int kInputs = 4, kOutputs = 8;
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
    } outputs[kOutputs];
    std::deque<int> ready;                              // outputs to acquire
    std::deque<std::pair<int64_t, uint32_t>> pending;  // decoded with every output slot busy
    bool started = false, format_sent = false;
    uint64_t frames_in = 0, frames_out = 0;

    // CriVpdSwitch puts its UV texture right after width x height bytes of Y, whatever offsets we report: rows of
    // exactly the width, no padding.
    int32_t stride() const { return width; }
    int32_t rows() const { return height; }
    uint64_t nv12_size() const { return uint64_t(stride()) * rows() * 3 / 2; }

    // A decoded frame into a free output slot (or the pending queue when all are taken).
    void emit(int64_t pts, uint32_t flags) {
        for (int k = 0; k < kOutputs; k++)
            if (!outputs[k].busy) {
                outputs[k] = {pts, flags, true};
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
    }
};

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
    g_decoders.erase(ld<uint64_t>(c.x[0] + 8));
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

// Status Start(), Stop(), Reset(), Flush()
void DecoderStart(CPU& c) {
    uint64_t input_event = 0;
    {
        std::lock_guard lk(g_lock);
        if (Decoder* d = decoder(c.x[0])) {
            d->reset_queues();
            d->started = true;
            input_event = d->events[kInputAvailable];
        }
    }
    signal(c, input_event);
    c.x[0] = 0;
}
void DecoderStop(CPU& c) {
    std::lock_guard lk(g_lock);
    if (Decoder* d = decoder(c.x[0])) {
        log("movie: decoder stopped after %llu frames in, %llu out", (unsigned long long)d->frames_in,
            (unsigned long long)d->frames_out);
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
        d->emit(pts, flags & kEndOfStream);
        d->free_inputs.push_back(index);
        if (!d->format_sent) d->format_sent = true, events.push_back(d->events[kFormatChanged]);
        events.push_back(d->events[kOutputAvailable]);
        events.push_back(d->events[kInputAvailable]);
    }
    signal_all(c, events);
    c.x[0] = 0;
}

// Status AcquireOutputBufferIndex(int* index, int64_t* pts, uint32_t* flags, int* more)
void DecoderAcquireOutputBufferIndex(CPU& c) {
    std::lock_guard lk(g_lock);
    Decoder* d = decoder(c.x[0]);
    if (!d || d->ready.empty()) return void(c.x[0] = kTryAgain);
    int index = d->ready.front();
    d->ready.pop_front();
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
    if (!d || index < 0 || index >= kOutputs || !d->outputs[index].busy || !c.x[2]) return void(c.x[0] = kTryAgain);
    static bool said = false;
    if (!said) said = true, log("movie: output format %u (4: NV12), frame %dx%d", d->format, d->width, d->height);
    fill_black(*d, c.x[2]);
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
    if (c.x[2]) st<int32_t>(c.x[2], kOutputs);
    c.x[0] = 0;
}

// Status ReleaseOutputBufferIndex(int index)
void DecoderReleaseOutputBufferIndex(CPU& c) {
    uint64_t output_event = 0;
    {
        std::lock_guard lk(g_lock);
        Decoder* d = decoder(c.x[0]);
        int index = int(c.x[1]);
        if (d && index >= 0 && index < kOutputs) {
            d->outputs[index].busy = false;
            if (!d->pending.empty()) {  // a slot is free again for a frame that waited
                auto [pts, flags] = d->pending.front();
                d->pending.pop_front();
                d->emit(pts, flags);
                output_event = d->events[kOutputAvailable];
            }
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
    log("movie: Decoder(type %u, output format %u, mode %u, priority %d)", d->type, d->format, d->mode, int(c.x[4]));
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

}  // namespace nx
