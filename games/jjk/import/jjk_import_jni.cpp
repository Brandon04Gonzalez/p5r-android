// Generic on-device NSP/XCI importer for Jujutsu Kaisen: Cursed Clash.
// Unlike the P5R importer, this does not extract or translate game-specific shaders.
#include <android/log.h>
#include <dirent.h>
#include <fcntl.h>
#include <jni.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <mutex>
#include <string>
#include <vector>

#include "nximport.h"

namespace {
constexpr uint64_t kTitleId = 0x01002FC012548000ull;
std::atomic<int> g_phase{0};
std::atomic<int> g_done{0};
std::atomic<int> g_total{0};
std::mutex g_error_mutex;
std::string g_error;

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
    if (p == "romfs") return 3;
    if (p == "verify") return 4;
    if (p == "done") return 5;
    return 1;
}

void set_error(const std::string& e) {
    std::lock_guard<std::mutex> lock(g_error_mutex);
    g_error = e;
}
} // namespace

extern "C" JNIEXPORT jint JNICALL
Java_io_github_jjkport_Importer_run(JNIEnv* env, jclass, jint game_fd, jstring game_name,
                                    jstring keys_path, jstring title_keys_path, jstring out_dir,
                                    jstring /*work_dir*/, jint /*jobs*/) {
    g_phase = 1;
    g_done = 0;
    g_total = 0;
    set_error("");

    const std::string name = jstr(env, game_name);
    const std::string keys = jstr(env, keys_path);
    const std::string title_keys = jstr(env, title_keys_path);
    nxi::Options opt;
    opt.out_dir = jstr(env, out_dir);
    opt.title_id = kTitleId;
    opt.keep_exefs = true;
    opt.on_progress = [](const nxi::Progress& p) {
        g_phase = phase_id(p.phase);
        // Java progress fields are jint. Report MiB rather than bytes, and treat
        // implausibly large totals (including UINT64_MAX sentinels) as unknown.
        constexpr uint64_t kMiB = 1024ull * 1024ull;
        constexpr uint64_t kMax = 0x7fffffff;
        g_done = static_cast<int>(std::min<uint64_t>(p.done / kMiB, kMax));
        if (p.total == 0 || p.total / kMiB > kMax)
            g_total = 0;
        else
            g_total = static_cast<int>((p.total + kMiB - 1) / kMiB);
        __android_log_print(ANDROID_LOG_INFO, "jjkimport", "%s: %llu/%llu %s", p.phase.c_str(),
                            static_cast<unsigned long long>(p.done),
                            static_cast<unsigned long long>(p.total), p.item.c_str());
    };
    opt.on_log = [](const std::string& line) { __android_log_print(ANDROID_LOG_INFO, "jjkimport", "%s", line.c_str()); };

    std::vector<nxi::InputFile> inputs;
    inputs.push_back({name, game_fd});
    const int kfd = open(keys.c_str(), O_RDONLY | O_CLOEXEC);
    if (kfd < 0) {
        set_error("cannot read prod.keys");
        return 1;
    }
    inputs.push_back({"prod.keys", kfd});
    int tfd = -1;
    if (!title_keys.empty()) {
        tfd = open(title_keys.c_str(), O_RDONLY | O_CLOEXEC);
        if (tfd >= 0) inputs.push_back({"title.keys", tfd});
    }

    std::string error;
    const int rc = nxi::run_import(inputs, opt, error);
    close(kfd);
    if (tfd >= 0) close(tfd);
    if (rc != 0) {
        set_error(error.empty() ? "import failed" : error);
        g_phase = 0;
        return rc;
    }
    g_phase = 5;
    return 0;
}

extern "C" JNIEXPORT jintArray JNICALL
Java_io_github_jjkport_Importer_progress(JNIEnv* env, jclass) {
    jint v[3] = {g_phase.load(), g_done.load(), g_total.load()};
    jintArray out = env->NewIntArray(3);
    if (out) env->SetIntArrayRegion(out, 0, 3, v);
    return out;
}

extern "C" JNIEXPORT jstring JNICALL
Java_io_github_jjkport_Importer_error(JNIEnv* env, jclass) {
    std::lock_guard<std::mutex> lock(g_error_mutex);
    return env->NewStringUTF(g_error.c_str());
}
