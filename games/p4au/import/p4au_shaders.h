#pragma once

#include <cstdint>
#include <functional>
#include <string>

#include "nxgame.h"

namespace p4au {

// Extracts shaders from data/shader/*.bnsh in RomFS into out_dir (manifest.tsv, code_*.bin, ctrl_*.bin).
// Returns number of distinct code blocks, or -1 on error.
long extract_shaders(const nxi::GameImage& game, const std::string& out_dir,
                     const std::function<void(int, int)>& progress, std::string& error);

}  // namespace p4au
