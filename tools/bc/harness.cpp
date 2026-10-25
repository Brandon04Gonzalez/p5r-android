#include <cstdio>
#include <cstdlib>
#include <vector>
#include "nvn_bc.h"
int main(int argc, char** argv) {
  unsigned fmt = strtoul(argv[1], 0, 16); unsigned w = atoi(argv[2]), h = atoi(argv[3]);
  FILE* f = fopen(argv[4], "rb"); std::vector<unsigned char> in(1<<24); size_t n = fread(in.data(), 1, in.size(), f); fclose(f);
  std::vector<unsigned char> out(size_t(w) * h * 4);
  nx::nvn::bc_decode(fmt, in.data(), w, h, 1, out.data());
  fwrite(out.data(), 1, size_t(w) * h * nx::nvn::bc_decoded_bytes(fmt), stdout); return 0; }
