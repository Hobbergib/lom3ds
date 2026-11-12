#include "menu.h"

#include "../core/config.h"
#include "../port/icons.h"
#include "../port/input.h"

#define MARGIN 8
#define TAB_Y 6
#define TAB_H 20
#define TAB_GAP 4
#define TAB_W ((BOT_W - 2 * MARGIN - (TAB_COUNT - 1) * TAB_GAP) / TAB_COUNT)

#define LIST_Y 32
#define LIST_H 150
#define ROW_Y (LIST_Y + 10)
#define ROW_H 22
#define ROW_X (MARGIN + 6)
#define ROW_W (BOT_W - 2 * ROW_X)
#define MAX_ROWS 6       /* rows that fit the panel */
#define VALUE_W 130      /* value area of a full-width row; touches in it change the value */
#define GRID_VALUE_W 64  /* the same in a two-column layout */

#define HELP_Y 188
#define HELP_H 46

/* Where an option of the current tab is drawn. */
typedef struct {
    int x, y, w;   /* the cell */
    int value_x, value_w;
} Cell;

static int s_tab;
static int s_row;
static bool s_stale = true; /* not drawn yet, or changed since */

/* Fills `ids` with the options of the current tab; returns how many. */
static int tab_options(int ids[OPT_COUNT])
{
    int i, n = 0;

    for (i = 0; i < OPT_COUNT; i++) {
        if (g_opt_defs[i].tab == s_tab) {
            ids[n++] = i;
        }
    }
    return n;
}

/* Options are laid out in one column, or in two when there are too many
 * for the panel (the control bindings): the first half down the left, the
 * rest down the right. */
static Cell cell_of(int i, int n)
{
    Cell c;
    int rows = n > MAX_ROWS ? (n + 1) / 2 : n;

    c.w = n > MAX_ROWS ? ROW_W / 2 : ROW_W;
    c.x = ROW_X + (i / rows) * c.w;
    c.y = ROW_Y + (i % rows) * ROW_H;
    c.value_w = n > MAX_ROWS ? GRID_VALUE_W : VALUE_W;
    c.value_x = c.x + c.w - c.value_w;
    return c;
}

static void select_tab(int tab)
{
    s_tab = (tab + TAB_COUNT) % TAB_COUNT;
    s_row = 0;
}

/* A control binding goes round its choices, skipping controls this console
 * lacks. Taking a control another binding holds hands that one the control
 * given up, so no control ends up doing two things. */
static bool step_binding(int id, int dir)
{
    int count = g_opt_defs[id].count, old = g_opt[id], v = old, i;

    do {
        v = (v + dir + count) % count;
    } while (!config_choice_available(id, v));
    if (v == old) {
        return false;
    }
    for (i = 0; i < OPT_COUNT; i++) {
        if (i != id && g_opt_defs[i].binding && g_opt[i] == v && v != BIND_NONE) {
            g_opt[i] = (uint8_t)old;
        }
    }
    g_opt[id] = (uint8_t)v;
    return true;
}

/* Two-choice options flip; longer lists stop at their ends. */
static bool step_value(int id, int dir)
{
    int count = g_opt_defs[id].count;
    int v = g_opt[id] + dir;

    if (g_opt_defs[id].binding) {
        return step_binding(id, dir);
    }

    if (count == 2) {
        v = (v + count) % count;
    } else if (v < 0 || v >= count) {
        return false;
    }
    g_opt[id] = (uint8_t)v;
    return true;
}

bool menu_update(const PlatInput *in, bool use_keys)
{
    int ids[OPT_COUNT];
    int n = tab_options(ids);
    int old_tab = s_tab, old_row = s_row;
    bool changed = false;

    if (use_keys) {
        if (in->down & PK_L) select_tab(s_tab - 1);
        if (in->down & PK_R) select_tab(s_tab + 1);
        n = tab_options(ids);
        if (in->repeat & PK_UP) s_row = (s_row + n - 1) % n;
        if (in->repeat & PK_DOWN) s_row = (s_row + 1) % n;
        if (in->repeat & PK_LEFT) changed |= step_value(ids[s_row], -1);
        if (in->repeat & PK_RIGHT) changed |= step_value(ids[s_row], 1);
    }

    if (in->touch_down) {
        int x = in->touch_x, y = in->touch_y;

        if (y >= TAB_Y && y < TAB_Y + TAB_H && x >= MARGIN) {
            int tab = (x - MARGIN) / (TAB_W + TAB_GAP);
            if (tab < TAB_COUNT && (x - MARGIN) % (TAB_W + TAB_GAP) < TAB_W) {
                select_tab(tab);
            }
        } else {
            int i;

            for (i = 0; i < n; i++) {
                Cell c = cell_of(i, n);

                if (x >= c.x && x < c.x + c.w && y >= c.y && y < c.y + ROW_H) {
                    s_row = i;
                    if (x >= c.value_x) {
                        changed |= step_value(ids[i], x < c.value_x + c.value_w / 2 ? -1 : 1);
                    }
                    break;
                }
            }
        }
    }
    if (changed || s_tab != old_tab || s_row != old_row) {
        s_stale = true;
    }
    return changed;
}

bool menu_changed(void)
{
    return s_stale;
}

void menu_draw(Surface *s)
{
    int ids[OPT_COUNT];
    int n = tab_options(ids);
    int i;

    s_stale = false;
    ui_background(s);

    for (i = 0; i < TAB_COUNT; i++) {
        int x = MARGIN + i * (TAB_W + TAB_GAP);
        bool lit = i == s_tab;

        ui_plaque(s, x, TAB_Y, TAB_W, TAB_H, lit);
        gfx_text_centered(s, x + TAB_W / 2, TAB_Y + 6, g_tab_names[i], lit ? COL_GOLD_HI : COL_GOLD, 1);
    }

    ui_panel(s, MARGIN, LIST_Y, BOT_W - 2 * MARGIN, LIST_H);
    for (i = 0; i < n; i++) {
        const OptDef *def = &g_opt_defs[ids[i]];
        Cell c = cell_of(i, n);
        int mid = c.y + ROW_H / 2 - 1;
        bool sel = i == s_row;
        uint16_t text = sel ? COL_CREAM : COL_INK;
        uint16_t arrow = sel ? COL_GOLD_HI : COL_INK_DIM;

        if (sel) {
            ui_plaque(s, c.x, c.y, c.w, ROW_H - 2, false);
            gfx_diamond(s, c.x + 8, mid, 3, COL_GOLD_HI);
        }
        gfx_text(s, c.x + 18, mid - 3, def->label, text, 1);
        if (def->binding && icon_for_key(input_key_for_choice(g_opt[ids[i]]))) {
            const ButtonIcon *icon = icon_for_key(input_key_for_choice(g_opt[ids[i]]));

            gfx_icon(s, c.value_x + (c.value_w - icon->w) / 2, mid - icon->h / 2, icon->pixels, icon->w, icon->h);
        } else {
            gfx_text_centered(s, c.value_x + c.value_w / 2, mid - 3, def->choices[g_opt[ids[i]]], text, 1);
        }
        gfx_arrow(s, c.value_x + 8, mid, 5, -1, arrow);
        gfx_arrow(s, c.value_x + c.value_w - 9, mid, 5, 1, arrow);
    }

    ui_panel(s, MARGIN, HELP_Y, BOT_W - 2 * MARGIN, HELP_H);
    gfx_text(s, MARGIN + 12, HELP_Y + 13, g_opt_defs[ids[s_row]].help, COL_INK, 1);
}
