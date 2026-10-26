// PC test of p5r_shaders.h: p5r_shaders <prod.keys> <x.nsp> <out dir>
#include <fcntl.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstring>

#include "p5r_shaders.h"

int main(int argc, char** argv) {
    if (argc != 4) return fprintf(stderr, "usage: p5r_shaders <prod.keys> <x.nsp> <out dir>\n"), 2;
    std::vector<nxi::InputFile> in;
    in.push_back({"prod.keys", open(argv[1], O_RDONLY)});
    const char* s = strrchr(argv[2], '\\');
    const char* f = strrchr(argv[2], '/');
    if (f && (!s || f > s)) s = f;
    in.push_back({s ? s + 1 : argv[2], open(argv[2], O_RDONLY)});
    std::string err;
    auto g = nxi::GameImage::open(in, 0, err);
    if (!g) return fprintf(stderr, "open: %s\n", err.c_str()), 1;
    auto t0 = std::chrono::steady_clock::now();
    long n = p5r::extract_shaders(*g, argv[3], [](int d, int t) { fprintf(stderr, "  %d/%d\n", d, t); }, err);
    double sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    if (n < 0) return fprintf(stderr, "extract: %s\n", err.c_str()), 1;
    printf("%ld distinct code blocks in %.1f s\n", n, sec);
    return 0;
}
