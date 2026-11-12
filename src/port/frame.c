/* One frame of the port around the game: show the picture on the upper
 * screen, run the settings menu on the lower screen, read the buttons. */
#include "../core/config.h"
#include "input.h"
#include "../plat/plat.h"
#include "../psx/psx.h"
#include "../ui/menu.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static inline uint16_t to_rgb565(uint16_t c)
{
    uint16_t g = (c >> 5) & 31;

    return (uint16_t)((c & 31) << 11 | g << 6 | (g >> 4) << 5 | ((c >> 10) & 31));
}

/* Scales the game's display area onto the upper screen, nearest pixel. */
static void show_picture(uint16_t *top)
{
    PsxRect d;
    bool on, rgb24;
    /* Source column for each output column; rebuilt when the mode changes. */
    static uint16_t column[TOP_W];
    static int column_src_w, column_out_w;
    int out_w = g_opt[OPT_SCREEN_FIT] == FIT_STRETCH ? TOP_W : 320;
    int left = (TOP_W - out_w) / 2;
    int x, y;

    psx_gpu_get_display(&d, &on, &rgb24);
    memset(top, 0, TOP_W * TOP_H * sizeof *top);
    if (!on || d.w <= 0 || d.h <= 0) {
        return;
    }
    if (column_src_w != d.w || column_out_w != out_w) {
        for (x = 0; x < out_w; x++) {
            column[x] = (uint16_t)(x * d.w / out_w);
        }
        column_src_w = d.w;
        column_out_w = out_w;
    }
    for (y = 0; y < TOP_H; y++) {
        int sy = (d.y + (d.h == TOP_H ? y : y * d.h / TOP_H)) & (VRAM_H - 1);
        const uint16_t *row = &psx_vram[sy * VRAM_W];
        uint16_t *out = top + y * TOP_W + left;

        for (x = 0; x < out_w; x++) {
            int sx = column[x];

            if (rgb24) {
                /* 24-bit mode packs three bytes per pixel into the same rows. */
                const uint8_t *p = (const uint8_t *)&row[d.x & (VRAM_W - 1)] + sx * 3;
                out[x] = (uint16_t)((p[0] >> 3) << 11 | (p[1] >> 2) << 5 | p[2] >> 3);
            } else {
                out[x] = to_rgb565(row[(d.x + sx) & (VRAM_W - 1)]);
            }
        }
    }
}

/* Debugging aid: the whole 1024x512 video memory as a PPM image. */
static void dump_vram(const char *path)
{
    FILE *f = fopen(path, "wb");
    int i;

    if (!f) {
        return;
    }
    fprintf(f, "P6\n%d %d\n255\n", VRAM_W, VRAM_H);
    for (i = 0; i < VRAM_W * VRAM_H; i++) {
        uint16_t c = psx_vram[i];
        uint8_t rgb[3] = {(uint8_t)((c & 31) << 3), (uint8_t)(((c >> 5) & 31) << 3), (uint8_t)(((c >> 10) & 31) << 3)};
        fwrite(rgb, 1, 3, f);
    }
    fclose(f);
}

/* Performance figures. The console shows 60 frames a second; the game asks
 * for one of them at a time and, like the original, mostly moves at 30. What
 * matters is the work between two of these calls: over 16.7 ms and the game
 * misses a frame and slows down. Each window of frames is summarised in the
 * log, and on screen if "Show frame rate" is on. */
#define PERF_WINDOW 300 /* frames: five seconds at full speed */
#define FRAME_BUDGET_US 16667

static struct {
    uint64_t last_end;       /* when the previous call returned */
    uint64_t window_start;
    uint32_t frames, late;   /* in this window */
    uint64_t work_total;
    uint32_t work_max;
    uint64_t show_total;     /* converting the picture and copying it to the screens */
    uint64_t draw_total;     /* the game's drawing, inside the work time */
    char text[40];           /* summary of the last finished window */
} s_perf;

/* Returns true when a new summary is ready, which changes the on-screen text. */
static bool perf_frame_start(uint64_t now)
{
    uint32_t work = s_perf.last_end ? (uint32_t)(now - s_perf.last_end) : 0;

    if (!s_perf.window_start) {
        s_perf.window_start = now;
    }
    s_perf.frames++;
    s_perf.work_total += work;
    s_perf.draw_total += psx_draw_time_us;
    psx_draw_time_us = 0;
    if (work > s_perf.work_max) {
        s_perf.work_max = work;
    }
    if (work > FRAME_BUDGET_US) {
        s_perf.late++;
    }
    if (s_perf.frames == PERF_WINDOW) {
        uint32_t elapsed = (uint32_t)(now - s_perf.window_start);
        uint32_t fps10 = elapsed ? (uint32_t)(PERF_WINDOW * 10000000ull / elapsed) : 0;
        uint32_t avg = (uint32_t)(s_perf.work_total / PERF_WINDOW);

        snprintf(s_perf.text, sizeof s_perf.text, "%u.%u fps  %u.%u ms", (unsigned)(fps10 / 10), (unsigned)(fps10 % 10),
                 (unsigned)(avg / 1000), (unsigned)(avg % 1000 / 100));
        uint32_t draw = (uint32_t)(s_perf.draw_total / PERF_WINDOW), show = (uint32_t)(s_perf.show_total / PERF_WINDOW);

        psx_log("speed: %u.%u frames/s, work per frame %u.%u ms average (drawing %u.%u), %u.%u ms worst, "
                "%u of %d frames over budget; showing the picture %u.%u ms",
                (unsigned)(fps10 / 10), (unsigned)(fps10 % 10), (unsigned)(avg / 1000), (unsigned)(avg % 1000 / 100),
                (unsigned)(draw / 1000), (unsigned)(draw % 1000 / 100), (unsigned)(s_perf.work_max / 1000),
                (unsigned)(s_perf.work_max % 1000 / 100), (unsigned)s_perf.late, PERF_WINDOW, (unsigned)(show / 1000),
                (unsigned)(show % 1000 / 100));
        s_perf.frames = s_perf.late = s_perf.work_max = 0;
        s_perf.work_total = s_perf.show_total = s_perf.draw_total = 0;
        s_perf.window_start = now;
        return true;
    }
    return false;
}

uint16_t port_frame(void)
{
    Surface bot = {plat_fb_bottom(), BOT_W, BOT_H};
    const char *dump = plat_vram_dump_path();
    PlatInput in;

    if (dump) {
        dump_vram(dump);
    }
    static bool redraw_top = true; /* a setting that affects the picture changed */
    uint64_t show_start = plat_time_us();
    bool top_changed, bottom_changed = menu_changed();

    /* Converting and copying a screen is costly, and the game draws only
     * every other frame, so each screen is redone only when it changed. */
    top_changed = perf_frame_start(show_start) | psx_gpu_take_display_changed() | redraw_top;
    redraw_top = false;
    if (top_changed) {
        show_picture(plat_fb_top());
        if (g_opt[OPT_SHOW_FPS] && s_perf.text[0]) {
            Surface top = {plat_fb_top(), TOP_W, TOP_H};

            gfx_fill(&top, 0, 0, gfx_text_width(s_perf.text, 1) + 6, 12, 0);
            gfx_text(&top, 3, 2, s_perf.text, COL_CREAM, 1);
        }
    }
    if (bottom_changed) {
        menu_draw(&bot);
    }
    s_perf.show_total += (uint32_t)(plat_time_us() - show_start);
    psx_stage = "waiting for the screen refresh";
    s_perf.show_total += plat_present(top_changed, bottom_changed);
    psx_stage = "reading input";

    if (!plat_frame_begin(&in)) {
        plat_exit();
        exit(0);
    }
    /* The buttons belong to the game; the menu is driven by touch. */
    /* Saved at once: the game may end in ways that never come back here. */
    if (menu_update(&in, false)) {
        config_save(plat_data_dir());
        plat_set_speedup(g_opt[OPT_N3DS_BOOST]);
        port_bindings_changed();
        redraw_top = true;
    }
    s_perf.last_end = plat_time_us();
    return input_pad_buttons(in.held);
}
