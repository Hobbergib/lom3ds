/* Scripted input: a run of the program driven by a list of steps instead of
 * a person. The host build takes one on its command line; the 3DS build
 * reads script.txt from its data directory, so the same run can be repeated
 * in an emulator or on a console with nobody at the controls.
 *
 * Steps are separated by commas, one per frame:
 *   A, B, X, Y, L, R, ZL, ZR, UP, DOWN, LEFT, RIGHT, START, SELECT   hold a key
 *   UP+RIGHT                                  keys joined by '+' are held together
 *   T<x>:<y>                                  tap the lower screen
 *   shot                                      ask for a screenshot
 *   vram                                      ask for a dump of the game's video memory
 *   -                                         idle frame
 * A step may end in *N to repeat it for N frames, as in "-*300" or "A*2". */
#ifndef PLAT_SCRIPT_H
#define PLAT_SCRIPT_H

#include "plat.h"

/* Starts a script. `steps` must stay valid while it runs. */
void script_start(const char *steps);
bool script_active(void);

/* Fills `in` for the next frame. Returns false once the script has run out.
 * `shot` and `vram` report the two requests. */
bool script_step(PlatInput *in, bool *shot, bool *vram);

#endif
