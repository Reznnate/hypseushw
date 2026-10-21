#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 HypseusHW contributors
#
# Fetches the pinned third-party sources (git submodules under src/, listed in
# UPSTREAM.md).  Same as: git submodule update --init
set -e
cd "$(dirname "$0")/.."
git submodule sync -q
git submodule update --init --depth 1 2>/dev/null || git submodule update --init
echo "dependencies in src/"
