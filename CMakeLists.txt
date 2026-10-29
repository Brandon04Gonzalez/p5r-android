cmake_minimum_required(VERSION 3.25)
project(SwitchGamesRecomp C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# The game this build is for: games/<NX_GAME>/ holds what only that game needs (game.cpp: nx/game.h), the shared
# runtime is the rest. One build directory per game.
set(NX_GAME "p5r" CACHE STRING "Game to build (a folder under games/)")
set(NX_GAME_DIR "${CMAKE_SOURCE_DIR}/games/${NX_GAME}")
if(NOT EXISTS "${NX_GAME_DIR}/game.cpp")
    message(FATAL_ERROR "games/${NX_GAME}/game.cpp not found")
endif()

# Generated code of the game (python -m recomp <nso> <gen dir>/<module> --name <module> --base ...). Never committed.
# Hades keeps the layout from before there were other games (private/gen); the others use private/<game>/gen.
if(NX_GAME STREQUAL "hades")
    set(NX_GEN_DEFAULT "${CMAKE_SOURCE_DIR}/private/gen")
else()
    set(NX_GEN_DEFAULT "${CMAKE_SOURCE_DIR}/private/${NX_GAME}/gen")
endif()
set(NX_GEN_DIR "${NX_GEN_DEFAULT}" CACHE PATH "Recompiled module sources")

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    add_compile_options(-Wno-unused-label -Wno-unused-value -Wno-parentheses-equality -Wno-unused-variable
                        -Wno-unused-but-set-variable -Wno-constant-logical-operand -Wno-unused-function)
endif()
if(MSVC)
    add_compile_options(/arch:AVX2)
endif()
if(WIN32)
    add_compile_definitions(NOMINMAX WIN32_LEAN_AND_MEAN)
endif()

# Unit tests of runtime pieces that build without the game (docs/nvn/WINDOWS-PLAN.md, "Test plan"): ctest.
option(NX_TESTS "Build the standalone unit tests" OFF)
# The public build: no log file and no file-triggered diagnostics (runtime/src/platform.h kDiagnostics).
option(NX_RELEASE "Public build: no nx.log file, no file-triggered diagnostics" OFF)
if(NX_TESTS AND NOT ANDROID)
    enable_testing()
    add_executable(gpu_pick_test tests/gpu_pick_test.cpp runtime/src/nvn/nvn_gpu_pick.cpp)
    target_compile_definitions(gpu_pick_test PRIVATE NX_GPU_PICK_STANDALONE)
    target_include_directories(gpu_pick_test PRIVATE runtime/src/nvn)
    add_test(NAME gpu_pick COMMAND gpu_pick_test)
    add_executable(tiling_test tests/tiling_test.cpp runtime/src/nvn/nvn_tiling.cpp)
    target_include_directories(tiling_test PRIVATE runtime/src/nvn)
    add_test(NAME tiling COMMAND tiling_test --selftest)
    add_executable(astc_test tests/astc_test.cpp runtime/src/nvn/nvn_astc.cpp)
    target_include_directories(astc_test PRIVATE runtime/src/nvn)
    add_test(NAME astc COMMAND astc_test --selftest)
    add_executable(shader_cache_test tests/shader_cache_test.cpp runtime/src/nvn/nvn_shaders.cpp
                   runtime/src/nvn/nvn_spirv_reflect.cpp)
    target_compile_definitions(shader_cache_test PRIVATE NX_SHADERS_STANDALONE)
    target_include_directories(shader_cache_test PRIVATE runtime/src/nvn)
    if(EXISTS "${CMAKE_SOURCE_DIR}/private/wonder/spirv")  # the game's own shaders: never in the repository
        add_test(NAME shader_cache COMMAND shader_cache_test "${CMAKE_SOURCE_DIR}/private/wonder")
    endif()
    if(WIN32)
        add_executable(memguard_test tests/memguard_test.cpp)
        target_link_libraries(memguard_test PRIVATE psapi)
        add_test(NAME memguard COMMAND memguard_test)
    endif()
endif()

# Native execution (runtime/src/nce): the game's own AArch64 code from the player's exefs runs on the host CPU, so the
# build holds no game code at all (Android and other AArch64 hosts). Without it, the recompiled modules are linked in.
option(NX_NCE "Run the game's own code from the player's exefs natively (AArch64 hosts): no recompiled code" OFF)
if(NX_NCE)
    if(NOT (ANDROID OR CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64|ARM64"))
        message(FATAL_ERROR "NX_NCE needs an AArch64 host (Android arm64-v8a)")
    endif()
    set(NX_GAME_LIBS "")
else()
    set(NX_GAME_LIBS game_main game_sdk)
endif()

# Without the generated code (a checkout with no game data) only the tests above are built.
file(GLOB NX_GEN_PROBE "${NX_GEN_DIR}/main/*.cpp")
if(NOT NX_GEN_PROBE AND NOT NX_NCE)
    message(WARNING "No generated code in ${NX_GEN_DIR}/main: building the unit tests only")
    return()
endif()

if(NOT NX_NCE)
foreach(mod main sdk)
    file(GLOB src CONFIGURE_DEPENDS "${NX_GEN_DIR}/${mod}/*.cpp")
    add_library(game_${mod} STATIC ${src})
    target_include_directories(game_${mod} PRIVATE runtime/include "${NX_GEN_DIR}/${mod}")
    if(ANDROID)
        # Hidden visibility: calls between recompiled functions are direct, not through the PLT.
        # armv8-a+crypto: cpu.h's AES/SHA paths need __ARM_FEATURE_AES/SHA2 (the NDK default armv8-a has neither).
        # Not armv8.2-a: Cortex-A53/A73 phones (Snapdragon 685) are ARMv8.0 and its LSE atomics were SIGILL; the
        # compiler's outline atomics still use LSE where the CPU has it.
        target_compile_options(game_${mod} PRIVATE -O2 -fPIC -fvisibility=hidden -fno-semantic-interposition
                               -march=armv8-a+crypto)
    elseif(APPLE)
        target_compile_options(game_${mod} PRIVATE -O2 -fvisibility=hidden)
    elseif(UNIX)
        # Linux x86-64: AVX2/FMA/BMI as the Windows build (/arch:AVX2); no FMA contraction, so a guest fmul + fadd
        # rounds twice, as on the Switch.
        target_compile_options(game_${mod} PRIVATE -O2 -fvisibility=hidden -march=x86-64-v3 -ffp-contract=off)
    endif()
endforeach()
endif()

set(NX_RUNTIME_SOURCES
    runtime/src/main.cpp
    runtime/src/sampler.cpp
    runtime/src/loader.cpp
    runtime/src/memory.cpp
    runtime/src/kernel.cpp
    runtime/src/kernel_watch.cpp
    runtime/src/ipc.cpp
    runtime/src/svc_am.cpp
    runtime/src/svc_set.cpp
    runtime/src/svc_misc.cpp
    runtime/src/svc_time.cpp
    runtime/src/svc_process.cpp
    runtime/src/hle_fs.cpp
    runtime/src/svc_account.cpp
    runtime/src/users.cpp
    runtime/src/hle_ro.cpp
    runtime/src/hle_vi.cpp
    runtime/src/hle_hid.cpp
    runtime/src/hle_net.cpp
    runtime/src/hle_audio.cpp
    runtime/src/hle_pl.cpp
    runtime/src/hle_fiber.cpp
    runtime/src/hle_movie.cpp
    runtime/src/input_script.cpp
    runtime/src/touch_layout.cpp
    runtime/src/perf_hint.cpp
    runtime/src/frame_stats.cpp
    runtime/src/write_watch.cpp
    runtime/src/nvn/nvn_caps.cpp
    runtime/src/nvn/nvn_renderpass.cpp
    runtime/src/nvn/nvn_gpu_busy.cpp
    runtime/src/nvn/nvn_gpu_probe.cpp
    runtime/src/nvn/nvn_janitor.cpp
    runtime/src/nvn/nvn_trace.cpp
    runtime/src/nvn/nvn_device.cpp
    runtime/src/nvn/nvn_objects.cpp
    runtime/src/nvn/nvn_incarnation.cpp
    runtime/src/nvn/nvn_texture_layout.cpp
    runtime/src/nvn/nvn_program.cpp
    runtime/src/nvn/nvn_queue.cpp
    runtime/src/nvn/nvn_cmd.cpp
    runtime/src/nvn/nvn_state.cpp
    runtime/src/nvn/nvn_format.cpp
    runtime/src/nvn/nvn_enums.cpp
    runtime/src/nvn/nvn_sampler.cpp
    runtime/src/nvn/nvn_sampler_objects.cpp
    runtime/src/nvn/nvn_draw_state.cpp
    runtime/src/nvn/nvn_occlusion.cpp
    runtime/src/nvn/nvn_raster.cpp
    runtime/src/nvn/nvn_worker.cpp
    runtime/src/nvn/nvn_shaders.cpp
    runtime/src/nvn/nvn_spirv_reflect.cpp
    runtime/src/nvn/nvn_tiling.cpp
    runtime/src/nvn/nvn_vk.cpp
    runtime/src/nvn/nvn_gpu_pick.cpp
    runtime/src/nvn/nvn_image.cpp
    runtime/src/nvn/nvn_frames.cpp
    runtime/src/nvn/nvn_dedup.cpp
    runtime/src/nvn/nvn_buffer_cache.cpp
    runtime/src/nvn/nvn_written.cpp
    runtime/src/nvn/nvn_replay.cpp
    runtime/src/nvn/nvn_debug.cpp
    runtime/src/nvn/nvn_binds.cpp
    runtime/src/nvn/nvn_copy.cpp
    runtime/src/nvn/nvn_pipeline.cpp
    runtime/src/nvn/nvn_dummy.cpp
    runtime/src/nvn/nvn_pipeline_cache.cpp
    runtime/src/nvn/nvn_upload.cpp
    runtime/src/nvn/nvn_astc.cpp
    runtime/src/nvn/nvn_spirv_patch.cpp
    runtime/src/nvn/nvn_spirv_clip.cpp
    runtime/src/nvn/nvn_ultrawide.cpp
    runtime/src/nvn/nvn_present.cpp
    runtime/src/nvn/nvn_pipeline_cache.cpp
    runtime/src/nvn/nvn_bc.cpp
    runtime/src/nvn/nvn_draw.cpp
    runtime/src/nvn/nvn_sets.cpp
    runtime/src/nvn/nvn_clears.cpp
    runtime/src/nvn/nvn_mask_clear.cpp
    runtime/src/nvn/nvn_draw_trace.cpp
    runtime/src/vk_bridge.cpp
    runtime/src/vk_swapchain.cpp
    runtime/src/vk_gen.cpp
    runtime/src/vk_present.cpp
)
file(GLOB NX_GAME_SOURCES CONFIGURE_DEPENDS "${NX_GAME_DIR}/*.cpp")
list(APPEND NX_RUNTIME_SOURCES ${NX_GAME_SOURCES})
# The game read in place from the player's NSP/XCI (runtime/src/game_image.cpp): the importer's NCA/RomFS reader.
set(NX_IMPORTER_DIR "${CMAKE_SOURCE_DIR}/android/wonder/importer")
list(APPEND NX_RUNTIME_SOURCES runtime/src/game_image.cpp "${NX_IMPORTER_DIR}/nximport.cpp" "${NX_IMPORTER_DIR}/nxcrypto.cpp")
if(WIN32)  # its POSIX calls, on Windows (only these two files see the shim)
    set_source_files_properties("${NX_IMPORTER_DIR}/nximport.cpp" "${NX_IMPORTER_DIR}/nxcrypto.cpp" PROPERTIES
        INCLUDE_DIRECTORIES "${CMAKE_SOURCE_DIR}/games/p5r/tools/win_compat"
        COMPILE_DEFINITIONS "_CRT_NONSTDC_NO_WARNINGS")
endif()
if(NX_NCE)
    list(APPEND NX_RUNTIME_SOURCES runtime/src/nce/nce.cpp runtime/src/nce/nso.cpp)
endif()

# Vulkan headers (the same registry version tools/gen_vk.py generated from). The loader comes with the GPU driver.
if(DEFINED ENV{VULKAN_SDK})
    set(NX_VULKAN_SDK "$ENV{VULKAN_SDK}" CACHE PATH "Vulkan SDK")
else()
    set(NX_VULKAN_SDK "C:/VulkanSDK/1.4.357.0" CACHE PATH "Vulkan SDK")
endif()

if(ANDROID)
    # NativeActivity app: lib<game>.so, entry android_main (android_native_app_glue).
    set(GLUE "${ANDROID_NDK}/sources/android/native_app_glue")
    add_library(${NX_GAME} SHARED ${NX_RUNTIME_SOURCES} runtime/src/platform_android.cpp
        runtime/src/platform_android_input.cpp runtime/src/platform_android_audio.cpp runtime/src/memguard_android.cpp
        "${GLUE}/android_native_app_glue.c")
    target_include_directories(${NX_GAME} PRIVATE runtime/include runtime/src "${NX_IMPORTER_DIR}" "${NX_GAME_DIR}" "${NX_VULKAN_SDK}/Include" "${GLUE}")
    # Our ANativeActivity_onCreate (platform_android.cpp) runs first, on the UI thread, then calls the glue's.
    set_source_files_properties("${GLUE}/android_native_app_glue.c" PROPERTIES
        COMPILE_DEFINITIONS "ANativeActivity_onCreate=glue_ANativeActivity_onCreate")
    target_link_libraries(${NX_GAME} PRIVATE ${NX_GAME_LIBS} android log aaudio mediandk)
    target_compile_options(${NX_GAME} PRIVATE -fvisibility=hidden -fno-semantic-interposition -march=armv8-a+crypto)
    # The glue's ANativeActivity_onCreate must be exported. 16 KB page alignment: Android 15+ devices with 16 KB pages.
    target_link_options(${NX_GAME} PRIVATE -u ANativeActivity_onCreate -Wl,-z,max-page-size=16384)
elseif(APPLE)
    # macOS: SDL3 (window, input, audio) and the Vulkan loader + MoltenVK from Homebrew
    # (brew install sdl3 vulkan-headers vulkan-loader molten-vk).
    find_package(SDL3 REQUIRED CONFIG)
    find_path(NX_VULKAN_INCLUDE vulkan/vulkan.h HINTS /opt/homebrew/include /usr/local/include)
    add_executable(${NX_GAME} ${NX_RUNTIME_SOURCES} runtime/src/platform_mac.cpp runtime/src/memguard_mac.cpp)
    target_include_directories(${NX_GAME} PRIVATE runtime/include runtime/src "${NX_IMPORTER_DIR}" "${NX_GAME_DIR}" "${NX_VULKAN_INCLUDE}")
    target_link_libraries(${NX_GAME} PRIVATE ${NX_GAME_LIBS} SDL3::SDL3)
    target_compile_options(${NX_GAME} PRIVATE -fvisibility=hidden)
    # The executable loads near 0x1_0000_0000, below the guest ranges (0x40_0000_0000 and up). The game runs on its
    # own thread with a 64 MiB stack (platform_mac.cpp), so the main thread's stack size does not matter.
elseif(UNIX)
    # Linux: SDL3 (window, input, audio) and the system Vulkan loader, as on macOS. SDL3 from the distribution or a
    # local build (-DCMAKE_PREFIX_PATH=<its install prefix>).
    find_package(SDL3 REQUIRED CONFIG)
    find_package(Threads REQUIRED)
    find_path(NX_VULKAN_INCLUDE vulkan/vulkan.h HINTS "${NX_VULKAN_SDK}/include" /usr/include /usr/local/include)
    add_executable(${NX_GAME} ${NX_RUNTIME_SOURCES} runtime/src/platform_linux.cpp runtime/src/memguard_linux.cpp)
    target_include_directories(${NX_GAME} PRIVATE runtime/include runtime/src "${NX_IMPORTER_DIR}" "${NX_GAME_DIR}" "${NX_VULKAN_INCLUDE}")
    target_link_libraries(${NX_GAME} PRIVATE ${NX_GAME_LIBS} SDL3::SDL3 Threads::Threads ${CMAKE_DL_LIBS})
    target_compile_options(${NX_GAME} PRIVATE -fvisibility=hidden -march=x86-64-v3 -ffp-contract=off)
    # The executable (PIE) loads near 0x55_5555_0000_0000, far above the guest ranges (0x40_0000_0000 and up). The
    # game runs on its own thread with a 64 MiB stack (platform_linux.cpp).
else()
    add_executable(${NX_GAME} ${NX_RUNTIME_SOURCES} runtime/src/platform_win.cpp runtime/src/memguard_win.cpp)
    target_include_directories(${NX_GAME} PRIVATE runtime/include runtime/src "${NX_IMPORTER_DIR}" "${NX_GAME_DIR}" "${NX_VULKAN_SDK}/Include")
    target_link_libraries(${NX_GAME} PRIVATE ${NX_GAME_LIBS} xinput xaudio2 ole32 psapi)
    # The runtime uses the portable C library (getenv, fopen, ...) on every host: no "_s" variants.
    target_compile_definitions(${NX_GAME} PRIVATE _CRT_SECURE_NO_WARNINGS _SILENCE_CXX20_U8PATH_DEPRECATION_WARNING)
    if(MSVC)
        # Keep host allocations low so the guest ranges (0x40_0000_0000..0x70_0000_0000) are always free; a big
        # main-thread stack (recompiled calls nest deeply); a link map for tools/symbolize.py.
        target_link_options(${NX_GAME} PRIVATE /STACK:67108864 /HIGHENTROPYVA:NO /MAP)
    endif()
endif()

# P5R's import on the phone (games/p5r/import, ImportActivity): the game file checked and its shaders extracted;
# the translation itself is libspirvharness.so (games/p5r/tools/build_harness_android.sh).
if(ANDROID AND NX_GAME STREQUAL "p5r")
    add_library(p5rimport SHARED games/p5r/import/p5r_import_jni.cpp games/p5r/import/p5r_shaders.cpp
        "${NX_IMPORTER_DIR}/nximport.cpp" "${NX_IMPORTER_DIR}/nxcrypto.cpp")
    target_include_directories(p5rimport PRIVATE "${NX_IMPORTER_DIR}" games/p5r/import)
    target_compile_options(p5rimport PRIVATE -O2 -fvisibility=hidden -march=armv8-a+crypto)
    target_link_libraries(p5rimport PRIVATE log dl)
    target_link_options(p5rimport PRIVATE -Wl,-z,max-page-size=16384)
endif()

# JJK's on-device import uses the shared NSP/XCI importer and writes raw ExeFS modules for NCE.
if(ANDROID AND NX_GAME STREQUAL "jjk")
    add_library(jjkimport SHARED games/jjk/import/jjk_import_jni.cpp
        "${NX_IMPORTER_DIR}/nximport.cpp" "${NX_IMPORTER_DIR}/nxcrypto.cpp")
    target_include_directories(jjkimport PRIVATE "${NX_IMPORTER_DIR}")
    target_compile_options(jjkimport PRIVATE -O2 -fvisibility=hidden -march=armv8-a+crypto)
    target_link_libraries(jjkimport PRIVATE log)
    target_link_options(jjkimport PRIVATE -Wl,-z,max-page-size=16384)
endif()

# NX_RELEASE: the runtime and the game's glue only (the recompiled game code does not change with it).
if(NX_RELEASE)
    target_compile_definitions(${NX_GAME} PRIVATE NX_RELEASE)
endif()
if(NX_NCE)
    target_compile_definitions(${NX_GAME} PRIVATE NX_NCE=1)
endif()
