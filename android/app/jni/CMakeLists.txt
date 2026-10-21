# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 HypseusHW contributors
#
# Native part of the standalone app: SDL3 (shared, full) and the libretro
# frontend (host/lr_host.cpp) as libmain.so.  The core itself is prebuilt.
cmake_minimum_required(VERSION 3.20)
project(daphnelr_app CXX C)

file(TO_CMAKE_PATH "${REPO_ROOT}" REPO_ROOT)

set(SDL_SHARED ON CACHE BOOL "" FORCE)
set(SDL_STATIC OFF CACHE BOOL "" FORCE)
set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
add_subdirectory("${REPO_ROOT}/src/SDL3" sdl3)

add_library(main SHARED "${REPO_ROOT}/host/lr_host.cpp")
set_target_properties(main PROPERTIES CXX_STANDARD 17)
target_include_directories(main PRIVATE "${REPO_ROOT}/src/libretro-common/include")
target_link_libraries(main PRIVATE SDL3::SDL3)
