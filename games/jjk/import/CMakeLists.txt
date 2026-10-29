cmake_minimum_required(VERSION 3.22)
project(jjkinspector LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(IMPORTER_DIR "${CMAKE_CURRENT_LIST_DIR}/../../../android/wonder/importer")
add_library(jjkinspector SHARED
    jjk_inspector_jni.cpp
    "${IMPORTER_DIR}/nximport.cpp"
    "${IMPORTER_DIR}/nxcrypto.cpp")
target_include_directories(jjkinspector PRIVATE "${IMPORTER_DIR}")
target_compile_options(jjkinspector PRIVATE -O2 -fvisibility=hidden -march=armv8-a+crypto)
target_link_libraries(jjkinspector PRIVATE android log)
