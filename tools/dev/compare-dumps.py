# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 HypseusHW contributors
"""Compares the frame dumps of two runs (same game, same inputs).

    python tools/dev/compare-dumps.py DIR_A DIR_B

Prints the mean absolute difference (0-255) of each pair of frameNNNNNN.png.
Decoders that differ only in rounding give well under 1; a different frame
gives much more.
"""
import os
import sys

import numpy as np
from PIL import Image

a_dir, b_dir = sys.argv[1], sys.argv[2]
worst = 0.0
for name in sorted(os.listdir(a_dir)):
    if not (name.startswith('frame') and name.endswith('.png')):
        continue
    b_path = os.path.join(b_dir, name)
    if not os.path.exists(b_path):
        continue
    a = np.asarray(Image.open(os.path.join(a_dir, name)).convert('RGB'), dtype=np.int16)
    b = np.asarray(Image.open(b_path).convert('RGB'), dtype=np.int16)
    if a.shape != b.shape:
        print(f'{name}: size differs {a.shape} {b.shape}')
        worst = 255
        continue
    d = float(np.abs(a - b).mean())
    worst = max(worst, d)
    print(f'{name}: {d:.2f}')
print(f'worst {worst:.2f}')
