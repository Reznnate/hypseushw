# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 HypseusHW contributors
#
# Daphne/Hypseus libretro core.
#
# Builds the laserdisc game engine from core/src (vendored hypseus-singe) together
# with the libretro glue in this directory into a single shared library,
# Singe included.  Third-party dependencies are compiled from source (under
# src/, see tools/fetch-deps.sh), except zlib, which comes from the system or
# the NDK.  GCC or Clang: MinGW-w64 (Windows), the Android NDK, Linux.

cmake_minimum_required(VERSION 3.20)
project(daphne_libretro C CXX)

# C++17's std::byte collides with the engine's 'using namespace std' + windows.h
set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
if (NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

get_filename_component(REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)
set(HYP_SRC "${REPO_ROOT}/core/src")

set(DEPS_SDL3_DIR            "${REPO_ROOT}/src/SDL3"            CACHE PATH "SDL3 source")
set(DEPS_OGG_DIR             "${REPO_ROOT}/src/libogg"          CACHE PATH "libogg source")
set(DEPS_VORBIS_DIR          "${REPO_ROOT}/src/libvorbis"       CACHE PATH "libvorbis source")
set(DEPS_LIBRETRO_COMMON_DIR "${REPO_ROOT}/src/libretro-common" CACHE PATH "libretro-common source")
set(DEPS_SDL3_TTF_DIR        "${REPO_ROOT}/src/SDL3_ttf"        CACHE PATH "SDL3_ttf source")
set(DEPS_SDL3_IMAGE_DIR      "${REPO_ROOT}/src/SDL3_image"      CACHE PATH "SDL3_image source")
set(DEPS_FREETYPE_DIR        "${REPO_ROOT}/src/freetype"        CACHE PATH "FreeType source")
# Not "daphne": RetroArch already ships an (older) daphne_libretro core, and
# sharing its file name would let the core updater replace this one.
set(CORE_NAME "hypseushw" CACHE STRING "Core library base name (<name>_libretro)")
option(LR_BUILD_STAMP "Add the build time to the core's version (development builds)" OFF)

# The core's version lives in one place: display_version in the .info file.
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/hypseushw_libretro.info" LR_VERSION_LINE
     REGEX "^display_version *=")
string(REGEX REPLACE "^display_version *= *\"([^\"]*)\".*" "\\1" LR_VERSION "${LR_VERSION_LINE}")
if (NOT LR_VERSION)
    message(FATAL_ERROR "No display_version in hypseushw_libretro.info")
endif()

# --------------------------------------------------------------------------
# SDL3: static, no video/audio/input backends.  Only threads, mutexes,
# software surfaces/blits, BMP/PNG/WAV loading and IO are used by the core.
# --------------------------------------------------------------------------
foreach(opt SHARED TEST_LIBRARY EXAMPLES TESTS VIDEO RENDER GPU AUDIO JOYSTICK
            HAPTIC HIDAPI SENSOR CAMERA POWER DIALOG TRAY)
    set(SDL_${opt} OFF CACHE BOOL "" FORCE)
endforeach()
set(SDL_STATIC ON CACHE BOOL "" FORCE)
set(SDL_STATIC_PIC ON CACHE BOOL "" FORCE)
# no X11/Wayland: SDL otherwise refuses to configure on Linux without them
set(SDL_UNIX_CONSOLE_BUILD ON CACHE BOOL "" FORCE)
add_subdirectory("${DEPS_SDL3_DIR}" sdl3 EXCLUDE_FROM_ALL)

# --------------------------------------------------------------------------
# Singe needs images (PNG, JPEG, GIF, BMP) and TrueType text: SDL3_image with
# its built-in decoders and SDL3_ttf on FreeType, all static, no other deps.
# --------------------------------------------------------------------------
set(BUILD_SHARED_LIBS OFF)
foreach(opt ZLIB BZIP2 PNG HARFBUZZ BROTLI)
    set(FT_DISABLE_${opt} ON CACHE BOOL "" FORCE)
endforeach()
add_subdirectory("${DEPS_FREETYPE_DIR}" freetype EXCLUDE_FROM_ALL)
target_compile_options(freetype PRIVATE -w)

foreach(opt INSTALL SAMPLES TESTS VENDORED DEPS_SHARED PNG_LIBPNG ANI AVIF JXL LBM PCX
            PNM QOI SVG TGA TIF WEBP XCF XPM XV)
    set(SDLIMAGE_${opt} OFF CACHE BOOL "" FORCE)
endforeach()
add_subdirectory("${DEPS_SDL3_IMAGE_DIR}" sdl3_image EXCLUDE_FROM_ALL)

add_library(lr_sdl3_ttf STATIC
    ${DEPS_SDL3_TTF_DIR}/src/SDL_ttf.c
    ${DEPS_SDL3_TTF_DIR}/src/SDL_hashtable.c
    ${DEPS_SDL3_TTF_DIR}/src/SDL_hashtable_ttf.c
    ${DEPS_SDL3_TTF_DIR}/src/SDL_gpu_textengine.c
    ${DEPS_SDL3_TTF_DIR}/src/SDL_renderer_textengine.c
    ${DEPS_SDL3_TTF_DIR}/src/SDL_surface_textengine.c)
target_include_directories(lr_sdl3_ttf PUBLIC "${DEPS_SDL3_TTF_DIR}/include")
target_link_libraries(lr_sdl3_ttf PUBLIC SDL3::SDL3-static PRIVATE freetype)
target_compile_options(lr_sdl3_ttf PRIVATE -w)

# --------------------------------------------------------------------------
# libogg / libvorbis (decoder only)
# --------------------------------------------------------------------------
configure_file("${CMAKE_CURRENT_SOURCE_DIR}/deps/config_types.h"
               "${CMAKE_CURRENT_BINARY_DIR}/ogg/ogg/config_types.h" COPYONLY)
add_library(lr_ogg STATIC
    ${DEPS_OGG_DIR}/src/bitwise.c
    ${DEPS_OGG_DIR}/src/framing.c)
target_include_directories(lr_ogg PUBLIC ${DEPS_OGG_DIR}/include "${CMAKE_CURRENT_BINARY_DIR}/ogg")

set(VORBIS_SOURCES analysis bitrate block codebook envelope floor0 floor1 info
    lookup lpc lsp mapping0 mdct psy registry res0 sharedbook smallft synthesis
    vorbisfile window)
list(TRANSFORM VORBIS_SOURCES PREPEND "${DEPS_VORBIS_DIR}/lib/")
list(TRANSFORM VORBIS_SOURCES APPEND ".c")
add_library(lr_vorbis STATIC ${VORBIS_SOURCES})
target_include_directories(lr_vorbis PUBLIC ${DEPS_VORBIS_DIR}/include PRIVATE ${DEPS_VORBIS_DIR}/lib)
target_link_libraries(lr_vorbis PUBLIC lr_ogg)
target_compile_options(lr_vorbis PRIVATE -w)

# --------------------------------------------------------------------------
# libmpeg2, built straight from the tarball vendored in core/
# --------------------------------------------------------------------------
set(MPEG2_ROOT "${CMAKE_CURRENT_BINARY_DIR}/libmpeg2-master")
if (NOT EXISTS "${MPEG2_ROOT}/libmpeg2/decode.c")
    file(ARCHIVE_EXTRACT INPUT "${HYP_SRC}/3rdparty/libmpeg2/libmpeg2-master.tgz"
         DESTINATION "${CMAKE_CURRENT_BINARY_DIR}")
endif()
set(MPEG2_SOURCES alloc cpu_accel cpu_state decode header idct motion_comp slice)
if (CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64|amd64")
    list(APPEND MPEG2_SOURCES idct_mmx motion_comp_mmx)
    set(MPEG2_ARCH ARCH_X86_64 ACCEL_DETECT)
endif()
list(TRANSFORM MPEG2_SOURCES PREPEND "${MPEG2_ROOT}/libmpeg2/")
list(TRANSFORM MPEG2_SOURCES APPEND ".c")
add_library(lr_mpeg2 STATIC ${MPEG2_SOURCES})
target_include_directories(lr_mpeg2
    PUBLIC  "${MPEG2_ROOT}/include"
    PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/deps/libmpeg2")
target_compile_definitions(lr_mpeg2 PRIVATE ${MPEG2_ARCH})
target_compile_options(lr_mpeg2 PRIVATE -std=gnu89 -w)

# --------------------------------------------------------------------------
# zlib: system/NDK.  Static on Windows, so the DLL needs no zlib1.dll (CMake
# 3.24+ honours this); shared elsewhere: libz.so.1 is part of every Linux
# system and of Android, and distro libz.a is not built for shared objects.
# --------------------------------------------------------------------------
if (WIN32)
    set(ZLIB_USE_STATIC_LIBS ON)
endif()
find_package(ZLIB REQUIRED)

# --------------------------------------------------------------------------
# The core
# --------------------------------------------------------------------------
set(HYP_SOURCES
    cpu/cpu.cpp cpu/mamewrap.cpp cpu/m80.cpp cpu/mc6809.cpp cpu/6809infc.cpp
    cpu/nes6502.cpp cpu/nes_6502.cpp cpu/cop.cpp cpu/copintf.cpp
    cpu/x86/i86.cpp cpu/x86/i86dasm.cpp
    game/singe.cpp
    game/astron.cpp game/badlands.cpp game/bega.cpp game/boardinfo.cpp
    game/cliff.cpp game/cobraconv.cpp game/esh.cpp game/game.cpp
    game/gpworld.cpp game/interstellar.cpp game/lair.cpp game/lair2.cpp
    game/laireuro.cpp game/mach3.cpp game/superd.cpp game/thayers.cpp
    io/cmdline.cpp io/conout.cpp io/error.cpp io/fileparse.cpp io/homedir.cpp
    io/input.cpp io/toolbox.cpp io/mpo_fileio.cpp io/parallel.cpp
    io/keycodes.cpp io/numstr.cpp io/sram.cpp io/unzip.cpp io/serialib.cpp
    ldp-in/ldv1000.cpp ldp-in/pr8210.cpp ldp-in/vip9500sg.cpp ldp-in/ldp1000.cpp
    ldp-in/vp931.cpp ldp-in/pr7820.cpp ldp-in/vp380.cpp ldp-in/vp932.cpp
    ldp-out/ldp.cpp ldp-out/framemod.cpp ldp-out/ldp-vldp.cpp
    ldp-out/ldp-sync.cpp ldp-out/ldp-vldp-audio.cpp
    scoreboard/hw_scoreboard.cpp scoreboard/usb_scoreboard.cpp
    scoreboard/bezel_scoreboard.cpp scoreboard/img_scoreboard.cpp
    scoreboard/null_scoreboard.cpp scoreboard/overlay_scoreboard.cpp
    scoreboard/scoreboard_collection.cpp scoreboard/scoreboard_factory.cpp
    scoreboard/scoreboard_interface.cpp
    sound/sound.cpp sound/ssi263.cpp sound/tqsynth.cpp sound/sn_intf.cpp
    sound/tms9919-sdl.cpp sound/tms9919.cpp sound/pc_beeper.cpp
    sound/gisound.cpp sound/dac.cpp sound/tonegen.cpp sound/samples.cpp
    sound/mix.cpp
    timer/timer.cpp
    video/video.cpp video/tms9128nl.cpp video/led.cpp video/palette.cpp
    vldp/vldp.cpp vldp/vldp_nonstd.cpp vldp/vldp_internal.cpp vldp/mpegscan.cpp
    vldp/vldp_hwdec.cpp vldp/vldp_mp4.cpp
)
if (ANDROID)
    # MediaCodec helpers shared by the MPEG-2 and MP4 decoders
    list(APPEND HYP_SOURCES vldp/vldp_mediacodec.cpp)
endif()
list(TRANSFORM HYP_SOURCES PREPEND "${HYP_SRC}/")

# Singe: Lua 5.1 and its libraries, as singe/CMakeLists.txt builds them
set(SINGE_SOURCES
    singeproxy.cpp lapi.c lcode.c ldo.c lgc.c llex.c
    loadlib.c loslib.c lstate.c ltable.c lundump.c print.c
    lauxlib.c ldblib.c ldump.c linit.c lmathlib.c lobject.c
    lparser.c lstring.c ltablib.c lvm.c random.c lbaselib.c
    luretro.c lstrlib.c ltm.c lzio.c lbit.c
    ldebug.c lfunc.c liolib.c lmem.c lopcodes.c lrandom.c md5.c
    md5lib.c lfs.c singe_utils.cpp sdl3_gfx/SDL3_rotozoom.c
    cjson/strbuf.c cjson/fpconv.c cjson/lua_cjson.c lfszippp.cpp
    lumux.c luars232.c rs232.c lpeg/lpcap.c lpeg/lpcode.c
    lpeg/lpcset.c lpeg/lpprint.c lpeg/lptree.c lpeg/lpvm.c
    socket/auxiliar.c socket/buffer.c socket/compat.c socket/except.c
    socket/inet.c socket/io.c socket/luasocket.c socket/mime.c
    socket/options.c socket/select.c socket/tcp.c socket/timeout.c socket/udp.c
)
if (WIN32)
    list(APPEND SINGE_SOURCES rs232_windows.c socket/wsocket.c)
else()
    list(APPEND SINGE_SOURCES rs232_posix.c socket/serial.c socket/usocket.c
         socket/unix.c socket/unixdgram.c socket/unixstream.c)
endif()
list(TRANSFORM SINGE_SOURCES PREPEND "${HYP_SRC}/game/singe/")
list(APPEND HYP_SOURCES ${SINGE_SOURCES})
set_source_files_properties("${HYP_SRC}/game/singe/luretro.c" PROPERTIES COMPILE_DEFINITIONS ABSTRACT_SINGE)
if (WIN32)
    set_property(SOURCE ${SINGE_SOURCES} APPEND PROPERTY COMPILE_DEFINITIONS RS232_STATIC)
endif()

# pics/ and sound/ from core/ compiled in, unpacked on first use
file(GLOB_RECURSE ASSET_FILES CONFIGURE_DEPENDS "${REPO_ROOT}/core/pics/*" "${REPO_ROOT}/core/sound/*")
add_custom_command(
    OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/lr_assets.c"
    COMMAND ${CMAKE_COMMAND} -DASSET_ROOT=${REPO_ROOT}/core
            -DOUT=${CMAKE_CURRENT_BINARY_DIR}/lr_assets.c
            -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_assets.cmake
    DEPENDS ${ASSET_FILES} ${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_assets.cmake
    COMMENT "Embedding pics/ and sound/")

set(LR_SOURCES
    lr_core.cpp
    lr_gl.cpp
    lr_vk.cpp
    lr_stubs.cpp
    lr_nozip.cpp
    "${CMAKE_CURRENT_BINARY_DIR}/lr_assets.c"
)

add_library(${CORE_NAME}_libretro SHARED ${HYP_SOURCES} ${LR_SOURCES})
set_target_properties(${CORE_NAME}_libretro PROPERTIES PREFIX "")
if (ANDROID)
    # RetroArch for Android names cores <name>_libretro_android.so
    set_target_properties(${CORE_NAME}_libretro PROPERTIES OUTPUT_NAME ${CORE_NAME}_libretro_android)
endif()

configure_file("${CMAKE_CURRENT_SOURCE_DIR}/config.h.in" "${CMAKE_CURRENT_BINARY_DIR}/config.h" @ONLY)

target_include_directories(${CORE_NAME}_libretro PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"               # lr_assets.h, for the generated lr_assets.c
    "${CMAKE_CURRENT_SOURCE_DIR}/shim"          # SDL3_mixer stand-in
    "${CMAKE_CURRENT_BINARY_DIR}"               # config.h
    "${HYP_SRC}"
    "${HYP_SRC}/3rdparty/plog/include"
    "${DEPS_LIBRETRO_COMMON_DIR}/include")

# The Vulkan renderer needs vulkan/vulkan.h: the NDK has it; on MinGW it is an
# optional package (mingw-w64-*-vulkan-headers), so fall back to SDL's copy.
include(CheckIncludeFileCXX)
check_include_file_cxx("vulkan/vulkan.h" LR_HAVE_SYSTEM_VULKAN_H)
if (NOT LR_HAVE_SYSTEM_VULKAN_H)
    target_include_directories(${CORE_NAME}_libretro PRIVATE "${DEPS_SDL3_DIR}/src/video/khronos")
endif()

target_compile_definitions(${CORE_NAME}_libretro PRIVATE
    __LIBRETRO__ SDL_DISABLE_OLD_NAMES)

# Route SDL_GetTicks/SDL_Delay in the engine through the libretro clock.  The
# engine's own unused-code warnings are upstream's business, so only the glue
# in this directory is held to them.
set_source_files_properties(${HYP_SOURCES} PROPERTIES
    COMPILE_OPTIONS "-include;${CMAKE_CURRENT_SOURCE_DIR}/lr_compat.h;-Wno-unused-variable;-Wno-unused-but-set-variable;-Wno-unused-function")
if (ANDROID)
    # Lua keeps string text past the end of its structs, which bionic's
    # fortified strchr() aborts on (as singe/CMakeLists.txt does)
    set_property(SOURCE ${SINGE_SOURCES} APPEND PROPERTY COMPILE_OPTIONS -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=0)
endif()

target_compile_options(${CORE_NAME}_libretro PRIVATE -Wall -fvisibility=hidden
    -ffunction-sections -fdata-sections)

# libco: one implementation file, picked by libco.c for the target arch; our
# own on AArch64 Linux/Android (ELF), which also keeps the callee-saved FP
# registers
if (CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64" AND (ANDROID OR (UNIX AND NOT APPLE)))
    target_sources(${CORE_NAME}_libretro PRIVATE lr_libco_aarch64.c)
else()
    target_sources(${CORE_NAME}_libretro PRIVATE "${DEPS_LIBRETRO_COMMON_DIR}/libco/libco.c")
endif()

target_link_libraries(${CORE_NAME}_libretro PRIVATE
    SDL3::SDL3-static SDL3_image::SDL3_image lr_sdl3_ttf lr_vorbis lr_ogg lr_mpeg2 ZLIB::ZLIB)

if (WIN32)
    # fully static runtime: the DLL must not depend on MinGW DLLs
    # ws2_32: Singe's network module; Media Foundation: MP4 video (Singe 2)
    target_link_libraries(${CORE_NAME}_libretro PRIVATE ws2_32 mfplat mfreadwrite mfuuid ole32)
    target_link_options(${CORE_NAME}_libretro PRIVATE -static -static-libgcc -static-libstdc++
        -Wl,--no-undefined)
elseif (ANDROID)
    # bionic has no FNDELAY (serial scoreboard support)
    target_compile_definitions(${CORE_NAME}_libretro PRIVATE FNDELAY=O_NDELAY)
    target_sources(${CORE_NAME}_libretro PRIVATE android_stubs.c)
    target_link_libraries(${CORE_NAME}_libretro PRIVATE log android mediandk)
    target_link_options(${CORE_NAME}_libretro PRIVATE -Wl,--no-undefined
        "-Wl,--version-script=${CMAKE_CURRENT_SOURCE_DIR}/link.T" -Wl,--gc-sections)

    # lr-host as a plain executable for headless runs from adb shell
    # (build with: ninja lr-host-cli)
    add_executable(lr-host-cli EXCLUDE_FROM_ALL "${REPO_ROOT}/host/lr_host.cpp" android_stubs.c)
    target_compile_definitions(lr-host-cli PRIVATE SDL_MAIN_HANDLED LR_HOST_CLI)
    target_include_directories(lr-host-cli PRIVATE "${DEPS_LIBRETRO_COMMON_DIR}/include")
    target_link_libraries(lr-host-cli PRIVATE SDL3::SDL3-static log android EGL dl)
    set_target_properties(lr-host-cli PROPERTIES CXX_STANDARD 17)
    if (NOT LR_HAVE_SYSTEM_VULKAN_H)
        target_include_directories(lr-host-cli PRIVATE "${DEPS_SDL3_DIR}/src/video/khronos")
    endif()
else()
    target_link_options(${CORE_NAME}_libretro PRIVATE -Wl,--no-undefined
        "-Wl,--version-script=${CMAKE_CURRENT_SOURCE_DIR}/link.T" -Wl,--gc-sections)
endif()
