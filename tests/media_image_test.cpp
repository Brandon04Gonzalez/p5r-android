// The decoder plane layout parser (runtime/src/media_image.h, used by hle_movie.cpp). Standalone:
//   clang-cl /std:c++20 /EHsc -Iruntime/src tests/media_image_test.cpp
// or through CMake (-DNX_TESTS=ON, ctest). Exit code 0: every case passed.
#include <cstdio>
#include <vector>

#include "media_image.h"

using nx::media::Planes;
using nx::media::parse_image_data;

namespace {

int g_failed = 0;

struct Plane {
    uint32_t off, col, row, horiz, vert;
};

// What a decoder hands out: MediaImage2 (Android media/hardware/VideoAPI.h, read the same way by
// android.media.MediaCodec's MediaImage): type, plane count, width, height, bit depth, allocated bit depth, then four
// PlaneInfo of offset, column increment, row increment, horizontal and vertical subsampling.
std::vector<uint32_t> image_data(uint32_t type, uint32_t width, uint32_t height, const std::vector<Plane>& planes) {
    std::vector<uint32_t> u = {type, uint32_t(planes.size()), width, height, 8, 8};
    for (const Plane& p : planes) u.insert(u.end(), {p.off, p.col, p.row, p.horiz, p.vert});
    u.resize(6 + 4 * 5);
    return u;
}

void expect(const std::vector<uint32_t>& u, size_t bytes, bool ok, const Planes& want, int line) {
    Planes got;
    bool r = parse_image_data(u.data(), bytes, got);
    bool same = true;
    for (int p = 0; ok && p < 3; p++)
        same = same && got.off[p] == want.off[p] && got.col[p] == want.col[p] && got.row[p] == want.row[p];
    if (r != ok || !same) {
        std::printf("FAIL line %d: parsed %d (expected %d)", line, int(r), int(ok));
        for (int p = 0; p < 3; p++)
            std::printf(" | plane %d off %u col %u row %u (expected %u %u %u)", p, got.off[p], got.col[p], got.row[p],
                        want.off[p], want.col[p], want.row[p]);
        std::printf("\n");
        g_failed++;
    }
}

}  // namespace

#define EXPECT(u, bytes, ok, want) expect(u, bytes, ok, want, __LINE__)

int main() {
    // Semi-planar NV12 with an aligned stride (c2.mtk.vp9.decoder, 1920x1080 in 1920x1088): V is U plus one.
    const uint32_t y = 1920 * 1088;
    auto nv12 = image_data(1, 1920, 1080, {{0, 1, 1920, 1, 1}, {y, 2, 1920, 2, 2}, {y + 1, 2, 1920, 2, 2}});
    EXPECT(nv12, nv12.size() * 4, true, (Planes{{0, y, y + 1}, {1, 2, 2}, {1920, 1920, 1920}}));

    // Planar I420 (c2.android.vp9.decoder, 1280x720 with a 1280 stride): U then V, each a quarter of Y.
    const uint32_t y2 = 1280 * 720, c2 = 640 * 360;
    auto i420 = image_data(1, 1280, 720, {{0, 1, 1280, 1, 1}, {y2, 1, 640, 2, 2}, {y2 + c2, 1, 640, 2, 2}});
    EXPECT(i420, i420.size() * 4, true, (Planes{{0, y2, y2 + c2}, {1, 1, 1}, {1280, 640, 640}}));

    // Only the three planes need to be there: 6 + 3 * 5 words.
    EXPECT(i420, (6 + 3 * 5) * 4, true, (Planes{{0, y2, y2 + c2}, {1, 1, 1}, {1280, 640, 640}}));
    EXPECT(i420, (6 + 3 * 5) * 4 - 1, false, Planes{});

    // Not YUV (MEDIA_IMAGE_TYPE_RGB), or fewer than three planes.
    auto rgb = image_data(3, 1280, 720, {{0, 4, 5120, 1, 1}, {1, 4, 5120, 1, 1}, {2, 4, 5120, 1, 1}});
    EXPECT(rgb, rgb.size() * 4, false, Planes{});
    auto two = image_data(1, 1280, 720, {{0, 1, 1280, 1, 1}, {y2, 2, 1280, 2, 2}});
    EXPECT(two, two.size() * 4, false, Planes{});
    EXPECT(nv12, 0, false, Planes{});

    std::printf("media_image_test: %s\n", g_failed ? "FAILED" : "all passed");
    return g_failed ? 1 : 0;
}
