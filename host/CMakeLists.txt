# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 HypseusHW contributors
#
# lr-host: desktop libretro test frontend.  Needs the system SDL3 and the
# Vulkan headers (the system's, or SDL3's copy under src/ from fetch-deps.sh);
# the Vulkan loader itself is only opened at run time, with --vulkan.
cmake_minimum_required(VERSION 3.20)
project(lr_host CXX)

set(CMAKE_CXX_STANDARD 17)
get_filename_component(REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)
set(DEPS_LIBRETRO_COMMON_DIR "${REPO_ROOT}/src/libretro-common" CACHE PATH "libretro-common source")
set(DEPS_SDL3_DIR "${REPO_ROOT}/src/SDL3" CACHE PATH "SDL3 source (for its copy of the Vulkan headers)")

find_package(SDL3 CONFIG REQUIRED)

add_executable(lr-host lr_host.cpp)
target_include_directories(lr-host PRIVATE "${DEPS_LIBRETRO_COMMON_DIR}/include")
# SDL3 ships the Khronos headers; used when the system has none
include(CheckIncludeFileCXX)
check_include_file_cxx(vulkan/vulkan.h HAVE_SYSTEM_VULKAN_H)
if (NOT HAVE_SYSTEM_VULKAN_H)
    target_include_directories(lr-host PRIVATE "${DEPS_SDL3_DIR}/src/video/khronos")
endif()
target_link_libraries(lr-host PRIVATE SDL3::SDL3)
