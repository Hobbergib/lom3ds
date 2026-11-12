#include "launcher.h"

#include "../core/config.h"
#include "../core/log.h"
#include "../net/sendlog.h"
#include "../net/update.h"
#include "../plat/plat.h"
#include "gfx.h"
#include "menu.h"

#include <stdio.h>
#include <string.h>

#define PANEL_X 24
#define PANEL_Y 78
#define PANEL_W (TOP_W - 2 * PANEL_X)
#define PANEL_H 128
#define TEXT_X (PANEL_X + 16)

static void draw_supported(Surface *s, int y)
{
    const RomVersion *v;
    char line[64];

    for (v = g_rom_versions; v->serial; v++, y += TEXT_LINE_H + 1) {
        snprintf(line, sizeof line, "%s   %s", v->serial, v->region);
        gfx_diamond(s, TEXT_X + 4, y + 3, 2, COL_GOLD);
        gfx_text(s, TEXT_X + 14, y, line, COL_INK, 1);
    }
}

/* --- updating ----------------------------------------------------------- */

typedef enum { UPDATE_IDLE, UPDATE_NEWEST, UPDATE_READY, UPDATE_FAILED, UPDATE_DONE } UpdateState;

static UpdateState s_update;
static UpdateInfo s_update_info;
static char s_update_text[160];
static const RomInfo *s_shown_rom;

static void draw_top(Surface *s, const RomInfo *rom);

/* Shows `text` under the panel straight away: the network calls that follow
 * do not return for a while. */
static void show_status(const char *text)
{
    Surface top = {plat_fb_top(), TOP_W, TOP_H};

    snprintf(s_update_text, sizeof s_update_text, "%s", text);
    draw_top(&top, s_shown_rom);
    plat_present(true, false);
}

static void show_progress(unsigned long done, unsigned long total)
{
    static int shown = -1;
    int percent = (int)(done * 100ull / total);
    char text[64];

    if (percent != shown) {
        shown = percent;
        snprintf(text, sizeof text, "Downloading build %s   %d%%", s_update_info.build, percent);
        show_status(text);
    }
}

/* Y was pressed: look for a new build, or fetch the one found. */
static void update_step(void)
{
    char err[120] = "";

    if (!plat_program_path()) {
        s_update = UPDATE_FAILED;
        snprintf(s_update_text, sizeof s_update_text, "This build cannot update itself.");
        return;
    }
    if (s_update != UPDATE_READY) {
        show_status("Looking for a new build...");
        if (!update_check(&s_update_info, err, sizeof err)) {
            s_update = UPDATE_FAILED;
            snprintf(s_update_text, sizeof s_update_text, "Update failed: %s", err);
        } else if (strcmp(s_update_info.build, g_build_id) > 0) {
            s_update = UPDATE_READY;
            snprintf(s_update_text, sizeof s_update_text, "Build %s is available.  Y  Install", s_update_info.build);
        } else {
            s_update = UPDATE_NEWEST;
            snprintf(s_update_text, sizeof s_update_text, "This is the newest build (%s).", g_build_id);
        }
        log_line("update check: %s", s_update_text);
        return;
    }
    show_progress(0, s_update_info.size);
    if (update_install(&s_update_info, plat_program_path(), show_progress, err, sizeof err)) {
        s_update = UPDATE_DONE;
        snprintf(s_update_text, sizeof s_update_text, "Updated to build %s. Press any button to close,\nthen start it again.",
                 s_update_info.build);
    } else {
        s_update = UPDATE_FAILED;
        snprintf(s_update_text, sizeof s_update_text, "Update failed: %s", err);
    }
    log_line("update install: %s", s_update_text);
}

static void draw_top(Surface *s, const RomInfo *rom)
{
    char line[96];
    int y = PANEL_Y + 14;

    ui_background(s);
    gfx_text_centered(s, TOP_W / 2 + 2, 20, "LEGEND OF MANA", COL_SHADOW, 3);
    gfx_text_centered(s, TOP_W / 2, 18, "LEGEND OF MANA", COL_GOLD_HI, 3);
    gfx_text_centered(s, TOP_W / 2, 48, "unofficial port for Nintendo 3DS", COL_CREAM, 1);
    ui_divider(s, TOP_W / 2, 66, 120);
    ui_panel(s, PANEL_X, PANEL_Y, PANEL_W, PANEL_H);

    switch (rom->status) {
    case ROM_MISSING:
        gfx_text(s, TEXT_X, y, "No game disc found", COL_EMBER, 2);
        gfx_text(s, TEXT_X, y + 26, "Copy a disc image of your own game to the SD card:", COL_INK, 1);
        snprintf(line, sizeof line, "%s/", plat_data_dir());
        gfx_text(s, TEXT_X + 14, y + 41, line, COL_LEAF, 2);
        gfx_text(s, TEXT_X, y + 64, "Any file name ending in .bin, .img or .iso works.", COL_INK_DIM, 1);
        draw_supported(s, y + 80);
        break;
    case ROM_UNRECOGNIZED:
        gfx_text(s, TEXT_X, y, "Disc image not recognised", COL_EMBER, 2);
        gfx_text(s, TEXT_X, y + 26, rom->file, COL_INK, 1);
        gfx_text(s, TEXT_X, y + 40, "is not one of the supported releases. A patched or\nbadly dumped disc is also rejected.",
                 COL_INK_DIM, 1);
        draw_supported(s, y + 68);
        break;
    case ROM_OK:
        gfx_text(s, TEXT_X, y, "Disc ready", COL_LEAF, 2);
        snprintf(line, sizeof line, "%s   %s", rom->version->serial, rom->version->region);
        gfx_text(s, TEXT_X, y + 26, line, COL_INK, 1);
        gfx_text(s, TEXT_X, y + 40, rom->file, COL_INK_DIM, 1);
        if (!rom->raw_sectors) {
            gfx_text(s, TEXT_X, y + 58, "An .iso image has no video or streamed audio.\nA .bin image keeps them.", COL_EMBER, 1);
        }
        break;
    }

    if (s_update_text[0]) {
        gfx_text_centered(s, TOP_W / 2, PANEL_Y + PANEL_H + 4, s_update_text, s_update == UPDATE_FAILED ? COL_GOLD_HI : COL_CREAM, 1);
    } else {
        snprintf(line, sizeof line, "build %s", g_build_id);
        gfx_text_centered(s, TOP_W / 2, PANEL_Y + PANEL_H + 4, line, COL_INK_DIM, 1);
    }
    if (s_update != UPDATE_DONE) {
        gfx_text_centered(s, TOP_W / 2, TOP_H - 13,
                          rom->status == ROM_OK ? "A / START  Begin      Y  Update      X  Send logs"
                                                : "A  Look again    Y  Update    X  Send logs    START  Quit",
                          COL_CREAM, 1);
    }
}

static void scan(RomInfo *rom)
{
    static const char *const outcome[] = {"no disc image found", "not a supported disc", "disc accepted"};

    rom_scan(plat_data_dir(), rom);
    log_line("scan of %s: %s%s%s", plat_data_dir(), outcome[rom->status], rom->file[0] ? ": " : "", rom->file);
}

bool launcher_run(RomInfo *rom)
{
    Surface top = {plat_fb_top(), TOP_W, TOP_H};
    Surface bot = {plat_fb_bottom(), BOT_W, BOT_H};
    bool dirty = false, start = false;
    FILE *autostart;
    char path[512];
    PlatInput in;

    scan(rom);

    /* Testing aid: with a file named "autostart" in the data directory the
     * game starts by itself, so a run needs nobody at the controls. */
    snprintf(path, sizeof path, "%s/autostart", plat_data_dir());
    autostart = fopen(path, "r");
    if (autostart) {
        fclose(autostart);
        if (rom->status == ROM_OK) {
            log_line("autostart file found: starting the game");
            return true;
        }
    }

    s_shown_rom = rom;
    while (plat_frame_begin(&in)) {
        if (s_update == UPDATE_DONE) {
            /* The program on the card is no longer the one running. */
            if (in.down) {
                break;
            }
            draw_top(&top, rom);
            plat_present(true, false);
            continue;
        }
        if (in.down & PK_Y) {
            update_step();
            continue;
        }
        if (in.down & PK_X) {
            char result[120];

            show_status("Sending the logs...");
            s_update = sendlog_run(plat_data_dir(), result, sizeof result) ? UPDATE_IDLE : UPDATE_FAILED;
            snprintf(s_update_text, sizeof s_update_text, "%s", result);
            log_line("send logs: %s", result);
            continue;
        }
        /* With a disc ready, START begins the game like A does: it is the
         * button people reach for. Without one it leaves the program. */
        if ((in.down & (PK_A | PK_START)) && rom->status == ROM_OK) {
            start = true;
            break;
        }
        if (in.down & PK_START) {
            break;
        }
        if (in.down & PK_A) {
            scan(rom);
        }
        if (menu_update(&in, true)) {
            dirty = true;
            plat_set_speedup(g_opt[OPT_N3DS_BOOST]);
        }
        draw_top(&top, rom);
        menu_draw(&bot);
        plat_present(true, true);
    }
    if (dirty) {
        config_save(plat_data_dir());
    }
    return start;
}
