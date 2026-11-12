#include "script.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *s_script = "";
static int s_repeat_left; /* further frames the current step still covers */

static uint32_t key_from_name(const char *s, size_t n)
{
    static const struct {
        const char *name;
        uint32_t key;
    } keys[] = {{"A", PK_A},         {"B", PK_B},       {"X", PK_X},       {"Y", PK_Y},         {"L", PK_L},
                {"R", PK_R},         {"ZL", PK_ZL},     {"ZR", PK_ZR},     {"UP", PK_UP},       {"DOWN", PK_DOWN},
                {"LEFT", PK_LEFT},   {"RIGHT", PK_RIGHT}, {"START", PK_START}, {"SELECT", PK_SELECT}};
    size_t i;

    for (i = 0; i < sizeof keys / sizeof keys[0]; i++) {
        if (strlen(keys[i].name) == n && !strncmp(keys[i].name, s, n)) {
            return keys[i].key;
        }
    }
    return 0;
}

void script_start(const char *steps)
{
    s_script = steps;
    s_repeat_left = 0;
}

bool script_active(void)
{
    return *s_script != 0;
}

bool script_step(PlatInput *in, bool *shot, bool *vram)
{
    const char *end, *star;
    size_t n;

    memset(in, 0, sizeof *in);
    *shot = *vram = false;
    if (!*s_script) {
        return false;
    }
    end = strchr(s_script, ',');
    n = end ? (size_t)(end - s_script) : strlen(s_script);
    star = memchr(s_script, '*', n);
    if (star) {
        if (s_repeat_left == 0) {
            s_repeat_left = atoi(star + 1);
        }
        s_repeat_left--;
        n = (size_t)(star - s_script);
    }

    if (n == 4 && !strncmp(s_script, "shot", 4)) {
        *shot = true;
    } else if (n == 4 && !strncmp(s_script, "vram", 4)) {
        *vram = true;
    } else if (s_script[0] == 'T' && sscanf(s_script, "T%d:%d", &in->touch_x, &in->touch_y) == 2) {
        in->touching = in->touch_down = true;
    } else if (!(n == 1 && s_script[0] == '-')) {
        const char *part = s_script, *stop = s_script + n;
        uint32_t keys = 0;

        while (part < stop) {
            const char *plus = memchr(part, '+', (size_t)(stop - part));
            size_t len = plus ? (size_t)(plus - part) : (size_t)(stop - part);
            uint32_t one = key_from_name(part, len);

            if (!one) {
                char message[80];

                snprintf(message, sizeof message, "bad script step: %.*s", (int)(n < 40 ? n : 40), s_script);
                plat_fatal(message);
            }
            keys |= one;
            part += len + 1;
        }
        in->held = in->down = in->repeat = keys;
    }
    if (s_repeat_left <= 0) {
        s_repeat_left = 0;
        s_script = end ? end + 1 : s_script + strlen(s_script);
    }
    return true;
}
