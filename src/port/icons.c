#include "icons.h"

#include "../plat/plat.h"

#include <stddef.h>
#include <string.h>

#include "button_icons.inc"

const ButtonIcon *icon_for_key(uint32_t key)
{
    static const struct {
        uint32_t key;
        const char *name;
    } names[] = {
        {PK_A, "A"},   {PK_B, "B"},   {PK_X, "X"},         {PK_Y, "Y"},           {PK_L, "L"},     {PK_R, "R"},
        {PK_ZL, "ZL"}, {PK_ZR, "ZR"}, {PK_START, "START"}, {PK_SELECT, "SELECT"}, {PK_UP, "DPAD"}, {PK_DOWN, "DPAD"},
        {PK_LEFT, "DPAD"}, {PK_RIGHT, "DPAD"},
    };
    size_t i, j;

    for (i = 0; i < sizeof names / sizeof names[0]; i++) {
        if (names[i].key != key) {
            continue;
        }
        for (j = 0; j < sizeof s_icons / sizeof s_icons[0]; j++) {
            if (!strcmp(s_icons[j].name, names[i].name)) {
                return &s_icons[j];
            }
        }
    }
    return NULL;
}
