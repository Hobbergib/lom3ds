/* Starts the game from a verified disc image. */
#ifndef PORT_GAME_H
#define PORT_GAME_H

#include "../core/rom.h"

/* Does not return: the game's main loop runs until the player quits. */
void game_run(const RomInfo *rom);

#endif
