#!/usr/bin/env python3
"""Joins each upper/lower screenshot pair under DIR into one PNG, laid out
like the console: upper screen above, lower screen centred below."""
import pathlib
import sys

from PIL import Image

for top in sorted(pathlib.Path(sys.argv[1]).rglob("shot*_top.ppm")):
    bottom = top.with_name(top.name.replace("_top", "_bot"))
    a, b = Image.open(top), Image.open(bottom)
    out = Image.new("RGB", (a.width, a.height + b.height), (0, 0, 0))
    out.paste(a, (0, 0))
    out.paste(b, ((a.width - b.width) // 2, a.height))
    out.save(top.with_name(top.name.replace("_top.ppm", ".png")))
