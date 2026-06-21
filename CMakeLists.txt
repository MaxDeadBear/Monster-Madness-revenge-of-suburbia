# monster_madness - ReXGlue Recompiled Project
#
# SDK boilerplate lives in generated/rexglue.cmake.

cmake_minimum_required(VERSION 3.25)
project(monster_madness LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(generated/rexglue.cmake)

# Plume — host render hardware interface (D3D12 + Vulkan) for the native renderer.
add_subdirectory(thirdparty/plume)

# XenosRecomp — translates Xbox 360 Xenos shader microcode -> HLSL -> DXIL/SPIR-V.
# We reuse its recompiler core as a static lib (no main.cpp / exe).
set(XENOS_RECOMP_THIRDPARTY_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/XenosRecomp/thirdparty")
add_subdirectory("${XENOS_RECOMP_THIRDPARTY_ROOT}" xenos_recomp_thirdparty)
set(XR_SRC "${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/XenosRecomp/XenosRecomp")
set(XR_SMOLV "${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/XenosRecomp/thirdparty/smol-v/source")
add_library(xenos_recomp STATIC
    ${XR_SRC}/shader_recompiler.cpp
    ${XR_SRC}/dxc_compiler.cpp
    ${XR_SMOLV}/smolv.cpp
    # MM's translate wrapper compiles HERE (inside the lib): it needs XenosRecomp's
    # pch (DXC headers, be<>) and must avoid MM's WIN32_LEAN_AND_MEAN. Its public
    # header (render/shader_translate.h) exposes only std types to MM.
    ${CMAKE_CURRENT_SOURCE_DIR}/src/render/shader_translate.cpp
    # Precompiled DXIL shader cache (XenosRecomp output) + runtime loader. Built
    # here for the zstd dependency + shadercache/ include.
    ${CMAKE_CURRENT_SOURCE_DIR}/shadercache/mm_shader_cache.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/render/shader_cache_runtime.cpp)
target_include_directories(xenos_recomp PUBLIC ${XR_SRC} ${XR_SMOLV}
    ${CMAKE_CURRENT_SOURCE_DIR}/src ${CMAKE_CURRENT_SOURCE_DIR}/shadercache)
target_link_libraries(xenos_recomp PUBLIC Microsoft::DirectXShaderCompiler xxHash::xxhash libzstd_static fmt::fmt)
target_compile_definitions(xenos_recomp PRIVATE XENOS_RECOMP_DXIL _CRT_SECURE_NO_WARNINGS)
target_compile_options(xenos_recomp PRIVATE -Wno-switch -Wno-unused-variable -Wno-null-arithmetic -fms-extensions)
target_precompile_headers(xenos_recomp PRIVATE ${XR_SRC}/pch.h)
if(WIN32)
    target_link_libraries(xenos_recomp PUBLIC Microsoft::DXIL)
endif()

# Sources
set(MONSTERMADNESS_SOURCES
    src/main.cpp
    src/render/mm_render_hooks.cpp
    src/render/mm_graphics.cpp
    src/render/d3d_hooks.cpp
    src/render/video.cpp
    src/render/detile.cpp
)

if(WIN32)
    add_executable(monster_madness WIN32 ${MONSTERMADNESS_SOURCES})
else()
    add_executable(monster_madness ${MONSTERMADNESS_SOURCES})
endif()

target_compile_definitions(monster_madness PRIVATE
    MONSTER_MADNESS_SOURCE_ROOT="${CMAKE_CURRENT_SOURCE_DIR}"
)

rexglue_setup_target(monster_madness)

# Link the Plume host renderer (its include dirs are PUBLIC -> propagate here).
# Plume's D3D12/Vulkan backends need the platform GPU libs; link them here since
# Plume's own CMake doesn't propagate them to consumers.
target_link_libraries(monster_madness PRIVATE plume xenos_recomp)
if(WIN32)
    target_link_libraries(monster_madness PRIVATE d3d12 dxgi dxguid)
endif()
