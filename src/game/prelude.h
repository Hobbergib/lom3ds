/* Included ahead of every game source file (see tools/build_game.py).
 *
 * The game addresses RAM, the scratchpad and hardware registers by fixed
 * PlayStation addresses. The port keeps one block, psx_ram, that stands in
 * for all three, and PSX_ADDR() turns a PlayStation address into the address
 * of the same byte inside that block. tools/build_game.py applies the same
 * mapping to the game's data symbols; keep image_offset() there in step. */
#ifndef PORT_PRELUDE_H
#define PORT_PRELUDE_H

#define PSX_RAM_SIZE 0x200000
#define PSX_SCRATCHPAD_OFFSET 0x1000 /* 0x1F800000, 1 KiB */
#define PSX_HW_OFFSET 0x2000         /* 0x1F801000, hardware registers */

extern unsigned char psx_ram[PSX_RAM_SIZE];

#define PSX_IMAGE_OFFSET(a)                                             \
    ((a) >= 0x80000000u   ? (a) - 0x80000000u                           \
     : (a) >= 0x1F801000u ? PSX_HW_OFFSET + ((a) - 0x1F801000u)         \
                          : PSX_SCRATCHPAD_OFFSET + ((a) - 0x1F800000u))

#define PSX_ADDR(a) ((unsigned int)psx_ram + PSX_IMAGE_OFFSET(a))

/* A pointer the game dereferences even when it is null. Address 0 is the
 * start of RAM on the console, so that is what a null pointer reads here. */
#define PSX_NULL_OK(p) ((p) ? (p) : (__typeof__(p))psx_ram)

#endif
