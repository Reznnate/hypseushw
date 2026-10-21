#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 HypseusHW contributors
#
# Real-time sweep of Singe games through the Windows core with lr-host.
# Usage: DATA=<Daphne folder> sweep-singe.sh title ...
# Each title is a framefile name (framefile/<title>.txt, as HyperSpin names
# them) whose batch file launches Singe.
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
CORE=${CORE:-$ROOT/build/core-win/hypseushw_libretro.dll}
HOST=${HOST:-$ROOT/build/host-win/lr-host.exe}
: "${DATA:?set DATA to the Daphne folder (the one holding framefile/)}"
OUT=${OUT:-$ROOT/sweep-singe}
FRAMES=${FRAMES:-2400}
PY=${PY:-python3}
[ $# -gt 0 ] || { echo "usage: DATA=<Daphne folder> $0 title ..." >&2; exit 1; }
mkdir -p "$OUT"
printf "%-20s %-6s %-8s %s\n" game status avg_ms "audio rms/sec | first error" > "$OUT/summary.txt"
for g in "$@"; do
  d="$OUT/${g// /_}"; rm -rf "$d"; mkdir -p "$d"
  timeout 600 "$HOST" "$CORE" "$DATA/framefile/$g.txt" --headless --realtime \
      --frames "$FRAMES" --dump "$d" --dump-every 200 --save "$OUT/saves" \
      --press 600:select:8 --press 700:start:8 --press 1000:b:6 --press 1300:up:10 \
      --press 1600:b:6 --press 1900:left:10 --press 2100:b:6 > "$d/log.txt" 2>&1
  rc=$?
  avg=$(grep -o "avg retro_run: [0-9.]*" "$d/log.txt" | awk '{print $3}')
  rms=$(grep "audio rms" "$d/log.txt" | cut -d: -f2 | cut -c1-60)
  err=$(grep -m1 -E "\[ERROR\]|failed|Segmentation" "$d/log.txt" | cut -c1-110)
  printf "%-20s %-6s %-8s %s | %s\n" "$g" "$rc" "$avg" "$rms" "$err" >> "$OUT/summary.txt"
  "$PY" -I "$ROOT/tools/dev/contact.py" "$d" "$d/sheet.png" 6 > /dev/null 2>&1
done
cat "$OUT/summary.txt"
