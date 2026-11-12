/* Pointer relocation for data loaded from the disc.
 *
 * The executable and the overlays carry tables of PlayStation addresses:
 * handler functions, strings, other tables. The port runs native functions
 * and keeps RAM at a different address, so each stored address is rewritten
 * when its file has been loaded.
 *
 * Which words are addresses comes from the build (tools/gen_relocs.py).
 * Inside data the game's declarations describe, only the words typed as
 * pointers are touched. Data with no declaration falls back to a value
 * test, kept strict because tables of 16-bit pairs produce address-like
 * words: the value must be word-aligned and must not point into code,
 * unless it is exactly the start of a known function. */
#include "psx.h"

#include <string.h>

#define FIRST_PROGRAM_ADDRESS 0x80010000u
#define RAM_END_ADDRESS (PSX_RAM_BASE + PSX_RAM_SIZE)
#define OVERLAY_ID_SIZE 4

int32_t game_cdrom_stream(int32_t resource_index, uint8_t *destination);

const PsxModule *psx_find_module(const char *name)
{
    int i;

    for (i = 0; i < psx_module_count; i++) {
        if (!strcmp(psx_modules[i].name, name)) {
            return &psx_modules[i];
        }
    }
    psx_fatal("no game module named %s", name);
}

static const PsxFunction *find_function(const PsxModule *m, uint32_t addr)
{
    int lo = 0, hi = m->function_count - 1;

    while (lo <= hi) {
        int mid = (lo + hi) / 2;

        if (m->functions[mid].psx_addr == addr) {
            return &m->functions[mid];
        }
        if (m->functions[mid].psx_addr < addr) {
            lo = mid + 1;
        } else {
            hi = mid - 1;
        }
    }
    return NULL;
}

static bool in_spans(const PsxSpan *spans, int count, uint32_t addr)
{
    int lo = 0, hi = count - 1;

    while (lo <= hi) {
        int mid = (lo + hi) / 2;

        if (addr < spans[mid].start) {
            hi = mid - 1;
        } else if (addr >= spans[mid].end) {
            lo = mid + 1;
        } else {
            return true;
        }
    }
    return false;
}

void psx_relocate(const PsxModule *module, uint8_t *start, uint32_t size)
{
    static const PsxModule *main_module;
    uint32_t addr = PSX_RAM_BASE + (uint32_t)(start - psx_ram), end = addr + (size & ~3u);
    int slot = 0, span = 0;
    int typed_count = 0, guessed = 0, left = 0, missing = 0;

    if (!main_module) {
        main_module = psx_find_module("main");
    }
    for (; addr < end; addr += 4) {
        uint32_t *word = psx_ptr(addr);
        uint32_t value = *word;
        const PsxFunction *f;
        bool typed, is_slot;

        while (slot < module->slot_count && module->slots[slot] < addr) {
            slot++;
        }
        while (span < module->typed_count && module->typed[span].end <= addr) {
            span++;
        }
        if (value < FIRST_PROGRAM_ADDRESS || value >= RAM_END_ADDRESS) {
            continue;
        }
        is_slot = slot < module->slot_count && module->slots[slot] == addr;
        typed = span < module->typed_count && module->typed[span].start <= addr;
        f = find_function(module, value);
        if (typed && !is_slot) {
            continue; /* declared as something other than a pointer */
        }
        if (f) {
            if (f->native) {
                *word = (uint32_t)(uintptr_t)f->native;
            } else {
                missing++;
                continue;
            }
        } else if (in_spans(module->code, module->code_count, value) ||
                   in_spans(main_module->code, main_module->code_count, value) || (!typed && (value & 3))) {
            /* Into code but not a function, or an odd value in undeclared
             * data: not an address the port can use, most likely not one. */
            left++;
            continue;
        } else {
            *word = (uint32_t)(uintptr_t)psx_ptr(value);
        }
        if (typed) {
            typed_count++;
        } else {
            guessed++;
        }
    }
    psx_log("%s: %d declared pointers and %d by value rewritten, %d address-like words left, %d to missing SDK functions",
            module->name, typed_count, guessed, left, missing);
}

/* The overlay last loaded into each of the two slots. */
#define SECONDARY_SLOT 0x80140000u
static const PsxModule *s_loaded[2];

/* The native function for a PlayStation address inside an overlay slot,
 * taken from the overlay loaded there. The game calls some overlay entry
 * points by address alone: several overlays share the address, and which
 * one is meant depends on what is loaded. */
void (*psx_lookup_function(uint32_t addr))(void);
void (*psx_lookup_function(uint32_t addr))(void)
{
    const PsxModule *m = s_loaded[addr >= SECONDARY_SLOT];
    const PsxFunction *f = m ? find_function(m, addr) : NULL;

    if (!f || !f->native) {
        psx_fatal("the game called %08lX, which is not a function of the loaded overlay (%s)", (unsigned long)addr,
                  m ? m->name : "none");
    }
    return f->native;
}

/* Wraps the game's loader: an overlay that has just been read into one of
 * the two overlay slots gets its pointers relocated before it runs. */
int32_t psx_cdrom_stream(int32_t resource_index, uint8_t *destination)
{
    int32_t size = game_cdrom_stream(resource_index, destination);
    int i;

    for (i = 0; i < psx_module_count; i++) {
        const PsxModule *m = &psx_modules[i];

        if (m->overlay_id && destination == (uint8_t *)psx_ptr(m->load_addr) && size >= OVERLAY_ID_SIZE &&
            !memcmp(destination, m->overlay_id, OVERLAY_ID_SIZE)) {
            psx_log("overlay %s loaded", m->name);
            psx_relocate(m, destination, (uint32_t)size);
            s_loaded[m->load_addr >= SECONDARY_SLOT] = m;
            break;
        }
    }
    return size;
}

/* --- field maps --------------------------------------------------------- */

#define LOAD_BUFFER 0x80180000u
#define LOAD_BUFFER_SIZE 0x5A000u
#define MAP_TEXTURE_BYTES 0x0C /* header fields */
#define MAP_TEXTURE 0x14
#define MAP_OBJECTS 0x20       /* pointer to a null-terminated pointer array */
#define OBJECT_IMAGE 0x04      /* object fields */
#define OBJECT_IMAGE_SIZE 0x28
#define OBJECT_PIXEL_COUNT 0x2A
#define MAX_MAP_IMAGES 64

typedef struct {
    uint32_t start, end; /* offsets in the load buffer */
} Span;

/* Offset in the load buffer that a stored address refers to, whether or not
 * it has been relocated already; LOAD_BUFFER_SIZE if it is not inside. */
static uint32_t buffer_offset(uint32_t value)
{
    uint32_t host_base = (uint32_t)(uintptr_t)psx_ptr(LOAD_BUFFER);

    if (value - LOAD_BUFFER < LOAD_BUFFER_SIZE) {
        return value - LOAD_BUFFER;
    }
    if (value - host_base < LOAD_BUFFER_SIZE) {
        return value - host_base;
    }
    return LOAD_BUFFER_SIZE;
}

static uint32_t read32(const uint8_t *base, uint32_t offset)
{
    uint32_t v;

    memcpy(&v, base + offset, 4);
    return v;
}

/* A field map file is linked for the address it loads at: its tables point
 * at each other with PlayStation addresses. Rewrites every such address,
 * leaving out the pixel data, where a chance match would damage the image. */
void psx_field_map_loaded(void)
{
    uint8_t *base = psx_ptr(LOAD_BUFFER);
    Span skip[MAX_MAP_IMAGES + 1];
    int skips = 0, i;
    uint32_t offset, list;

    offset = buffer_offset(read32(base, MAP_TEXTURE));
    if (offset < LOAD_BUFFER_SIZE) {
        skip[skips++] = (Span){offset, offset + read32(base, MAP_TEXTURE_BYTES)};
    }
    list = buffer_offset(read32(base, MAP_OBJECTS));
    for (; list < LOAD_BUFFER_SIZE - 4 && skips <= MAX_MAP_IMAGES; list += 4) {
        uint32_t object = read32(base, list), image, size, pixels;
        uint16_t image_size, pixel_count;

        if (!object) {
            break;
        }
        object = buffer_offset(object);
        if (object >= LOAD_BUFFER_SIZE - OBJECT_PIXEL_COUNT - 2) {
            continue; /* not in this file: nothing of it to protect */
        }
        image = buffer_offset(read32(base, object + OBJECT_IMAGE));
        memcpy(&image_size, base + object + OBJECT_IMAGE_SIZE, 2);
        memcpy(&pixel_count, base + object + OBJECT_PIXEL_COUNT, 2);
        /* Rows of 256 pixels plus a palette, or a plain pixel count. */
        size = (uint32_t)(image_size >> 8) * 512 + (image_size & 0xFF) * 2;
        pixels = (uint32_t)pixel_count * 2;
        if (image < LOAD_BUFFER_SIZE) {
            skip[skips++] = (Span){image, image + (size > pixels ? size : pixels)};
        }
    }

    for (offset = 0; offset < LOAD_BUFFER_SIZE; offset += 4) {
        uint32_t value = read32(base, offset);

        for (i = 0; i < skips; i++) {
            if (offset >= skip[i].start && offset < skip[i].end) {
                break;
            }
        }
        if (i == skips && value - LOAD_BUFFER < LOAD_BUFFER_SIZE) {
            value = (uint32_t)(uintptr_t)psx_ptr(value);
            memcpy(base + offset, &value, 4);
        }
    }
}

/* Video playback needs the MDEC decoder, which the port does not have yet:
 * movies are skipped. */
void psx_movie_play(uint16_t movie_index)
{
    psx_log("movie %d skipped (no video decoder yet)", movie_index);
}

/* The game records its own internal errors (a bad script opcode, a missing
 * resource) in a small buffer nobody sees. The port also writes them to the
 * log, where they are often the first sign of something it gets wrong. */
void game_record_game_diagnostic(int32_t status, int32_t code, int32_t arg0, int32_t arg1);

void psx_record_game_diagnostic(int32_t status, int32_t code, int32_t arg0, int32_t arg1)
{
    psx_log("game diagnostic: status 0x%X code %d (0x%X), arguments %d, %d", (unsigned)status, (int)code,
            (unsigned)code, (int)arg0, (int)arg1);
    game_record_game_diagnostic(status, code, arg0, arg1);
}
