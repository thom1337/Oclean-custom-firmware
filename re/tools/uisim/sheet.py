#!/usr/bin/env python3
"""Tile PPM/PNG frames into one labelled contact sheet.  sheet.py out.png frame..."""
import sys, os
from PIL import Image, ImageDraw
out, files = sys.argv[1], sys.argv[2:]
scale, cols = 2, min(12, len(files))
rows = (len(files) + cols - 1) // cols
cw, ch = 80 * scale + 8, 160 * scale + 16
sheet = Image.new('RGB', (cols * cw, rows * ch), (40, 0, 60))
d = ImageDraw.Draw(sheet)
for i, f in enumerate(files):
    im = Image.open(f).convert('RGB').resize((80 * scale, 160 * scale), Image.NEAREST)
    x, y = (i % cols) * cw + 4, (i // cols) * ch + 14
    sheet.paste(im, (x, y))
    d.text((x, y - 12), os.path.splitext(os.path.basename(f))[0], fill=(255, 255, 0))
sheet.save(out)
