# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 HypseusHW contributors
"""Side-by-side summary of two sweeps (e.g. hardware vs software decoding).

    python tools/dev/decoder-table.py DIR_A DIR_B game...

Reads each game's log.txt from both sweep directories.
"""
import re
import sys


def parse(path):
    t = open(path, encoding='utf-8', errors='replace').read()
    r = {}
    m = re.search(r'MPEG decoder ([\d.]+) ms', t)
    r['dec'] = float(m.group(1)) if m else None
    m = re.search(r'media codec processes, CPU per frame: ([\d.]+)', t)
    r['codec'] = float(m.group(1)) if m else 0.0
    m = re.search(r'avg retro_run: ([\d.]+) ms\s+worst: ([\d.]+) ms\s+over 16.7 ms: (\d+)', t)
    r['avg'], r['worst'], r['over'] = (float(m.group(1)), float(m.group(2)), int(m.group(3))) if m else (0, 0, 0)
    m = re.search(r'decoder behind: (\d+); frames the engine gave back: (\d+)', t)
    r['late'], r['stall'] = (int(m.group(1)), int(m.group(2))) if m else (0, 0)
    m = re.search(r'Searches and skips: (\d+); target decoded after median ([\d.]+), p90 ([\d.]+), max ([\d.]+) ms; '
                  r'shown after median (\d+)', t)
    r['n'], r['ready'], r['ready90'], r['readymax'], r['shown'] = \
        (int(m.group(1)), float(m.group(2)), float(m.group(3)), float(m.group(4)), int(m.group(5))) if m else \
        (0, 0, 0, 0, 0)
    m = re.search(r'VLDP: decoding \S+ \(\d+x\d+\) with (\S+)', t)
    r['hw'] = bool(m)
    return r


a_dir, b_dir, games = sys.argv[1], sys.argv[2], sys.argv[3:]
print('| Game | Decoder CPU per frame (A / B) | Search decoded, median (A / B) | Search shown, game ms (A / B) '
      '| Late frames (A / B) | Engine gave back (A / B) | Avg / worst frame (A / B) |')
print('|---|---|---|---|---|---|---|')
for g in games:
    a, b = parse(f'{a_dir}/{g}/log.txt'), parse(f'{b_dir}/{g}/log.txt')
    print(f"| {g}{'' if a['hw'] else ' (software in A)'} "
          f"| {a['dec'] + a['codec']:.1f} / {b['dec'] + b['codec']:.1f} "
          f"| {a['ready']:.0f} / {b['ready']:.0f} ms ({a['n']} / {b['n']}) "
          f"| {a['shown']} / {b['shown']} "
          f"| {a['late']} / {b['late']} | {a['stall']} / {b['stall']} "
          f"| {a['avg']:.1f}, {a['worst']:.1f} / {b['avg']:.1f}, {b['worst']:.1f} |")
