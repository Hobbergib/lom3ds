/* Software 2D drawing on RGB565 surfaces, plus the widgets that give the
 * port's own screens their look: parchment panels, wooden plaques, gold trim. */
#ifndef GFX_H
#define GFX_H

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    uint16_t *px;
    int w, h;
} Surface;

#define RGB565(r, g, b) ((uint16_t)((((r) >> 3) << 11) | (((g) >> 2) << 5) | ((b) >> 3)))

/* Palette */
#define COL_INK RGB565(0x3A, 0x24, 0x12)       /* text on parchment */
#define COL_INK_DIM RGB565(0x8E, 0x78, 0x52)   /* secondary text on parchment */
#define COL_CREAM RGB565(0xF6, 0xEC, 0xCB)     /* text on dark surfaces */
#define COL_GOLD RGB565(0xC2, 0x9A, 0x3E)
#define COL_GOLD_HI RGB565(0xEE, 0xD0, 0x72)
#define COL_WOOD RGB565(0x5A, 0x38, 0x1C)
#define COL_WOOD_DARK RGB565(0x34, 0x20, 0x10)
#define COL_LEAF RGB565(0x2C, 0x7A, 0x46)      /* good news */
#define COL_EMBER RGB565(0xA2, 0x3C, 0x1C)     /* problems */
#define COL_SHADOW RGB565(0x06, 0x12, 0x0C)

#define TEXT_LINE_H 10 /* line height at scale 1 */

void gfx_fill(Surface *s, int x, int y, int w, int h, uint16_t c);
void gfx_vgradient(Surface *s, int x, int y, int w, int h, uint16_t top, uint16_t bottom);
void gfx_diamond(Surface *s, int cx, int cy, int r, uint16_t c);
/* A solid triangle pointing left (dir < 0) or right (dir > 0), tip at x. */
void gfx_arrow(Surface *s, int x, int cy, int size, int dir, uint16_t c);

/* Draws 15-bit pixels (red in the low bits); those without bit 15 are skipped. */
void gfx_icon(Surface *s, int x, int y, const uint16_t *pixels, int w, int h);

/* Text is proportional. '\n' starts a new line at the same x. */
int gfx_text_width(const char *str, int scale); /* first line only */
void gfx_text(Surface *s, int x, int y, const char *str, uint16_t c, int scale);
void gfx_text_centered(Surface *s, int cx, int y, const char *str, uint16_t c, int scale);

void ui_background(Surface *s);
void ui_panel(Surface *s, int x, int y, int w, int h);
void ui_plaque(Surface *s, int x, int y, int w, int h, bool lit);
/* A horizontal gold rule with a diamond in the middle. */
void ui_divider(Surface *s, int cx, int y, int half_w);

#endif
