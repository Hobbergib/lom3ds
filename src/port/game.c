#include "game.h"

#include "../core/disc.h"
#include "../psx/psx.h"

#include <string.h>

#define EXE_HEADER_SIZE 0x800
#define EXE_LOAD_ADDRESS 0x18 /* header fields */
#define EXE_LOAD_SIZE 0x1C
/* Primitive links hold 24-bit addresses, so the image must sit below this. */
#define LINK_ADDRESS_LIMIT 0x1000000u

unsigned char psx_ram[PSX_RAM_SIZE] __attribute__((aligned(0x10000)));

/* The game's entry point (src/main/main.c in the decompilation). */
void main_game_loop(void);

static Disc s_disc;

static uint32_t le32(const uint8_t *p)
{
    return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

/* Copies the boot executable's code and data image to its load address.
 * Only the data is used: the code is the PlayStation original, which the
 * port replaces with the natively compiled decompilation. */
static void load_executable(const char *name)
{
    uint8_t header[DISC_SECTOR_DATA];
    uint32_t addr, size;
    DiscEntry e;

    if (disc_find(&s_disc, name, &e) || disc_read(&s_disc, e.lba, 1, header)) {
        psx_fatal("cannot read %s from the disc image", name);
    }
    addr = le32(header + EXE_LOAD_ADDRESS);
    size = le32(header + EXE_LOAD_SIZE);
    if (addr < PSX_RAM_BASE || size % DISC_SECTOR_DATA || (addr - PSX_RAM_BASE) + size > PSX_RAM_SIZE ||
        size > e.size - EXE_HEADER_SIZE) {
        psx_fatal("%s has an unexpected header", name);
    }
    if (disc_read(&s_disc, e.lba + EXE_HEADER_SIZE / DISC_SECTOR_DATA, size / DISC_SECTOR_DATA, psx_ptr(addr))) {
        psx_fatal("cannot read %s from the disc image", name);
    }
    psx_relocate(psx_find_module("main"), psx_ptr(addr), size);
}

void game_run(const RomInfo *rom)
{
    if ((uintptr_t)psx_ram + PSX_RAM_SIZE > LINK_ADDRESS_LIMIT) {
        psx_fatal("the program is loaded too high in memory (%p)", (void *)psx_ram);
    }
    if (disc_open(&s_disc, rom->path)) {
        psx_fatal("cannot open %s", rom->path);
    }
    memset(psx_ram, 0, PSX_RAM_SIZE);
    load_executable(rom->version->exe_name);
    psx_gpu_reset();
    psx_cd_attach(&s_disc);
    main_game_loop();
}
