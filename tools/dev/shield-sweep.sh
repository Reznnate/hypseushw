#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 HypseusHW contributors
#
# Headless sweep on an Android device over adb: lr-host-cli and the core read
# the games straight from the device's view of the Daphne folder.
# Usage: DATA=<Daphne folder as the device sees it> shield-sweep.sh [game ...]
export MSYS_NO_PATHCONV=1
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
host_path() { cygpath -w "$1" 2>/dev/null || echo "$1"; }
BUILD=${BUILD:-$ROOT/build/core-android-arm64}
: "${DATA:?set DATA to the Daphne folder as the device sees it, e.g. /storage/<volume>/Daphne}"
OUT=${OUT:-$ROOT/sweep-shield}
FRAMES=${FRAMES:-2400}
REALTIME=${REALTIME---realtime}   # REALTIME= for an unthrottled run
ENVV=${ENVV-DAPHNE_LR_PROFILE=1}  # e.g. ENVV="DAPHNE_LR_PROFILE=1 DAPHNE_LR_VERIFY_GPU=1"
EXTRA=${EXTRA:-}                  # more lr-host-cli arguments, e.g. "--opt daphne_cpu_fallback=enabled"
EVERY=${EVERY:-200}               # dump a frame every EVERY frames
PRESSES=${PRESSES:---press 900:select:8 --press 960:select:8 --press 1100:start:8 --press 1400:b:6 --press 1600:up:10 --press 1800:b:6}
DEV=${DEV:-/data/local/tmp/dl}
PY=${PY:-python3}
if [ $# -eq 0 ]; then
  set -- lair ace astron galaxy badlands bega cobraab roadblaster esh gpworld interstellar mach3 sdq tq uvt
fi

adb shell mkdir -p $DEV/saves $DEV/sys
adb push "$(host_path "$BUILD/lr-host-cli")" $DEV/ > /dev/null
adb push "$(host_path "$BUILD/hypseushw_libretro_android.so")" $DEV/core.so > /dev/null
adb shell chmod 755 $DEV/lr-host-cli

mkdir -p "$OUT"
printf "%-13s %-6s %-46s %s\n" game status timing "per-frame split" > "$OUT/summary.txt"
for g in "$@"; do
  d="$OUT/${g// /_}"; rm -rf "$d"; mkdir -p "$d"
  adb shell "rm -rf $DEV/out; mkdir -p $DEV/out; cd $DEV && $ENVV ./lr-host-cli ./core.so \"$DATA/framefile/$g.txt\" \
      --headless $REALTIME $EXTRA --frames $FRAMES --dump $DEV/out --dump-every $EVERY --save $DEV/saves --system $DEV/sys \
      $PRESSES; echo rc=\$?" > "$d/log.txt" 2>&1
  adb pull $DEV/out/. "$(host_path "$d")" > /dev/null
  rc=$(grep -o 'rc=[0-9]*' "$d/log.txt" | cut -d= -f2)
  t=$(grep -o 'avg retro_run.*' "$d/log.txt" | sed 's/avg retro_run: //' | tr -d '\r')
  split=$(grep 'Per-frame' "$d/log.txt" | grep -o 'engine .*' | tr -d '\r\n')
  printf "%-13s %-6s %-46s %s\n" "$g" "$rc" "$t" "$split" >> "$OUT/summary.txt"
  "$PY" -I "$(host_path "$ROOT/tools/dev/contact.py")" "$(host_path "$d")" "$(host_path "$d/sheet.png")" 6 > /dev/null 2>&1
done
cat "$OUT/summary.txt"
