# Windows (MSVC) build of the shared importer, android/wonder/importer, for the P5R port:
#   cmake -S games/p5r/tools/win_compat -B private/build/importer -G Ninja && ninja -C private/build/importer
cmake_minimum_required(VERSION 3.16)
project(nximport_win CXX)
set(CMAKE_CXX_STANDARD 17)
set(IMP "${CMAKE_CURRENT_SOURCE_DIR}/../../../../android/wonder/importer")
add_executable(nximport "${IMP}/nximport_cli.cpp" "${IMP}/nximport.cpp" "${IMP}/nxcrypto.cpp")
target_include_directories(nximport BEFORE PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}" "${IMP}")
target_compile_definitions(nximport PRIVATE _CRT_SECURE_NO_WARNINGS _CRT_NONSTDC_NO_WARNINGS)
target_compile_options(nximport PRIVATE /O2 /EHsc /utf-8)
# The game read in place (nxgame.h), on the PC: nxgame <prod.keys> <x.nsp> [--compare <romfs dir>] [--read <path>]
add_executable(nxgame "${IMP}/nxgame_cli.cpp" "${IMP}/nximport.cpp" "${IMP}/nxcrypto.cpp")
target_include_directories(nxgame BEFORE PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}" "${IMP}")
target_compile_definitions(nxgame PRIVATE _CRT_SECURE_NO_WARNINGS _CRT_NONSTDC_NO_WARNINGS)
target_compile_options(nxgame PRIVATE /O2 /EHsc /utf-8)
# P5R's shader extraction from the game file (games/p5r/import/p5r_shaders.cpp), on the PC
add_executable(p5r_shaders "${CMAKE_CURRENT_SOURCE_DIR}/../../import/shaders_cli.cpp" "${CMAKE_CURRENT_SOURCE_DIR}/../../import/p5r_shaders.cpp"
               "${IMP}/nximport.cpp" "${IMP}/nxcrypto.cpp")
target_include_directories(p5r_shaders BEFORE PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}" "${IMP}" "${CMAKE_CURRENT_SOURCE_DIR}/../../import")
target_compile_definitions(p5r_shaders PRIVATE _CRT_SECURE_NO_WARNINGS _CRT_NONSTDC_NO_WARNINGS)
target_compile_options(p5r_shaders PRIVATE /O2 /EHsc /utf-8)
