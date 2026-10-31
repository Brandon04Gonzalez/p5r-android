// nn::fs at the API level, on host files. The game imports these by name (main), so no fsp-srv IPC is involved:
//   rom:   -> the game's RomFS read in place from the player's NSP (game_image.h), else private/extract/romfs
//   save:  -> private/user/save/<mount>
//   cache: -> private/user/cache<index>
#include <cctype>
#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#if defined(_WIN32)
#include <io.h>
#else
#include <unistd.h>
#endif

#include <nx/runtime.h>

#include "game_image.h"
#include "platform.h"
#include "users.h"

namespace fs = std::filesystem;

namespace nx {

namespace {

// NX_TRACE_FS=1: every file the game opens (and the ones it looks for and does not find).
bool trace_fs() {
    static const bool on = g_trace || std::getenv("NX_TRACE_FS") != nullptr;
    return on;
}

// The last opens, newest last: a crash report that says what was being loaded (log_recent_files, read by the crash
// handlers; try_lock there, a handler must not wait for a thread that holds the lock).
constexpr size_t kRecentOps = 24;
std::mutex g_recent_lock;
struct RecentOp {
    int64_t thread = 0;
    uint64_t seq = 0;
    char text[200] = {0};
} g_recent[kRecentOps];
uint64_t g_recent_seq = 0;

void note_open(const char* what, const std::string& guest_path, const char* result) {
    std::lock_guard lk(g_recent_lock);
    RecentOp& r = g_recent[g_recent_seq % kRecentOps];
    r.thread = plat::current_thread_id();
    r.seq = ++g_recent_seq;
    std::snprintf(r.text, sizeof r.text, "%s %s: %s", what, guest_path.c_str(), result);
}

constexpr uint32_t kOk = 0;
constexpr uint32_t kPathNotFound = 0x202;       // fs 1
constexpr uint32_t kPathAlreadyExists = 0x402;  // fs 2
constexpr uint32_t kTargetNotFound = 0x2EE202;  // fs 6001-ish: generic "not mounted"

std::mutex g_fs;
std::map<std::string, fs::path> g_mounts;
// Open files: shared so a read in progress survives a close from another thread. Reads run outside g_fs (which
// only guards the tables): one global lock made every streaming thread (Bink animations, FMOD banks) wait behind
// any big read, felt as hitches.
struct File {
    FILE* f;
    std::string path;
    const nxi::GameImage::Entry* entry = nullptr;  // a RomFS file of the game image (f is null)
    File(FILE* file, std::string p) : f(file), path(std::move(p)) {}
    File(const nxi::GameImage::Entry* e, std::string p) : f(nullptr), path(std::move(p)), entry(e) {}
    File(const File&) = delete;  // owns the FILE: one instance only
    File& operator=(const File&) = delete;
    ~File() {
        if (f) std::fclose(f);
    }
};
std::map<uint64_t, std::shared_ptr<File>> g_files;

// Files the runtime adds or replaces (mods): key = path under the mount point, '/' separated, lower case.
std::map<std::string, std::string> g_virtual;

std::string virtual_key(const fs::path& p) {
    auto g = p.lexically_normal().generic_u8string();
    std::string s(g.begin(), g.end());
    for (char& ch : s) ch = char(std::tolower(static_cast<unsigned char>(ch)));
    return s;
}

// The virtual file whose key the resolved path ends with, or nullptr.
const std::string* find_virtual(const fs::path& resolved) {
    if (g_virtual.empty() || resolved.empty()) return nullptr;
    std::string k = virtual_key(resolved);
    for (auto& [key, data] : g_virtual)
        if (k.size() >= key.size() + 1 && k.compare(k.size() - key.size(), key.size(), key) == 0 &&
            k[k.size() - key.size() - 1] == '/')
            return &data;
    return nullptr;
}
struct Dir {
    std::vector<fs::directory_entry> entries;
    std::vector<std::pair<std::string, nxi::GameImage::Entry>> image;  // a folder of the game image's RomFS
    bool from_image = false;
    size_t pos = 0;
    size_t count() const { return from_image ? image.size() : entries.size(); }
};
std::map<uint64_t, Dir> g_dirs;
uint64_t g_next = 1;

fs::path data_root() { return fs::u8path(plat::data_root()); }

std::string utf8(const fs::path& p) {
    auto s = p.u8string();
    return std::string(s.begin(), s.end());
}

std::string cstr(uint64_t p) { return p ? std::string(reinterpret_cast<const char*>(p)) : std::string(); }

// "mount:/a/b" -> host path; empty when the mount is unknown.
fs::path resolve(const std::string& guest) {
    auto colon = guest.find(':');
    if (colon == std::string::npos) return {};
    auto it = g_mounts.find(guest.substr(0, colon));
    if (it == g_mounts.end()) return {};
    std::string rest = guest.substr(colon + 1);
    while (!rest.empty() && (rest[0] == '/' || rest[0] == '\\')) rest.erase(0, 1);
    return it->second / fs::u8path(rest);
}

void mount(const std::string& name, const fs::path& p) {
    fs::create_directories(p);
    g_mounts[name] = p;
    log("fs: mounted %s: -> %s", name.c_str(), p.string().c_str());
}

// Mounts served from the game image (the RomFS read in place). They keep a host folder in g_mounts too, only so the
// runtime's virtual files (add_virtual_file) still match their paths.
std::map<std::string, bool> g_image_mounts;

// The RomFS path of `guest` when its mount is served from the game image.
bool image_rel(const std::string& guest, std::string& rel) {
    auto colon = guest.find(':');
    if (colon == std::string::npos || !g_image_mounts.count(guest.substr(0, colon))) return false;
    rel = game_image_path(guest.substr(colon + 1));
    return true;
}

// ---- mods (LayeredFS style) -----------------------------------------------------------------------------------
// <data folder>/mods/<ModName>/romfs/<same path as in the game's RomFS>. A file there replaces the game's file of the
// same path (or adds one). Folders whose name starts with '.' or '_' are ignored (rename to _Name to disable a mod).
// Mods apply in alphabetical order; the last one wins. Paths are case sensitive on Android: copy the game's case.
// Callers hold g_fs.
std::vector<fs::path> g_mod_roots;  // low priority first
const nxi::GameImage::Entry kModDir{0, 0, true};

fs::path find_case_insensitive(const fs::path& base, const std::string& rel) {
    fs::path current = base;
    size_t start = 0;
    std::error_code ec;
    while (start < rel.size()) {
        size_t next = rel.find('/', start);
        std::string part = (next == std::string::npos) ? rel.substr(start) : rel.substr(start, next - start);
        start = (next == std::string::npos) ? rel.size() : next + 1;
        if (part.empty()) continue;

        fs::path direct = current / fs::u8path(part);
        if (fs::exists(direct, ec)) {
            current = direct;
            continue;
        }
        bool found = false;
        if (fs::is_directory(current, ec)) {
            for (auto& entry : fs::directory_iterator(current, ec)) {
                auto name_u8 = entry.path().filename().u8string();
                std::string name(name_u8.begin(), name_u8.end());
                if (name.size() == part.size()) {
                    bool match = true;
                    for (size_t i = 0; i < name.size(); i++) {
                        if (std::tolower(static_cast<unsigned char>(name[i])) !=
                            std::tolower(static_cast<unsigned char>(part[i]))) {
                            match = false;
                            break;
                        }
                    }
                    if (match) {
                        current = entry.path();
                        found = true;
                        break;
                    }
                }
            }
        }
        if (!found) return {};
    }
    return current;
}

void scan_mods() {
    g_mod_roots.clear();
    std::error_code ec;
    fs::path root = data_root() / "mods";
    std::vector<fs::path> found;

    // Check direct mods/romfs (Atmosphere layout dropped directly into mods/)
    if (fs::is_directory(root / "romfs", ec)) {
        found.push_back(root / "romfs");
    }

    // Check atmosphere/contents/<title_id>/romfs
    fs::path atmos = data_root() / "atmosphere" / "contents";
    if (fs::is_directory(atmos, ec)) {
        for (auto& e : fs::directory_iterator(atmos, ec)) {
            if (e.is_directory(ec) && fs::is_directory(e.path() / "romfs", ec))
                found.push_back(e.path() / "romfs");
        }
    }

    // Check mods/<ModName>...
    if (fs::is_directory(root, ec)) {
        for (auto& e : fs::directory_iterator(root, ec)) {
            if (!e.is_directory(ec)) continue;
            auto u8 = e.path().filename().u8string();
            std::string name(u8.begin(), u8.end());
            if (name.empty() || name[0] == '.' || name[0] == '_') continue;
            if (name == "romfs" || name == "exefs") continue;
            if (fs::is_directory(e.path() / "romfs", ec)) {
                found.push_back(e.path() / "romfs");
            } else if (fs::is_directory(e.path() / "CPK", ec) || fs::is_directory(e.path() / "cpk", ec) ||
                       fs::is_directory(e.path() / "sound", ec) || fs::is_directory(e.path() / "movie", ec) ||
                       fs::is_directory(e.path() / "field", ec) || fs::is_directory(e.path() / "app_config", ec)) {
                found.push_back(e.path());
            }
        }
    }

    std::sort(found.begin(), found.end());
    found.erase(std::unique(found.begin(), found.end()), found.end());
    g_mod_roots = std::move(found);
    for (auto& r : g_mod_roots) log("fs: mod active: %s", r.string().c_str());

    // Crowd removal option (NX_NO_CROWD=1): stub RESRCCROWDUNIT.BIN to prevent crowd spawning
    if (const char* e = std::getenv("NX_NO_CROWD")) {
        if (e[0] == '1' || std::strcmp(e, "true") == 0) {
            log("fs: crowd removal active (NX_NO_CROWD=1): stubbing RESRCCROWDUNIT.BIN");
            add_virtual_file("resource/resrccrowdunit.bin", "");
            add_virtual_file("base/resource/resrccrowdunit.bin", "");
            add_virtual_file("cpk/patch1/base/resource/resrccrowdunit.bin", "");
        }
    }
}

bool safe_rel(const std::string& rel) {  // no ".." part: a mod path never leaves its folder
    size_t i = 0;
    while (i <= rel.size()) {
        size_t j = rel.find('/', i);
        if (j == std::string::npos) j = rel.size();
        if (rel.compare(i, j - i, "..") == 0) return false;
        i = j + 1;
    }
    return true;
}

// The host file of the highest priority mod that has `rel`, or empty.
fs::path mod_file(const std::string& rel) {
    if (g_mod_roots.empty() || rel.empty() || !safe_rel(rel)) return {};
    std::error_code ec;
    for (auto it = g_mod_roots.rbegin(); it != g_mod_roots.rend(); ++it) {
        fs::path p = *it / fs::u8path(rel);
        if (fs::is_regular_file(p, ec)) return p;
        fs::path ci = find_case_insensitive(*it, rel);
        if (!ci.empty() && fs::is_regular_file(ci, ec)) return ci;
    }
    return {};
}

bool mod_has_dir(const std::string& rel) {
    if (g_mod_roots.empty() || !safe_rel(rel)) return false;
    std::error_code ec;
    for (auto& r : g_mod_roots) {
        if (fs::is_directory(rel.empty() ? r : r / fs::u8path(rel), ec)) return true;
        if (!rel.empty()) {
            fs::path ci = find_case_insensitive(r, rel);
            if (!ci.empty() && fs::is_directory(ci, ec)) return true;
        }
    }
    return false;
}

// ---- mounts ---------------------------------------------------------------------------------------------------

void QueryMountRomCacheSize(CPU& c) {
    st<uint64_t>(c.x[0], 0x1000);
    c.x[0] = kOk;
}

void MountRom(CPU& c) {
    std::lock_guard lk(g_fs);
    std::string name = cstr(c.x[0]);
    scan_mods();
    if (game_image()) {
        g_mounts[name] = data_root() / "extract" / "romfs";
        g_image_mounts[name] = true;
        log("fs: mounted %s: -> the game file's RomFS", name.c_str());
    } else {
        mount(name, data_root() / "extract" / "romfs");
    }
    c.x[0] = kOk;
}

// The save folder of the account::Uid at a guest address: user/save for the first user, user/save_<uid> for the others.
fs::path save_dir_of(uint64_t uid_addr) {
    uint64_t lo = ld<uint64_t>(uid_addr), hi = ld<uint64_t>(uid_addr + 8);
    user_opened(lo, hi);
    return data_root() / "user" / user_save_dir(lo, hi);
}

void EnsureSaveData(CPU& c) {
    std::error_code ec;
    fs::path p = save_dir_of(c.x[0]);
    fs::create_directories(p, ec);
    log("fs: EnsureSaveData -> %s", p.string().c_str());
    c.x[0] = kOk;
}

void MountSaveData(CPU& c) {
    std::lock_guard lk(g_fs);
    std::string name = cstr(c.x[0]);
    mount(name, save_dir_of(c.x[1]));
    c.x[0] = kOk;
}

void Unmount(CPU& c) {
    std::lock_guard lk(g_fs);
    g_mounts.erase(cstr(c.x[0]));
    g_image_mounts.erase(cstr(c.x[0]));
    c.x[0] = kOk;
}

void Commit(CPU& c) { c.x[0] = kOk; }
void EnableGlobalFileDataCache(CPU& c) { c.x[0] = kOk; }
void DisableGlobalFileDataCache(CPU& c) { c.x[0] = kOk; }

// ---- files ----------------------------------------------------------------------------------------------------

void CreateFile(CPU& c) {
    std::lock_guard lk(g_fs);
    fs::path p = resolve(cstr(c.x[0]));
    int64_t size = int64_t(c.x[1]);
    if (p.empty() || !fs::exists(p.parent_path())) {
        c.x[0] = kPathNotFound;
        return;
    }
    if (fs::exists(p)) {
        c.x[0] = kPathAlreadyExists;
        return;
    }
    FILE* f = plat::fopen_utf8(utf8(p), "wb");
    if (f) {
        if (size > 0) {
            plat::fseek64(f, size - 1, SEEK_SET);
            fputc(0, f);
        }
        fclose(f);
    }
    c.x[0] = f ? kOk : kPathNotFound;
}

void DeleteFile(CPU& c) {
    std::lock_guard lk(g_fs);
    fs::path p = resolve(cstr(c.x[0]));
    std::error_code ec;
    c.x[0] = !p.empty() && fs::remove(p, ec) ? kOk : kPathNotFound;
}

void CreateDirectory(CPU& c) {
    std::lock_guard lk(g_fs);
    fs::path p = resolve(cstr(c.x[0]));
    if (p.empty()) {
        c.x[0] = kPathNotFound;
        return;
    }
    if (fs::exists(p)) {
        c.x[0] = kPathAlreadyExists;
        return;
    }
    std::error_code ec;
    c.x[0] = fs::create_directory(p, ec) ? kOk : kPathNotFound;
}

void GetEntryType(CPU& c) {  // (DirectoryEntryType* out, path)
    std::lock_guard lk(g_fs);
    fs::path p = resolve(cstr(c.x[1]));
    if (find_virtual(p)) {
        st<uint32_t>(c.x[0], 1);
        c.x[0] = kOk;
        return;
    }
    if (std::string rel; image_rel(cstr(c.x[1]), rel)) {
        if (!mod_file(rel).empty()) {
            st<uint32_t>(c.x[0], 1);
            c.x[0] = kOk;
            return;
        }
        const nxi::GameImage::Entry* e = game_image()->find(rel);
        if (e) st<uint32_t>(c.x[0], e->dir ? 0 : 1);
        else if (mod_has_dir(rel)) st<uint32_t>(c.x[0], 0), e = &kModDir;
        c.x[0] = e ? kOk : kPathNotFound;
        return;
    }
    std::error_code ec;
    if (p.empty() || !fs::exists(p, ec)) {
        c.x[0] = kPathNotFound;
        return;
    }
    st<uint32_t>(c.x[0], fs::is_directory(p) ? 0 : 1);
    c.x[0] = kOk;
}

void OpenFile(CPU& c) {  // (FileHandle* out, path, int mode: 1 read, 2 write, 4 append)
    std::lock_guard lk(g_fs);
    std::string guest = cstr(c.x[1]);
    fs::path p = resolve(guest);
    int mode = int(c.x[2]);
    if (const std::string* v = find_virtual(p)) {  // served from memory, through a temporary file
        FILE* f = std::tmpfile();
        if (!f) {
            c.x[0] = kPathNotFound;
            return;
        }
        std::fwrite(v->data(), 1, v->size(), f);
        std::fflush(f);
        uint64_t h = g_next++;
        g_files[h] = std::make_shared<File>(f, guest);
        st<uint64_t>(c.x[0], h);
        note_open("OpenFile", guest, "virtual");
        if (trace_fs()) log("fs: OpenFile(%s) -> virtual, %zu bytes", guest.c_str(), v->size());
        c.x[0] = kOk;
        return;
    }
    if (std::string rel; image_rel(guest, rel)) {  // the game image's RomFS: read only
        if (fs::path mp = mod_file(rel); !mp.empty()) {  // a mod's file replaces (or adds to) the game's
            if (FILE* f = plat::fopen_utf8(utf8(mp), "rb")) {
                uint64_t h = g_next++;
                g_files[h] = std::make_shared<File>(f, guest);
                st<uint64_t>(c.x[0], h);
                note_open("OpenFile", guest, "ok (mod)");
                log("fs: mod file %s", rel.c_str());
                c.x[0] = kOk;
                return;
            }
        }
        const nxi::GameImage::Entry* e = game_image()->find(rel);
        if (!e || e->dir) {
            note_open("OpenFile", guest, "not in the game file");
            if (trace_fs()) log("fs: OpenFile(%s) not found in the game file", guest.c_str());
            c.x[0] = kPathNotFound;
            return;
        }
        uint64_t h = g_next++;
        g_files[h] = std::make_shared<File>(e, guest);
        st<uint64_t>(c.x[0], h);
        note_open("OpenFile", guest, "ok (game file)");
        if (trace_fs()) log("fs: OpenFile(%s, %d) -> %llu (game file)", guest.c_str(), mode, (unsigned long long)h);
        c.x[0] = kOk;
        return;
    }
    std::error_code ec;
    if (p.empty() || !fs::is_regular_file(p, ec)) {
        note_open("OpenFile", guest, p.empty() ? "unknown mount" : "not found");
        if (trace_fs()) log("fs: OpenFile(%s) not found", guest.c_str());
        c.x[0] = kPathNotFound;
        return;
    }
    FILE* f = plat::fopen_utf8(utf8(p), (mode & 2) ? "r+b" : "rb");
    if (!f) {
        note_open("OpenFile", guest, "the host could not open it");
        if (trace_fs()) log("fs: OpenFile(%s) failed to open on the host (errno %d)", guest.c_str(), errno);
        c.x[0] = kPathNotFound;
        return;
    }
    uint64_t h = g_next++;
    g_files[h] = std::make_shared<File>(f, guest);
    st<uint64_t>(c.x[0], h);
    note_open("OpenFile", guest, "ok");
    if (trace_fs()) log("fs: OpenFile(%s, %d) -> %llu", guest.c_str(), mode, (unsigned long long)h);
    c.x[0] = kOk;
}

std::shared_ptr<File> open_file(uint64_t h) {
    std::lock_guard lk(g_fs);
    auto it = g_files.find(h);
    return it == g_files.end() ? nullptr : it->second;
}

void CloseFile(CPU& c) {
    std::lock_guard lk(g_fs);
    g_files.erase(c.x[0]);
}

void GetFileSize(CPU& c) {  // (s64* out, FileHandle)
    auto file = open_file(c.x[1]);
    if (!file) {
        c.x[0] = kTargetNotFound;
        return;
    }
    if (file->entry) {
        st<int64_t>(c.x[0], int64_t(file->entry->size));
        c.x[0] = kOk;
        return;
    }
    std::lock_guard lk(g_fs);  // the FILE position is shared with writers
    FILE* f = file->f;
    int64_t cur = plat::ftell64(f);
    plat::fseek64(f, 0, SEEK_END);
    st<int64_t>(c.x[0], plat::ftell64(f));
    if (g_trace) log("fs: size %s = %lld", file->path.c_str(), (long long)ld<int64_t>(c.x[0]));
    plat::fseek64(f, cur, SEEK_SET);
    c.x[0] = kOk;
}

uint64_t read_at(const File& file, int64_t off, uint64_t buf, uint64_t size) {
    uint64_t t0 = plat::monotonic_ns();
    uint64_t n = file.entry ? game_image()->read(*file.entry, uint64_t(off), reinterpret_cast<void*>(buf), size)
                            : plat::read_at(file.f, off, reinterpret_cast<void*>(buf), size);
    uint64_t ms = (plat::monotonic_ns() - t0) / 1'000'000;
    if (g_trace) log("fs: read %s at %lld size %llu -> %llu", file.path.c_str(), (long long)off, (unsigned long long)size,
                     (unsigned long long)n);
    if (trace_fs() && file.path.find(".bwav") != std::string::npos) {  // streamed audio: is it being consumed?
        static std::mutex m;
        static std::map<std::string, uint64_t> reads;
        std::lock_guard lk(m);
        uint64_t k = ++reads[file.path];
        if (k <= 3 || k % 50 == 0) log("fs: stream read #%llu %s at %lld", (unsigned long long)k, file.path.c_str(), (long long)off);
    }
    if (ms >= 30)
        log("fs: slow read %s: %llu bytes at %lld took %llu ms", file.path.c_str(), (unsigned long long)size,
            (long long)off, (unsigned long long)ms);
    return n;
}

void ReadFileOut(CPU& c) {  // (size_t* out, FileHandle, s64 off, void* buf, size_t size)
    auto file = open_file(c.x[1]);
    if (!file) {
        c.x[0] = kTargetNotFound;
        return;
    }
    st<uint64_t>(c.x[0], read_at(*file, int64_t(c.x[2]), c.x[3], c.x[4]));
    c.x[0] = kOk;
}

void ReadFile(CPU& c) {  // (FileHandle, s64 off, void* buf, size_t size)
    auto file = open_file(c.x[0]);
    if (!file) {
        c.x[0] = kTargetNotFound;
        return;
    }
    read_at(*file, int64_t(c.x[1]), c.x[2], c.x[3]);
    c.x[0] = kOk;
}

void WriteFile(CPU& c) {  // (FileHandle, s64 off, const void* buf, size_t size, const WriteOption&)
    auto file = open_file(c.x[0]);
    if (!file || !file->f) {  // the game image is read only
        c.x[0] = kTargetNotFound;
        return;
    }
    std::lock_guard lk(g_fs);
    plat::fseek64(file->f, int64_t(c.x[1]), SEEK_SET);
    fwrite(reinterpret_cast<const void*>(c.x[2]), 1, c.x[3], file->f);
    fflush(file->f);  // positional reads bypass the FILE buffer (saves only: rare and small)
    c.x[0] = kOk;
}

void FlushFile(CPU& c) {
    if (auto file = open_file(c.x[0]); file && file->f) {
        std::lock_guard lk(g_fs);
        fflush(file->f);
    }
    c.x[0] = kOk;
}

// ---- directories ----------------------------------------------------------------------------------------------

void OpenDirectory(CPU& c) {  // (DirectoryHandle* out, path, int mode: 1 dirs, 2 files)
    std::lock_guard lk(g_fs);
    fs::path p = resolve(cstr(c.x[1]));
    int mode = int(c.x[2]);
    if (std::string rel; image_rel(cstr(c.x[1]), rel)) {
        const nxi::GameImage::Entry* e = game_image()->find(rel);
        bool in_game = e && e->dir, in_mod = mod_has_dir(rel);
        note_open("OpenDirectory", cstr(c.x[1]), in_game || in_mod ? "ok (game file)" : "not in the game file");
        if (!in_game && !in_mod) {
            c.x[0] = kPathNotFound;
            return;
        }
        Dir d;
        d.from_image = true;
        // The game's folder merged with the same folder of each mod (a mod's file wins; new names are added).
        std::vector<std::pair<std::string, nxi::GameImage::Entry>> all;
        std::unordered_map<std::string, size_t> at;
        if (in_game) all = game_image()->list(rel);
        for (size_t i = 0; i < all.size(); i++) at[all[i].first] = i;
        std::error_code ec;
        for (auto& root : g_mod_roots) {
            fs::path dir = rel.empty() ? root : root / fs::u8path(rel);
            if (!safe_rel(rel) || !fs::is_directory(dir, ec)) continue;
            for (auto& me : fs::directory_iterator(dir, ec)) {
                auto u8 = me.path().filename().u8string();
                std::string name(u8.begin(), u8.end());
                nxi::GameImage::Entry en;
                en.dir = me.is_directory(ec);
                en.size = en.dir ? 0 : uint64_t(me.file_size(ec));
                if (auto f = at.find(name); f != at.end()) {
                    if (!(all[f->second].second.dir && en.dir)) all[f->second].second = en;  // dir + dir: keep
                } else {
                    at[name] = all.size();
                    all.emplace_back(name, en);
                }
            }
        }
        for (auto& kid : all)
            if ((kid.second.dir && (mode & 1)) || (!kid.second.dir && (mode & 2))) d.image.push_back(kid);
        uint64_t h = g_next++;
        g_dirs[h] = std::move(d);
        st<uint64_t>(c.x[0], h);
        c.x[0] = kOk;
        return;
    }
    std::error_code ec;
    bool found = !p.empty() && fs::is_directory(p, ec);
    note_open("OpenDirectory", cstr(c.x[1]), found ? "ok" : "not found");
    if (trace_fs()) log("fs: OpenDirectory(%s, %d)%s", cstr(c.x[1]).c_str(), mode, found ? "" : " not found");
    if (!found) {
        c.x[0] = kPathNotFound;
        return;
    }
    Dir d;
    for (auto& e : fs::directory_iterator(p, ec)) {
        bool dir = e.is_directory();
        if ((dir && (mode & 1)) || (!dir && (mode & 2))) d.entries.push_back(e);
    }
    uint64_t h = g_next++;
    g_dirs[h] = std::move(d);
    st<uint64_t>(c.x[0], h);
    c.x[0] = kOk;
}

void ReadDirectory(CPU& c) {  // (s64* out_count, DirectoryEntry* entries, DirectoryHandle, s64 max)
    std::lock_guard lk(g_fs);
    auto it = g_dirs.find(c.x[2]);
    if (it == g_dirs.end()) {
        c.x[0] = kTargetNotFound;
        return;
    }
    Dir& d = it->second;
    int64_t n = 0, max = int64_t(c.x[3]);
    for (; d.from_image && n < max && d.pos < d.image.size(); n++, d.pos++) {
        const auto& [name, e] = d.image[d.pos];
        uint64_t out = c.x[1] + uint64_t(n) * 0x310;
        std::memset(reinterpret_cast<void*>(out), 0, 0x310);
        std::memcpy(reinterpret_cast<void*>(out), name.c_str(), std::min<size_t>(name.size(), 0x300));
        st<int8_t>(out + 0x304, e.dir ? 0 : 1);
        st<int64_t>(out + 0x308, e.dir ? 0 : int64_t(e.size));
    }
    for (; !d.from_image && n < max && d.pos < d.entries.size(); n++, d.pos++) {
        const auto& e = d.entries[d.pos];
        uint64_t out = c.x[1] + uint64_t(n) * 0x310;
        std::memset(reinterpret_cast<void*>(out), 0, 0x310);
        auto u8 = e.path().filename().u8string();
        std::string name(u8.begin(), u8.end());
        std::memcpy(reinterpret_cast<void*>(out), name.c_str(), std::min<size_t>(name.size(), 0x300));
        bool dir = e.is_directory();
        st<int8_t>(out + 0x304, dir ? 0 : 1);
        st<int64_t>(out + 0x308, dir ? 0 : int64_t(e.file_size()));
    }
    st<int64_t>(c.x[0], n);
    c.x[0] = kOk;
}

void GetDirectoryEntryCount(CPU& c) {  // (s64* out, DirectoryHandle)
    std::lock_guard lk(g_fs);
    auto it = g_dirs.find(c.x[1]);
    st<int64_t>(c.x[0], it == g_dirs.end() ? 0 : int64_t(it->second.count()));
    c.x[0] = kOk;
}

void CloseDirectory(CPU& c) {
    std::lock_guard lk(g_fs);
    g_dirs.erase(c.x[0]);
}

// ---- cache storage --------------------------------------------------------------------------------------------

void OpenCacheStorageList(CPU& c) {
    st<uint64_t>(c.x[0], 1);
    c.x[0] = kOk;
}
void ReadCacheStorageList(CPU& c) {  // (int* out, CacheStorageInfo*, handle, int)
    st<int32_t>(c.x[0], 0);
    c.x[0] = kOk;
}
void CloseCacheStorageList(CPU& c) {}
void CreateCacheStorage(CPU& c) { c.x[0] = kOk; }
void DeleteCacheStorage(CPU& c) { c.x[0] = kOk; }
void MountCacheStorage(CPU& c) {  // (name, int index)
    std::lock_guard lk(g_fs);
    mount(cstr(c.x[0]), data_root() / "user" / ("cache" + std::to_string(int(c.x[1]))));
    c.x[0] = kOk;
}

// ---- SDK 16 additions (Super Mario Bros. Wonder) ---------------------------------------------------------------
// MountRom(name) without a cache buffer, and ReadFile(size_t* out, FileHandle, s64, void*, size_t, const ReadOption&)
// are the older calls with one argument less / more: the same functions serve them.

void IsSaveDataExisting(CPU& c) { c.x[0] = 1; }  // (const account::Uid&) -> bool: EnsureSaveData made it

void SetFileSize(CPU& c) {  // (FileHandle, s64 size)
    auto file = open_file(c.x[0]);
    if (!file) {
        c.x[0] = kTargetNotFound;
        return;
    }
    std::lock_guard lk(g_fs);
    if (!file->f) {  // the game image is read only
        c.x[0] = kTargetNotFound;
        return;
    }
    std::fflush(file->f);
#if defined(_WIN32)
    bool ok = _chsize_s(_fileno(file->f), int64_t(c.x[1])) == 0;
#else
    bool ok = ftruncate(fileno(file->f), off_t(c.x[1])) == 0;
#endif
    c.x[0] = ok ? kOk : kPathNotFound;
}

void DeleteDirectoryRecursively(CPU& c) {
    std::lock_guard lk(g_fs);
    fs::path p = resolve(cstr(c.x[0]));
    std::error_code ec;
    if (p.empty() || !fs::is_directory(p, ec)) {
        c.x[0] = kPathNotFound;
        return;
    }
    fs::remove_all(p, ec);
    c.x[0] = ec ? kPathNotFound : kOk;
}

}  // namespace

NX_HLE("_ZN2nn2fs22QueryMountRomCacheSizeEPm", QueryMountRomCacheSize);
NX_HLE("_ZN2nn2fs8MountRomEPKcPvm", MountRom);
NX_HLE("_ZN2nn2fs14EnsureSaveDataERKNS_7account3UidE", EnsureSaveData);
NX_HLE("_ZN2nn2fs13MountSaveDataEPKcRKNS_7account3UidE", MountSaveData);
NX_HLE("_ZN2nn2fs7UnmountEPKc", Unmount);
NX_HLE("_ZN2nn2fs10DeleteFileEPKc", DeleteFile);
NX_HLE("_ZN2nn2fs10CreateFileEPKcl", CreateFile);
NX_HLE("_ZN2nn2fs8OpenFileEPNS0_10FileHandleEPKci", OpenFile);
NX_HLE("_ZN2nn2fs11GetFileSizeEPlNS0_10FileHandleE", GetFileSize);
NX_HLE("_ZN2nn2fs15CreateDirectoryEPKc", CreateDirectory);
NX_HLE("_ZN2nn2fs12GetEntryTypeEPNS0_18DirectoryEntryTypeEPKc", GetEntryType);
NX_HLE("_ZN2nn2fs9CloseFileENS0_10FileHandleE", CloseFile);
NX_HLE("_ZN2nn2fs8ReadFileEPmNS0_10FileHandleElPvm", ReadFileOut);
NX_HLE("_ZN2nn2fs8ReadFileENS0_10FileHandleElPvm", ReadFile);
NX_HLE("_ZN2nn2fs9WriteFileENS0_10FileHandleElPKvmRKNS0_11WriteOptionE", WriteFile);
NX_HLE("_ZN2nn2fs9FlushFileENS0_10FileHandleE", FlushFile);
NX_HLE("_ZN2nn2fs25EnableGlobalFileDataCacheEPvm", EnableGlobalFileDataCache);
NX_HLE("_ZN2nn2fs26DisableGlobalFileDataCacheEv", DisableGlobalFileDataCache);
NX_HLE("_ZN2nn2fs6CommitEPKc", Commit);
NX_HLE("_ZN2nn2fs13OpenDirectoryEPNS0_15DirectoryHandleEPKci", OpenDirectory);
NX_HLE("_ZN2nn2fs13ReadDirectoryEPlPNS0_14DirectoryEntryENS0_15DirectoryHandleEl", ReadDirectory);
NX_HLE("_ZN2nn2fs14CloseDirectoryENS0_15DirectoryHandleE", CloseDirectory);
NX_HLE("_ZN2nn2fs22GetDirectoryEntryCountEPlNS0_15DirectoryHandleE", GetDirectoryEntryCount);
NX_HLE("_ZN2nn2fs20OpenCacheStorageListEPNS0_22CacheStorageListHandleE", OpenCacheStorageList);
NX_HLE("_ZN2nn2fs20ReadCacheStorageListEPiPNS0_16CacheStorageInfoENS0_22CacheStorageListHandleEi", ReadCacheStorageList);
NX_HLE("_ZN2nn2fs21CloseCacheStorageListENS0_22CacheStorageListHandleE", CloseCacheStorageList);
NX_HLE("_ZN2nn2fs18CreateCacheStorageEill", CreateCacheStorage);
NX_HLE("_ZN2nn2fs18DeleteCacheStorageEi", DeleteCacheStorage);
NX_HLE("_ZN2nn2fs17MountCacheStorageEPKci", MountCacheStorage);
NX_HLE("_ZN2nn2fs8MountRomEPKc", MountRom);
NX_HLE("_ZN2nn2fs8ReadFileEPmNS0_10FileHandleElPvmRKNS0_10ReadOptionE", ReadFileOut);
NX_HLE("_ZN2nn2fs18IsSaveDataExistingERKNS_7account3UidE", IsSaveDataExisting);
NX_HLE("_ZN2nn2fs11SetFileSizeENS0_10FileHandleEl", SetFileSize);
NX_HLE("_ZN2nn2fs26DeleteDirectoryRecursivelyEPKc", DeleteDirectoryRecursively);

void log_recent_files() {
    std::unique_lock lk(g_recent_lock, std::try_to_lock);
    if (!lk.owns_lock()) return;
    log("  last file opens (newest last; thread = host thread id):");
    for (uint64_t k = 0; k < kRecentOps; k++) {
        const RecentOp& r = g_recent[(g_recent_seq + k) % kRecentOps];
        if (r.seq) log("    #%llu thread %lld  %s", (unsigned long long)r.seq, (long long)r.thread, r.text);
    }
}

void add_virtual_file(const std::string& path, std::string data) {
    std::lock_guard lk(g_fs);
    g_virtual[virtual_key(fs::u8path(path))] = std::move(data);
}

std::string read_game_file(const std::string& relative_to_romfs) {
    {
        std::lock_guard lk(g_fs);
        if (fs::path mp = mod_file(game_image_path(relative_to_romfs)); !mp.empty()) {
            if (FILE* f = plat::fopen_utf8(utf8(mp), "rb")) {
                std::string s;
                char buf[65536];
                size_t n;
                while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) s.append(buf, n);
                std::fclose(f);
                return s;
            }
        }
    }
    if (const nxi::GameImage* img = game_image()) {
        const nxi::GameImage::Entry* e = img->find(game_image_path(relative_to_romfs));
        if (!e || e->dir) return {};
        std::string s(size_t(e->size), '\0');
        s.resize(size_t(img->read(*e, 0, s.data(), e->size)));
        return s;
    }
    FILE* f = plat::fopen_utf8(utf8(data_root() / "extract" / "romfs" / fs::u8path(relative_to_romfs)), "rb");
    if (!f) return {};
    std::string s;
    char buf[65536];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) s.append(buf, n);
    std::fclose(f);
    return s;
}

}  // namespace nx
