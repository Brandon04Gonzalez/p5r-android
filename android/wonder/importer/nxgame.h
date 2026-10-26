// The player's NSP or XCI used in place, without extracting it (the P5R port reads its game from it): the base
// program's ExeFS files, and its RomFS decrypted on the fly at each read. The RomFS is not hash-checked here (the
// import checks the dump once); the ExeFS is. Reads are thread safe (pread, no shared buffers). The base game only: an
// update is not applied. Built from the importer's own pieces (nximport.cpp).
#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "nximport.h"

namespace nxi {

class GameImage {
public:
    struct Entry {
        uint64_t off = 0, size = 0;  // a file: its bytes in the RomFS data
        bool dir = false;
    };

    // inputs: the NSP/XCI (or the parts of a split XCI) and prod.keys (title.keys if the dump needs it), as for
    // run_import. title_id 0: the only game found. Null with `error` set on failure (never key material).
    static std::unique_ptr<GameImage> open(const std::vector<InputFile>& inputs, uint64_t title_id, std::string& error);
    ~GameImage();

    uint64_t title_id() const;
    // The ExeFS files ("main", "sdk", "subsdk0", "main.npdm", ...), checked against the NCA's hash table.
    const std::map<std::string, std::vector<uint8_t>>& exefs() const;
    // RomFS paths are '/'-separated with no leading slash ("CPK/ALL_USEU.CPK"); "" is the root. As dumped (case kept).
    const Entry* find(const std::string& path) const;
    std::vector<std::pair<std::string, Entry>> list(const std::string& dir) const;  // a folder's children
    size_t file_count() const;
    // Bytes of a RomFS file from `off`, clipped to its size. The count read (0 past the end or on a read error).
    uint64_t read(const Entry& file, uint64_t off, void* dst, uint64_t n) const;

private:
    GameImage();
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// An NSO (an ExeFS module) as one flat image: text, ro and data at their offsets, each segment checked against its
// SHA-256 (tools/nso2bin.py's main.bin). False with `error` set on failure.
bool nso_to_flat_image(const std::vector<uint8_t>& nso, std::vector<uint8_t>& out, std::string& error);

}  // namespace nxi
