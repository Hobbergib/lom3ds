#!/usr/bin/env python3
"""Surveys a disc image for pictures that show PlayStation button symbols.

The game paints its button symbols into finished pictures (instruction
panels, menu sheets), so the port has to know every picture that has one.
This walks the disc's resource table, finds the TIM pictures in each
resource (raw, or after the game's decompression), decodes them and looks
for the symbol's dark lozenge. The lozenge is learned from one picture whose
symbol position is known, so no game artwork is kept in this repository.

Usage: find_buttons.py DISC.bin DECOMP_DIR OUT_DIR [MAX_RESOURCE_BYTES]
Writes OUT_DIR/report.txt and a PNG crop of every hit.
"""
import pathlib
import struct
import sys
import time

from PIL import Image

RAW_SECTOR, DATA_OFFSET, SECTOR = 2352, 24, 2048
TABLE_LBA, TABLE_BYTES = 24, 46488

# The picture the lozenge is learned from: the world map's "Place the
# Mailbox" panel, by the FNV-1a hash of its pixel words (as src/port/textures.c
# computes it), and where its symbol sits.
REFERENCE_HASH, REFERENCE_BOX = 0xA9BDAEEC, (74, 118, 14, 12)


def bcd(v):
    return (v >> 4) * 10 + (v & 15)


def fnv(words):
    h = 2166136261
    for w in words:
        h = ((h ^ w) * 16777619) & 0xFFFFFFFF
    return h


class Disc:
    def __init__(self, path):
        self.f = open(path, "rb")

    def read(self, lba, size):
        out = bytearray()
        while len(out) < size:
            self.f.seek(lba * RAW_SECTOR + DATA_OFFSET)
            out += self.f.read(SECTOR)
            lba += 1
        return bytes(out[:size])

    def resources(self):
        table = self.read(TABLE_LBA, TABLE_BYTES)
        for i in range(len(table) // 8):
            m, s, f, mode, size = struct.unpack_from("<BBBBI", table, i * 8)
            if mode == 0xFF and size:
                yield i, (bcd(m) * 60 + bcd(s)) * 75 + bcd(f) - 150, size


def find_tims(blob):
    """Yields (offset, bits, cluts, x, y, w, h, words) for each TIM with a palette."""
    pos = blob.find(b"\x10\x00\x00\x00")
    while pos >= 0:
        try:
            flags, clut_len, cx, cy, cw, ch = struct.unpack_from("<IIHHHH", blob, pos + 4)
            if flags in (8, 9) and clut_len == 12 + cw * ch * 2 and 0 < cw <= 256 and 0 < ch <= 64:
                p = pos + 8 + clut_len
                img_len, x, y, w, h = struct.unpack_from("<IHHHH", blob, p)
                if img_len == 12 + w * h * 2 and 0 < w <= 512 and 0 < h <= 512 and x < 1024 and y < 512:
                    cluts = struct.unpack_from(f"<{cw * ch}H", blob, pos + 20)
                    words = struct.unpack_from(f"<{w * h}H", blob, p + 12)
                    yield pos, 4 if flags == 8 else 8, (cw, ch, cluts), x, y, w, h, words
        except struct.error:
            pass
        pos = blob.find(b"\x10\x00\x00\x00", pos + 4)


def decode(bits, clut, w, h, words):
    """Rows of (r, g, b) for a picture drawn with one palette."""
    per_word = 16 // bits
    mask = (1 << bits) - 1
    rows = []
    for y in range(h):
        row = []
        for x in range(w * per_word):
            c = clut[(words[y * w + x // per_word] >> ((x % per_word) * bits)) & mask]
            row.append((c & 31, (c >> 5) & 31, (c >> 10) & 31))
        rows.append(row)
    return rows


def learn_lozenge(rows):
    """The symbol's dark pixels, as offsets inside its box, and their colour."""
    bx, by, bw, bh = REFERENCE_BOX
    dark = [(x, y) for y in range(bh) for x in range(bw) if sum(rows[by + y][bx + x]) < 30]
    return dark, (bw, bh)


def find_lozenges(rows, dark, size):
    """Top-left corners where the dark lozenge shape appears on a lighter ground."""
    bw, bh = size
    h, w = len(rows), len(rows[0])
    lum = [[sum(p) for p in row] for row in rows]
    hits = []
    for y in range(h - bh + 1):
        for x in range(w - bw + 1):
            # Cheap rejection on two body pixels before the full comparison.
            if lum[y + dark[0][1]][x + dark[0][0]] >= 30 or lum[y + dark[-1][1]][x + dark[-1][0]] >= 30:
                continue
            if sum(lum[y + dy][x + dx] < 30 for dx, dy in dark) < len(dark) * 0.92:
                continue
            # The corners of the box lie outside the lozenge and must not be dark too.
            if all(lum[y + dy][x + dx] < 30 for dx, dy in ((0, 0), (bw - 1, 0), (0, bh - 1), (bw - 1, bh - 1))):
                continue
            hits.append((x, y))
    return hits


def main():
    disc_path, decomp, out = sys.argv[1], pathlib.Path(sys.argv[2]), pathlib.Path(sys.argv[3])
    limit = int(sys.argv[4]) if len(sys.argv) > 4 else 1 << 20
    sys.path.insert(0, str(decomp / "tools" / "compression"))
    from decompress import decompress

    out.mkdir(parents=True, exist_ok=True)
    disc = Disc(disc_path)
    pictures, started = [], time.time()
    for index, lba, size in disc.resources():
        if size > limit:
            continue
        blob = disc.read(lba, size)
        found = list(find_tims(blob))
        packed = False
        if not found and blob[:1] == b"\x01":
            try:
                found = list(find_tims(bytes(decompress(blob[1:]))))
                packed = True
            except Exception:
                found = []
        for tim in found:
            pictures.append((index, packed) + tim)
    print(f"{len(pictures)} pictures with palettes in {time.time() - started:.0f}s")

    reference = None
    for p in pictures:
        if p[7] * p[8] == 128 * 240 and fnv(p[9]) == REFERENCE_HASH:
            reference = p
            break
    report = []
    if reference is None:
        sys.exit("the reference picture was not found among them")
    bits, (cw, ch, cluts) = reference[3], reference[4]
    dark, size = learn_lozenge(decode(bits, cluts[:cw], reference[7], reference[8], reference[9]))
    report.append(f"reference: resource {reference[0]}{' (packed)' if reference[1] else ''}, offset {reference[2]}")

    hits = 0
    for index, packed, offset, bits, (cw, ch, cluts), x, y, w, h, words in pictures:
        colours = 1 << bits
        for row in range(min(ch * cw // colours, 16)):
            rows = decode(bits, cluts[row * colours:(row + 1) * colours], w, h, words)
            found = find_lozenges(rows, dark, size)
            if not found:
                continue
            hits += 1
            report.append(f"resource {index}{' (packed)' if packed else ''} offset {offset} upload {x},{y} {w}x{h} "
                          f"{bits}bpp palette row {row} hash {fnv(words):08X}: " + " ".join(f"{a},{b}" for a, b in found))
            img = Image.new("RGB", (len(rows[0]), h))
            img.putdata([(r << 3, g << 3, b << 3) for line in rows for r, g, b in line])
            img.save(out / f"res{index}_{offset}_{row}.png")
            break
    report.insert(0, f"{len(pictures)} pictures scanned, {hits} show the lozenge")
    (out / "report.txt").write_text("\n".join(report) + "\n")
    print(report[0])


if __name__ == "__main__":
    main()
