/* Replacing parts of the game's graphics after it uploads them.
 *
 * Some button symbols are not text but part of a finished picture, such as
 * the world map's instruction panels. Each entry below names one such
 * picture by where it is uploaded and a hash of its pixels, and the spot in
 * it that shows a PlayStation button. When the game uploads that picture,
 * the port paints over the spot with the icon of the 3DS control currently
 * bound to that button, and repaints it if the player changes the binding.
 *
 * LOM3DS_UPLOAD_TRACE=1 logs every upload with its hash, which is how new
 * entries are found. */
#include "../psx/psx.h"
#include "icons.h"
#include "input.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>

typedef struct {
    PsxRect upload;         /* the upload this applies to */
    uint32_t hash;          /* FNV-1a of its pixel words */
    int16_t clut_x, clut_y; /* its 256-colour palette in video memory */
    int16_t x, y, w, h;     /* the button symbol, in picture pixels */
    uint16_t pad_button;    /* the PlayStation button it shows */
} ButtonPatch;

static const ButtonPatch s_patches[] = {
    /* World map, "Place the Mailbox on the Map ... and press (X)". */
    {{320, 256, 128, 240}, 0xA9BDAEEC, 704, 504, 74, 118, 14, 12, PSX_PAD_CROSS},
};

static uint32_t hash_pixels(const uint16_t *pixels, int count)
{
    uint32_t h = 2166136261u;
    int i;

    for (i = 0; i < count; i++) {
        h = (h ^ pixels[i]) * 16777619u;
    }
    return h;
}

/* Index of the palette entry closest to a 15-bit colour, among the entries
 * that are drawn solid. These pictures are drawn translucent: entries with
 * the top bit set blend with what is behind them, the others (which the
 * original button symbol uses) do not. */
static uint8_t nearest_color(const uint16_t *clut, uint16_t color)
{
    int best = 0, best_dist = 1 << 30, i;

    for (i = 0; i < 256; i++) {
        int dr = (clut[i] & 31) - (color & 31);
        int dg = ((clut[i] >> 5) & 31) - ((color >> 5) & 31);
        int db = ((clut[i] >> 10) & 31) - ((color >> 10) & 31);
        int dist = dr * dr + dg * dg + db * db;

        /* An entry of all zeroes is drawn as transparent. */
        if (clut[i] && !(clut[i] & 0x8000) && dist < best_dist) {
            best = i;
            best_dist = dist;
        }
    }
    return (uint8_t)best;
}

#define PATCH_COUNT (sizeof s_patches / sizeof s_patches[0])
#define SAVE_W 32 /* wide enough for the widest icon */
#define SAVE_H 12

/* Per patch: whether its picture is in video memory now, and the pixels
 * that were under the symbol, so a different icon can replace the first. */
static struct {
    bool active;
    uint8_t under[SAVE_H][SAVE_W];
} s_state[PATCH_COUNT];

static uint16_t *pixel_word(const ButtonPatch *p, int px, int py)
{
    return &psx_vram[(p->upload.y + py) * VRAM_W + p->upload.x + px / 2];
}

/* Two 8-bit pixels share each word of video memory. */
static uint8_t get_pixel(const ButtonPatch *p, int px, int py)
{
    return (uint8_t)(*pixel_word(p, px, py) >> ((px & 1) * 8));
}

static void set_pixel(const ButtonPatch *p, int px, int py, uint8_t index)
{
    uint16_t *word = pixel_word(p, px, py);

    *word = (px & 1) ? (uint16_t)((*word & 0x00FF) | index << 8) : (uint16_t)((*word & 0xFF00) | index);
}

/* Calls `visit` for each pixel of the saved area around the symbol that
 * lies inside the picture. */
static void each_saved_pixel(size_t n, void (*visit)(size_t n, int sx, int sy, int px, int py))
{
    const ButtonPatch *p = &s_patches[n];
    int left = p->x + (p->w - SAVE_W) / 2, top = p->y + (p->h - SAVE_H) / 2, x, y;

    for (y = 0; y < SAVE_H; y++) {
        for (x = 0; x < SAVE_W; x++) {
            if (left + x >= 0 && top + y >= 0 && left + x < p->upload.w * 2 && top + y < p->upload.h) {
                visit(n, x, y, left + x, top + y);
            }
        }
    }
}

static void save_pixel(size_t n, int sx, int sy, int px, int py)
{
    s_state[n].under[sy][sx] = get_pixel(&s_patches[n], px, py);
}

static void restore_pixel(size_t n, int sx, int sy, int px, int py)
{
    set_pixel(&s_patches[n], px, py, s_state[n].under[sy][sx]);
}

/* Paints the icon of the control bound to the patch's button, centred on
 * the symbol, into an 8-bit picture that is already in video memory. With
 * no control bound, the game's own symbol stays. */
static void paint_patch(size_t n)
{
    const ButtonPatch *p = &s_patches[n];
    const ButtonIcon *icon = icon_for_key(input_key_for_pad(p->pad_button));
    const uint16_t *clut = &psx_vram[p->clut_y * VRAM_W + p->clut_x];
    int left, top, x, y;

    each_saved_pixel(n, restore_pixel);
    if (!icon) {
        return;
    }
    left = p->x + (p->w - icon->w) / 2;
    top = p->y + (p->h - icon->h) / 2;
    for (y = 0; y < icon->h; y++) {
        for (x = 0; x < icon->w; x++) {
            uint16_t color = icon->pixels[y * icon->w + x];
            int px = left + x, py = top + y;

            if ((color & 0x8000) && px >= 0 && py >= 0 && px < p->upload.w * 2 && py < p->upload.h) {
                set_pixel(p, px, py, nearest_color(clut, color));
            }
        }
    }
}

void port_bindings_changed(void)
{
    size_t i;

    for (i = 0; i < PATCH_COUNT; i++) {
        if (s_state[i].active) {
            paint_patch(i);
        }
    }
}

static bool overlaps(const PsxRect *a, const PsxRect *b)
{
    return a->x < b->x + b->w && b->x < a->x + a->w && a->y < b->y + b->h && b->y < a->y + a->h;
}

void port_image_uploaded(const PsxRect *rect, const uint16_t *pixels)
{
    static int trace = -1;
    size_t i;

    if (trace < 0) {
        trace = getenv("LOM3DS_UPLOAD_TRACE") != NULL;
    }
    if (trace) {
        psx_log("upload %d,%d %dx%d hash %08X", rect->x, rect->y, rect->w, rect->h,
                (unsigned)hash_pixels(pixels, rect->w * rect->h));
    }
    for (i = 0; i < PATCH_COUNT; i++) {
        const ButtonPatch *p = &s_patches[i];

        if (!overlaps(rect, &p->upload)) {
            continue;
        }
        /* Something new now covers the picture's place. It is the picture
         * itself only if position, size and contents all match; the cheap
         * comparisons come first, as hashing every upload would cost too much. */
        s_state[i].active = rect->x == p->upload.x && rect->y == p->upload.y && rect->w == p->upload.w &&
                            rect->h == p->upload.h && hash_pixels(pixels, rect->w * rect->h) == p->hash;
        if (s_state[i].active) {
            each_saved_pixel(i, save_pixel);
            paint_patch(i);
        }
    }
}
