#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 HypseusHW contributors
#
# Headless sweep of framefiles through the Windows core with lr-host.
# Usage: DATA=<Daphne folder> sweep-core.sh [game ...]
# Each game's log, frame dumps and a contact sheet land in $OUT/<game>/.
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
CORE=${CORE:-$ROOT/build/core-win/hypseushw_libretro.dll}
HOST=${HOST:-$ROOT/build/host-win/lr-host.exe}
: "${DATA:?set DATA to the Daphne folder (the one holding framefile/ and vldp/)}"
OUT=${OUT:-$ROOT/sweep-core}
FRAMES=${FRAMES:-2400}
PY=${PY:-python3}
if [ $# -eq 0 ]; then
  set -- lair ace astron galaxy badlands bega cobraab roadblaster esh gpworld interstellar mach3 sdq tq uvt
fi
mkdir -p "$OUT"
printf "%-13s %-6s %-8s %s\n" game status avg_ms "audio rms/sec | first error" > "$OUT/summary.txt"
for g in "$@"; do
  d="$OUT/$g"; rm -rf "$d"; mkdir -p "$d"
  timeout 600 "$HOST" "$CORE" "$DATA/framefile/$g.txt" --headless \
      --frames "$FRAMES" --dump "$d" --dump-every 200 --save "$OUT/saves" \
      --press 900:select:8 --press 960:select:8 --press 1100:start:8 \
      --press 1400:b:6 --press 1600:up:10 --press 1800:b:6 > "$d/log.txt" 2>&1
  rc=$?
  avg=$(grep -o "avg retro_run: [0-9.]*" "$d/log.txt" | awk '{print $3}')
  rms=$(grep "audio rms" "$d/log.txt" | cut -d: -f2 | cut -c1-70)
  err=$(grep -m1 -E "\[ERROR\]|failed|Segmentation" "$d/log.txt" | cut -c1-90)
  printf "%-13s %-6s %-8s %s | %s\n" "$g" "$rc" "$avg" "$rms" "$err" >> "$OUT/summary.txt"
  "$PY" -I "$ROOT/tools/dev/contact.py" "$d" "$d/sheet.png" 6 > /dev/null 2>&1
done
cat "$OUT/summary.txt"
