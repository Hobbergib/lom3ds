#!/usr/bin/env python3
"""Tiles the upper-screen screenshots under DIR into one contact sheet,
DIR/sheet.png, three across, for a quick look at a scripted run."""
import pathlib
import sys

from PIL import Image

tops = sorted(pathlib.Path(sys.argv[1]).glob("shot*_top.ppm"))
cols = 3
sheet = Image.new("RGB", (400 * cols, 240 * ((len(tops) + cols - 1) // cols)))
for i, path in enumerate(tops):
    sheet.paste(Image.open(path), (400 * (i % cols), 240 * (i // cols)))
sheet.save(pathlib.Path(sys.argv[1]) / "sheet.png")
