// JJK on-device import: extract the player's own dump, extract generic ExeFS Maxwell blocks,
// then translate them with the same NativeAOT harness used by the P5R port.
#include <android/log.h>
#include <dirent.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <jni.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include "nxgame.h"
#include "nximport.h"
#include "p5r_shaders.h"

namespace {
constexpr uint64_t kTitleId = 0x01002FC012548000ull;
std::atomic<int> g_phase{0};
std::atomic<int> g_done{0};
std::atomic<int> g_total{0};
std::mutex g_error_mutex;
std::string g_error;

using HarnessMain = int (*)(int, const char**);
using HarnessProgress = void (*)(int*, int*);
HarnessProgress g_harness_progress = nullptr;

std::string jstr(JNIEnv* env, jstring s) {
    if (!s) return {};
    const char* p = env->GetStringUTFChars(s, nullptr);
    std::string out = p ? p : "";
    if (p) env->ReleaseStringUTFChars(s, p);
    return out;
}

int phase_id(const std::string& p) {
    if (p == "scan") return 1;
    if (p == "exefs") return 2;
    if (p == "romfs") return 2;
    if (p == "verify") return 4;
    if (p == "done") return 2;
    return 1;
}

void set_error(const std::string& e) {
    std::lock_guard<std::mutex> lock(g_error_mutex);
    g_error = e;
}

int fail(const std::string& e) {
    set_error(e);
    g_phase = 0;
    return 1;
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

int run(int game_fd, const std::string& game_name, const std::string& keys,
        const std::string& title_keys, const std::string& data_dir,
        const std::string& work_dir, int jobs) {
    g_phase = 1;
    g_done = 0;
    g_total = 0;
    g_harness_progress = nullptr;
    set_error("");

    int kfd = open(keys.c_str(), O_RDONLY | O_CLOEXEC);
    if (kfd < 0) return fail("cannot read prod.keys");
    int tfd = title_keys.empty() ? -1 : open(title_keys.c_str(), O_RDONLY | O_CLOEXEC);

    std::vector<nxi::InputFile> inputs;
    inputs.push_back({game_name, game_fd});
    inputs.push_back({"prod.keys", kfd});
    if (tfd >= 0) inputs.push_back({"title.keys", tfd});

    nxi::Options opt;
    opt.out_dir = data_dir;
    opt.title_id = kTitleId;
    opt.keep_exefs = true;
    opt.on_progress = [](const nxi::Progress& p) {
        g_phase = phase_id(p.phase);
        constexpr uint64_t kMiB = 1024ull * 1024ull;
        constexpr uint64_t kMax = 0x7fffffff;
        g_done = static_cast<int>(std::min<uint64_t>(p.done / kMiB, kMax));
        g_total = (p.total == 0 || p.total / kMiB > kMax)
            ? 0 : static_cast<int>((p.total + kMiB - 1) / kMiB);
        __android_log_print(ANDROID_LOG_INFO, "jjkimport", "%s: %llu/%llu %s", p.phase.c_str(),
            static_cast<unsigned long long>(p.done), static_cast<unsigned long long>(p.total), p.item.c_str());
    };
    opt.on_log = [](const std::string& line) {
        __android_log_print(ANDROID_LOG_INFO, "jjkimport", "%s", line.c_str());
    };

    std::string error;
    const int rc = nxi::run_import(inputs, opt, error);
    if (rc != 0) {
        close(kfd);
        if (tfd >= 0) close(tfd);
        return fail(error.empty() ? "import failed" : error);
    }

    // Re-open the parsed game image using the same fds. This uses pread, so it does not
    // disturb the importer or the SAF descriptor's shared file position.
    auto game = nxi::GameImage::open(inputs, kTitleId, error);
    close(kfd);
    if (tfd >= 0) close(tfd);
    if (!game) return fail(error.empty() ? "cannot reopen game image for shader extraction" : error);

    // The P5R GSC/CPK extractor is game-specific. JJK gets the generic ExeFS pass first;
    // shader containers inside its Unreal RomFS are a separate format and are not guessed here.
    g_phase = 2;
    g_done = 0;
    g_total = 1;
    remove_tree(work_dir);
    if (mkdir(work_dir.c_str(), 0700) != 0) return fail("cannot create shader work directory");
    const std::string extracted = work_dir + "/shaders";
    long blocks = p5r::extract_exefs_shaders(*game, extracted, error);
    if (blocks < 0) return fail(error.empty() ? "ExeFS shader extraction failed" : error);
    __android_log_print(ANDROID_LOG_INFO, "jjkimport", "Extracted %ld distinct ExeFS shader blocks", blocks);

    g_phase = 3;
    g_done = 0;
    g_total = static_cast<int>(std::min<long>(blocks, 0x7fffffff));
    void* lib = dlopen("libspirvharness.so", RTLD_NOW);
    if (!lib) return fail(std::string("shader translator is missing: ") + dlerror());
    auto harness = reinterpret_cast<HarnessMain>(dlsym(lib, "spirv_harness_main"));
    g_harness_progress = reinterpret_cast<HarnessProgress>(dlsym(lib, "spirv_harness_progress"));
    if (!harness || !g_harness_progress) return fail("shader translator is incomplete");

    const std::string out_new = data_dir + "/spirv.new";
    remove_tree(out_new);
    std::string jobs_s = std::to_string(jobs > 0 ? jobs : 4);
    const char* args[] = {"--batch", extracted.c_str(), "--out", out_new.c_str(),
                          "--jobs", jobs_s.c_str(), "--no-val"};
    const int translate_rc = harness(static_cast<int>(sizeof(args) / sizeof(args[0])), args);
    if (translate_rc != 0)
        return fail("shader translation failed (code " + std::to_string(translate_rc) + ")");

    g_phase = 4;
    const std::string out = data_dir + "/spirv";
    const std::string old = data_dir + "/spirv.old";
    remove_tree(old);
    rename(out.c_str(), old.c_str());
    if (rename(out_new.c_str(), out.c_str()) != 0)
        return fail("cannot publish translated shaders to " + out);
    remove_tree(old);
    remove_tree(work_dir);
    if (FILE* f = std::fopen((out + "/import.txt").c_str(), "w")) {
        std::fprintf(f, "%s\n%ld ExeFS shader blocks\n", game_name.c_str(), blocks);
        std::fclose(f);
    }
    g_phase = 5;
    g_done = g_total = 1;
    return 0;
}
} // namespace

extern "C" JNIEXPORT jint JNICALL
Java_io_github_jjkport_Importer_run(JNIEnv* env, jclass, jint game_fd, jstring game_name,
                                    jstring keys_path, jstring title_keys_path, jstring out_dir,
                                    jstring work_dir, jint jobs) {
    return run(game_fd, jstr(env, game_name), jstr(env, keys_path), jstr(env, title_keys_path),
               jstr(env, out_dir), jstr(env, work_dir), jobs);
}

extern "C" JNIEXPORT jintArray JNICALL
Java_io_github_jjkport_Importer_progress(JNIEnv* env, jclass) {
    jint v[3] = {g_phase.load(), g_done.load(), g_total.load()};
    if (v[0] == 3 && g_harness_progress) g_harness_progress(&v[1], &v[2]);
    jintArray out = env->NewIntArray(3);
    if (out) env->SetIntArrayRegion(out, 0, 3, v);
    return out;
}

extern "C" JNIEXPORT jstring JNICALL
Java_io_github_jjkport_Importer_error(JNIEnv* env, jclass) {
    std::lock_guard<std::mutex> lock(g_error_mutex);
    return env->NewStringUTF(g_error.c_str());
}
