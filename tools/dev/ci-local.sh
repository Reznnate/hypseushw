#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 HypseusHW contributors
#
# Runs the libretro buildbot's Linux and Windows jobs locally with Docker, the
# way .gitlab-ci.yml does through libretro's CI templates.
# Usage: ci-local.sh linux|windows [image]
#
# The images are libretro's build containers.  When their registry
# (git.libretro.com:5050) is not reachable, build them from their published
# Dockerfiles and pass the local tag:
#   libretro-infrastructure/libretro-build-amd64-ubuntu, Dockerfile.xenial-gcc9
#   libretro-infrastructure/libretro-build-mxe-win-cross-cores, Dockerfile.gcc16
#     (only the x86_64-w64-mingw32.static target and its cc cmake zlib pkgconf
#     ninja packages are needed)
# The job runs in a copy of the repository (submodules included) under
# $WORK; the core lands in $WORK.
set -eu
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
job=${1:?usage: $0 linux|windows [image]}
REG=git.libretro.com:5050/libretro-infrastructure
case $job in
  linux)
    IMAGE=${2:-$REG/libretro-build-amd64-ubuntu:xenial-gcc9}
    LIBNAME=hypseushw_libretro.so; CMAKE=cmake; BUILD_DIR=build/linux-x86_64 ;;
  windows)
    IMAGE=${2:-$REG/libretro-build-mxe-win-cross-cores:gcc16}
    LIBNAME=hypseushw_libretro.dll; CMAKE=x86_64-w64-mingw32.static-cmake; BUILD_DIR=build/windows-x86_64 ;;
  *) echo "unknown job: $job" >&2; exit 2 ;;
esac
WORK=${WORK:-/tmp/hypseushw-ci-$job}
rm -rf "$WORK"
git clone -q "$ROOT" "$WORK"
git -C "$WORK" submodule update --init --depth 1 -q
docker run --rm -v "$WORK":/build -w /build "$IMAGE" bash -ec "
  $CMAKE -DCMAKE_BUILD_TYPE=Release -DCORE_NAME=hypseushw libretro -B$BUILD_DIR
  $CMAKE --build $BUILD_DIR --target hypseushw_libretro --config Release -- -j \$(nproc)
  mv $BUILD_DIR/$LIBNAME $LIBNAME
  strip --strip-unneeded $LIBNAME
"
ls -l "$WORK/$LIBNAME"
