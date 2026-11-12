#include "config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CONFIG_FILE "config.ini"

static const char *const s_off_on[] = {"Off", "On"};
static const char *const s_fit[] = {"Original", "Stretch"};
static const char *const s_skip[] = {"Off", "Auto", "1", "2"};
static const char *const s_volume[] = {"0", "1", "2", "3", "4", "5", "6", "7", "8", "9", "10"};
static const char *const s_interp[] = {"Gaussian", "Linear", "Nearest"};

static const char *const s_keys[] = {"A", "B", "X", "Y", "L", "R", "ZL", "ZR", "START", "SELECT", "None"};

#define CHOICES(a) (uint8_t)(sizeof(a) / sizeof((a)[0])), a

#define BINDING(key, label, tab, def) \
    {key, label, "The 3DS control that acts as this\nbutton. Picking one in use swaps them.", tab, def, CHOICES(s_keys), true}

const char *const g_tab_names[TAB_COUNT] = {"Display", "Speed", "Audio", "Controls"};

const OptDef g_opt_defs[OPT_COUNT] = {
    [OPT_SCREEN_FIT] = {"screen_fit", "Screen fit", "Original keeps the 320x240 picture.\nStretch fills the upper screen.",
                        TAB_DISPLAY, FIT_ORIGINAL, CHOICES(s_fit)},
    [OPT_SMOOTHING] = {"smoothing", "Smoothing", "Softens the picture when it is\nstretched.", TAB_DISPLAY, 0,
                       CHOICES(s_off_on)},
    [OPT_DITHERING] = {"dithering", "Dithering", "The original dither pattern on\nshaded graphics.", TAB_DISPLAY, 1,
                       CHOICES(s_off_on)},
    [OPT_SHOW_FPS] = {"show_fps", "Show frame rate", "Draws a frame counter in the corner.", TAB_DISPLAY, 0,
                      CHOICES(s_off_on)},
    [OPT_FRAME_SKIP] = {"frame_skip", "Frame skip", "Auto skips drawing only when the\ngame falls behind.", TAB_SPEED,
                        SKIP_AUTO, CHOICES(s_skip)},
    [OPT_N3DS_BOOST] = {"n3ds_boost", "New 3DS boost", "Runs the processor at 804 MHz.\nNew 3DS systems only.", TAB_SPEED, 1,
                        CHOICES(s_off_on)},
    [OPT_THREADED_RENDER] = {"threaded_render", "Threaded drawing", "Draws on a second processor core.", TAB_SPEED, 1,
                             CHOICES(s_off_on)},
    [OPT_VOLUME] = {"volume", "Volume", "Overall sound level.", TAB_AUDIO, 10, CHOICES(s_volume)},
    [OPT_INTERPOLATION] = {"interpolation", "Sound quality", "Gaussian is the original sound.\nNearest is the fastest.",
                           TAB_AUDIO, INTERP_GAUSSIAN, CHOICES(s_interp)},
    [OPT_REVERB] = {"reverb", "Reverb", "The echo effect of the original\nsound chip.", TAB_AUDIO, 1, CHOICES(s_off_on)},
    /* The buttons keep the places they have on a PlayStation pad. */
    [OPT_BIND_CROSS] = BINDING("bind_cross", "Cross", TAB_CONTROLS, BIND_A),
    [OPT_BIND_CIRCLE] = BINDING("bind_circle", "Circle", TAB_CONTROLS, BIND_B),
    [OPT_BIND_SQUARE] = BINDING("bind_square", "Square", TAB_CONTROLS, BIND_Y),
    [OPT_BIND_TRIANGLE] = BINDING("bind_triangle", "Triangle", TAB_CONTROLS, BIND_X),
    [OPT_BIND_START] = BINDING("bind_start", "Start", TAB_CONTROLS, BIND_START),
    [OPT_BIND_SELECT] = BINDING("bind_select", "Select", TAB_CONTROLS, BIND_SELECT),
    [OPT_BIND_L1] = BINDING("bind_l1", "L1", TAB_CONTROLS, BIND_L),
    [OPT_BIND_R1] = BINDING("bind_r1", "R1", TAB_CONTROLS, BIND_R),
    [OPT_BIND_L2] = BINDING("bind_l2", "L2", TAB_CONTROLS, BIND_ZL),
    [OPT_BIND_R2] = BINDING("bind_r2", "R2", TAB_CONTROLS, BIND_ZR),
};

uint8_t g_opt[OPT_COUNT];
bool g_config_has_zl_zr = true;

bool config_choice_available(int id, int value)
{
    return !(g_opt_defs[id].binding && !g_config_has_zl_zr && (value == BIND_ZL || value == BIND_ZR));
}

/* Bindings to controls this console lacks become "None". */
static void drop_unavailable(void)
{
    int i;

    for (i = 0; i < OPT_COUNT; i++) {
        if (!config_choice_available(i, g_opt[i])) {
            g_opt[i] = BIND_NONE;
        }
    }
}

void config_reset(void)
{
    int i;

    for (i = 0; i < OPT_COUNT; i++) {
        g_opt[i] = g_opt_defs[i].def;
    }
    drop_unavailable();
}

void config_reset_bindings(void)
{
    int i;

    for (i = 0; i < OPT_COUNT; i++) {
        if (g_opt_defs[i].binding) {
            g_opt[i] = g_opt_defs[i].def;
        }
    }
    drop_unavailable();
}

void config_load(const char *dir)
{
    char path[512], line[128];
    FILE *f;

    config_reset();
    snprintf(path, sizeof path, "%s/" CONFIG_FILE, dir);
    f = fopen(path, "r");
    if (!f) {
        return;
    }
    while (fgets(line, sizeof line, f)) {
        char *eq = strchr(line, '=');
        char *end;
        long v;
        int i;

        if (!eq) {
            continue;
        }
        *eq = 0;
        v = strtol(eq + 1, &end, 10);
        if (end == eq + 1) {
            continue;
        }
        for (i = 0; i < OPT_COUNT; i++) {
            if (!strcmp(line, g_opt_defs[i].key) && v >= 0 && v < g_opt_defs[i].count) {
                g_opt[i] = (uint8_t)v;
            }
        }
    }
    fclose(f);
    drop_unavailable();
}

bool config_save(const char *dir)
{
    char path[512];
    FILE *f;
    bool ok;
    int i;

    snprintf(path, sizeof path, "%s/" CONFIG_FILE, dir);
    f = fopen(path, "w");
    if (!f) {
        return false;
    }
    for (i = 0; i < OPT_COUNT; i++) {
        fprintf(f, "%s=%d\n", g_opt_defs[i].key, g_opt[i]);
    }
    ok = !ferror(f);
    return fclose(f) == 0 && ok;
}
