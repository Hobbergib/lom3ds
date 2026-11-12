#!/usr/bin/env python3
"""Draws the HOME Menu banner picture and makes its short chime, both from
scratch (nothing here comes from the game).

Usage: mkbanner.py OUT.png OUT.wav
"""
import math
import struct
import sys
import wave

from PIL import Image, ImageDraw, ImageFont

W, H = 256, 128
GOLD, GOLD_HI, CREAM = (194, 154, 62), (238, 208, 114), (246, 236, 203)


def text(img, cx, y, s, color, scale):
    font = ImageFont.load_default()
    box = font.getbbox(s)
    tile = Image.new("RGBA", (box[2], box[3] + 2), (0, 0, 0, 0))
    ImageDraw.Draw(tile).text((0, 0), s, font=font, fill=color + (255,))
    tile = tile.resize((tile.width * scale, tile.height * scale), Image.NEAREST)
    img.alpha_composite(tile, (cx - tile.width // 2, y))


def picture(path):
    img = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    for y in range(12, H - 12):
        t = (y - 12) / (H - 24)
        d.line([(10, y), (W - 11, y)], fill=(int(14 + 10 * t), int(52 - 18 * t), int(36 - 10 * t), 255))
    d.rectangle([10, 12, W - 11, H - 13], outline=GOLD + (255,), width=2)
    d.rectangle([14, 16, W - 15, H - 17], outline=(90, 56, 28, 255))
    text(img, W // 2 + 1, 37, "LEGEND OF MANA", (6, 18, 12), 2)
    text(img, W // 2, 36, "LEGEND OF MANA", GOLD_HI, 2)
    d.line([(60, 70), (W - 61, 70)], fill=GOLD + (255,))
    d.polygon([(W // 2, 66), (W // 2 + 4, 70), (W // 2, 74), (W // 2 - 4, 70)], fill=GOLD_HI + (255,))
    text(img, W // 2, 80, "unofficial port for Nintendo 3DS", CREAM, 1)
    img.save(path)


def chime(path):
    rate, notes = 32000, [(523.25, 0.0), (659.25, 0.18), (783.99, 0.36)]
    samples = []
    for i in range(int(rate * 1.6)):
        t, v = i / rate, 0.0
        for freq, start in notes:
            if t >= start:
                v += math.sin(2 * math.pi * freq * (t - start)) * math.exp(-3.5 * (t - start))
        samples.append(int(max(-1, min(1, v * 0.25)) * 32767))
    with wave.open(path, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(rate)
        w.writeframes(struct.pack(f"<{len(samples)}h", *samples))


picture(sys.argv[1])
chime(sys.argv[2])
