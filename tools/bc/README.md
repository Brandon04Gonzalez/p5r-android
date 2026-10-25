# BC decoder checks (runtime/src/nvn/nvn_bc.cpp)

- `derive.py` derives the BC7 partition and anchor tables from Pillow's decoder. It builds blocks whose subsets
  decode to distinct colours (DEBUG-2.md #85); the output is `tables.json`, the source of the tables in
  `nvn_bc.cpp`.
- `compare.py` checks `nvn_bc.cpp` against Pillow on random blocks for BC1, BC2, BC3, BC4, BC5 and BC7, all BC7 modes
  included. It needs the `harness` binary:
  - put a stub `nvn_vk.h` next to a copy of `nvn_bc.cpp`; the stub declares
    `const VkPhysicalDeviceFeatures& vk_features()`;
  - build: `clang++ -std=c++20 -O2 -I<stub dir> -I/opt/homebrew/include harness.cpp <stub dir>/nvn_bc.cpp -o harness`.

  Last result: a maximum difference of 0 on every format.
