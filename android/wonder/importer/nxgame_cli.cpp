// Test of nxgame.h on the PC: opens the game in place and reads from it.
//   nxgame <prod.keys> [title.keys] <x.nsp|x.xci> [--compare <extracted romfs dir>] [--read <romfs path>]
// --compare reads 64 KiB at a few offsets of every 500th file and compares them with the extracted RomFS;
// --read times a whole file's read.
#include <fcntl.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "nxgame.h"

int main(int argc, char** argv) {
    std::vector<nxi::InputFile> in;
    std::string compare, read_path;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        if (a == "--compare" && i + 1 < argc) { compare = argv[++i]; continue; }
        if (a == "--read" && i + 1 < argc) { read_path = argv[++i]; continue; }
        int fd = open(argv[i], O_RDONLY);
        if (fd < 0) return fprintf(stderr, "cannot open %s\n", argv[i]), 2;
        const char* s = strrchr(argv[i], '/');
        const char* b = strrchr(argv[i], '\\');
        if (b && (!s || b > s)) s = b;
        in.push_back({s ? s + 1 : argv[i], fd});
    }
    auto t0 = std::chrono::steady_clock::now();
    std::string err;
    auto g = nxi::GameImage::open(in, 0, err);
    double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    if (!g) return fprintf(stderr, "open failed: %s\n", err.c_str()), 1;
    printf("title %016llx opened in %.0f ms; %zu RomFS files\n", (unsigned long long)g->title_id(), ms, g->file_count());
    for (auto& [n, v] : g->exefs()) printf("  exefs %s: %zu bytes\n", n.c_str(), v.size());
    for (auto& [n, e] : g->list("")) printf("  / %s%s %llu\n", n.c_str(), e.dir ? "/" : "", (unsigned long long)e.size);
    if (!compare.empty()) {
        std::vector<std::pair<std::string, nxi::GameImage::Entry>> all;
        std::vector<std::string> stack{""};
        while (!stack.empty()) {
            std::string d = stack.back();
            stack.pop_back();
            for (auto& [n, e] : g->list(d)) {
                std::string p = d.empty() ? n : d + "/" + n;
                if (e.dir) stack.push_back(p);
                else all.push_back({p, e});
            }
        }
        size_t checked = 0, bad = 0;
        std::vector<uint8_t> a(65536), b(65536);
        for (size_t i = 0; i < all.size(); i += all.size() > 1000 ? 500 : 1) {
            auto& [p, e] = all[i];
            FILE* f = fopen((compare + "/" + p).c_str(), "rb");
            if (!f) { printf("  missing in the extracted copy: %s\n", p.c_str()); bad++; continue; }
            for (uint64_t off : {uint64_t(0), e.size / 3, e.size / 2 + 12345, e.size > 65536 ? e.size - 65536 : 0}) {
                uint64_t n = g->read(e, off, a.data(), a.size());
#ifdef _WIN32
                _fseeki64(f, int64_t(off), SEEK_SET);
#else
                fseeko(f, off_t(off), SEEK_SET);
#endif
                size_t m = fread(b.data(), 1, a.size(), f);
                if (n != m || memcmp(a.data(), b.data(), n) != 0) { printf("  DIFFERS: %s at %llu\n", p.c_str(), (unsigned long long)off); bad++; }
            }
            fclose(f);
            checked++;
        }
        printf("compared %zu files of %zu: %zu differences\n", checked, all.size(), bad);
    }
    if (!read_path.empty()) {
        const nxi::GameImage::Entry* e = g->find(read_path);
        if (!e) return fprintf(stderr, "%s not in the RomFS\n", read_path.c_str()), 1;
        std::vector<uint8_t> buf(4 << 20);
        uint64_t total = std::min<uint64_t>(e->size, 512ull << 20);
        auto t1 = std::chrono::steady_clock::now();
        for (uint64_t off = 0; off < total; off += buf.size()) g->read(*e, off, buf.data(), buf.size());
        double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t1).count();
        printf("read %.0f MB of %s in %.2f s (%.0f MB/s)\n", double(total) / 1e6, read_path.c_str(), s, double(total) / 1e6 / s);
    }
    return 0;
}
