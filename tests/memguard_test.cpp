// The Windows memory guard's helpers (runtime/src/memguard_win.cpp): the limit parsing, and that the footprint it
// reads follows this process's own commits. Built by CMake with -DNX_TESTS=ON (ctest). Exit code 0: passed.
#define NX_MEMGUARD_STANDALONE
#include "../runtime/src/memguard_win.cpp"

#include <vector>

int main() {
    using namespace nx::plat;
    int failed = 0;
    auto check = [&](bool ok, const char* what) {
        if (!ok) std::printf("FAIL: %s\n", what), failed++;
    };
    check(memory_limit_gb(nullptr) == 8.0, "unset limit is 8 GB");
    check(memory_limit_gb("") == 8.0, "empty limit is 8 GB");
    check(memory_limit_gb("0") == 8.0, "zero limit is 8 GB");
    check(memory_limit_gb("-2") == 8.0, "negative limit is 8 GB");
    check(memory_limit_gb("5.5") == 5.5, "5.5 GB");
    check(memory_limit_gb("12") == 12.0, "12 GB");

    const uint64_t before = memory_footprint();
    check(before > 0, "footprint readable");
    std::vector<char> block(size_t(256) << 20, 1);  // 256 MB, touched
    const uint64_t after = memory_footprint();
    check(after >= before + (uint64_t(200) << 20), "a 256 MB commit shows in the footprint");
    std::printf("memguard_test: footprint %.1f MB -> %.1f MB (%d)\n", double(before) / 1048576.0,
                double(after) / 1048576.0, int(block[12345]));
    std::printf("memguard_test: %s\n", failed ? "FAILED" : "all passed");
    return failed ? 1 : 0;
}
