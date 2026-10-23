// hwopus: the Switch's hardware Opus decoder service, on libopus (private/third_party/opus-1.5.2, BSD; CMake adds it
// when it is there and defines NX_HAVE_OPUS). P4AU decodes its voices and music with nn::codec::HardwareOpusDecoder,
// whose IPC lands here. Without libopus every decode gives silence of the packet's length.
//
// IHardwareOpusDecoderManager: 0 Open(sample rate, channels, work size; transfer memory), 1 GetWorkBufferSize,
// 2/3 the multistream ones, 4 OpenEx(OpusParametersEx {rate, channels, large frames, pad}, work size), 5 GetWorkBufferSizeEx.
// IHardwareOpusDecoder: 0 DecodeInterleavedOld, 4 ...WithPerfOld, 6 ...WithPerfAndResetOld(bool), 8 DecodeInterleaved
// (bool reset), and 2/5/7/9 for multistream. Input: packets of {u32 size BE, u32 final range BE, data}; output: s16
// interleaved. Results: consumed bytes, samples per channel[, u64 time].
#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <vector>

#if defined(NX_HAVE_OPUS)
#include <opus.h>
#include <opus_multistream.h>
#endif

#include "ipc.h"

namespace nx {

namespace {

struct OpusState {
    int rate = 48000, channels = 2;
    bool multistream = false;
    std::mutex lock;
#if defined(NX_HAVE_OPUS)
    OpusDecoder* dec = nullptr;
    OpusMSDecoder* ms = nullptr;
    ~OpusState() {
        if (dec) opus_decoder_destroy(dec);
        if (ms) opus_multistream_decoder_destroy(ms);
    }
#endif
};

uint32_t be32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }

// One packet from `in` into `out` (s16 interleaved). Returns false on a malformed input.
bool decode(OpusState& s, IpcCtx& c, bool reset, bool perf) {
    const Buffer in = c.in_buf(0), out = c.out_buf(0);
    if (!in.addr || in.size < 8) return false;
    const uint8_t* src = reinterpret_cast<const uint8_t*>(in.addr);
    const uint32_t size = be32(src);
    if (size > in.size - 8) return false;
    int16_t* pcm = reinterpret_cast<int16_t*>(out.addr);
    const int max_samples = out.addr ? int(out.size / (2 * uint64_t(s.channels))) : 0;
    int samples = 0;
    std::lock_guard lk(s.lock);
#if defined(NX_HAVE_OPUS)
    if (reset) {
        if (s.dec) opus_decoder_ctl(s.dec, OPUS_RESET_STATE);
        if (s.ms) opus_multistream_decoder_ctl(s.ms, OPUS_RESET_STATE);
    }
    if (max_samples > 0) {
        samples = s.ms ? opus_multistream_decode(s.ms, src + 8, opus_int32(size), pcm, max_samples, 0)
                : s.dec ? opus_decode(s.dec, src + 8, opus_int32(size), pcm, max_samples, 0)
                        : OPUS_INVALID_STATE;
        if (samples < 0) {
            static int said = 0;
            if (said++ < 10) log("hwopus: decode of %u bytes failed (%d)", size, samples);
            samples = 0;
        }
    }
#else
    (void)reset;
    samples = std::min(max_samples, s.rate / 50);  // 20 ms of silence
    if (pcm && samples > 0) std::memset(pcm, 0, size_t(samples) * 2 * s.channels);
#endif
    // Every 2000 packets: how many and the loudest sample since (0 = silence: not decoding).
    static std::atomic<uint64_t> packets{0};
    static std::atomic<int> peak{0};
    for (int k = 0; pcm && k < samples * s.channels; k += 7) {
        int v = std::abs(int(pcm[k]));
        if (v > peak.load(std::memory_order_relaxed)) peak.store(v, std::memory_order_relaxed);
    }
    const uint64_t n = ++packets;
    if (n == 1 || n == 100 || n % 2000 == 0)
        log("hwopus: %llu packets decoded, peak %d since the last line", (unsigned long long)n, peak.exchange(0));
    c.push<uint32_t>(8 + size);
    c.push<uint32_t>(uint32_t(samples));
    if (perf) c.push<uint64_t>(0);
    return true;
}

std::shared_ptr<Service> decoder_service(std::shared_ptr<OpusState> st) {
    auto svc = std::make_shared<Service>("IHardwareOpusDecoder");
    auto plain = [st](bool perf, bool has_reset) {
        return [st, perf, has_reset](IpcCtx& c) -> uint32_t {
            const bool reset = has_reset && c.arg<uint8_t>(0) != 0;
            return decode(*st, c, reset, perf) ? 0u : 0x10ce99u;  // codec: invalid argument
        };
    };
    svc->cmds[0] = plain(false, false);  // DecodeInterleavedOld
    svc->cmds[1] = [](IpcCtx&) { return 0u; };  // SetContext
    svc->cmds[2] = plain(false, false);  // DecodeInterleavedForMultiStreamOld
    svc->cmds[3] = [](IpcCtx&) { return 0u; };
    svc->cmds[4] = plain(true, false);   // DecodeInterleavedWithPerfOld
    svc->cmds[5] = plain(true, false);   // ...ForMultiStreamWithPerfOld
    svc->cmds[6] = plain(true, true);    // DecodeInterleavedWithPerfAndResetOld(bool reset)
    svc->cmds[7] = plain(true, true);
    svc->cmds[8] = plain(true, true);    // DecodeInterleaved(bool reset)
    svc->cmds[9] = plain(true, true);
    return svc;
}

std::shared_ptr<OpusState> make_state(int rate, int channels) {
    auto st = std::make_shared<OpusState>();
    st->rate = rate > 0 ? rate : 48000;
    st->channels = channels >= 1 && channels <= 2 ? channels : 2;
#if defined(NX_HAVE_OPUS)
    int err = 0;
    st->dec = opus_decoder_create(st->rate, st->channels, &err);
    if (err != OPUS_OK) log("hwopus: opus_decoder_create(%d, %d) failed (%d)", st->rate, st->channels, err);
#endif
    static int opened = 0;
    if (opened++ < 8) log("hwopus: decoder %d Hz, %d channels%s", st->rate, st->channels,
#if defined(NX_HAVE_OPUS)
                          ""
#else
                          " (no libopus in this build: silence)"
#endif
    );
    return st;
}

// The work buffer the game allocates and maps for us (transfer memory). libopus keeps its own state, so only a token
// size: P4AU makes many decoders from a small sound heap (80 KB each ran it out: memset of a null buffer).
uint32_t work_size(int channels) { return 0x1000u * uint32_t(channels > 0 ? channels : 2); }

ServiceRegistrar r_hwopus("hwopus", [] {
    auto s = std::make_shared<Service>("hwopus");
    s->cmds[0] = [](IpcCtx& c) {  // OpenHardwareOpusDecoder(u32 rate, u32 channels, u32 work size; handle)
        c.push_object(decoder_service(make_state(c.arg<int32_t>(0), c.arg<int32_t>(4))));
        return 0u;
    };
    s->cmds[1] = [](IpcCtx& c) { c.push<uint32_t>(work_size(c.arg<int32_t>(4))); return 0u; };
    s->cmds[2] = [](IpcCtx& c) {  // multistream: parameters in a buffer {rate, channels, streams, coupled, mapping[256]}
        const Buffer b = c.in_buf(0);
        int rate = 48000, ch = 2;
        if (b.addr && b.size >= 8) {
            std::memcpy(&rate, reinterpret_cast<void*>(b.addr), 4);
            std::memcpy(&ch, reinterpret_cast<void*>(b.addr + 4), 4);
        }
        c.push_object(decoder_service(make_state(rate, ch)));
        return 0u;
    };
    s->cmds[3] = [](IpcCtx& c) { c.push<uint32_t>(work_size(6)); return 0u; };
    s->cmds[4] = [](IpcCtx& c) {  // OpenHardwareOpusDecoderEx(OpusParametersEx, u32 work size; handle)
        c.push_object(decoder_service(make_state(c.arg<int32_t>(0), c.arg<int32_t>(4))));
        return 0u;
    };
    s->cmds[5] = [](IpcCtx& c) { c.push<uint32_t>(work_size(c.arg<int32_t>(4))); return 0u; };
    s->cmds[6] = s->cmds[2];
    s->cmds[7] = [](IpcCtx& c) { c.push<uint32_t>(work_size(6)); return 0u; };
    s->cmds[8] = [](IpcCtx& c) { c.push<uint32_t>(work_size(c.arg<int32_t>(4))); return 0u; };
    return s;
});

}  // namespace

}  // namespace nx
