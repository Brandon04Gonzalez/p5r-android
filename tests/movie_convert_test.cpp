#include "movie_convert.h"

#include <cstdio>
#include <cstdlib>
#include <random>
#include <vector>

using nx::movie::FrameConverter;
using nx::movie::Nv12Layout;
using nx::movie::nv12_copy_lines;

namespace {

std::vector<uint8_t> make_source(const Nv12Layout& l, size_t extra) {
    size_t need = size_t(l.stride) * size_t(l.rows) * 2 + extra + 4096;
    std::vector<uint8_t> v(need);
    std::mt19937 rng(1234 + unsigned(l.width * 31 + l.height * 17 + l.stride));
    for (auto& b : v) b = uint8_t(rng());
    return v;
}

bool check(const Nv12Layout& l, size_t extra) {
    std::vector<uint8_t> src = make_source(l, extra);
    std::vector<uint8_t> serial;
    serial.resize(l.bytes());
    nv12_copy_lines(l, src.data(), src.size(), serial.data(), 0, l.lines());

    for (int workers = 2; workers <= 4; workers++) {
        FrameConverter c(workers, false);
        std::vector<uint8_t> parallel;
        c.convert(l, src.data(), src.size(), parallel);
        if (parallel != serial) {
            std::printf("mismatch: %dx%d stride %d rows %d crop %d,%d planar %d workers %d\n", l.width, l.height,
                        l.stride, l.rows, l.crop_left, l.crop_top, l.planar, workers);
            return false;
        }
    }
    return true;
}

}  // namespace

int main() {
    bool ok = true;
    const int sizes[][2] = {{2, 2}, {16, 16}, {320, 180}, {1280, 720}, {1920, 1080}, {641, 359}};
    for (auto& s : sizes) {
        for (bool planar : {false, true}) {
            for (int pad : {0, 3, 32, 128}) {
                Nv12Layout l;
                l.width = s[0];
                l.height = s[1];
                l.stride = (l.width + pad + 1) & ~1;
                l.rows = l.height + pad;
                l.crop_left = pad ? 2 : 0;
                l.crop_top = pad ? 2 : 0;
                l.planar = planar;
                ok = check(l, 0) && ok;
            }
        }
    }
    Nv12Layout bad;
    bad.width = 64;
    bad.height = 32;
    bad.stride = 64;
    bad.rows = 32;
    ok = check(bad, 0) && ok;
    std::printf(ok ? "movie_convert: ok\n" : "movie_convert: FAILED\n");
    return ok ? 0 : 1;
}
