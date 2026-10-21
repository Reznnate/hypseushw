# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 HypseusHW contributors
#
# FFmpeg, reduced to what Singe 2's MP4 video needs: the MP4 and Matroska
# demuxers and the H.264 and AAC decoders, as static libraries (LGPL).
# Built with FFmpeg's own configure, so it needs a POSIX shell and make (on
# Windows, MSYS2's).  Defines the imported target ffmpeg::ffmpeg.

include(ExternalProject)
include(ProcessorCount)

set(FFMPEG_SRC "${DEPS_FFMPEG_DIR}")
set(FFMPEG_BUILD "${CMAKE_CURRENT_BINARY_DIR}/ffmpeg-build")
set(FFMPEG_PREFIX "${CMAKE_CURRENT_BINARY_DIR}/ffmpeg")

find_program(FFMPEG_SH NAMES sh HINTS C:/msys64/usr/bin REQUIRED)
find_program(FFMPEG_MAKE NAMES make gmake HINTS C:/msys64/usr/bin REQUIRED)
get_filename_component(FFMPEG_SH_DIR "${FFMPEG_SH}" DIRECTORY)

if (WIN32)
    set(FF_OS mingw32)
    set(FF_THREADS --enable-w32threads)
elseif (ANDROID)
    set(FF_OS android)
    set(FF_THREADS --enable-pthreads)
else()
    set(FF_OS linux)
    set(FF_THREADS --enable-pthreads)
endif()

if (CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64|amd64")
    set(FF_ARCH x86_64)
elseif (CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64")
    set(FF_ARCH aarch64)
else()
    set(FF_ARCH ${CMAKE_SYSTEM_PROCESSOR})
endif()

# The compiler, with the target and sysroot the toolchain file gave CMake
# (the NDK's clang needs both)
set(FF_CFLAGS "${CMAKE_C_FLAGS} ${CMAKE_C_FLAGS_RELEASE}")
if (CMAKE_C_COMPILER_TARGET)
    string(APPEND FF_CFLAGS " --target=${CMAKE_C_COMPILER_TARGET}")
endif()
if (CMAKE_SYSROOT)
    string(APPEND FF_CFLAGS " --sysroot=${CMAKE_SYSROOT}")
endif()
set(FF_LDFLAGS "")
if (CMAKE_C_COMPILER_TARGET)
    set(FF_LDFLAGS "--target=${CMAKE_C_COMPILER_TARGET}")
endif()

set(FF_ARGS
    --prefix=${FFMPEG_PREFIX}
    --target-os=${FF_OS} --arch=${FF_ARCH}
    --cc=${CMAKE_C_COMPILER} --ar=${CMAKE_AR} --ranlib=${CMAKE_RANLIB} --nm=${CMAKE_NM}
    "--extra-cflags=${FF_CFLAGS}" "--extra-ldflags=${FF_LDFLAGS}"
    --enable-static --disable-shared --enable-pic
    --disable-autodetect ${FF_THREADS}
    --disable-programs --disable-doc --disable-debug --disable-network
    --disable-avdevice --disable-avfilter --disable-swscale --disable-swresample
    --disable-everything
    --enable-protocol=file
    --enable-demuxer=mov,matroska
    --enable-decoder=h264,aac
    --enable-parser=h264,aac)
if (CMAKE_CROSSCOMPILING)
    # configure also wants a compiler for the build machine
    find_program(FFMPEG_HOST_CC NAMES cc gcc clang HINTS C:/msys64/ucrt64/bin REQUIRED)
    list(APPEND FF_ARGS --enable-cross-compile --host-cc=${FFMPEG_HOST_CC})
endif()

# Hand-written x86 assembly needs nasm; without it the decoders are C only
# (slower, still correct)
if (FF_ARCH STREQUAL "x86_64")
    find_program(FFMPEG_NASM nasm)
    if (FFMPEG_NASM)
        list(APPEND FF_ARGS --x86asmexe=${FFMPEG_NASM})
    else()
        message(WARNING "nasm not found: FFmpeg is built without its x86 assembly (slower MP4 decoding)")
        list(APPEND FF_ARGS --disable-x86asm)
    endif()
endif()

ProcessorCount(FF_JOBS)
if (FF_JOBS EQUAL 0)
    set(FF_JOBS 4)
endif()

set(FF_LIBS avformat avcodec avutil)
set(FF_BYPRODUCTS "")
foreach(l ${FF_LIBS})
    list(APPEND FF_BYPRODUCTS "${FFMPEG_PREFIX}/lib/lib${l}.a")
endforeach()

# configure and make run with the shell's own tools first on the PATH
# (MSYS2's on Windows, rather than whichever sed or awk comes first), then
# the compilers' folders (MinGW's gcc finds its assembler and its cc1's DLLs
# on the PATH, so the host compiler's folder goes before the NDK's)
set(FF_TOOL_DIRS "")
foreach(tool ${FFMPEG_HOST_CC} ${CMAKE_C_COMPILER})
    get_filename_component(d "${tool}" DIRECTORY)
    string(APPEND FF_TOOL_DIRS " \"${d}\"")
endforeach()
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/ffmpeg-run.sh"
"p=/usr/bin:/bin
for d in${FF_TOOL_DIRS}; do
    command -v cygpath >/dev/null 2>&1 && d=$(cygpath -u \"$d\")
    p=\"$p:$d\"
done
export PATH=\"$p:$PATH\"
# Windows compilers need a temporary folder, which MSYS2's shell may not pass on
if command -v cygpath >/dev/null 2>&1 && [ -z \"$TMP$TEMP\" ]; then
    TMP=$(cygpath -w /tmp); TEMP=$TMP; export TMP TEMP
fi
exec \"$@\"
")
set(FF_ENV ${FFMPEG_SH} "${CMAKE_CURRENT_BINARY_DIR}/ffmpeg-run.sh")

ExternalProject_Add(ffmpeg_ext
    SOURCE_DIR "${FFMPEG_SRC}"
    BINARY_DIR "${FFMPEG_BUILD}"
    CONFIGURE_COMMAND ${FF_ENV} ${FFMPEG_SH} "${FFMPEG_SRC}/configure" ${FF_ARGS}
    BUILD_COMMAND ${FF_ENV} ${FFMPEG_MAKE} -j${FF_JOBS}
    INSTALL_COMMAND ${FF_ENV} ${FFMPEG_MAKE} install
    BUILD_BYPRODUCTS ${FF_BYPRODUCTS}
    USES_TERMINAL_BUILD ON)

file(MAKE_DIRECTORY "${FFMPEG_PREFIX}/include")
add_library(ffmpeg::ffmpeg INTERFACE IMPORTED)
add_dependencies(ffmpeg::ffmpeg ffmpeg_ext)
set(FF_LINK ${FF_BYPRODUCTS})
if (WIN32)
    list(APPEND FF_LINK bcrypt)
elseif (NOT ANDROID)
    list(APPEND FF_LINK pthread m)
else()
    list(APPEND FF_LINK m)
endif()
set_target_properties(ffmpeg::ffmpeg PROPERTIES
    INTERFACE_INCLUDE_DIRECTORIES "${FFMPEG_PREFIX}/include"
    INTERFACE_LINK_LIBRARIES "${FF_LINK}")
