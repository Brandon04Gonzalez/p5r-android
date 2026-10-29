#include <jni.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <mutex>
#include <string>
#include <vector>

#include "nxgame.h"
#include "nximport.h"

namespace {
constexpr uint64_t kJjkTitleId = 0x01002FC012548000ull;
std::mutex g_progress_mutex;
std::string g_progress = "idle|0|0|";
std::atomic<bool> g_importing{false};

std::string java_string(JNIEnv* env, jstring value) {
    if (!value) return {};
    const char* chars = env->GetStringUTFChars(value, nullptr);
    std::string out = chars ? chars : "";
    if (chars) env->ReleaseStringUTFChars(value, chars);
    return out;
}

std::string title_hex(uint64_t id) {
    char out[17];
    std::snprintf(out, sizeof(out), "%016llX", static_cast<unsigned long long>(id));
    return out;
}

std::string build_id(const std::vector<uint8_t>& main) {
    if (main.size() < 0x60 || std::string(reinterpret_cast<const char*>(main.data()), 4) != "NSO0")
        return "unavailable";
    std::string out;
    static const char* hex = "0123456789ABCDEF";
    for (size_t i = 0x40; i < 0x60; ++i) {
        out.push_back(hex[main[i] >> 4]);
        out.push_back(hex[main[i] & 15]);
    }
    return out;
}
}

extern "C" JNIEXPORT jstring JNICALL
Java_io_github_jjkrecomp_Launcher_inspectNative(JNIEnv* env, jobject, jint game_fd, jstring game_name, jint keys_fd) {
    std::vector<nxi::InputFile> inputs = {
        {java_string(env, game_name), game_fd},
        {"prod.keys", keys_fd}
    };
    std::string error;
    auto image = nxi::GameImage::open(inputs, 0, error);
    if (!image) return env->NewStringUTF(("INSPECT_ERROR: " + error).c_str());

    const auto& exefs = image->exefs();
    auto main = exefs.find("main");
    std::string output = "TITLE_ID=" + title_hex(image->title_id());
    output += "\nEXEFS_MAIN_BUILD_ID=" + (main == exefs.end() ? std::string("missing") : build_id(main->second));
    output += "\nEXEFS_FILES=" + std::to_string(exefs.size());
    if (image->title_id() != kJjkTitleId)
        output += "\nWARNING=Title ID does not match JJK target";
    return env->NewStringUTF(output.c_str());
}

extern "C" JNIEXPORT jstring JNICALL
Java_io_github_jjkrecomp_Launcher_importNative(JNIEnv* env, jobject, jint game_fd, jstring game_name,
                                                jint keys_fd, jstring out_dir) {
    if (g_importing.exchange(true))
        return env->NewStringUTF("IMPORT_ERROR: An import is already running.");

    nxi::Options options;
    options.out_dir = java_string(env, out_dir);
    options.title_id = kJjkTitleId;
    options.keep_exefs = true;
    {
        std::lock_guard<std::mutex> lock(g_progress_mutex);
        g_progress = "starting|0|0|";
    }
    options.on_progress = [](const nxi::Progress& p) {
        std::string value = p.phase + "|" + std::to_string(p.done) + "|" + std::to_string(p.total) + "|" + p.item;
        std::lock_guard<std::mutex> lock(g_progress_mutex);
        g_progress = std::move(value);
    };
    options.on_log = [](const std::string& line) {
        // Importer messages never include key values. Keep logs useful without logging file contents.
        __android_log_print(ANDROID_LOG_INFO, "jjkinspector", "%s", line.c_str());
    };

    std::vector<nxi::InputFile> inputs = {
        {java_string(env, game_name), game_fd},
        {"prod.keys", keys_fd}
    };
    std::string error;
    const int rc = nxi::run_import(inputs, options, error);
    g_importing = false;
    {
        std::lock_guard<std::mutex> lock(g_progress_mutex);
        g_progress = rc == 0 ? "done|1|1|complete" : "error|0|0|" + error;
    }
    if (rc != 0) return env->NewStringUTF(("IMPORT_ERROR: " + error).c_str());
    return env->NewStringUTF(("IMPORT_OK: " + options.out_dir + "\nTarget Title ID: " + title_hex(kJjkTitleId)).c_str());
}

extern "C" JNIEXPORT jstring JNICALL
Java_io_github_jjkrecomp_Launcher_importProgress(JNIEnv* env, jobject) {
    std::lock_guard<std::mutex> lock(g_progress_mutex);
    return env->NewStringUTF(g_progress.c_str());
}
