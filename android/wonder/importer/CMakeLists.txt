# Host build of the importer: the PC test harness (nximport) and the container-parsing test (nximport_test).
#   cmake -S . -B build && cmake --build build && ./build/nximport_test
cmake_minimum_required(VERSION 3.16)
project(nximport CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
add_library(nxi STATIC nximport.cpp nxcrypto.cpp)
target_compile_options(nxi PRIVATE -O2 -Wall -Wextra)
add_executable(nximport nximport_cli.cpp)
target_link_libraries(nximport nxi)
add_executable(nximport_test nximport_test.cpp)
target_link_libraries(nximport_test nxi)
enable_testing()
add_test(NAME nximport_test COMMAND nximport_test)
