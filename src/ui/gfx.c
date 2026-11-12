#include "gfx.h"

#include "font8x8_basic.h"

#include <stdlib.h>
#include <string.h>

#define SPACE_W 3
#define GLYPH_GAP 1

void gfx_fill(Surface *s, int x, int y, int w, int h, uint16_t c)
{
    int x1 = x + w, y1 = y + h, i;

    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x1 > s->w) x1 = s->w;
    if (y1 > s->h) y1 = s->h;
    for (; y < y1; y++) {
        uint16_t *row = s->px + y * s->w;
        for (i = x; i < x1; i++) {
            row[i] = c;
        }
    }
}

/* Blends two colours; t runs from 0 (all a) to max (all b). */
static uint16_t mix(uint16_t a, uint16_t b, int t, int max)
{
    int r = (a >> 11) + (((b >> 11) - (a >> 11)) * t) / max;
    int g = ((a >> 5) & 63) + ((((b >> 5) & 63) - ((a >> 5) & 63)) * t) / max;
    int bl = (a & 31) + (((b & 31) - (a & 31)) * t) / max;

    return (uint16_t)(r << 11 | g << 5 | bl);
}

void gfx_vgradient(Surface *s, int x, int y, int w, int h, uint16_t top, uint16_t bottom)
{
    int i;

    for (i = 0; i < h; i++) {
        gfx_fill(s, x, y + i, w, 1, mix(top, bottom, i, h > 1 ? h - 1 : 1));
    }
}

void gfx_diamond(Surface *s, int cx, int cy, int r, uint16_t c)
{
    int dy;

    for (dy = -r; dy <= r; dy++) {
        int half = r - abs(dy);
        gfx_fill(s, cx - half, cy + dy, half * 2 + 1, 1, c);
    }
}

void gfx_arrow(Surface *s, int x, int cy, int size, int dir, uint16_t c)
{
    int i;

    for (i = 0; i < size; i++) {
        gfx_fill(s, dir < 0 ? x + i : x - i, cy - i, 1, i * 2 + 1, c);
    }
}

void gfx_icon(Surface *s, int x, int y, const uint16_t *pixels, int w, int h)
{
    int i, j;

    for (j = 0; j < h; j++) {
        for (i = 0; i < w; i++) {
            uint16_t c = pixels[j * w + i];

            if (c & 0x8000) {
                uint16_t g = (c >> 5) & 31;
                gfx_fill(s, x + i, y + j, 1, 1, (uint16_t)((c & 31) << 11 | g << 6 | (g >> 4) << 5 | ((c >> 10) & 31)));
            }
        }
    }
}

/* Leftmost and rightmost lit columns of a glyph; false if it is blank. */
static bool glyph_span(unsigned char ch, int *lo, int *hi)
{
    unsigned bits = 0;
    int r;

    if (ch >= 128) {
        return false;
    }
    for (r = 0; r < 8; r++) {
        bits |= (uint8_t)font8x8_basic[ch][r];
    }
    if (!bits) {
        return false;
    }
    for (*lo = 0; !(bits >> *lo & 1); (*lo)++) {
    }
    for (*hi = 7; !(bits >> *hi & 1); (*hi)--) {
    }
    return true;
}

int gfx_text_width(const char *str, int scale)
{
    int w = 0, lo, hi;

    for (; *str && *str != '\n'; str++) {
        w += glyph_span((unsigned char)*str, &lo, &hi) ? hi - lo + 1 + GLYPH_GAP : SPACE_W;
    }
    return w * scale;
}

void gfx_text(Surface *s, int x, int y, const char *str, uint16_t c, int scale)
{
    int pen = x, lo, hi, r, col;

    for (; *str; str++) {
        unsigned char ch = (unsigned char)*str;

        if (ch == '\n') {
            pen = x;
            y += TEXT_LINE_H * scale;
            continue;
        }
        if (!glyph_span(ch, &lo, &hi)) {
            pen += SPACE_W * scale;
            continue;
        }
        for (r = 0; r < 8; r++) {
            uint8_t bits = (uint8_t)font8x8_basic[ch][r];
            for (col = lo; col <= hi; col++) {
                if (bits >> col & 1) {
                    gfx_fill(s, pen + (col - lo) * scale, y + r * scale, scale, scale, c);
                }
            }
        }
        pen += (hi - lo + 1 + GLYPH_GAP) * scale;
    }
}

void gfx_text_centered(Surface *s, int cx, int y, const char *str, uint16_t c, int scale)
{
    gfx_text(s, cx - gfx_text_width(str, scale) / 2, y, str, c, scale);
}

/* Deep forest green with a faint diagonal lattice. Rendered once per surface
 * size and copied afterwards. */
void ui_background(Surface *s)
{
    static uint16_t cache[2][400 * 240];
    static int cache_w[2];
    const uint16_t top = RGB565(0x17, 0x3A, 0x28), bottom = RGB565(0x0A, 0x1C, 0x13);
    int slot = s->w == 400 ? 0 : 1;
    int x, y;

    if (s->w * s->h > 400 * 240) {
        return;
    }
    if (cache_w[slot] != s->w) {
        Surface c = {cache[slot], s->w, s->h};

        gfx_vgradient(&c, 0, 0, c.w, c.h, top, bottom);
        for (y = 0; y < c.h; y++) {
            for (x = 0; x < c.w; x++) {
                if ((x + y) % 24 == 0 || (x - y + 240) % 24 == 0) {
                    uint16_t *p = &c.px[y * c.w + x];
                    *p = mix(*p, COL_LEAF, 1, 4);
                }
            }
        }
        cache_w[slot] = s->w;
    }
    memcpy(s->px, cache[slot], (size_t)(s->w * s->h) * sizeof *s->px);
}

void ui_panel(Surface *s, int x, int y, int w, int h)
{
    const uint16_t hi = RGB565(0xF3, 0xE7, 0xC2), lo = RGB565(0xDB, 0xC6, 0x92);

    gfx_fill(s, x + 3, y + 3, w, h, COL_SHADOW);
    gfx_fill(s, x, y, w, h, COL_WOOD_DARK);
    gfx_vgradient(s, x + 2, y + 2, w - 4, h - 4, hi, lo);
    /* Inner gold line. */
    gfx_fill(s, x + 4, y + 4, w - 8, 1, COL_GOLD);
    gfx_fill(s, x + 4, y + h - 5, w - 8, 1, COL_GOLD);
    gfx_fill(s, x + 4, y + 4, 1, h - 8, COL_GOLD);
    gfx_fill(s, x + w - 5, y + 4, 1, h - 8, COL_GOLD);
    /* Corner studs. */
    gfx_diamond(s, x + 4, y + 4, 3, COL_WOOD_DARK);
    gfx_diamond(s, x + w - 5, y + 4, 3, COL_WOOD_DARK);
    gfx_diamond(s, x + 4, y + h - 5, 3, COL_WOOD_DARK);
    gfx_diamond(s, x + w - 5, y + h - 5, 3, COL_WOOD_DARK);
    gfx_diamond(s, x + 4, y + 4, 1, COL_GOLD_HI);
    gfx_diamond(s, x + w - 5, y + 4, 1, COL_GOLD_HI);
    gfx_diamond(s, x + 4, y + h - 5, 1, COL_GOLD_HI);
    gfx_diamond(s, x + w - 5, y + h - 5, 1, COL_GOLD_HI);
}

void ui_plaque(Surface *s, int x, int y, int w, int h, bool lit)
{
    gfx_fill(s, x, y, w, h, lit ? COL_GOLD : COL_WOOD_DARK);
    gfx_vgradient(s, x + 1, y + 1, w - 2, h - 2, lit ? RGB565(0x7C, 0x50, 0x28) : COL_WOOD,
                  lit ? COL_WOOD : COL_WOOD_DARK);
}

void ui_divider(Surface *s, int cx, int y, int half_w)
{
    gfx_fill(s, cx - half_w, y, half_w * 2 + 1, 1, COL_GOLD);
    gfx_diamond(s, cx, y, 4, COL_GOLD);
    gfx_diamond(s, cx, y, 2, COL_GOLD_HI);
    gfx_diamond(s, cx - half_w, y, 2, COL_GOLD);
    gfx_diamond(s, cx + half_w, y, 2, COL_GOLD);
}
