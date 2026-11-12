/* The settings menu on the lower screen. It works by touch alone, so it can
 * stay usable while the buttons belong to the game; the launcher also feeds
 * it the d-pad and shoulder buttons. */
#ifndef MENU_H
#define MENU_H

#include "../plat/plat.h"
#include "gfx.h"

/* Applies input. Returns true if an option changed. `use_keys` lets the
 * d-pad move the cursor and L/R switch tabs. */
bool menu_update(const PlatInput *in, bool use_keys);
void menu_draw(Surface *s);
/* True if the menu looks different from when it was last drawn. */
bool menu_changed(void);

#endif
