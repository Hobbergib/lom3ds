/* How the 3DS controls map onto the PlayStation pad. Everything that needs
 * to know which 3DS button does what (reading input, choosing the icon shown
 * for a PlayStation button) asks here. The mapping is the player's: the
 * control bindings in the settings (core/config.h). */
#ifndef PORT_INPUT_H
#define PORT_INPUT_H

#include <stdint.h>

/* The PlayStation pad state (PSX_PAD_* bits) for held 3DS keys (PK_* bits). */
uint16_t input_pad_buttons(uint32_t held);

/* The 3DS key (PK_* bit) bound to a PlayStation button, or 0 if none is. */
uint32_t input_key_for_pad(uint16_t pad_button);

/* The 3DS key (PK_* bit) a binding choice (BIND_*) stands for, or 0. */
uint32_t input_key_for_choice(int choice);

/* Call after the player changes a binding: button icons already painted
 * into the game's pictures are redrawn (src/port/textures.c). */
void port_bindings_changed(void);

#endif
