#include "disc.h"

#include <ctype.h>
#include <string.h>

#define PVD_LBA 16
#define MAX_DIR_SECTORS 64 /* bounds the walk if a directory record is garbage */

static uint32_t le32(const uint8_t *p)
{
    return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

int disc_read(Disc *d, uint32_t lba, uint32_t count, void *dst)
{
    uint8_t *out = dst;

    if (lba >= d->sectors || count > d->sectors - lba) {
        return -1;
    }
    if (d->sector_size == DISC_SECTOR_DATA) {
        if (fseek(d->f, (long)(lba * DISC_SECTOR_DATA), SEEK_SET)) {
            return -1;
        }
        return fread(out, DISC_SECTOR_DATA, count, d->f) == count ? 0 : -1;
    }
    for (; count; count--, lba++, out += DISC_SECTOR_DATA) {
        if (fseek(d->f, (long)(lba * d->sector_size + d->data_offset), SEEK_SET) ||
            fread(out, DISC_SECTOR_DATA, 1, d->f) != 1) {
            return -1;
        }
    }
    return 0;
}

static uint8_t to_bcd(uint32_t v)
{
    return (uint8_t)((v / 10) << 4 | v % 10);
}

int disc_read_whole(Disc *d, uint32_t lba, uint8_t *dst)
{
    uint32_t frame = lba + 150; /* two-second lead-in */

    if (lba >= d->sectors) {
        return -1;
    }
    if (d->sector_size == DISC_SECTOR_RAW) {
        return fseek(d->f, (long)(lba * DISC_SECTOR_RAW + 12), SEEK_SET) ||
                       fread(dst, DISC_SECTOR_WHOLE, 1, d->f) != 1
                   ? -1
                   : 0;
    }
    memset(dst, 0, DISC_SECTOR_WHOLE);
    dst[0] = to_bcd(frame / (75 * 60));
    dst[1] = to_bcd(frame / 75 % 60);
    dst[2] = to_bcd(frame % 75);
    dst[3] = 2;
    dst[6] = dst[10] = 0x08; /* submode: data */
    return disc_read(d, lba, 1, dst + 12);
}

int disc_open(Disc *d, const char *path)
{
    static const uint8_t sync[12] = {0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00};
    uint8_t sec[DISC_SECTOR_DATA];
    long size;

    memset(d, 0, sizeof *d);
    d->f = fopen(path, "rb");
    if (!d->f) {
        return -1;
    }
    if (fseek(d->f, 0, SEEK_END) || (size = ftell(d->f)) < 0 || fseek(d->f, 0, SEEK_SET)) {
        goto fail;
    }
    /* A raw sector starts with a sync pattern; byte 15 is the mode. Mode 2
     * (every PlayStation disc) has an 8-byte subheader before the data. */
    if (fread(sec, 1, 16, d->f) == 16 && !memcmp(sec, sync, sizeof sync)) {
        d->sector_size = DISC_SECTOR_RAW;
        d->data_offset = sec[15] == 2 ? 24 : 16;
    } else {
        d->sector_size = DISC_SECTOR_DATA;
        d->data_offset = 0;
    }
    d->sectors = (uint32_t)size / d->sector_size;

    if (disc_read(d, PVD_LBA, 1, sec) || sec[0] != 1 || memcmp(sec + 1, "CD001", 5)) {
        goto fail;
    }
    return 0;

fail:
    disc_close(d);
    return -1;
}

void disc_close(Disc *d)
{
    if (d->f) {
        fclose(d->f);
        d->f = NULL;
    }
}

bool disc_is_raw(const Disc *d)
{
    return d->sector_size == DISC_SECTOR_RAW;
}

static bool name_matches(const uint8_t *rec_name, size_t rec_len, const char *name, size_t len)
{
    size_t i;

    /* Drop the ";1" version suffix of file identifiers. */
    for (i = 0; i < rec_len; i++) {
        if (rec_name[i] == ';') {
            rec_len = i;
            break;
        }
    }
    if (rec_len != len) {
        return false;
    }
    for (i = 0; i < len; i++) {
        if (toupper(rec_name[i]) != toupper((unsigned char)name[i])) {
            return false;
        }
    }
    return true;
}

static int dir_lookup(Disc *d, const DiscEntry *dir, const char *name, size_t len, DiscEntry *out)
{
    uint8_t sec[DISC_SECTOR_DATA];
    uint32_t n = (dir->size + DISC_SECTOR_DATA - 1) / DISC_SECTOR_DATA;
    uint32_t i;

    if (n > MAX_DIR_SECTORS) {
        n = MAX_DIR_SECTORS;
    }
    for (i = 0; i < n; i++) {
        uint32_t p = 0;

        if (disc_read(d, dir->lba + i, 1, sec)) {
            return -1;
        }
        /* Records never span sectors; a zero length ends this sector. */
        while (p + 34 <= DISC_SECTOR_DATA && sec[p] >= 34 && p + sec[p] <= DISC_SECTOR_DATA) {
            const uint8_t *rec = sec + p;
            uint8_t name_len = rec[32];

            if (33u + name_len <= rec[0] && name_matches(rec + 33, name_len, name, len)) {
                out->lba = le32(rec + 2);
                out->size = le32(rec + 10);
                out->is_dir = (rec[25] & 2) != 0;
                return 0;
            }
            p += rec[0];
        }
    }
    return -1;
}

int disc_find(Disc *d, const char *path, DiscEntry *out)
{
    uint8_t sec[DISC_SECTOR_DATA];
    DiscEntry cur;

    if (disc_read(d, PVD_LBA, 1, sec)) {
        return -1;
    }
    /* The root directory record sits at offset 156 of the volume descriptor. */
    cur.lba = le32(sec + 156 + 2);
    cur.size = le32(sec + 156 + 10);
    cur.is_dir = true;

    while (*path) {
        size_t len = strcspn(path, "/\\");

        if (len) {
            if (!cur.is_dir || dir_lookup(d, &cur, path, len, &cur)) {
                return -1;
            }
        }
        path += len;
        if (*path) {
            path++;
        }
    }
    *out = cur;
    return 0;
}
