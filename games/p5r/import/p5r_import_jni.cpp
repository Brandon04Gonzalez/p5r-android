// The import on the phone (libp5rimport.so, loaded by ImportActivity in its own process ":import"): the player's NSP or
// XCI and keys -> the game checked (title, version 1.0.0) and its shaders extracted (p5r_shaders.cpp) and translated
// to SPIR-V (libspirvharness.so: the shader translator built with NativeAOT) into <data>/spirv. The game itself is
// read from the NSP at every start (runtime game_image.cpp): nothing else is copied.
//   Importer.run(nspFd, nspName, keysPath, titleKeysPath, dataDir, workDir, jobs) -> 0, or an error (Importer.error())
//   Importer.progress() -> {phase, done, total}: 1 opening, 2 extracting, 3 translating, 4 finishing, 5 done
#include <dirent.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <jni.h>
#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include "nxgame.h"
#include "p5r_shaders.h"

namespace {

bool is_supported_title(uint64_t t) {
    uint64_t base = t & ~0xFFFull;
    return base == 0x01005CA01580E000ull  // US
        || base == 0x01004B10157F2000ull  // EU
        || base == 0x0100B880154FC000ull  // JP
        || base == 0x01006BB00C6F0000ull; // Asia
}

bool is_supported_build(const char* id) {
    const char* known[] = {
        "708BCACA873EA32B", // US 1.0.0
        "B78A03E489E5BD92", // EU 1.0.0
        "6E1F4CAE92A28C91", // JP 1.0.0
        "2F2253ACD6BE5D87", // Asia 1.0.0
    };
    for (const char* k : known) {
        if (std::strcmp(id, k) == 0) return true;
    }
    return false;
}

std::atomic<int> g_phase{0}, g_done{0}, g_total{0};
std::mutex g_error_lock;
std::string g_error;

using HarnessMain = int (*)(int, const char**);
using HarnessProgress = void (*)(int*, int*);
HarnessProgress g_harness_progress = nullptr;

void set_error(const std::string& e) {
    std::lock_guard lk(g_error_lock);
    g_error = e;
}

std::string jstr(JNIEnv* env, jstring s) {
    if (!s) return {};
    const char* c = env->GetStringUTFChars(s, nullptr);
    std::string out = c ? c : "";
    if (c) env->ReleaseStringUTFChars(s, c);
    return out;
}

void remove_tree(const std::string& path) {
    if (DIR* d = opendir(path.c_str())) {
        while (dirent* e = readdir(d)) {
            std::string n = e->d_name;
            if (n == "." || n == "..") continue;
            std::string p = path + "/" + n;
            struct stat st{};
            if (lstat(p.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) remove_tree(p);
            else unlink(p.c_str());
        }
        closedir(d);
    }
    rmdir(path.c_str());
}

int fail(const std::string& e) {
    set_error(e);
    g_phase = 0;
    return 1;
}

int run(int nsp_fd, const std::string& nsp_name, const std::string& keys, const std::string& title_keys,
        const std::string& data_dir, const std::string& work_dir, int jobs) {
    g_phase = 1, g_done = 0, g_total = 0;
    set_error("");
    // 1. the game file and the keys
    std::vector<nxi::InputFile> in;
    in.push_back({nsp_name, nsp_fd});
    int kfd = open(keys.c_str(), O_RDONLY | O_CLOEXEC);
    if (kfd < 0) return fail("cannot read prod.keys");
    in.push_back({"prod.keys", kfd});
    int tfd = title_keys.empty() ? -1 : open(title_keys.c_str(), O_RDONLY | O_CLOEXEC);
    if (tfd >= 0) in.push_back({"title.keys", tfd});
    std::string err;
    auto game = nxi::GameImage::open(in, 0, err);
    close(kfd);
    if (tfd >= 0) close(tfd);
    if (!game) return fail(err);
    if (!is_supported_title(game->title_id()))
        return fail("unsupported Title ID: " + nxi::tid_str(game->title_id()) + " (expected Persona 5 Royal US, EU, JP, or Asia)");
    auto main_it = game->exefs().find("main");
    if (main_it == game->exefs().end() || main_it->second.size() < 0x48) return fail("the game file has no main");
    char id[17];
    for (int i = 0; i < 8; i++) std::snprintf(id + 2 * i, 3, "%02X", main_it->second[0x40 + i]);
    if (!is_supported_build(id)) {
        // Log warning for updates/custom builds instead of blocking
    }

    // 2. the shaders out of the game
    g_phase = 2;
    remove_tree(work_dir);
    mkdir(work_dir.c_str(), 0700);
    const std::string extracted = work_dir + "/shaders";
    long blocks = p5r::extract_shaders(*game, extracted, [](int d, int t) { g_done = d, g_total = t; }, err);
    if (blocks < 0) return fail(err);

    // 3. translated (the harness counts its own progress)
    g_phase = 3, g_done = 0, g_total = int(blocks);
    void* lib = dlopen("libspirvharness.so", RTLD_NOW);
    if (!lib) return fail(std::string("the shader translator is missing: ") + dlerror());
    auto harness = reinterpret_cast<HarnessMain>(dlsym(lib, "spirv_harness_main"));
    g_harness_progress = reinterpret_cast<HarnessProgress>(dlsym(lib, "spirv_harness_progress"));
    if (!harness || !g_harness_progress) return fail("the shader translator is incomplete");
    const std::string out_new = data_dir + "/spirv.new";
    remove_tree(out_new);
    std::string jobs_s = std::to_string(jobs > 0 ? jobs : 4);
    const char* args[] = {"--batch", extracted.c_str(), "--out", out_new.c_str(), "--jobs", jobs_s.c_str(), "--no-val"};
    int rc = harness(int(sizeof args / sizeof args[0]), args);
    if (rc != 0) return fail("the shader translation failed (code " + std::to_string(rc) + ")");

    // 4. in place of the old set; a note of what it was made from
    g_phase = 4;
    const std::string out = data_dir + "/spirv", old = data_dir + "/spirv.old";
    remove_tree(old);
    rename(out.c_str(), old.c_str());
    if (rename(out_new.c_str(), out.c_str()) != 0) return fail("cannot write " + out);
    remove_tree(old);
    remove_tree(work_dir);
    if (FILE* f = std::fopen((out + "/import.txt").c_str(), "w")) {
        std::fprintf(f, "%s\n%s\n%ld shaders\n", nsp_name.c_str(), id, blocks);
        std::fclose(f);
    }
    g_phase = 5;
    return 0;
}

}  // namespace

extern "C" {

JNIEXPORT jint JNICALL Java_io_github_p5rrecomp_Importer_run(JNIEnv* env, jclass, jint nsp_fd, jstring nsp_name,
                                                             jstring keys, jstring title_keys, jstring data_dir,
                                                             jstring work_dir, jint jobs) {
    return run(nsp_fd, jstr(env, nsp_name), jstr(env, keys), jstr(env, title_keys), jstr(env, data_dir),
               jstr(env, work_dir), jobs);
}

JNIEXPORT jintArray JNICALL Java_io_github_p5rrecomp_Importer_progress(JNIEnv* env, jclass) {
    int v[3] = {g_phase.load(), g_done.load(), g_total.load()};
    if (v[0] == 3 && g_harness_progress) g_harness_progress(&v[1], &v[2]);
    jintArray a = env->NewIntArray(3);
    env->SetIntArrayRegion(a, 0, 3, v);
    return a;
}

JNIEXPORT jstring JNICALL Java_io_github_p5rrecomp_Importer_error(JNIEnv* env, jclass) {
    std::lock_guard lk(g_error_lock);
    return env->NewStringUTF(g_error.c_str());
}

}  // extern "C"
