#include <jni.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include "nxgame.h"

extern "C" JNIEXPORT jstring JNICALL
Java_io_github_jjkrecomp_Launcher_inspectNative(JNIEnv* env, jobject,
                                                 jint game_fd, jstring game_name,
                                                 jint keys_fd) {
    const char* raw_name = env->GetStringUTFChars(game_name, nullptr);
    std::string name = raw_name ? raw_name : "game.xci";
    if (raw_name) env->ReleaseStringUTFChars(game_name, raw_name);
    std::vector<nxi::InputFile> inputs;
    inputs.push_back({name, game_fd});
    inputs.push_back({"prod.keys", keys_fd});
    std::string error;
    auto game = nxi::GameImage::open(inputs, 0, error);
    if (!game) {
        std::string msg = "INSPECT_ERROR: " + error;
        return env->NewStringUTF(msg.c_str());
    }
    char title[32];
    std::snprintf(title, sizeof(title), "%016llX",
                  static_cast<unsigned long long>(game->title_id()));
    auto it = game->exefs().find("main");
    std::string build = "unavailable";
    if (it != game->exefs().end() && it->second.size() >= 0x60 &&
        std::memcmp(it->second.data(), "NSO0", 4) == 0) {
        static const char* hex = "0123456789ABCDEF";
        build.clear();
        for (size_t i = 0x40; i < 0x60; ++i) {
            unsigned char b = it->second[i];
            build += hex[b >> 4];
            build += hex[b & 15];
        }
    }
    std::string result = std::string("TITLE_ID=") + title +
        "\nMAIN_BUILD_ID=" + build +
        "\nEXEFS_FILES=" + std::to_string(game->exefs().size()) +
        "\nROMFS_FILES=" + std::to_string(game->file_count());
    return env->NewStringUTF(result.c_str());
}
