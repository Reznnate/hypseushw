# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 HypseusHW contributors
#
# Convenience wrapper around the CMake build in libretro/.
#
#   make                      core for this machine (build/core-<platform>)
#   make platform=android     arm64 Android core (needs NDK=/path/to/ndk)
#   make deps                 fetch the pinned dependencies (git submodules)
#   make clean
#
# Windows builds run in an MSYS2 UCRT64 shell.

CMAKE   ?= cmake
CONFIG  ?= Release
NDK     ?= $(ANDROID_NDK_HOME)

ifeq ($(platform),)
  UNAME := $(shell uname -s 2>/dev/null)
  ifneq ($(filter Windows_NT,$(OS))$(findstring MINGW,$(UNAME))$(findstring MSYS,$(UNAME)),)
    platform = win
  else
    platform = unix
  endif
endif

ifeq ($(platform),android)
  BUILD_DIR  = build/core-android-arm64
  CMAKE_ARGS = -DCMAKE_TOOLCHAIN_FILE=$(NDK)/build/cmake/android.toolchain.cmake \
               -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=24 -DANDROID_STL=c++_static
else
  BUILD_DIR  = build/core-$(platform)
  CMAKE_ARGS =
endif

.PHONY: all deps clean

all: src/libretro-common/include/libretro.h
	$(CMAKE) -S libretro -B $(BUILD_DIR) -G Ninja -DCMAKE_BUILD_TYPE=$(CONFIG) $(CMAKE_ARGS)
	$(CMAKE) --build $(BUILD_DIR)

src/libretro-common/include/libretro.h:
	tools/fetch-deps.sh

deps:
	tools/fetch-deps.sh

clean:
	rm -rf $(BUILD_DIR)
