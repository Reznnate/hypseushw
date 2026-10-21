#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 HypseusHW contributors
#
# Builds hwseek for arm64 Android (needs the core's Android build for libmpeg2).
# Targets API 28, the first with AMediaCodec_getName (the core itself loads
# that call at run time and so targets API 24).
# Run on the device:
#   adb push build/hwseek/hwseek /data/local/tmp/dl/
#   adb shell /data/local/tmp/dl/hwseek '<Daphne>/vldp/lair/lair.m2v' '<Daphne>/vldp/lair/lair.dat'
# NDK= the NDK folder (default: the newest under $ANDROID_HOME/ndk or
# %LOCALAPPDATA%/Android/Sdk/ndk).
set -e
ROOT=$(cd "$(dirname "$0")/../../.." && pwd)
if [ -z "$NDK" ]; then
  for sdk in "$ANDROID_HOME" "$ANDROID_SDK_ROOT" "$LOCALAPPDATA/Android/Sdk"; do
    [ -n "$sdk" ] && [ -d "$sdk/ndk" ] && NDK=$(ls -d "$sdk"/ndk/* | tail -1) && break
  done
fi
[ -n "$NDK" ] || { echo "no NDK found; set NDK=" >&2; exit 1; }
NDK=$(cygpath -u "$NDK" 2>/dev/null || echo "$NDK")
CXX=$(ls "$NDK"/toolchains/llvm/prebuilt/*/bin/aarch64-linux-android28-clang++* 2>/dev/null | head -1)
[ -n "$CXX" ] || { echo "no aarch64-linux-android28-clang++ in $NDK" >&2; exit 1; }
BUILD=$ROOT/build/core-android-arm64
mkdir -p "$ROOT/build/hwseek"
"$CXX" -O2 -std=c++17 "$ROOT/tools/dev/hwseek/hwseek.cpp" -I"$BUILD/libmpeg2-master/include" \
    "$BUILD/liblr_mpeg2.a" -lmediandk -landroid -static-libstdc++ -o "$ROOT/build/hwseek/hwseek"
echo "built $ROOT/build/hwseek/hwseek"
