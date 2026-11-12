#!/usr/bin/env python3
"""Draws the port's own button icons, one per 3DS control, and writes them as
C data (src/port/button_icons.inc) and as a preview sheet.

They stand in for the PlayStation button symbols in the game's graphics.
Each is shaped like the control it names: round face buttons, shoulder
buttons with a curved top, small pills for START and SELECT, a cross for the
+Control Pad, a dished disc for the Circle Pad and a small nub for the
C-Stick. All share one grey scheme with a white legend, so they read on the
game's light and dark panels alike and survive being matched to a picture's
palette. Everything here is original artwork made of letters and simple
shapes.

Usage: mkbuttons.py OUT.inc PREVIEW.png
"""
import sys

from PIL import Image

SIZE = 12  # height of every icon; narrower shapes are centred in it
RIM = (0x3C, 0x3E, 0x46)
BODY_TOP, BODY_BOTTOM = (0x9A, 0x9C, 0xA6), (0x7E, 0x80, 0x8C)
LEGEND = (0xFF, 0xFF, 0xFF)
DISH = (0x62, 0x64, 0x70)

# The legends are drawn in the port's own lettering: thin, upright and
# without serifs, like the letters printed on the console's buttons.
LETTERS = {
    "A": [".###.", "#...#", "#...#", "#####", "#...#", "#...#", "#...#"],
    "B": ["####.", "#...#", "#...#", "####.", "#...#", "#...#", "####."],
    "X": ["#...#", "#...#", ".#.#.", "..#..", ".#.#.", "#...#", "#...#"],
    "Y": ["#...#", "#...#", ".#.#.", "..#..", "..#..", "..#..", "..#.."],
    "L": ["#....", "#....", "#....", "#....", "#....", "#....", "#####"],
    "R": ["####.", "#...#", "#...#", "####.", "#.#..", "#..#.", "#...#"],
    "Z": ["#####", "....#", "...#.", "..#..", ".#...", "#....", "#####"],
}

# START and SELECT are too long for those; they use this 3x5 lettering.
TINY = {
    "S": ["###", "#..", "###", "..#", "###"], "T": ["###", ".#.", ".#.", ".#.", ".#."],
    "A": [".#.", "#.#", "###", "#.#", "#.#"], "R": ["##.", "#.#", "##.", "#.#", "#.#"],
    "E": ["###", "#..", "##.", "#..", "###"], "L": ["#..", "#..", "#..", "#..", "###"],
    "C": ["###", "#..", "#..", "#..", "###"],
}

# name, legend, shape
BUTTONS = [
    ("A", "A", "face"), ("B", "B", "face"), ("X", "X", "face"), ("Y", "Y", "face"),
    ("L", "L", "shoulder"), ("R", "R", "shoulder"), ("ZL", "ZL", "shoulder"), ("ZR", "ZR", "shoulder"),
    ("START", "START", "pill"), ("SELECT", "SELECT", "pill"),
    ("DPAD", "", "cross"), ("CPAD", "", "pad"), ("CSTICK", "", "nub"),
]


def rows_of(pattern):
    return [[c == "#" for c in row] for row in pattern]


def disc(width, height, diameter):
    """Mask of a disc centred in a width x height canvas."""
    r = diameter / 2
    return [[(x + 0.5 - width / 2) ** 2 + (y + 0.5 - height / 2) ** 2 <= r * r for x in range(width)]
            for y in range(height)]


def rounded(width, height, top_radius, bottom_radius, y0=0):
    """Mask of a rectangle with rounded corners, starting at row y0 of a SIZE-high canvas."""
    mask = [[False] * width for _ in range(SIZE)]
    for y in range(height):
        radius = top_radius if y < top_radius else bottom_radius if y >= height - bottom_radius else 0
        if radius:
            dy = (radius - y - 0.5) if y < top_radius else (y + 0.5 - (height - radius))
            inset = round(radius - (radius * radius - dy * dy) ** 0.5)
        else:
            inset = 0
        for x in range(inset, width - inset):
            mask[y0 + y][x] = True
    return mask


def paint(mask):
    """The grey body of a control: darker rim, lighter towards the top."""
    h, w = len(mask), len(mask[0])
    img = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    for y in range(h):
        for x in range(w):
            if not mask[y][x]:
                continue
            edge = any(not (0 <= y + dy < h and 0 <= x + dx < w and mask[y + dy][x + dx])
                       for dx, dy in ((1, 0), (-1, 0), (0, 1), (0, -1)))
            img.putpixel((x, y), (RIM if edge else BODY_TOP if y < h // 2 else BODY_BOTTOM) + (255,))
    return img


def draw_rows(img, rows, x, y, colour):
    for j, row in enumerate(rows):
        for i, on in enumerate(row):
            if on:
                img.putpixel((x + i, y + j), colour + (255,))


def draw_legend(img, legend, y):
    glyphs = [rows_of(LETTERS[ch]) for ch in legend]
    x = (img.width - (sum(len(g[0]) for g in glyphs) + len(glyphs) - 1)) // 2
    for g in glyphs:
        draw_rows(img, g, x, y, LEGEND)
        x += len(g[0]) + 1


def make(legend, shape):
    if shape == "face":
        img = paint(disc(SIZE, SIZE, SIZE))
        draw_legend(img, legend, 2)
    elif shape == "shoulder":
        img = paint(rounded(13 if len(legend) == 1 else 17, SIZE, 5, 2))
        draw_legend(img, legend, 3)
    elif shape == "pill":
        img = paint(rounded(len(legend) * 4 + 5, 9, 4, 4, y0=2))
        for i, ch in enumerate(legend):
            draw_rows(img, rows_of(TINY[ch]), 3 + i * 4, 4, LEGEND)
    elif shape == "cross":
        mask = [[4 <= x < 8 or 4 <= y < 8 for x in range(SIZE)] for y in range(SIZE)]
        img = paint(mask)
        for x, y in ((5, 5), (6, 5), (5, 6), (6, 6)):
            img.putpixel((x, y), DISH + (255,))
    elif shape == "pad":
        img = paint(disc(SIZE, SIZE, SIZE))
        inner = disc(SIZE, SIZE, 6)
        for y in range(SIZE):
            for x in range(SIZE):
                if inner[y][x]:
                    img.putpixel((x, y), DISH + (255,))
    else:  # nub: a small disc with a dished centre
        img = paint(disc(SIZE, SIZE, 8))
        for x, y in ((5, 5), (6, 5), (5, 6), (6, 6)):
            img.putpixel((x, y), DISH + (255,))
    return img


def main():
    icons = [(name, make(legend, shape)) for name, legend, shape in BUTTONS]

    lines = ["/* Generated by tools/mkbuttons.py: the port's button icons. Pixels are",
             " * 15-bit colour, red in the low bits, with bit 15 set where the icon is",
             " * opaque. */"]
    for name, img in icons:
        words = []
        for y in range(img.height):
            for x in range(img.width):
                r, g, b, a = img.getpixel((x, y))
                words.append(f"0x{(0x8000 | r >> 3 | (g >> 3) << 5 | (b >> 3) << 10) if a else 0:04X}")
        lines.append(f"static const uint16_t icon_{name}_pixels[] = {{{', '.join(words)}}};")
    lines.append("static const ButtonIcon s_icons[] = {")
    lines += [f"    {{\"{name}\", {img.width}, {img.height}, icon_{name}_pixels}}," for name, img in icons]
    lines.append("};")
    open(sys.argv[1], "w").write("\n".join(lines) + "\n")

    scale, pad = 6, 8
    width = sum(img.width for _, img in icons) + pad * (len(icons) + 1)
    sheet = Image.new("RGB", (width, (SIZE + 2 * pad) * 2))
    for row, back in enumerate([(0xDD, 0xC9, 0x96), (0x1A, 0x3A, 0x2A)]):
        sheet.paste(back, (0, row * (SIZE + 2 * pad), width, (row + 1) * (SIZE + 2 * pad)))
        x = pad
        for _, img in icons:
            sheet.paste(img, (x, row * (SIZE + 2 * pad) + pad), img)
            x += img.width + pad
    sheet.resize((sheet.width * scale, sheet.height * scale), Image.NEAREST).save(sys.argv[2])


if __name__ == "__main__":
    main()
