/* Player settings. One table drives the settings menu, the saved file and
 * the code that reads the values, so adding an option is a single entry. */
#ifndef CONFIG_H
#define CONFIG_H

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    /* Display */
    OPT_SCREEN_FIT,
    OPT_SMOOTHING,
    OPT_DITHERING,
    OPT_SHOW_FPS,
    /* Speed */
    OPT_FRAME_SKIP,
    OPT_N3DS_BOOST,
    OPT_THREADED_RENDER,
    /* Audio */
    OPT_VOLUME,
    OPT_INTERPOLATION,
    OPT_REVERB,
    /* Controls: which 3DS control acts as each PlayStation button. The
     * menu shows the first five in its left column, the rest in its right. */
    OPT_BIND_CROSS,
    OPT_BIND_CIRCLE,
    OPT_BIND_SQUARE,
    OPT_BIND_TRIANGLE,
    OPT_BIND_START,
    OPT_BIND_L1,
    OPT_BIND_R1,
    OPT_BIND_L2,
    OPT_BIND_R2,
    OPT_BIND_SELECT,
    OPT_COUNT
} OptId;

typedef enum { TAB_DISPLAY, TAB_SPEED, TAB_AUDIO, TAB_CONTROLS, TAB_COUNT } OptTab;

enum { FIT_ORIGINAL, FIT_STRETCH };
enum { SKIP_OFF, SKIP_AUTO, SKIP_1, SKIP_2 };
enum { INTERP_GAUSSIAN, INTERP_LINEAR, INTERP_NEAREST };
/* The choices of a control binding: a 3DS button, or none. */
enum { BIND_A, BIND_B, BIND_X, BIND_Y, BIND_L, BIND_R, BIND_ZL, BIND_ZR, BIND_START, BIND_SELECT, BIND_NONE };

typedef struct {
    const char *key;   /* name in the config file */
    const char *label; /* name in the menu */
    const char *help;  /* one or two lines shown under the list */
    uint8_t tab;
    uint8_t def;
    uint8_t count; /* number of choices */
    const char *const *choices;
    bool binding; /* a control binding: choices are the BIND_* values */
} OptDef;

extern const OptDef g_opt_defs[OPT_COUNT];
extern const char *const g_tab_names[TAB_COUNT];

/* Current value of every option: an index into its choices. */
extern uint8_t g_opt[OPT_COUNT];

/* Whether the console has ZL and ZR (New 3DS). Set before loading; without
 * them those two are not offered and bindings to them become "None". */
extern bool g_config_has_zl_zr;

/* False for a choice this console cannot provide. */
bool config_choice_available(int id, int value);

void config_reset(void);
/* Missing files, unknown keys and out-of-range values fall back to defaults. */
void config_load(const char *dir);
/* Returns false if the file could not be written. */
bool config_save(const char *dir);

#endif
