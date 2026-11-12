#include "input.h"

#include "../core/config.h"
#include "../plat/plat.h"
#include "../psx/psx.h"

#include <stddef.h>

/* Directions are not rebindable; the Circle Pad doubles the +Control Pad. */
static const struct {
    uint32_t key;
    uint16_t pad;
} s_directions[] = {
    {PK_UP, PSX_PAD_UP}, {PK_DOWN, PSX_PAD_DOWN}, {PK_LEFT, PSX_PAD_LEFT}, {PK_RIGHT, PSX_PAD_RIGHT}};

static const struct {
    OptId option;
    uint16_t pad;
} s_buttons[] = {
    {OPT_BIND_CROSS, PSX_PAD_CROSS},   {OPT_BIND_CIRCLE, PSX_PAD_CIRCLE}, {OPT_BIND_SQUARE, PSX_PAD_SQUARE},
    {OPT_BIND_TRIANGLE, PSX_PAD_TRIANGLE}, {OPT_BIND_START, PSX_PAD_START}, {OPT_BIND_SELECT, PSX_PAD_SELECT},
    {OPT_BIND_L1, PSX_PAD_L1},         {OPT_BIND_R1, PSX_PAD_R1},         {OPT_BIND_L2, PSX_PAD_L2},
    {OPT_BIND_R2, PSX_PAD_R2},
};

#define COUNT(a) (sizeof(a) / sizeof((a)[0]))

uint32_t input_key_for_choice(int choice)
{
    static const uint32_t keys[] = {[BIND_A] = PK_A,   [BIND_B] = PK_B,         [BIND_X] = PK_X,
                                    [BIND_Y] = PK_Y,   [BIND_L] = PK_L,         [BIND_R] = PK_R,
                                    [BIND_ZL] = PK_ZL, [BIND_ZR] = PK_ZR,       [BIND_START] = PK_START,
                                    [BIND_SELECT] = PK_SELECT, [BIND_NONE] = 0};

    return choice >= 0 && (size_t)choice < COUNT(keys) ? keys[choice] : 0;
}

uint16_t input_pad_buttons(uint32_t held)
{
    uint16_t out = 0;
    size_t i;

    for (i = 0; i < COUNT(s_directions); i++) {
        if (held & s_directions[i].key) {
            out |= s_directions[i].pad;
        }
    }
    for (i = 0; i < COUNT(s_buttons); i++) {
        if (held & input_key_for_choice(g_opt[s_buttons[i].option])) {
            out |= s_buttons[i].pad;
        }
    }
    return out;
}

uint32_t input_key_for_pad(uint16_t pad_button)
{
    size_t i;

    for (i = 0; i < COUNT(s_directions); i++) {
        if (s_directions[i].pad == pad_button) {
            return s_directions[i].key;
        }
    }
    for (i = 0; i < COUNT(s_buttons); i++) {
        if (s_buttons[i].pad == pad_button) {
            return input_key_for_choice(g_opt[s_buttons[i].option]);
        }
    }
    return 0;
}
