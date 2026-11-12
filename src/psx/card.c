/* Memory cards: the BIOS card functions and file calls, backed by a card
 * image on the SD card.
 *
 * The image (card1.mcr in the data directory) is a standard 128 KiB
 * PlayStation memory card, so saves can be carried to and from emulators.
 * Slot 1 always holds that card; slot 2 is empty.
 *
 * Operations finish at once here, but the game expects their completion
 * later, as an event, and often clears its event flags just after starting
 * one. So each completion is held back until the game has polled its card
 * events a few times or a frame has passed. */
#include "psx.h"

#include "../plat/plat.h"

#include <stdio.h>
#include <string.h>

#define CARD_SIZE 0x20000
#define FRAME 128         /* the card's unit of transfer */
#define BLOCK 0x2000      /* a save block: 64 frames */
#define BLOCKS 15         /* usable blocks; block 0 holds the directory */
#define NAME_OFFSET 10    /* fields of a directory frame */
#define NAME_SIZE 21
#define STATE_FIRST 0x51
#define STATE_MIDDLE 0x52
#define STATE_LAST 0x53
#define STATE_FREE 0xA0
#define NO_NEXT 0xFFFF

#define DESC_SOFTWARE 0xF4000001u /* results of card operations */
#define DESC_HARDWARE 0xF0000011u /* results of raw transfers */
#define EV_DONE 0x0004
#define EV_TIMEOUT 0x0100

#define MODE_CREATE 0x0200
#define MAX_OPEN 4
#define FIRST_HANDLE 3
/* Card event polls a completion is held back for; a clearing pass over the
 * four software and four hardware events is eight. */
#define POLLS_BEFORE_COMPLETION 8

typedef struct {
    char name[20];
    int32_t attr;
    int32_t size;
    uint32_t next; /* a pointer in the game's 32-bit layout */
    int32_t head;
    char system[4];
} DirEntry;

static uint8_t s_card[CARD_SIZE];
static bool s_loaded;
static struct {
    bool open;
    int first; /* first block */
    uint32_t pos;
} s_files[MAX_OPEN];
static struct {
    bool waiting;
    uint32_t desc;
    int32_t spec;
    int polls_left;
    int frame;
} s_pending;
/* The directory search firstfile started, continued by nextfile. */
static char s_pattern[NAME_SIZE + 1];
static int s_search_block;

/* --- the card image ----------------------------------------------------- */

static void card_path(char *out, size_t size)
{
    snprintf(out, size, "%s/card1.mcr", plat_data_dir());
}

static void seal_frame(uint8_t *frame)
{
    uint8_t sum = 0;
    int i;

    for (i = 0; i < FRAME - 1; i++) {
        sum ^= frame[i];
    }
    frame[FRAME - 1] = sum;
}

static uint8_t *dir_frame(int block)
{
    return s_card + block * FRAME;
}

static void format_card(void)
{
    int i;

    memset(s_card, 0, sizeof s_card);
    s_card[0] = 'M';
    s_card[1] = 'C';
    seal_frame(s_card);
    for (i = 1; i <= BLOCKS; i++) {
        uint8_t *d = dir_frame(i);

        d[0] = STATE_FREE;
        d[8] = d[9] = 0xFF;
        seal_frame(d);
    }
    /* The list of replaced sectors: none. */
    for (i = 16; i < 36; i++) {
        memset(s_card + i * FRAME, 0xFF, 4);
        s_card[i * FRAME + 8] = s_card[i * FRAME + 9] = 0xFF;
        seal_frame(s_card + i * FRAME);
    }
    memcpy(s_card + 63 * FRAME, s_card, FRAME);
}

static void save_card(void)
{
    char path[512];
    FILE *f;

    char fresh[528];
    bool ok;

    /* Written beside the card and then moved into its place, so losing power
     * half way leaves the old card whole. */
    card_path(path, sizeof path);
    snprintf(fresh, sizeof fresh, "%s.new", path);
    f = fopen(fresh, "wb");
    ok = f && fwrite(s_card, 1, sizeof s_card, f) == sizeof s_card;
    if (f && fclose(f) != 0) {
        ok = false;
    }
    if (ok) {
        remove(path); /* the SD card's file system does not rename over a file */
        ok = rename(fresh, path) == 0;
    }
    if (!ok) {
        psx_log("memory card: could not write %s", path);
    }
}

static void load_card(void)
{
    char path[512];
    FILE *f;

    if (s_loaded) {
        return;
    }
    s_loaded = true;
    card_path(path, sizeof path);
    f = fopen(path, "rb");
    if (f && fread(s_card, 1, sizeof s_card, f) == sizeof s_card && s_card[0] == 'M' && s_card[1] == 'C') {
        fclose(f);
        return;
    }
    if (f) {
        char kept[528];

        /* Not a plain 128 KiB card image (another save format, or cut
         * short). It is set aside, never overwritten. */
        fclose(f);
        snprintf(kept, sizeof kept, "%s.unreadable", path);
        remove(kept);
        if (rename(path, kept) != 0) {
            psx_fatal("%s is not a memory card image and could not be moved aside. Remove it and start again.", path);
        }
        psx_log("memory card: %s is not a card image; kept as %s, starting with an empty card", path, kept);
    }
    format_card();
}

static int next_block(int block)
{
    const uint8_t *d = dir_frame(block);
    int next = d[8] | d[9] << 8;

    /* A link outside the card (a damaged image) ends the file. */
    return next >= BLOCKS ? 0 : next + 1;
}

static int find_file(const char *name)
{
    int i;

    for (i = 1; i <= BLOCKS; i++) {
        const uint8_t *d = dir_frame(i);

        if (d[0] == STATE_FIRST && !strncmp((const char *)d + NAME_OFFSET, name, NAME_SIZE - 1)) {
            return i;
        }
    }
    return 0;
}

/* "bu00:NAME" names a file on the card in slot 1, "bu10:NAME" in slot 2. */
static const char *file_name(const char *path, bool *slot1)
{
    const char *colon = strchr(path, ':');

    *slot1 = path[0] == 'b' && path[1] == 'u' && path[2] == '0';
    return colon ? colon + 1 : path;
}

/* --- completions -------------------------------------------------------- */

static void complete_later(uint32_t desc, int32_t spec)
{
    s_pending.waiting = true;
    s_pending.desc = desc;
    s_pending.spec = spec;
    s_pending.polls_left = POLLS_BEFORE_COMPLETION;
    s_pending.frame = psx_frame_count();
}

/* Called whenever the game tests an event, and once a frame. */
void psx_card_poll(void)
{
    if (!s_pending.waiting) {
        return;
    }
    if (s_pending.polls_left-- > 0 && s_pending.frame == psx_frame_count()) {
        return;
    }
    s_pending.waiting = false;
    psx_deliver_event(s_pending.desc, s_pending.spec);
}

/* --- BIOS card functions ------------------------------------------------ */

void psx_InitCARD(long pad_enable)
{
    (void)pad_enable;
    load_card();
}

long psx_StartCARD(void)
{
    return 1;
}

void psx__bu_init(void)
{
}

void psx__new_card(void)
{
}

long psx__card_wait(long drive)
{
    (void)drive;
    return 1;
}

/* Is a card present? Channel 0x00 is slot 1, 0x10 slot 2. */
long psx__card_info(long channel)
{
    load_card();
    complete_later(DESC_SOFTWARE, (channel & 0x10) ? EV_TIMEOUT : EV_DONE);
    return 1;
}

long psx__card_load(long channel)
{
    complete_later(DESC_SOFTWARE, (channel & 0x10) ? EV_TIMEOUT : EV_DONE);
    return 1;
}

long psx__card_clear(long channel)
{
    complete_later(DESC_HARDWARE, (channel & 0x10) ? EV_TIMEOUT : EV_DONE);
    return 1;
}

long psx__card_read(long channel, long sector, uint8_t *buf)
{
    if (!(channel & 0x10) && sector >= 0 && sector < CARD_SIZE / FRAME) {
        memcpy(buf, s_card + sector * FRAME, FRAME);
        complete_later(DESC_HARDWARE, EV_DONE);
    } else {
        complete_later(DESC_HARDWARE, EV_TIMEOUT);
    }
    return 1;
}

long psx__card_write(long channel, long sector, const uint8_t *buf)
{
    if (!(channel & 0x10) && sector >= 0 && sector < CARD_SIZE / FRAME) {
        memcpy(s_card + sector * FRAME, buf, FRAME);
        save_card();
        complete_later(DESC_HARDWARE, EV_DONE);
    } else {
        complete_later(DESC_HARDWARE, EV_TIMEOUT);
    }
    return 1;
}

long psx__card_format(long channel)
{
    if (channel & 0x10) {
        return 0;
    }
    format_card();
    save_card();
    return 1;
}

/* --- files -------------------------------------------------------------- */

long psx_open(const char *path, unsigned long mode)
{
    bool slot1;
    const char *name = file_name(path, &slot1);
    int first, fd, i;

    load_card();
    if (!slot1) {
        return -1;
    }
    for (fd = 0; fd < MAX_OPEN && s_files[fd].open; fd++) {
    }
    if (fd == MAX_OPEN) {
        return -1;
    }
    first = find_file(name);
    if (mode & MODE_CREATE) {
        /* The upper half of the mode is the size in blocks. */
        int want = (int)(mode >> 16), found = 0, chain[BLOCKS];

        if (first || want < 1 || want > BLOCKS) {
            return -1;
        }
        for (i = 1; i <= BLOCKS && found < want; i++) {
            if ((dir_frame(i)[0] & 0xF0) == STATE_FREE) {
                chain[found++] = i;
            }
        }
        if (found < want) {
            return -1;
        }
        for (i = 0; i < want; i++) {
            uint8_t *d = dir_frame(chain[i]);
            int next = i + 1 < want ? chain[i + 1] - 1 : NO_NEXT;
            uint32_t size = i == 0 ? (uint32_t)want * BLOCK : 0;

            memset(d, 0, FRAME);
            d[0] = i == 0 ? STATE_FIRST : i + 1 < want ? STATE_MIDDLE : STATE_LAST;
            memcpy(d + 4, &size, 4);
            d[8] = (uint8_t)next;
            d[9] = (uint8_t)(next >> 8);
            if (i == 0) {
                strncpy((char *)d + NAME_OFFSET, name, NAME_SIZE - 1);
            }
            seal_frame(d);
        }
        first = chain[0];
        save_card();
    } else if (!first) {
        return -1;
    }
    s_files[fd].open = true;
    s_files[fd].first = first;
    s_files[fd].pos = 0;
    return FIRST_HANDLE + fd;
}

long psx_close(long handle)
{
    long fd = handle - FIRST_HANDLE;

    if (fd < 0 || fd >= MAX_OPEN || !s_files[fd].open) {
        return -1;
    }
    s_files[fd].open = false;
    return handle;
}

/* Reads or writes at the file's position, following its chain of blocks. */
static long transfer(long handle, uint8_t *buf, long size, bool writing)
{
    long fd = handle - FIRST_HANDLE, done = 0;

    if (fd < 0 || fd >= MAX_OPEN || !s_files[fd].open || size < 0) {
        return -1;
    }
    while (done < size) {
        uint32_t pos = s_files[fd].pos;
        int block = s_files[fd].first, skip = (int)(pos / BLOCK);
        long n = BLOCK - (long)(pos % BLOCK);
        uint8_t *at;

        while (skip-- > 0 && block) {
            block = next_block(block);
        }
        if (!block) {
            break;
        }
        if (n > size - done) {
            n = size - done;
        }
        at = s_card + (uint32_t)block * BLOCK + pos % BLOCK;
        if (writing) {
            memcpy(at, buf + done, (size_t)n);
        } else {
            memcpy(buf + done, at, (size_t)n);
        }
        s_files[fd].pos += (uint32_t)n;
        done += n;
    }
    if (writing && done) {
        save_card();
    }
    complete_later(DESC_SOFTWARE, EV_DONE);
    return done;
}

long psx_read(long handle, uint8_t *buf, long size)
{
    return transfer(handle, buf, size, false);
}

long psx_write(long handle, uint8_t *buf, long size)
{
    return transfer(handle, buf, size, true);
}

long psx_erase(const char *path)
{
    bool slot1;
    const char *name = file_name(path, &slot1);
    int block = slot1 ? find_file(name) : 0;

    if (!block) {
        return 0;
    }
    while (block) {
        uint8_t *d = dir_frame(block);
        int next = next_block(block);

        memset(d, 0, FRAME);
        d[0] = STATE_FREE;
        d[8] = d[9] = 0xFF;
        seal_frame(d);
        block = next;
    }
    save_card();
    return 1;
}

long psx_rename(const char *old_path, const char *new_path)
{
    bool slot1, slot1_new;
    const char *name = file_name(old_path, &slot1), *new_name = file_name(new_path, &slot1_new);
    int block = slot1 && slot1_new ? find_file(name) : 0;
    uint8_t *d;

    if (!block || find_file(new_name)) {
        return 0;
    }
    d = dir_frame(block);
    memset(d + NAME_OFFSET, 0, NAME_SIZE);
    strncpy((char *)d + NAME_OFFSET, new_name, NAME_SIZE - 1);
    seal_frame(d);
    save_card();
    return 1;
}

/* '?' matches one character and '*' the rest of the name. */
static bool matches(const char *pattern, const char *name)
{
    for (; *pattern; pattern++, name++) {
        if (*pattern == '*') {
            return true;
        }
        if (*pattern != '?' && *pattern != *name) {
            return false;
        }
    }
    return *name == 0;
}

static DirEntry *search_from(int block, DirEntry *out)
{
    for (; block <= BLOCKS; block++) {
        const uint8_t *d = dir_frame(block);
        char name[NAME_SIZE] = "";

        memcpy(name, d + NAME_OFFSET, NAME_SIZE - 1);
        if (d[0] == STATE_FIRST && matches(s_pattern, name)) {
            memset(out, 0, sizeof *out);
            memcpy(out->name, name, sizeof out->name);
            out->attr = STATE_FIRST;
            memcpy(&out->size, d + 4, 4);
            out->head = block;
            s_search_block = block + 1;
            return out;
        }
    }
    s_search_block = BLOCKS + 1;
    return NULL;
}

DirEntry *psx_firstfile(const char *path, DirEntry *out)
{
    bool slot1;
    const char *pattern = file_name(path, &slot1);

    load_card();
    if (!slot1) {
        return NULL;
    }
    snprintf(s_pattern, sizeof s_pattern, "%s", pattern);
    return search_from(1, out);
}

DirEntry *psx_nextfile(DirEntry *out)
{
    return search_from(s_search_block, out);
}

/* --- PocketStation ------------------------------------------------------ */

/* The card is an ordinary one: every PocketStation query says so. */
void psx_McxStartCom(void)
{
}

long psx_McxCardType(long port)
{
    (void)port;
    return 0;
}

long psx_McxSync(long mode, long *command, long *result)
{
    (void)mode;
    (void)command;
    (void)result;
    return 0;
}

long psx_McxExecFlag(long port, long block, long flag)
{
    (void)port;
    (void)block;
    (void)flag;
    return 0;
}

long psx_McxGetTime(long port, uint8_t *time)
{
    (void)port;
    (void)time;
    return 0;
}

long psx_McxHideTrans(long port, long dir)
{
    (void)port;
    (void)dir;
    return 0;
}
