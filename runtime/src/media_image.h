// The plane layout an Android video decoder reports in its output format's "image-data" buffer (hle_movie.cpp).
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace nx::media {

// Byte offset of the first sample, bytes to the next sample in a row, bytes to the next row; Y, U, V.
struct Planes {
    uint32_t off[3] = {}, col[3] = {}, row[3] = {};
};

// "image-data" is a MediaImage2 (media/hardware/VideoAPI.h; android.media.MediaCodec reads it the same way): six
// words (type, plane count, width, height, bit depth, allocated bit depth), then one PlaneInfo of five words per plane
// (offset, column increment, row increment, horizontal and vertical subsampling).
constexpr size_t kHeaderWords = 6, kPlaneWords = 5;
constexpr uint32_t kTypeYuv = 1;

// The Y, U and V planes of a 3-plane YUV image description of `size` bytes; false for anything else.
inline bool parse_image_data(const void* data, size_t size, Planes& out) {
    uint32_t u[kHeaderWords + 3 * kPlaneWords];
    if (!data || size < sizeof(u)) return false;
    std::memcpy(u, data, sizeof(u));
    if (u[0] != kTypeYuv || u[1] < 3) return false;
    for (int p = 0; p < 3; p++) {
        const uint32_t* plane = u + kHeaderWords + p * kPlaneWords;
        out.off[p] = plane[0];
        out.col[p] = plane[1];
        out.row[p] = plane[2];
    }
    return true;
}

}  // namespace nx::media
