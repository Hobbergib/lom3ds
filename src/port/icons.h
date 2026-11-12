/* The port's own button icons, one per 3DS control (tools/mkbuttons.py). */
#ifndef PORT_ICONS_H
#define PORT_ICONS_H

#include <stdint.h>

typedef struct {
    const char *name;
    int w, h;
    const uint16_t *pixels; /* 15-bit colour, red low; bit 15 set where opaque */
} ButtonIcon;

/* The icon for a 3DS key (PK_* bit), or NULL. */
const ButtonIcon *icon_for_key(uint32_t key);

#endif
