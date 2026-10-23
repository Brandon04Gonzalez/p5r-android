// A small MP4 (ISO BMFF) demuxer for nn::movie's Extractor (hle_movie.cpp): the tracks of a progressive file (moov +
// mdat, no fragments), their sample tables, H.264 parameter sets and the AAC AudioSpecificConfig. Portable: the same
// code feeds AMediaCodec on Android and the black/silent decoder elsewhere.
#pragma once

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace nx::mp4 {

struct Sample {
    uint64_t offset = 0;
    uint32_t size = 0;
    int64_t time_us = 0;  // presentation (decode time + composition offset)
    bool sync = false;
};

struct Track {
    std::string mime;  // "video/avc", "audio/mp4a-latm", or "" (unsupported)
    bool video = false, audio = false;
    uint32_t timescale = 0;
    int64_t duration_us = 0;
    int32_t width = 0, height = 0;
    int32_t sample_rate = 0, channels = 0;
    int32_t nal_length_size = 4;  // AVC: bytes before each NAL unit in the samples
    std::vector<uint8_t> csd0, csd1;  // AVC: SPS / PPS with start codes; AAC: AudioSpecificConfig (csd0)
    std::vector<Sample> samples;
    uint32_t max_sample_size = 0;
    double frame_rate() const {
        return samples.size() > 1 && duration_us > 0 ? double(samples.size()) * 1e6 / double(duration_us) : 30.0;
    }
};

struct File {
    FILE* f = nullptr;
    std::vector<Track> tracks;
    ~File();
    bool open(const std::string& host_path, std::string& error);
    // A sample's bytes as the decoder takes them: AVC length prefixes become Annex-B start codes.
    bool read(const Track& t, const Sample& s, std::vector<uint8_t>& out);
};

}  // namespace nx::mp4
