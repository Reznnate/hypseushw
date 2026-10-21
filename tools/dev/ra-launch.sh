#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 HypseusHW contributors
#
# ra-launch.sh <content path on the device>: start RetroArch on Android with
# the core and content the way a launcher such as HyperSpin does
# (RetroActivityFuture extras).
export MSYS_NO_PATHCONV=1
[ $# -eq 1 ] || { echo "usage: $0 <content path on the device>" >&2; exit 1; }
CORE=${CORE:-/data/user/0/com.retroarch/cores/hypseushw_libretro_android.so}
CONFIG=${CONFIG:-/storage/emulated/0/Android/data/com.retroarch/files/retroarch.cfg}
adb shell am force-stop com.retroarch
adb shell am start -n com.retroarch/com.retroarch.browser.retroactivity.RetroActivityFuture \
  --es ROM "'$1'" --es LIBRETRO "$CORE" --es CONFIGFILE "$CONFIG" | tail -1
