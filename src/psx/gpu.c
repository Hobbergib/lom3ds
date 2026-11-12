/* The PlayStation GPU in software: a 1024x512 16-bit VRAM and the GP0
 * drawing commands, plus the libgpu functions the game calls.
 *
 * Pixels are the console's own format: 5 bits each of red, green and blue
 * from bit 0 up, and a mask bit on top. This file favours being exact and
 * readable; the hot paths get specialised versions where profiling on the
 * 3DS asks for them. */
#include "psx.h"

#include "../plat/plat.h"

#include <stdlib.h>
#include <string.h>

#define OT_END 0x00FFFFFFu
#define OT_ADDR_MASK 0x00FFFFFFu
#define MAX_OT_NODES 0x100000
#define MASK_BIT 0x8000
#define FRAC 12 /* fixed-point bits of interpolated attributes */

/* Polygon and rectangle command bits. */
enum { CMD_RAW = 0x01, CMD_SEMI = 0x02, CMD_TEXTURED = 0x04, CMD_QUAD = 0x08, CMD_GOURAUD = 0x10 };

typedef struct {
    int x, y;
    int u, v;
    int r, g, b;
} Vertex;

typedef struct {
    int clip_x0, clip_y0, clip_x1, clip_y1; /* drawing area, inclusive */
    int off_x, off_y;
    int page_x, page_y;   /* texture page origin */
    int blend;            /* semi-transparency mode 0..3 */
    int depth;            /* texture colour depth: 0 = 4 bit, 1 = 8 bit, 2 = 15 bit */
    bool dither;
    int win_mask_x, win_mask_y, win_off_x, win_off_y; /* texture window, in texels */
    uint16_t mask_set;    /* MASK_BIT if drawn pixels get the mask bit */
    bool mask_check;      /* skip pixels whose mask bit is set */
    PsxRect display;
    bool display_on;
    bool rgb24;
    void (*draw_sync_callback)(void);
    bool draw_done_pending; /* a drawing or transfer finished; callback owed */
    bool display_changed;   /* something was drawn or the display moved */
} Gpu;

uint16_t psx_vram[VRAM_W * VRAM_H];
uint32_t psx_draw_time_us;
static Gpu s_gpu;

static void drawing_done(void);

static const int8_t s_dither[4][4] = {{-4, 0, -3, 1}, {2, -2, 3, -1}, {-3, 1, -4, 0}, {3, -1, 2, -2}};

/* --- pixels ------------------------------------------------------------- */

static inline uint16_t rgb24_to_15(uint32_t c)
{
    return (uint16_t)(((c >> 3) & 0x1F) | ((c >> 6) & 0x3E0) | ((c >> 9) & 0x7C00));
}

static inline int clamp(int v, int lo, int hi)
{
    return v < lo ? lo : v > hi ? hi : v;
}

/* The four translucency modes, on all three 5-bit channels at once. */
#define CHANNELS 0x7FFFu
#define TOP_BITS 0x4210u /* the highest bit of each channel */
#define LOW_BITS 0x0421u /* the lowest bit of each channel */

/* Adds channel by channel, stopping each at its maximum. The lower four
 * bits of every channel are added first, where a carry cannot leave the
 * channel; the top bits then tell which channels overflowed. */
static inline uint32_t add_saturating(uint32_t back, uint32_t front)
{
    uint32_t low = (back & ~TOP_BITS & CHANNELS) + (front & ~TOP_BITS & CHANNELS);
    uint32_t bt = back & TOP_BITS, ft = front & TOP_BITS, carry = low & TOP_BITS;
    uint32_t over = (bt & ft) | ((bt | ft) & carry);

    return ((low & ~TOP_BITS) | (bt ^ ft ^ carry)) | ((over >> 4) * 31);
}

static inline uint16_t blend(uint16_t back, uint16_t front, int mode)
{
    uint32_t b = back & CHANNELS, f = front & CHANNELS;

    switch (mode) {
    case 0: /* average */
        return (uint16_t)((b + f - ((b ^ f) & LOW_BITS)) >> 1);
    case 1: /* add */
        return (uint16_t)add_saturating(b, f);
    case 2: /* subtract, stopping at black: the complement of adding to the complement */
        return (uint16_t)(~add_saturating(~b & CHANNELS, f) & CHANNELS);
    default: /* add a quarter */
        return (uint16_t)add_saturating(b, (f >> 2) & 0x1CE7u);
    }
}

/* Writes one pixel. `color` carries the texel's mask bit for textured
 * drawing; `semi` asks for blending with what is already there. */
static inline void plot(int x, int y, uint16_t color, bool semi)
{
    uint16_t *dst = &psx_vram[y * VRAM_W + x];

    if (s_gpu.mask_check && (*dst & MASK_BIT)) {
        return;
    }
    if (semi) {
        color = (uint16_t)(blend(*dst, color, s_gpu.blend) | (color & MASK_BIT));
    }
    *dst = color | s_gpu.mask_set;
}

static inline uint16_t fetch_texel(int u, int v, const uint16_t *clut)
{
    const uint16_t *row;

    u = ((u & ~s_gpu.win_mask_x) | (s_gpu.win_off_x & s_gpu.win_mask_x)) & 0xFF;
    v = ((v & ~s_gpu.win_mask_y) | (s_gpu.win_off_y & s_gpu.win_mask_y)) & 0xFF;
    row = &psx_vram[((s_gpu.page_y + v) & (VRAM_H - 1)) * VRAM_W];
    switch (s_gpu.depth) {
    case 0:
        return clut[(row[(s_gpu.page_x + (u >> 2)) & (VRAM_W - 1)] >> ((u & 3) * 4)) & 0xF];
    case 1:
        return clut[(row[(s_gpu.page_x + (u >> 1)) & (VRAM_W - 1)] >> ((u & 1) * 8)) & 0xFF];
    default:
        return row[(s_gpu.page_x + u) & (VRAM_W - 1)];
    }
}

static inline const uint16_t *clut_at(uint32_t clut_word)
{
    return &psx_vram[((clut_word >> 6) & 0x1FF) * VRAM_W + (clut_word & 0x3F) * 16];
}

/* Multiplies a texel by a colour where 0x80 is neutral. */
static inline uint16_t modulate(uint16_t texel, int r, int g, int b, int dither)
{
    int tr = ((texel & 31) * r >> 4) + dither;
    int tg = (((texel >> 5) & 31) * g >> 4) + dither;
    int tb = (((texel >> 10) & 31) * b >> 4) + dither;

    return (uint16_t)(clamp(tr, 0, 255) >> 3 | (clamp(tg, 0, 255) >> 3) << 5 | (clamp(tb, 0, 255) >> 3) << 10 |
                      (texel & MASK_BIT));
}

static inline uint16_t shade(int r, int g, int b, int dither)
{
    return (uint16_t)(clamp(r + dither, 0, 255) >> 3 | (clamp(g + dither, 0, 255) >> 3) << 5 |
                      (clamp(b + dither, 0, 255) >> 3) << 10);
}

static void set_texpage(uint32_t tp)
{
    s_gpu.page_x = (int)(tp & 0xF) * 64;
    s_gpu.page_y = (int)((tp >> 4) & 1) * 256;
    s_gpu.blend = (int)((tp >> 5) & 3);
    s_gpu.depth = (int)((tp >> 7) & 3);
    if (s_gpu.depth == 3) {
        s_gpu.depth = 2;
    }
}

/* --- fills and copies --------------------------------------------------- */

void psx_gpu_fill(int x, int y, int w, int h, uint16_t color)
{
    int i, j;

    /* The hardware fills in 16-pixel columns and wraps at the VRAM edges. */
    x &= 0x3F0;
    y &= 0x1FF;
    w = ((w & 0x3FF) + 0xF) & ~0xF;
    h &= 0x1FF;
    for (j = 0; j < h; j++) {
        uint16_t *row = &psx_vram[((y + j) & (VRAM_H - 1)) * VRAM_W];
        for (i = 0; i < w; i++) {
            row[(x + i) & (VRAM_W - 1)] = color;
        }
    }
}

static void copy_rect(int sx, int sy, int dx, int dy, int w, int h)
{
    int i, j;

    if (w > VRAM_W) {
        w = VRAM_W;
    }
    if (h > VRAM_H) {
        h = VRAM_H;
    }

    for (j = 0; j < h; j++) {
        /* Rows are copied through a buffer so overlapping moves behave. */
        uint16_t line[VRAM_W];
        const uint16_t *src = &psx_vram[((sy + j) & (VRAM_H - 1)) * VRAM_W];
        uint16_t *dst = &psx_vram[((dy + j) & (VRAM_H - 1)) * VRAM_W];

        for (i = 0; i < w; i++) {
            line[i] = src[(sx + i) & (VRAM_W - 1)];
        }
        for (i = 0; i < w; i++) {
            uint16_t *p = &dst[(dx + i) & (VRAM_W - 1)];
            if (!(s_gpu.mask_check && (*p & MASK_BIT))) {
                *p = line[i] | s_gpu.mask_set;
            }
        }
    }
}

/* --- rectangles ---------------------------------------------------------- */

/* Most of what the game draws is rectangles of texture: sprites, tiles,
 * panels, and quads that are really rectangles. They all come through
 * here. The texture is stepped in 12-bit fixed point, so the same code
 * serves unscaled sprites (a step of one texel) and stretched or mirrored
 * quads. Plain opaque copies, the bulk of it, take the short loops. */
static void draw_textured_rect(int x, int y, int w, int h, int u_fp, int v_fp, int du, int dv, uint32_t clut_word,
                               uint32_t color, int cmd)
{
    bool semi = (cmd & CMD_SEMI) != 0;
    int r = color & 0xFF, g = (color >> 8) & 0xFF, b = (color >> 16) & 0xFF;
    bool plain = (cmd & CMD_RAW) || (r == 0x80 && g == 0x80 && b == 0x80);
    bool window = (s_gpu.win_mask_x | s_gpu.win_mask_y) != 0;
    bool simple = plain && !semi && !window && !s_gpu.mask_check && !s_gpu.mask_set;
    const uint16_t *clut = clut_at(clut_word);
    int x0 = x < s_gpu.clip_x0 ? s_gpu.clip_x0 : x;
    int y0 = y < s_gpu.clip_y0 ? s_gpu.clip_y0 : y;
    int x1 = x + w - 1 > s_gpu.clip_x1 ? s_gpu.clip_x1 : x + w - 1;
    int y1 = y + h - 1 > s_gpu.clip_y1 ? s_gpu.clip_y1 : y + h - 1;
    int n = x1 - x0 + 1, page_x = s_gpu.page_x, py, i;

    if (n <= 0) {
        return;
    }
    u_fp += du * (x0 - x);
    v_fp += dv * (y0 - y);
    for (py = y0; py <= y1; py++, v_fp += dv) {
        uint16_t *dst = &psx_vram[py * VRAM_W + x0];
        const uint16_t *row;
        int u = u_fp;

        if (simple) {
            row = &psx_vram[((s_gpu.page_y + ((v_fp >> FRAC) & 0xFF)) & (VRAM_H - 1)) * VRAM_W];

            switch (s_gpu.depth) {
            case 0:
                for (i = 0; i < n; i++, u += du) {
                    int uu = (u >> FRAC) & 0xFF;
                    uint16_t t = clut[(row[(page_x + (uu >> 2)) & (VRAM_W - 1)] >> ((uu & 3) * 4)) & 0xF];
                    if (t) dst[i] = t;
                }
                break;
            case 1:
                for (i = 0; i < n; i++, u += du) {
                    int uu = (u >> FRAC) & 0xFF;
                    uint16_t t = clut[(row[(page_x + (uu >> 1)) & (VRAM_W - 1)] >> ((uu & 1) * 8)) & 0xFF];
                    if (t) dst[i] = t;
                }
                break;
            default:
                for (i = 0; i < n; i++, u += du) {
                    uint16_t t = row[(page_x + ((u >> FRAC) & 0xFF)) & (VRAM_W - 1)];
                    if (t) dst[i] = t;
                }
                break;
            }
            continue;
        }
        row = &psx_vram[((s_gpu.page_y + ((v_fp >> FRAC) & 0xFF)) & (VRAM_H - 1)) * VRAM_W];
        for (i = 0; i < n; i++, u += du) {
            int uu = (u >> FRAC) & 0xFF;
            uint16_t t = window              ? fetch_texel(u >> FRAC, v_fp >> FRAC, clut)
                         : s_gpu.depth == 0 ? clut[(row[(page_x + (uu >> 2)) & (VRAM_W - 1)] >> ((uu & 3) * 4)) & 0xF]
                         : s_gpu.depth == 1 ? clut[(row[(page_x + (uu >> 1)) & (VRAM_W - 1)] >> ((uu & 1) * 8)) & 0xFF]
                                            : row[(page_x + uu) & (VRAM_W - 1)];

            if (!t || (s_gpu.mask_check && (dst[i] & MASK_BIT))) {
                continue;
            }
            if (!plain) {
                t = modulate(t, r, g, b, 0);
            }
            if (semi && (t & MASK_BIT)) {
                t = (uint16_t)(blend(dst[i], t, s_gpu.blend) | MASK_BIT);
            }
            dst[i] = t | s_gpu.mask_set;
        }
    }
}

static void draw_flat_rect(int x, int y, int w, int h, uint32_t color, int cmd)
{
    bool semi = (cmd & CMD_SEMI) != 0;
    uint16_t flat = rgb24_to_15(color);
    int x0 = x < s_gpu.clip_x0 ? s_gpu.clip_x0 : x;
    int y0 = y < s_gpu.clip_y0 ? s_gpu.clip_y0 : y;
    int x1 = x + w - 1 > s_gpu.clip_x1 ? s_gpu.clip_x1 : x + w - 1;
    int y1 = y + h - 1 > s_gpu.clip_y1 ? s_gpu.clip_y1 : y + h - 1;
    int px, py;

    for (py = y0; py <= y1; py++) {
        for (px = x0; px <= x1; px++) {
            plot(px, py, flat, semi);
        }
    }
}

static void draw_rect(int x, int y, int w, int h, uint32_t color, int u0, int v0, uint32_t clut_word, int cmd)
{
    if (cmd & CMD_TEXTURED) {
        draw_textured_rect(x, y, w, h, u0 << FRAC, v0 << FRAC, 1 << FRAC, 1 << FRAC, clut_word, color, cmd);
    } else {
        draw_flat_rect(x, y, w, h, color, cmd);
    }
}

/* --- triangles ---------------------------------------------------------- */

static inline int floor_div(int a, int b) /* b > 0 */
{
    return a >= 0 ? a / b : -((-a + b - 1) / b);
}

/* One edge of a triangle wound so that its inside is on the positive side. */
typedef struct {
    int x, y, dx, dy;
    int bias; /* 0 for a top or left edge, whose pixels are inside; 1 otherwise */
} Edge;

static void triangle_edges(const Vertex *a, const Vertex *b, const Vertex *c, Edge e[3])
{
    const Vertex *from[3] = {a, b, c}, *to[3] = {b, c, a};
    int i;

    for (i = 0; i < 3; i++) {
        e[i].x = from[i]->x;
        e[i].y = from[i]->y;
        e[i].dx = to[i]->x - from[i]->x;
        e[i].dy = to[i]->y - from[i]->y;
        e[i].bias = (e[i].dy < 0 || (e[i].dy == 0 && e[i].dx > 0)) ? 0 : 1;
    }
}

/* Narrows [*xl, *xr] to the pixels of row y inside the triangle. Returns
 * false if there are none. */
static bool triangle_span(const Edge e[3], int y, int *xl, int *xr)
{
    int i;

    for (i = 0; i < 3; i++) {
        int k = e[i].dx * (y - e[i].y) - e[i].bias;

        if (e[i].dy > 0) {
            int lim = e[i].x + floor_div(k, e[i].dy);
            if (lim < *xr) *xr = lim;
        } else if (e[i].dy < 0) {
            int lim = e[i].x - floor_div(k, -e[i].dy);
            if (lim > *xl) *xl = lim;
        } else if (k < 0) {
            return false;
        }
    }
    return *xl <= *xr;
}

static void draw_triangle(const Vertex *a, const Vertex *b, const Vertex *c, uint32_t clut_word, int cmd)
{
    bool textured = (cmd & CMD_TEXTURED) != 0, semi = (cmd & CMD_SEMI) != 0, gouraud = (cmd & CMD_GOURAUD) != 0;
    bool plain = textured && ((cmd & CMD_RAW) || (!gouraud && a->r == 0x80 && a->g == 0x80 && a->b == 0x80));
    bool dithered = s_gpu.dither && (gouraud || (textured && !plain));
    const uint16_t *clut = clut_at(clut_word);
    /* A texture page is 256 rows starting at row 0 or 256, so it never wraps. */
    const uint16_t *page = &psx_vram[s_gpu.page_y * VRAM_W];
    int page_x = s_gpu.page_x;
    bool plain_pixels = !s_gpu.mask_check && !s_gpu.mask_set; /* no mask bit to test or set */
    bool fast = plain && !semi && plain_pixels && !(s_gpu.win_mask_x | s_gpu.win_mask_y);
    Edge edges[3];
    int64_t cross;
    int min_x, max_x, min_y, max_y, y, i;
    int grad_x[5] = {0}, grad_y[5] = {0}, base[5];
    uint16_t flat = (uint16_t)(a->r >> 3 | (a->g >> 3) << 5 | (a->b >> 3) << 10);

    cross = (int64_t)(b->x - a->x) * (c->y - a->y) - (int64_t)(c->x - a->x) * (b->y - a->y);
    if (cross == 0) {
        return;
    }
    if (cross < 0) {
        const Vertex *t = b;
        b = c;
        c = t;
        cross = -cross;
    }

    min_x = a->x < b->x ? (a->x < c->x ? a->x : c->x) : (b->x < c->x ? b->x : c->x);
    max_x = a->x > b->x ? (a->x > c->x ? a->x : c->x) : (b->x > c->x ? b->x : c->x);
    min_y = a->y < b->y ? (a->y < c->y ? a->y : c->y) : (b->y < c->y ? b->y : c->y);
    max_y = a->y > b->y ? (a->y > c->y ? a->y : c->y) : (b->y > c->y ? b->y : c->y);
    /* The hardware refuses polygons this large. */
    if (max_x - min_x > 1023 || max_y - min_y > 511) {
        return;
    }
    if (min_x < s_gpu.clip_x0) min_x = s_gpu.clip_x0;
    if (min_y < s_gpu.clip_y0) min_y = s_gpu.clip_y0;
    if (max_x > s_gpu.clip_x1) max_x = s_gpu.clip_x1;
    if (max_y > s_gpu.clip_y1) max_y = s_gpu.clip_y1;

    if (gouraud || textured) {
        const int av[5] = {a->r, a->g, a->b, a->u, a->v};
        const int bv[5] = {b->r, b->g, b->b, b->u, b->v};
        const int cv[5] = {c->r, c->g, c->b, c->u, c->v};
        /* Only what this triangle varies: colour if shaded, texture
         * coordinates if textured. The size limit above keeps every product
         * here inside 32 bits (255 * 511 * 2 * 4096 < 2^31), which matters
         * on a processor with no divide instruction. */
        int first = gouraud ? 0 : 3, last = textured ? 5 : 3;
        int area = (int)cross, cy = c->y - a->y, by = b->y - a->y, bx = b->x - a->x, cx = c->x - a->x;

        for (i = first; i < last; i++) {
            grad_x[i] = (((bv[i] - av[i]) * cy - (cv[i] - av[i]) * by) * (1 << FRAC)) / area;
            grad_y[i] = (((cv[i] - av[i]) * bx - (bv[i] - av[i]) * cx) * (1 << FRAC)) / area;
        }
        for (i = 0; i < 5; i++) {
            base[i] = (av[i] << FRAC) + (1 << (FRAC - 1));
        }
    }

    triangle_edges(a, b, c, edges);

    for (y = min_y; y <= max_y; y++) {
        int xl = min_x, xr = max_x, x;
        int val[5];

        if (!triangle_span(edges, y, &xl, &xr)) {
            continue;
        }
        if (!gouraud && !textured) {
            uint16_t *dst = &psx_vram[y * VRAM_W];

            if (!plain_pixels) {
                for (x = xl; x <= xr; x++) {
                    plot(x, y, flat, semi);
                }
            } else if (semi) {
                for (x = xl; x <= xr; x++) {
                    dst[x] = blend(dst[x], flat, s_gpu.blend);
                }
            } else {
                for (x = xl; x <= xr; x++) {
                    dst[x] = flat;
                }
            }
            continue;
        }
        for (i = 0; i < 5; i++) {
            val[i] = base[i] + grad_x[i] * (xl - a->x) + grad_y[i] * (y - a->y);
        }
        if (fast) {
            /* Untinted, opaque texture: the bulk of the 3D scenes. */
            uint16_t *dst = &psx_vram[y * VRAM_W];
            int u = val[3], v = val[4], du = grad_x[3], dv = grad_x[4];

            switch (s_gpu.depth) {
            case 0:
                for (x = xl; x <= xr; x++, u += du, v += dv) {
                    int uu = (u >> FRAC) & 0xFF;
                    uint16_t t = clut[(page[(((v >> FRAC) & 0xFF) << 10) + ((page_x + (uu >> 2)) & (VRAM_W - 1))] >>
                                       ((uu & 3) * 4)) & 0xF];
                    if (t) dst[x] = t;
                }
                break;
            case 1:
                for (x = xl; x <= xr; x++, u += du, v += dv) {
                    int uu = (u >> FRAC) & 0xFF;
                    uint16_t t = clut[(page[(((v >> FRAC) & 0xFF) << 10) + ((page_x + (uu >> 1)) & (VRAM_W - 1))] >>
                                       ((uu & 1) * 8)) & 0xFF];
                    if (t) dst[x] = t;
                }
                break;
            default:
                for (x = xl; x <= xr; x++, u += du, v += dv) {
                    uint16_t t = page[(((v >> FRAC) & 0xFF) << 10) + ((page_x + ((u >> FRAC) & 0xFF)) & (VRAM_W - 1))];
                    if (t) dst[x] = t;
                }
                break;
            }
            continue;
        }
        for (x = xl; x <= xr; x++) {
            int d = dithered ? s_dither[y & 3][x & 3] : 0;

            if (textured) {
                uint16_t t = fetch_texel(val[3] >> FRAC, val[4] >> FRAC, clut);
                if (t) {
                    if (!plain) {
                        t = gouraud ? modulate(t, val[0] >> FRAC, val[1] >> FRAC, val[2] >> FRAC, d)
                                    : modulate(t, a->r, a->g, a->b, d);
                    }
                    plot(x, y, t, semi && (t & MASK_BIT));
                }
            } else if (plain_pixels) {
                uint16_t color = shade(val[0] >> FRAC, val[1] >> FRAC, val[2] >> FRAC, d);
                uint16_t *dst = &psx_vram[y * VRAM_W + x];

                *dst = semi ? blend(*dst, color, s_gpu.blend) : color;
            } else {
                plot(x, y, shade(val[0] >> FRAC, val[1] >> FRAC, val[2] >> FRAC, d), semi);
            }
            for (i = 0; i < 5; i++) {
                val[i] += grad_x[i];
            }
        }
    }
}

/* --- lines -------------------------------------------------------------- */

static void draw_line(const Vertex *a, const Vertex *b, int cmd)
{
    bool semi = (cmd & CMD_SEMI) != 0, gouraud = (cmd & CMD_GOURAUD) != 0;
    int dx = b->x - a->x, dy = b->y - a->y;
    int adx = dx < 0 ? -dx : dx, ady = dy < 0 ? -dy : dy;
    int steps = adx > ady ? adx : ady, i;

    if (adx > 1023 || ady > 511) {
        return;
    }
    for (i = 0; i <= steps; i++) {
        int x = steps ? a->x + (dx * i + (dx >= 0 ? steps / 2 : -(steps / 2))) / steps : a->x;
        int y = steps ? a->y + (dy * i + (dy >= 0 ? steps / 2 : -(steps / 2))) / steps : a->y;
        int r = a->r, g = a->g, bl = a->b;

        if (x < s_gpu.clip_x0 || x > s_gpu.clip_x1 || y < s_gpu.clip_y0 || y > s_gpu.clip_y1) {
            continue;
        }
        if (gouraud && steps) {
            r += (b->r - a->r) * i / steps;
            g += (b->g - a->g) * i / steps;
            bl += (b->b - a->b) * i / steps;
        }
        plot(x, y, shade(r, g, bl, gouraud && s_gpu.dither ? s_dither[y & 3][x & 3] : 0), semi);
    }
}

/* --- GP0 ---------------------------------------------------------------- */

static inline int sext11(uint32_t v)
{
    return (int)((v & 0x7FF) ^ 0x400) - 0x400;
}

static void read_xy(uint32_t word, Vertex *v)
{
    v->x = sext11(word) + s_gpu.off_x;
    v->y = sext11(word >> 16) + s_gpu.off_y;
}

static void read_rgb(uint32_t word, Vertex *v)
{
    v->r = (int)(word & 0xFF);
    v->g = (int)((word >> 8) & 0xFF);
    v->b = (int)((word >> 16) & 0xFF);
}

/* A textured quad whose corners form an upright rectangle, with the texture
 * laid straight across it, is drawn as a rectangle: the same pixels and
 * texels the two triangles would give, without the per-line edge work.
 * Returns false if the quad is not of that kind. */
static bool draw_quad_as_rect(const Vertex *v, uint32_t clut_word, uint32_t color, int cmd)
{
    int dx = v[1].x - v[0].x, dy = v[2].y - v[0].y;
    int xa, ya, du, dv, width, height, u_base, v_fp, slack, row;
    Edge first[3];

    if (v[0].x != v[2].x || v[1].x != v[3].x || v[0].y != v[1].y || v[2].y != v[3].y || dx == 0 || dy == 0 ||
        v[0].u != v[2].u || v[1].u != v[3].u || v[0].v != v[1].v || v[2].v != v[3].v) {
        return false;
    }
    if (dx > 1023 || dx < -1023 || dy > 511 || dy < -511) {
        return true; /* the hardware refuses polygons this large */
    }
    if (s_gpu.dither && !((cmd & CMD_RAW) || (v[0].r == 0x80 && v[0].g == 0x80 && v[0].b == 0x80))) {
        return false; /* tinted and dithered: left to the triangle code */
    }
    /* The texel at a pixel is the one the triangle interpolation gives:
     * the value at the first corner plus the gradient, with half a texel
     * of rounding. Mirrored quads simply have a negative gradient. */
    du = (v[1].u - v[0].u) * (1 << FRAC) / dx;
    dv = (v[2].v - v[0].v) * (1 << FRAC) / dy;
    xa = dx > 0 ? v[0].x : v[1].x;
    ya = dy > 0 ? v[0].y : v[2].y;
    width = dx > 0 ? dx : -dx;
    height = dy > 0 ? dy : -dy;
    u_base = (v[0].u << FRAC) + (1 << (FRAC - 1)) - du * v[0].x; /* u at x = 0 */
    v_fp = (v[0].v << FRAC) + (1 << (FRAC - 1)) + dv * (ya - v[0].y);

    /* The second triangle interpolates from the second corner. Where the
     * gradient is not a whole number of steps the two disagree by this
     * much, so each half gets its own start. Unscaled quads have none. */
    slack = ((v[1].u - v[0].u) << FRAC) - du * dx;
    if (slack == 0) {
        draw_textured_rect(xa, ya, width, height, u_base + du * xa, v_fp, du, dv, clut_word, color, cmd);
        return true;
    }
    if ((int64_t)dx * dy > 0) {
        triangle_edges(&v[0], &v[1], &v[2], first);
    } else {
        triangle_edges(&v[0], &v[2], &v[1], first);
    }
    for (row = 0; row < height; row++, v_fp += dv) {
        int y = ya + row, xl = xa, xr = xa + width - 1;

        if (!triangle_span(first, y, &xl, &xr)) {
            draw_textured_rect(xa, y, width, 1, u_base + slack + du * xa, v_fp, du, 0, clut_word, color, cmd);
            continue;
        }
        draw_textured_rect(xl, y, xr - xl + 1, 1, u_base + du * xl, v_fp, du, 0, clut_word, color, cmd);
        /* The first triangle holds one end of the row; the second the rest. */
        if (xl > xa) {
            draw_textured_rect(xa, y, xl - xa, 1, u_base + slack + du * xa, v_fp, du, 0, clut_word, color, cmd);
        }
        if (xr < xa + width - 1) {
            draw_textured_rect(xr + 1, y, xa + width - 1 - xr, 1, u_base + slack + du * (xr + 1), v_fp, du, 0, clut_word,
                               color, cmd);
        }
    }
    return true;
}

static bool s_last_quad_was_rect; /* for the drawing profile */

static int gp0_polygon(const uint32_t *w, int avail)
{
    int cmd = (int)(w[0] >> 24);
    int n = (cmd & CMD_QUAD) ? 4 : 3;
    bool textured = (cmd & CMD_TEXTURED) != 0, gouraud = (cmd & CMD_GOURAUD) != 0;
    int len = 1 + n * (textured ? 2 : 1) + (gouraud ? n - 1 : 0);
    uint32_t clut_word = 0;
    Vertex v[4];
    int i, pos = 0;

    if (len > avail) {
        return avail;
    }
    for (i = 0; i < n; i++) {
        if (i == 0 || gouraud) {
            read_rgb(w[pos++], &v[i]);
        } else {
            v[i].r = v[0].r, v[i].g = v[0].g, v[i].b = v[0].b;
        }
        read_xy(w[pos++], &v[i]);
        if (textured) {
            v[i].u = (int)(w[pos] & 0xFF);
            v[i].v = (int)((w[pos] >> 8) & 0xFF);
            if (i == 0) {
                clut_word = w[pos] >> 16;
            } else if (i == 1) {
                set_texpage(w[pos] >> 16);
            }
            pos++;
        } else {
            v[i].u = v[i].v = 0;
        }
    }
    s_last_quad_was_rect = n == 4 && textured && !gouraud && draw_quad_as_rect(v, clut_word, w[0], cmd);
    if (s_last_quad_was_rect) {
        return len;
    }
    draw_triangle(&v[0], &v[1], &v[2], clut_word, cmd);
    if (n == 4) {
        draw_triangle(&v[1], &v[2], &v[3], clut_word, cmd);
    }
    return len;
}

static int gp0_line(const uint32_t *w, int avail)
{
    int cmd = (int)(w[0] >> 24);
    bool gouraud = (cmd & CMD_GOURAUD) != 0, poly = (cmd & 0x08) != 0;
    Vertex prev, cur;
    int pos = 0, count = 0;

    read_rgb(w[pos++], &prev);
    cur = prev;
    for (;;) {
        if (count > 0) {
            if (poly && pos < avail && (w[pos] & 0xF000F000u) == 0x50005000u) {
                return pos + 1;
            }
            if (gouraud) {
                if (pos >= avail) return avail;
                read_rgb(w[pos++], &cur);
            }
        }
        if (pos >= avail) {
            return avail;
        }
        read_xy(w[pos++], &cur);
        if (count > 0) {
            draw_line(&prev, &cur, cmd);
            if (!poly) {
                return pos;
            }
        }
        prev = cur;
        count++;
    }
}

static int gp0_rect(const uint32_t *w, int avail)
{
    static const int sizes[4] = {0, 1, 8, 16};
    int cmd = (int)(w[0] >> 24);
    bool textured = (cmd & CMD_TEXTURED) != 0;
    int size = sizes[(cmd >> 3) & 3];
    int len = 2 + (textured ? 1 : 0) + (size ? 0 : 1);
    int pos = 2, width = size, height = size, u = 0, v = 0;
    uint32_t clut_word = 0;
    Vertex at;

    if (len > avail) {
        return avail;
    }
    read_xy(w[1], &at);
    if (textured) {
        u = (int)(w[pos] & 0xFF);
        v = (int)((w[pos] >> 8) & 0xFF);
        clut_word = w[pos] >> 16;
        pos++;
    }
    if (!size) {
        width = (int)(w[pos] & 0x3FF);
        height = (int)((w[pos] >> 16) & 0x1FF);
    }
    draw_rect(at.x, at.y, width, height, w[0], u, v, clut_word, cmd);
    return len;
}

/* Debugging: LOM3DS_GPU_TRACE=<frame> logs every command of that frame. */
static bool tracing(void)
{
    static int frame = -2;

    if (frame == -2) {
        const char *env = getenv("LOM3DS_GPU_TRACE");
        frame = env ? atoi(env) : -1;
    }
    return frame >= 0 && psx_frame_count() == frame;
}

/* Debugging: LOM3DS_DRAW_PROFILE=1 times every drawing command and logs the
 * totals by kind every 600 frames, to show what a scene spends its drawing
 * time on. The timing itself costs, so the figures are for comparing kinds,
 * not for absolute speed. */
enum { KIND_RECT, KIND_QUAD_AS_RECT, KIND_TEXTURED_POLY, KIND_SHADED_POLY, KIND_LINE, KIND_OTHER, KIND_COUNT };

static struct {
    int enabled; /* -1 until the environment has been read */
    uint64_t time[KIND_COUNT];
    uint32_t count[KIND_COUNT];
    int last_report;
} s_profile = {-1, {0}, {0}, 0};


static void profile_report(void)
{
    static const char *const names[KIND_COUNT] = {"sprites", "rectangle quads", "textured polygons", "plain polygons",
                                                  "lines", "other"};
    int i;

    for (i = 0; i < KIND_COUNT; i++) {
        if (s_profile.count[i]) {
            psx_log("draw profile: %-17s %7u drawn, %6u ms", names[i], (unsigned)s_profile.count[i],
                    (unsigned)(s_profile.time[i] / 1000));
        }
        s_profile.time[i] = 0;
        s_profile.count[i] = 0;
    }
}

void psx_gpu_gp0(const uint32_t *w, int count)
{
    if (s_profile.enabled < 0) {
        s_profile.enabled = getenv("LOM3DS_DRAW_PROFILE") != NULL;
    }
    while (count > 0) {
        uint32_t word = w[0];
        int cmd = (int)(word >> 24);
        int used = 1;
        uint64_t started = s_profile.enabled ? plat_time_us() : 0;

        if (tracing()) {
            psx_log("gp0 %08X %08X %08X %08X %08X %08X (left %d) clip %d,%d-%d,%d ofs %d,%d", (unsigned)word,
                    (unsigned)(count > 1 ? w[1] : 0), (unsigned)(count > 2 ? w[2] : 0), (unsigned)(count > 3 ? w[3] : 0),
                    (unsigned)(count > 4 ? w[4] : 0), (unsigned)(count > 5 ? w[5] : 0), count, s_gpu.clip_x0, s_gpu.clip_y0, s_gpu.clip_x1,
                    s_gpu.clip_y1, s_gpu.off_x, s_gpu.off_y);
        }

        if (cmd == 0x02 && count >= 3) {
            psx_gpu_fill((int)(w[1] & 0xFFFF), (int)(w[1] >> 16), (int)(w[2] & 0xFFFF), (int)(w[2] >> 16),
                         rgb24_to_15(word));
            used = 3;
        } else if (cmd >= 0x20 && cmd < 0x40) {
            used = gp0_polygon(w, count);
        } else if (cmd >= 0x40 && cmd < 0x60) {
            used = gp0_line(w, count);
        } else if (cmd >= 0x60 && cmd < 0x80) {
            used = gp0_rect(w, count);
        } else if (cmd >= 0x80 && cmd < 0xA0 && count >= 4) {
            int cw = (int)(((w[3] & 0xFFFF) - 1) & 0x3FF) + 1, ch = (int)(((w[3] >> 16) - 1) & 0x1FF) + 1;
            copy_rect((int)(w[1] & 0x3FF), (int)((w[1] >> 16) & 0x1FF), (int)(w[2] & 0x3FF), (int)((w[2] >> 16) & 0x1FF),
                      cw, ch);
            used = 4;
        } else if (cmd == 0xE1) {
            set_texpage(word);
            s_gpu.dither = (word >> 9) & 1;
        } else if (cmd == 0xE2) {
            s_gpu.win_mask_x = (int)(word & 0x1F) * 8;
            s_gpu.win_mask_y = (int)((word >> 5) & 0x1F) * 8;
            s_gpu.win_off_x = (int)((word >> 10) & 0x1F) * 8;
            s_gpu.win_off_y = (int)((word >> 15) & 0x1F) * 8;
        } else if (cmd == 0xE3) {
            s_gpu.clip_x0 = (int)(word & 0x3FF);
            s_gpu.clip_y0 = (int)((word >> 10) & 0x1FF);
        } else if (cmd == 0xE4) {
            s_gpu.clip_x1 = (int)(word & 0x3FF);
            s_gpu.clip_y1 = (int)((word >> 10) & 0x1FF);
        } else if (cmd == 0xE5) {
            s_gpu.off_x = sext11(word);
            s_gpu.off_y = sext11(word >> 11);
        } else if (cmd == 0xE6) {
            s_gpu.mask_set = (word & 1) ? MASK_BIT : 0;
            s_gpu.mask_check = (word & 2) != 0;
        }
        if (s_profile.enabled) {
            int kind = cmd >= 0x60 && cmd < 0x80   ? KIND_RECT
                       : cmd >= 0x40 && cmd < 0x60 ? KIND_LINE
                       : cmd < 0x20 || cmd >= 0x40 ? KIND_OTHER
                       : !(cmd & CMD_TEXTURED)     ? KIND_SHADED_POLY
                       : s_last_quad_was_rect      ? KIND_QUAD_AS_RECT
                                                   : KIND_TEXTURED_POLY;

            s_profile.time[kind] += plat_time_us() - started;
            s_profile.count[kind]++;
            if (psx_frame_count() - s_profile.last_report >= 600) {
                s_profile.last_report = psx_frame_count();
                profile_report();
            }
        }
        w += used;
        count -= used;
    }
}

void psx_gpu_reset(void)
{
    memset(&s_gpu, 0, sizeof s_gpu);
    s_gpu.clip_x1 = VRAM_W - 1;
    s_gpu.clip_y1 = VRAM_H - 1;
    s_gpu.display = (PsxRect){0, 0, 320, 240};
}

void psx_gpu_get_display(PsxRect *area, bool *enabled, bool *rgb24)
{
    *area = s_gpu.display;
    *enabled = s_gpu.display_on;
    *rgb24 = s_gpu.rgb24;
}

/* --- libgpu ------------------------------------------------------------- */

typedef struct {
    uint32_t tag;
    uint32_t code[15];
} DrEnv;

typedef struct {
    PsxRect clip;
    int16_t ofs[2];
    PsxRect tw;
    uint16_t tpage;
    uint8_t dtd, dfe, isbg, r0, g0, b0;
    DrEnv dr_env;
} DrawEnv;

typedef struct {
    PsxRect disp, screen;
    uint8_t isinter, isrgb24, pad0, pad1;
} DispEnv;

static void set_tag(void *prim, int len)
{
    *(uint32_t *)prim = ((uint32_t)len << 24) | OT_END;
}

int psx_ResetGraph(int mode)
{
    /* Only mode 0 is a full reset. The others cancel drawing in progress,
     * which here is always finished, and keep the display and callbacks. */
    if ((mode & 7) == 0) {
        psx_gpu_reset();
    }
    return 0;
}

int psx_SetGraphDebug(int level)
{
    (void)level;
    return 0;
}

void psx_SetDispMask(int mask)
{
    s_gpu.display_changed = true;
    s_gpu.display_on = mask != 0;
}

DrawEnv *psx_SetDefDrawEnv(DrawEnv *env, int x, int y, int w, int h)
{
    env->clip = (PsxRect){(int16_t)x, (int16_t)y, (int16_t)w, (int16_t)h};
    env->ofs[0] = (int16_t)x;
    env->ofs[1] = (int16_t)y;
    env->tw = (PsxRect){0, 0, 0, 0};
    env->tpage = 10;
    env->dtd = 1;
    env->dfe = h > 256;
    env->isbg = 0;
    env->r0 = env->g0 = env->b0 = 0;
    return env;
}

DispEnv *psx_SetDefDispEnv(DispEnv *env, int x, int y, int w, int h)
{
    env->disp = (PsxRect){(int16_t)x, (int16_t)y, (int16_t)w, (int16_t)h};
    env->screen = (PsxRect){0, 0, 0, 0};
    env->isinter = env->isrgb24 = env->pad0 = env->pad1 = 0;
    return env;
}

void psx_SetDrawEnv(DrEnv *dr, const DrawEnv *env)
{
    int x0 = clamp(env->clip.x, 0, VRAM_W - 1), y0 = clamp(env->clip.y, 0, VRAM_H - 1);
    int x1 = clamp(env->clip.x + env->clip.w - 1, 0, VRAM_W - 1), y1 = clamp(env->clip.y + env->clip.h - 1, 0, VRAM_H - 1);
    int n = 0;

    dr->code[n++] = 0xE3000000u | (uint32_t)(y0 << 10) | (uint32_t)x0;
    dr->code[n++] = 0xE4000000u | (uint32_t)(y1 << 10) | (uint32_t)x1;
    dr->code[n++] = 0xE5000000u | (((uint32_t)env->ofs[1] & 0x7FF) << 11) | ((uint32_t)env->ofs[0] & 0x7FF);
    dr->code[n++] = 0xE1000000u | (env->dtd ? 0x200u : 0) | (env->dfe ? 0x400u : 0) | (env->tpage & 0x9FFu);
    dr->code[n++] = 0xE2000000u | ((((uint32_t)env->tw.y & 0xFF) >> 3) << 15) | ((((uint32_t)env->tw.x & 0xFF) >> 3) << 10) |
                    (((~(uint32_t)(env->tw.h - 1) & 0xFF) >> 3) << 5) | ((~(uint32_t)(env->tw.w - 1) & 0xFF) >> 3);
    dr->code[n++] = 0xE6000000u;
    if (env->isbg) {
        /* Clear the drawing area: a rectangle relative to the offset. */
        dr->code[n++] = 0x60000000u | (uint32_t)env->b0 << 16 | (uint32_t)env->g0 << 8 | env->r0;
        dr->code[n++] = (((uint32_t)(y0 - env->ofs[1]) & 0xFFFF) << 16) | ((uint32_t)(x0 - env->ofs[0]) & 0xFFFF);
        dr->code[n++] = ((uint32_t)(y1 - y0 + 1) << 16) | (uint32_t)(x1 - x0 + 1);
    }
    set_tag(dr, n);
}

DrawEnv *psx_PutDrawEnv(DrawEnv *env)
{
    psx_SetDrawEnv(&env->dr_env, env);
    psx_gpu_gp0(env->dr_env.code, (int)(env->dr_env.tag >> 24));
    s_gpu.display_changed = true;
    return env;
}

DispEnv *psx_PutDispEnv(DispEnv *env)
{
    s_gpu.display = env->disp;
    s_gpu.rgb24 = env->isrgb24 != 0;
    s_gpu.display_changed = true;
    return env;
}

int psx_ClearImage(const PsxRect *rect, uint8_t r, uint8_t g, uint8_t b)
{
    uint16_t color = rgb24_to_15((uint32_t)r | (uint32_t)g << 8 | (uint32_t)b << 16);
    int x, y;

    for (y = rect->y < 0 ? 0 : rect->y; y < rect->y + rect->h && y < VRAM_H; y++) {
        for (x = rect->x < 0 ? 0 : rect->x; x < rect->x + rect->w && x < VRAM_W; x++) {
            psx_vram[y * VRAM_W + x] = color;
        }
    }
    drawing_done();
    return 0;
}

int psx_LoadImage(const PsxRect *rect, const uint32_t *src)
{
    const uint16_t *p = (const uint16_t *)src;
    uint64_t start = plat_time_us();
    int x, y;

    for (y = 0; y < rect->h; y++) {
        uint16_t *row = &psx_vram[((rect->y + y) & (VRAM_H - 1)) * VRAM_W];
        for (x = 0; x < rect->w; x++) {
            row[(rect->x + x) & (VRAM_W - 1)] = *p++;
        }
    }
    port_image_uploaded(rect, (const uint16_t *)src);
    psx_draw_time_us += (uint32_t)(plat_time_us() - start);
    drawing_done();
    return 0;
}

int psx_StoreImage(const PsxRect *rect, uint32_t *dst)
{
    uint16_t *p = (uint16_t *)dst;
    int x, y;

    for (y = 0; y < rect->h; y++) {
        const uint16_t *row = &psx_vram[((rect->y + y) & (VRAM_H - 1)) * VRAM_W];
        for (x = 0; x < rect->w; x++) {
            *p++ = row[(rect->x + x) & (VRAM_W - 1)];
        }
    }
    drawing_done();
    return 0;
}

int psx_MoveImage(const PsxRect *rect, int x, int y)
{
    copy_rect(rect->x, rect->y, x, y, rect->w, rect->h);
    drawing_done();
    return 0;
}

int psx_LoadImage2(const PsxRect *rect, const uint32_t *src)
{
    return psx_LoadImage(rect, src);
}

int psx_MoveImage2(const PsxRect *rect, int x, int y)
{
    return psx_MoveImage(rect, x, y);
}

uint32_t *psx_ClearOTagR(uint32_t *ot, int n)
{
    int i;

    for (i = n - 1; i > 0; i--) {
        ot[i] = (uint32_t)(uintptr_t)&ot[i - 1] & OT_ADDR_MASK;
    }
    ot[0] = OT_END;
    return ot;
}

/* Drawing and transfers finish at once here, but the game expects their
 * completion callback later, as an interrupt: it is owed until the next
 * poll point. */
static void drawing_done(void)
{
    s_gpu.draw_done_pending = true;
    s_gpu.display_changed = true;
}

bool psx_gpu_take_display_changed(void)
{
    bool changed = s_gpu.display_changed;

    s_gpu.display_changed = false;
    return changed;
}

void psx_gpu_poll(void)
{
    if (s_gpu.draw_done_pending) {
        s_gpu.draw_done_pending = false;
        if (s_gpu.draw_sync_callback) {
            s_gpu.draw_sync_callback();
        }
    }
}

void psx_DrawOTag(const uint32_t *ot)
{
    const uint32_t *p = ot;
    uint64_t start = plat_time_us();
    int guard;

    psx_stage = "drawing";

    for (guard = 0; guard < MAX_OT_NODES; guard++) {
        uint32_t tag = *p;
        uint32_t next = tag & OT_ADDR_MASK;

        if (tag >> 24) {
            psx_gpu_gp0(p + 1, (int)(tag >> 24));
        }
        if (next == OT_END) {
            psx_draw_time_us += (uint32_t)(plat_time_us() - start);
            drawing_done();
            return;
        }
        p = (const uint32_t *)(uintptr_t)next;
    }
    psx_fatal("ordering table at %p does not end", (const void *)ot);
}

void psx_DrawPrim(const uint32_t *prim)
{
    psx_gpu_gp0(prim + 1, (int)(*prim >> 24));
    drawing_done();
}

int psx_DrawSync(int mode)
{
    (void)mode;
    psx_poll();
    return 0;
}

uint32_t psx_DrawSyncCallback(void (*func)(void))
{
    void (*old)(void) = s_gpu.draw_sync_callback;

    s_gpu.draw_sync_callback = func;
    return (uint32_t)(uintptr_t)old;
}

uint32_t *psx_BreakDraw(void)
{
    return NULL;
}

void psx_SetPolyFT4(uint32_t *prim)
{
    prim[0] = (prim[0] & OT_ADDR_MASK) | (9u << 24);
    ((uint8_t *)prim)[7] = 0x2C;
}

void psx_SetPolyG4(uint32_t *prim)
{
    prim[0] = (prim[0] & OT_ADDR_MASK) | (8u << 24);
    ((uint8_t *)prim)[7] = 0x38;
}
