# monster_madness - ReXGlue Recompiled Project
#
# SDK boilerplate lives in generated/rexglue.cmake.

cmake_minimum_required(VERSION 3.25)
project(monster_madness LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(generated/rexglue.cmake)

# Sources
set(MONSTERMADNESS_SOURCES
    src/main.cpp
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
