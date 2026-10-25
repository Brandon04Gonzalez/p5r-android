// The local Switch users: a list kept in <data root>/users.txt, one "<32 hex digits of the uid> <nickname>" per line.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace nx {

constexpr size_t kMaxUsers = 8;  // the Switch's limit

struct UserInfo {
    uint64_t lo = 0, hi = 0;  // account::Uid { u64[2] }
    std::string nickname;
    bool valid() const { return lo || hi; }
};

std::vector<UserInfo> users_all();
bool user_find(uint64_t lo, uint64_t hi, UserInfo* out = nullptr);
// A new user ("Player 2", "Player 3", ...) saved to users.txt; an invalid UserInfo when 8 users exist already.
UserInfo user_create();
// The user the game boots into: NX_USER=<n> (1 = the first), else the last one opened, else the first.
UserInfo user_boot();
void user_opened(uint64_t lo, uint64_t hi);  // remember it as the last opened user
UserInfo user_last_opened();
// The save folder of a user, relative to the data root's "user" folder: "save" for the first user (the one every
// earlier version used), "save_<uid hex>" for the others.
std::string user_save_dir(uint64_t lo, uint64_t hi);

}  // namespace nx
