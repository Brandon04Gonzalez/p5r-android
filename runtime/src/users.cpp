#include "users.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>

#include <nx/runtime.h>

#include "platform.h"

namespace nx {

namespace {

// The user every earlier version had (kept so existing saves load unchanged).
constexpr uint64_t kFirstLo = 0x0000000000000001ull, kFirstHi = 0x4E58485553455231ull;

std::mutex g_users_mtx;
std::vector<UserInfo> g_users;
bool g_loaded = false;
UserInfo g_last;
bool g_have_last = false;

std::filesystem::path users_path() { return std::filesystem::u8path(plat::data_root()) / "users.txt"; }

std::string hex_uid(uint64_t lo, uint64_t hi) {
    char b[40];
    std::snprintf(b, sizeof b, "%016llx%016llx", (unsigned long long)hi, (unsigned long long)lo);
    return b;
}

void save_locked() {
    std::ofstream f(users_path(), std::ios::trunc);
    for (auto& u : g_users) f << hex_uid(u.lo, u.hi) << ' ' << u.nickname << '\n';
}

void load_locked() {
    if (g_loaded) return;
    g_loaded = true;
    std::ifstream f(users_path());
    std::string line;
    while (f && std::getline(f, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.size() < 33 || line[32] != ' ') continue;
        UserInfo u;
        try {
            u.hi = std::stoull(line.substr(0, 16), nullptr, 16);
            u.lo = std::stoull(line.substr(16, 16), nullptr, 16);
        } catch (...) {
            continue;
        }
        u.nickname = line.substr(33);
        if (u.nickname.size() > 31) u.nickname.resize(31);
        if (u.valid() && g_users.size() < kMaxUsers) g_users.push_back(u);
    }
    if (g_users.empty()) {
        g_users.push_back({kFirstLo, kFirstHi, "Zagreus"});
        save_locked();
        log("users: created %s with the first user", users_path().string().c_str());
    }
    for (auto& u : g_users) log("users: %s %s", hex_uid(u.lo, u.hi).c_str(), u.nickname.c_str());
}

}  // namespace

std::vector<UserInfo> users_all() {
    std::lock_guard lk(g_users_mtx);
    load_locked();
    return g_users;
}

bool user_find(uint64_t lo, uint64_t hi, UserInfo* out) {
    for (auto& u : users_all())
        if (u.lo == lo && u.hi == hi) {
            if (out) *out = u;
            return true;
        }
    return false;
}

UserInfo user_create() {
    std::lock_guard lk(g_users_mtx);
    load_locked();
    if (g_users.size() >= kMaxUsers) return {};
    uint64_t next_lo = 0;
    for (auto& u : g_users) next_lo = std::max(next_lo, u.lo);
    UserInfo n;
    n.lo = next_lo + 1;
    n.hi = kFirstHi;
    for (size_t k = g_users.size() + 1;; k++) {
        std::string name = "Player " + std::to_string(k);
        bool used = false;
        for (auto& u : g_users) used |= u.nickname == name;
        if (!used) {
            n.nickname = name;
            break;
        }
    }
    g_users.push_back(n);
    save_locked();
    log("users: created user %s %s", hex_uid(n.lo, n.hi).c_str(), n.nickname.c_str());
    return n;
}

UserInfo user_boot() { return user_last_opened(); }

void user_opened(uint64_t lo, uint64_t hi) {
    UserInfo u;
    if (!user_find(lo, hi, &u)) return;
    std::lock_guard lk(g_users_mtx);
    g_last = u;
    g_have_last = true;
}

UserInfo user_last_opened() {
    auto all = users_all();
    std::lock_guard lk(g_users_mtx);
    if (g_have_last) return g_last;
    if (const char* e = std::getenv("NX_USER")) {
        int n = std::atoi(e);
        if (n >= 1 && size_t(n) <= all.size()) return all[size_t(n) - 1];
    }
    return all.front();
}

std::string user_save_dir(uint64_t lo, uint64_t hi) {
    if (lo == kFirstLo && hi == kFirstHi) return "save";
    return "save_" + hex_uid(lo, hi);
}

}  // namespace nx
