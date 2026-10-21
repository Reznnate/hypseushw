#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 HypseusHW contributors
#
# shield.sh key1 key2 ...: send Android keyevents over adb, then save a
# screenshot to $SHOT (default ./screen.png).
export MSYS_NO_PATHCONV=1
OUT=${SHOT:-screen.png}
for k in "$@"; do adb shell input keyevent "$k"; sleep "${KEYDELAY:-0.6}"; done
sleep "${SETTLE:-1}"
adb exec-out screencap -p > "$OUT"
