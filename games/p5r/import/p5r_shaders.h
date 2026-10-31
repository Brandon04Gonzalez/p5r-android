// Persona 5 Royal's shaders out of the game image, on the phone: what games/p5r/tools/gsc_shaders.py (with
// tools/maxwell/exefs_shaders.py) does on the PC, in the same layout, for the translator (spirv_harness --batch):
//   <out>/<group>/manifest.tsv, code_<sha1>.bin, ctrl_<sha1>.bin
// groups: every BASE/*.GSC of CPK/ALL_USEU.CPK (their HNSH V002 records), then "exefs" (the blocks built into main).
#pragma once

#include <cstdint>
#include <functional>
#include <string>

#include "nxgame.h"

namespace p5r {

// Distinct code blocks written, or -1 with `error` set. progress(done, total) counts the GSC files and main.
// Generic Switch ExeFS shader extraction, without P5R's CPK/GSC-specific cache.
long extract_exefs_shaders(const nxi::GameImage& game, const std::string& out_dir, std::string& error);

long extract_shaders(const nxi::GameImage& game, const std::string& out_dir,
                     const std::function<void(int, int)>& progress, std::string& error);

}  // namespace p5r
