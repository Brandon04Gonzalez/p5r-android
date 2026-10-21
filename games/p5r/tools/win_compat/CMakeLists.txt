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
