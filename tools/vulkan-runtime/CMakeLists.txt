# SPDX-License-Identifier: GPL-3.0-or-later
cmake_minimum_required(VERSION 3.18)
project(prospero_vulkan_runtime LANGUAGES C CXX)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
get_filename_component(PROS_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
set(PROS_LLAMA_SOURCE "${PROS_ROOT}/.deps/llama.cpp" CACHE PATH "Upstream llama.cpp source")
set(PROS_GGML_SOURCE "${PROS_ROOT}/build/ggml-upstream" CACHE PATH "Shared upstream GGML source")
# One upstream GGML target, tensor ABI and backend for every producer and consumer.
add_compile_definitions(GGML_MAX_NAME=160)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(GGML_VULKAN ON CACHE BOOL "" FORCE)
set(GGML_OPENMP OFF CACHE BOOL "" FORCE)
set(GGML_NATIVE OFF CACHE BOOL "" FORCE)
set(LLAMA_BUILD_COMMON OFF CACHE BOOL "" FORCE)
set(LLAMA_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(LLAMA_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
set(LLAMA_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(LLAMA_BUILD_SERVER OFF CACHE BOOL "" FORCE)
set(LLAMA_BUILD_APP OFF CACHE BOOL "" FORCE)
set(LLAMA_CURL OFF CACHE BOOL "" FORCE)
set(LLAMA_OPENSSL OFF CACHE BOOL "" FORCE)
if(NOT EXISTS "${PROS_LLAMA_SOURCE}/CMakeLists.txt")
    message(FATAL_ERROR "Missing pinned upstream llama.cpp source at ${PROS_LLAMA_SOURCE}")
endif()
if(NOT EXISTS "${PROS_GGML_SOURCE}/CMakeLists.txt")
    message(FATAL_ERROR "Missing pinned upstream GGML source at ${PROS_GGML_SOURCE}")
endif()
add_subdirectory("${PROS_GGML_SOURCE}" ggml)
add_subdirectory("${PROS_LLAMA_SOURCE}" llama)
set(SD_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(SD_BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(SD_USE_SYSTEM_GGML ON CACHE BOOL "" FORCE)
set(SD_USE_UPSTREAM_GGML ON CACHE BOOL "" FORCE)
set(SD_GGML_SOURCE_DIR "${PROS_GGML_SOURCE}" CACHE PATH "" FORCE)
set(SD_VULKAN ON CACHE BOOL "" FORCE)
add_subdirectory("${PROS_ROOT}/.deps/stable-diffusion-vulkan" sd)
target_compile_features(stable-diffusion PRIVATE cxx_std_20)
target_compile_options(stable-diffusion PRIVATE
    "$<$<COMPILE_LANGUAGE:CXX>:-include${CMAKE_CURRENT_LIST_DIR}/sd-constant-tls.hpp>")
# The source archive has no .git; do not let its version report the parent app's commit.
set_property(SOURCE "${PROS_ROOT}/.deps/stable-diffusion-vulkan/src/version.cpp"
    DIRECTORY "${PROS_ROOT}/.deps/stable-diffusion-vulkan"
    PROPERTY COMPILE_DEFINITIONS
    SDCPP_BUILD_COMMIT=f89d9b13d730eabeede7314ce49dacd18d3c90c2 SDCPP_BUILD_VERSION=prospero-pinned)
# Speech on the same ggml/Vulkan backend. kokoro.cpp reuses the ggml target added above
# instead of its bundled submodule, and espeak-ng comes from the pinned headers plus the
# prebuilt PS5 archive, because the cross environment has no system copy.
set(KOKORO_BUILD_CLI OFF CACHE BOOL "" FORCE)
set(KOKORO_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(KOKORO_VULKAN ON CACHE BOOL "" FORCE)
set(KOKORO_STATIC ON CACHE BOOL "" FORCE)
set(KOKORO_GGML_DIR "${PROS_GGML_SOURCE}" CACHE PATH "" FORCE)
set(ESPEAK_NG_INCLUDEDIR "${PROS_ROOT}/.deps/espeak-ng/src/include" CACHE PATH "" FORCE)
if(CMAKE_CROSSCOMPILING)
    set(ESPEAK_NG_LIB "${PROS_ROOT}/vendor/lib/libespeak-ng.a" CACHE FILEPATH "" FORCE)
    # Without this, pkg-config finds the host's libespeak-ng-dev and drags /usr/include into
    # the cross compile, where <stdint.h> then resolves to the host glibc header.
    set(CMAKE_DISABLE_FIND_PACKAGE_PkgConfig ON)
    set(ESPEAK_NG_FOUND TRUE)
    add_library(PkgConfig::ESPEAK_NG STATIC IMPORTED)
    set_target_properties(PkgConfig::ESPEAK_NG PROPERTIES
        IMPORTED_LOCATION "${ESPEAK_NG_LIB}"
        INTERFACE_INCLUDE_DIRECTORIES "${ESPEAK_NG_INCLUDEDIR}")
endif()
if(CMAKE_CROSSCOMPILING)
    # The console is one fixed CPU, so Highway needs no runtime dispatch. Compiling only the
    # static target also avoids its AVX-512 code paths, which the x86_64-sie-ps5 target
    # rejects, and AVX2/FMA/F16C match what ggml is already built with here.
    add_compile_definitions(HWY_COMPILE_ONLY_STATIC=1)
    add_compile_options(-mavx2 -mfma -mf16c)
endif()
add_subdirectory("${PROS_ROOT}/.deps/kokoro.cpp" kokoro)

# audio.cpp models use this directory as a child project and reuse the exact
# GGML targets registered above. Its C API and model modules must not build a
# second copy of ggml, which would create a second allocator/backend registry.
set(AUDIOCPP_BUILD_C_API ON CACHE BOOL "Build audio.cpp C API" FORCE)
set(AUDIOCPP_DEPLOYMENT_BUILD ON CACHE BOOL "Embed audio.cpp model specs" FORCE)
set(AUDIOCPP_MODEL_SET custom CACHE STRING "audio.cpp models for ProsperoAI" FORCE)
set(AUDIOCPP_MODELS "stable_audio,ace_step,chatterbox,yue2,sheetsage2,demucs" CACHE STRING "audio.cpp model targets" FORCE)
set(AUDIOCPP_GGML_SOURCE_DIR "${PROS_GGML_SOURCE}" CACHE PATH "Shared upstream GGML" FORCE)
set(AUDIOCPP_BUILD_EXAMPLES OFF CACHE BOOL "Disable audio.cpp examples" FORCE)
set(AUDIOCPP_BUILD_TESTS OFF CACHE BOOL "Disable audio.cpp tests" FORCE)
set(AUDIOCPP_BUILD_EXTENDED_TESTS OFF CACHE BOOL "Disable audio.cpp extended tests" FORCE)
set(AUDIOCPP_BUILD_MODEL_TESTS OFF CACHE BOOL "Disable audio.cpp model tests" FORCE)
set(ENGINE_ENABLE_VULKAN ON CACHE BOOL "Use shared Vulkan backend" FORCE)
set(ENGINE_ENABLE_OPENMP OFF CACHE BOOL "PS5 runtime uses fixed worker counts" FORCE)
set(ENGINE_ENABLE_NATIVE_CPU OFF CACHE BOOL "Use portable CPU kernels" FORCE)
set(SPM_NO_THREADLOCAL ON CACHE BOOL "Avoid sentencepiece TLS on PS5" FORCE)
# The YuE2 sources call ggml_round_bf16, which exists only in the ggml fork
# audio.cpp vendors; declare it for the shared pinned ggml and link the cast
# round-trip definition from tools/yue2-ggml-compat.cpp where the symbol is
# consumed (native eboot objects, media test below). The copy into the build
# tree makes the -include path independent of the checkout layout and fails
# the configure step early when the header is missing.
configure_file("${CMAKE_CURRENT_LIST_DIR}/../yue2-ggml-compat.hpp"
    "${CMAKE_BINARY_DIR}/yue2-ggml-compat.hpp" COPYONLY)
add_compile_options("-include${CMAKE_BINARY_DIR}/yue2-ggml-compat.hpp")
add_subdirectory("${PROS_ROOT}/build/audio-cpp-upstream" audio-cpp)

include("${CMAKE_CURRENT_LIST_DIR}/sd-portability.cmake")
if(NOT CMAKE_CROSSCOMPILING)
    add_executable(vulkan-runtime-abi-test "${PROS_ROOT}/tests/vulkan_runtime_abi_test.cpp")
    target_link_libraries(vulkan-runtime-abi-test PRIVATE llama stable-diffusion ggml-vulkan)
    target_compile_features(vulkan-runtime-abi-test PRIVATE cxx_std_20)
    target_compile_options(vulkan-runtime-abi-test PRIVATE -UNDEBUG
        "-include${CMAKE_CURRENT_LIST_DIR}/sd-constant-tls.hpp")
    add_executable(vulkan-runtime-media-test "${PROS_ROOT}/tests/media_vulkan_test.cpp"
        "${PROS_ROOT}/inference/backends/vulkan/media_bridge.cpp"
        "${PROS_ROOT}/tools/yue2-ggml-compat.cpp")
    target_include_directories(vulkan-runtime-media-test BEFORE PRIVATE
        "${PROS_GGML_SOURCE}/include" "${PROS_ROOT}/include" "${PROS_ROOT}/vendor/include")
    target_compile_options(vulkan-runtime-media-test PRIVATE -UNDEBUG)
    target_link_libraries(vulkan-runtime-media-test PRIVATE ggml-vulkan ggml)
endif()
