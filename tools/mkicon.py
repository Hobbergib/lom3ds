#!/usr/bin/env python3
"""Draws the 48x48 home-menu icon: an original leaf-on-parchment emblem."""
import sys

from PIL import Image, ImageDraw

S = 8  # supersampling factor
img = Image.new("RGB", (48 * S, 48 * S), (0x12, 0x30, 0x20))
d = ImageDraw.Draw(img)
d.rounded_rectangle([2 * S, 2 * S, 46 * S, 46 * S], radius=8 * S, fill=(0x34, 0x20, 0x10))
d.rounded_rectangle([4 * S, 4 * S, 44 * S, 44 * S], radius=6 * S, fill=(0xEB, 0xDB, 0xAE))
d.rounded_rectangle([6 * S, 6 * S, 42 * S, 42 * S], radius=5 * S, outline=(0xC2, 0x9A, 0x3E), width=S)
# Leaf: two arcs meeting at the tips, with a midrib.
d.pieslice([4 * S, 12 * S, 40 * S, 48 * S], 270, 360, fill=(0x2C, 0x7A, 0x46))
d.pieslice([8 * S, 0 * S, 44 * S, 36 * S], 90, 180, fill=(0x2C, 0x7A, 0x46))
d.line([14 * S, 34 * S, 34 * S, 14 * S], fill=(0xEB, 0xDB, 0xAE), width=S)
d.line([12 * S, 36 * S, 16 * S, 32 * S], fill=(0x34, 0x20, 0x10), width=S + S // 2)
img.resize((48, 48), Image.LANCZOS).save(sys.argv[1])
