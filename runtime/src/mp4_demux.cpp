// MP4 demuxer for nn::movie's Extractor: see mp4_demux.h.
#include "mp4_demux.h"

#include <algorithm>
#include <cstring>

#include "platform.h"

namespace nx::mp4 {

namespace {

uint32_t be32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }
uint16_t be16(const uint8_t* p) { return uint16_t(p[0] << 8 | p[1]); }
uint64_t be64(const uint8_t* p) { return uint64_t(be32(p)) << 32 | be32(p + 4); }

bool seek(FILE* f, uint64_t at) {
#if defined(_WIN32)
    return _fseeki64(f, int64_t(at), SEEK_SET) == 0;
#else
    return fseeko(f, off_t(at), SEEK_SET) == 0;
#endif
}

// A box inside a buffer: [type, payload begin, payload end).
struct Box {
    uint32_t type;
    const uint8_t* p;
    const uint8_t* end;
};

constexpr uint32_t fourcc(const char* s) { return uint32_t(s[0]) << 24 | uint32_t(s[1]) << 16 | uint32_t(s[2]) << 8 | uint32_t(s[3]); }

std::vector<Box> children(const uint8_t* p, const uint8_t* end) {
    std::vector<Box> out;
    while (end - p >= 8) {
        uint64_t size = be32(p);
        uint32_t type = be32(p + 4);
        size_t head = 8;
        if (size == 1) {
            if (end - p < 16) break;
            size = be64(p + 8), head = 16;
        } else if (size == 0) {
            size = uint64_t(end - p);
        }
        if (size < head || size > uint64_t(end - p)) break;
        out.push_back({type, p + head, p + size});
        p += size;
    }
    return out;
}

const Box* find(const std::vector<Box>& v, uint32_t type) {
    for (const Box& b : v)
        if (b.type == type) return &b;
    return nullptr;
}

// MPEG-4 descriptor length (1..4 bytes of 7 bits).
uint32_t desc_len(const uint8_t*& p, const uint8_t* end) {
    uint32_t n = 0;
    for (int k = 0; k < 4 && p < end; k++) {
        uint8_t b = *p++;
        n = n << 7 | (b & 0x7f);
        if (!(b & 0x80)) break;
    }
    return n;
}

// esds -> the DecoderSpecificInfo (AudioSpecificConfig) bytes.
std::vector<uint8_t> esds_asc(const uint8_t* p, const uint8_t* end) {
    p += 4;  // version, flags
    while (p < end) {
        uint8_t tag = *p++;
        uint32_t len = desc_len(p, end);
        const uint8_t* body = p;
        if (tag == 3) {  // ES_Descriptor: ES_ID, flags, then nested descriptors
            if (end - body < 3) break;
            uint8_t flags = body[2];
            p = body + 3;
            if (flags & 0x80) p += 2;
            if ((flags & 0x40) && p < end) p += 1 + *p;
            if (flags & 0x20) p += 2;
            continue;
        }
        if (tag == 4) {  // DecoderConfigDescriptor: 13 bytes, then nested descriptors
            p = body + 13;
            continue;
        }
        if (tag == 5) return std::vector<uint8_t>(body, std::min(end, body + len));
        p = body + len;
    }
    return {};
}

void parse_stsd(Track& t, const uint8_t* p, const uint8_t* end) {
    if (end - p < 8) return;
    auto entries = children(p + 8, end);
    if (entries.empty()) return;
    const Box& e = entries[0];
    const uint8_t* q = e.p;
    if (e.type == fourcc("avc1") || e.type == fourcc("avc3")) {
        if (e.end - q < 78) return;
        t.mime = "video/avc";
        t.width = be16(q + 24), t.height = be16(q + 26);
        auto kids = children(q + 78, e.end);
        if (const Box* c = find(kids, fourcc("avcC")); c && c->end - c->p >= 7) {
            const uint8_t* a = c->p;
            t.nal_length_size = (a[4] & 3) + 1;
            int nsps = a[5] & 31;
            const uint8_t* r = a + 6;
            static const uint8_t sc[4] = {0, 0, 0, 1};
            for (int k = 0; k < nsps && c->end - r >= 2; k++) {
                uint16_t n = be16(r);
                r += 2;
                if (c->end - r < n) break;
                t.csd0.insert(t.csd0.end(), sc, sc + 4), t.csd0.insert(t.csd0.end(), r, r + n);
                r += n;
            }
            if (r < c->end) {
                int npps = *r++;
                for (int k = 0; k < npps && c->end - r >= 2; k++) {
                    uint16_t n = be16(r);
                    r += 2;
                    if (c->end - r < n) break;
                    t.csd1.insert(t.csd1.end(), sc, sc + 4), t.csd1.insert(t.csd1.end(), r, r + n);
                    r += n;
                }
            }
        }
    } else if (e.type == fourcc("mp4a")) {
        if (e.end - q < 28) return;
        uint16_t version = be16(q + 8);
        t.channels = be16(q + 16);
        t.sample_rate = int32_t(be32(q + 24) >> 16);
        size_t skip = 28 + (version == 1 ? 16 : version == 2 ? 36 : 0);
        if (size_t(e.end - q) < skip) return;
        auto kids = children(q + skip, e.end);
        if (const Box* c = find(kids, fourcc("esds"))) t.csd0 = esds_asc(c->p, c->end);
        // The AudioSpecificConfig's own rate and channels win over the sample entry's (often 2 / 0 there).
        if (t.csd0.size() >= 2) {
            static const int kRates[13] = {96000, 88200, 64000, 48000, 44100, 32000, 24000, 22050, 16000, 12000, 11025, 8000, 7350};
            int idx = ((t.csd0[0] & 7) << 1) | (t.csd0[1] >> 7), ch = (t.csd0[1] >> 3) & 15;
            if (idx < 13) t.sample_rate = kRates[idx];
            if (ch >= 1 && ch <= 6) t.channels = ch;
        }
        t.mime = "audio/mp4a-latm";
    }
}

struct Tables {
    std::vector<std::pair<uint32_t, uint32_t>> stts, ctts, stsc3;  // stsc: (first chunk, samples per chunk)
    std::vector<uint32_t> stss, sizes;
    std::vector<uint64_t> chunks;
    uint32_t fixed_size = 0, count = 0;
    int64_t edit_media_time = 0;
};

void parse_stbl(Track& t, Tables& tb, const uint8_t* p, const uint8_t* end) {
    for (const Box& b : children(p, end)) {
        const uint8_t* q = b.p;
        const size_t n = size_t(b.end - q);
        if (n < 8) continue;
        uint32_t cnt = be32(q + 4);
        switch (b.type) {
            case fourcc("stsd"): parse_stsd(t, q, b.end); break;
            case fourcc("stts"):
                for (uint32_t k = 0; k < cnt && 8 + 8 * size_t(k) + 8 <= n; k++)
                    tb.stts.emplace_back(be32(q + 8 + 8 * k), be32(q + 12 + 8 * k));
                break;
            case fourcc("ctts"):
                for (uint32_t k = 0; k < cnt && 8 + 8 * size_t(k) + 8 <= n; k++)
                    tb.ctts.emplace_back(be32(q + 8 + 8 * k), be32(q + 12 + 8 * k));
                break;
            case fourcc("stss"):
                for (uint32_t k = 0; k < cnt && 8 + 4 * size_t(k) + 4 <= n; k++) tb.stss.push_back(be32(q + 8 + 4 * k));
                break;
            case fourcc("stsz"):
                if (n < 12) break;
                tb.fixed_size = be32(q + 4), tb.count = be32(q + 8);
                if (!tb.fixed_size)
                    for (uint32_t k = 0; k < tb.count && 12 + 4 * size_t(k) + 4 <= n; k++) tb.sizes.push_back(be32(q + 12 + 4 * k));
                break;
            case fourcc("stsc"):
                for (uint32_t k = 0; k < cnt && 8 + 12 * size_t(k) + 12 <= n; k++)
                    tb.stsc3.emplace_back(be32(q + 8 + 12 * k), be32(q + 12 + 12 * k));
                break;
            case fourcc("stco"):
                for (uint32_t k = 0; k < cnt && 8 + 4 * size_t(k) + 4 <= n; k++) tb.chunks.push_back(be32(q + 8 + 4 * k));
                break;
            case fourcc("co64"):
                for (uint32_t k = 0; k < cnt && 8 + 8 * size_t(k) + 8 <= n; k++) tb.chunks.push_back(be64(q + 8 + 8 * k));
                break;
            default: break;
        }
    }
}

void build_samples(Track& t, const Tables& tb) {
    const uint32_t count = tb.count ? tb.count : uint32_t(tb.sizes.size());
    t.samples.resize(count);
    // Offsets: chunk by chunk.
    uint32_t s = 0;
    for (size_t c = 0; c < tb.chunks.size() && s < count; c++) {
        uint32_t per = 0;
        for (const auto& [first, spc] : tb.stsc3)
            if (first <= c + 1) per = spc;
        uint64_t off = tb.chunks[c];
        for (uint32_t k = 0; k < per && s < count; k++, s++) {
            uint32_t size = tb.fixed_size ? tb.fixed_size : (s < tb.sizes.size() ? tb.sizes[s] : 0);
            t.samples[s].offset = off, t.samples[s].size = size;
            t.max_sample_size = std::max(t.max_sample_size, size);
            off += size;
        }
    }
    t.samples.resize(s);
    // Times: decode deltas plus composition offsets, less the edit list's start.
    int64_t dts = 0;
    size_t i = 0;
    for (const auto& [n, delta] : tb.stts)
        for (uint32_t k = 0; k < n && i < t.samples.size(); k++, i++) t.samples[i].time_us = dts, dts += delta;
    i = 0;
    for (const auto& [n, off] : tb.ctts)
        for (uint32_t k = 0; k < n && i < t.samples.size(); k++, i++) t.samples[i].time_us += int32_t(off);
    const double scale = t.timescale ? 1e6 / double(t.timescale) : 1.0;
    for (auto& sm : t.samples) sm.time_us = int64_t(double(sm.time_us - tb.edit_media_time) * scale);
    if (tb.stss.empty())
        for (auto& sm : t.samples) sm.sync = true;
    else
        for (uint32_t n : tb.stss)
            if (n >= 1 && n <= t.samples.size()) t.samples[n - 1].sync = true;
    if (!t.duration_us) t.duration_us = int64_t(double(dts) * scale);
}

}  // namespace

File::~File() {
    if (f) std::fclose(f);
}

bool File::open(const std::string& host_path, std::string& error) {
    f = plat::fopen_utf8(host_path, "rb");
    if (!f) return error = "cannot open " + host_path, false;
    // Top-level boxes: find moov.
    uint64_t at = 0;
    std::vector<uint8_t> moov;
    for (;;) {
        uint8_t h[16];
        if (!seek(f, at) || std::fread(h, 1, 8, f) != 8) break;
        uint64_t size = be32(h);
        uint32_t type = be32(h + 4);
        uint64_t head = 8;
        if (size == 1) {
            if (std::fread(h + 8, 1, 8, f) != 8) break;
            size = be64(h + 8), head = 16;
        }
        if (size == 0 || size < head) break;
        if (type == fourcc("moov")) {
            if (size > (256ull << 20)) return error = "moov too big", false;
            moov.resize(size_t(size - head));
            if (std::fread(moov.data(), 1, moov.size(), f) != moov.size()) return error = "short moov", false;
            break;
        }
        at += size;
    }
    if (moov.empty()) return error = "no moov box", false;
    const uint8_t* m = moov.data();
    for (const Box& trak : children(m, m + moov.size())) {
        if (trak.type != fourcc("trak")) continue;
        Track t;
        Tables tb;
        auto tk = children(trak.p, trak.end);
        if (const Box* edts = find(tk, fourcc("edts")))
            if (const Box* elst = find(children(edts->p, edts->end), fourcc("elst")); elst && elst->end - elst->p >= 8) {
                const uint8_t* q = elst->p;
                const bool v1 = q[0] == 1;
                const uint32_t n = be32(q + 4);
                const size_t es = v1 ? 20 : 12;
                for (uint32_t k = 0; k < n && size_t(elst->end - q) >= 8 + es * (k + 1); k++) {
                    const uint8_t* e = q + 8 + es * k;
                    int64_t media_time = v1 ? int64_t(be64(e + 8)) : int64_t(int32_t(be32(e + 4)));
                    if (media_time >= 0) {
                        tb.edit_media_time = media_time;
                        break;
                    }
                }
            }
        const Box* mdia = find(tk, fourcc("mdia"));
        if (!mdia) continue;
        auto md = children(mdia->p, mdia->end);
        if (const Box* h = find(md, fourcc("mdhd")); h && h->end - h->p >= 24) {
            const uint8_t* q = h->p;
            if (q[0] == 1 && h->end - q >= 32)
                t.timescale = be32(q + 20), t.duration_us = t.timescale ? int64_t(double(be64(q + 24)) * 1e6 / t.timescale) : 0;
            else
                t.timescale = be32(q + 12), t.duration_us = t.timescale ? int64_t(double(be32(q + 16)) * 1e6 / t.timescale) : 0;
        }
        uint32_t handler = 0;
        if (const Box* h = find(md, fourcc("hdlr")); h && h->end - h->p >= 12) handler = be32(h->p + 8);
        const Box* minf = find(md, fourcc("minf"));
        if (!minf) continue;
        const Box* stbl = find(children(minf->p, minf->end), fourcc("stbl"));
        if (!stbl) continue;
        parse_stbl(t, tb, stbl->p, stbl->end);
        build_samples(t, tb);
        t.video = handler == fourcc("vide") && !t.mime.empty();
        t.audio = handler == fourcc("soun") && !t.mime.empty();
        tracks.push_back(std::move(t));
    }
    if (tracks.empty()) return error = "no tracks", false;
    return true;
}

bool File::read(const Track& t, const Sample& s, std::vector<uint8_t>& out) {
    out.resize(s.size);
    if (!f || !seek(f, s.offset) || std::fread(out.data(), 1, s.size, f) != s.size) return false;
    if (!t.video) return true;
    // Length-prefixed NAL units -> start codes. Four-byte lengths are replaced in place.
    const int L = t.nal_length_size;
    if (L == 4) {
        for (size_t p = 0; p + 4 <= out.size();) {
            uint32_t n = be32(&out[p]);
            out[p] = 0, out[p + 1] = 0, out[p + 2] = 0, out[p + 3] = 1;
            p += 4 + size_t(n);
        }
        return true;
    }
    std::vector<uint8_t> conv;
    conv.reserve(out.size() + 64);
    for (size_t p = 0; p + L <= out.size();) {
        uint32_t n = 0;
        for (int k = 0; k < L; k++) n = n << 8 | out[p + k];
        p += L;
        if (n > out.size() - p) break;
        static const uint8_t sc[4] = {0, 0, 0, 1};
        conv.insert(conv.end(), sc, sc + 4);
        conv.insert(conv.end(), out.begin() + p, out.begin() + p + n);
        p += n;
    }
    out.swap(conv);
    return true;
}

}  // namespace nx::mp4
