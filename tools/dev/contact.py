# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 HypseusHW contributors
"""Make a labelled contact sheet from frameNNNNNN.png files in a directory."""
import glob
import os
import sys

from PIL import Image, ImageDraw

src = sys.argv[1]
out = sys.argv[2] if len(sys.argv) > 2 else os.path.join(src, "sheet.png")
cols = int(sys.argv[3]) if len(sys.argv) > 3 else 4
tw = 320

files = sorted(glob.glob(os.path.join(src, "frame*.png")))
if not files:
    sys.exit("no frames")

thumbs = []
for f in files:
    im = Image.open(f).convert("RGB")
    th = int(im.height * tw / im.width)
    im = im.resize((tw, th))
    d = ImageDraw.Draw(im)
    label = os.path.basename(f)[5:11].lstrip("0") or "0"
    d.rectangle([0, 0, 52, 14], fill=(0, 0, 0))
    d.text((3, 2), label, fill=(255, 255, 0))
    thumbs.append(im)

th = max(t.height for t in thumbs)
rows = (len(thumbs) + cols - 1) // cols
sheet = Image.new("RGB", (cols * tw, rows * th), (40, 40, 40))
for i, t in enumerate(thumbs):
    sheet.paste(t, ((i % cols) * tw, (i // cols) * th))
sheet.save(out)
print(out, sheet.size)
