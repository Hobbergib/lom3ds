/* The start-up screen: finds the player's disc image, explains where to put
 * one if it is missing, and hosts the settings menu on the lower screen. */
#ifndef LAUNCHER_H
#define LAUNCHER_H

#include "../core/rom.h"

/* Runs until the player quits. Returns true if a supported disc was found
 * and the player chose to start; `rom` then describes it. */
bool launcher_run(RomInfo *rom);

/* This build's stamp (src/main.c). */
extern char g_build_id[24];

#endif
