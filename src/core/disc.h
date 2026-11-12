/* Read-only access to a PlayStation disc image and its ISO 9660 file system.
 * Accepts raw images (2352-byte sectors: .bin/.img) and cooked ones
 * (2048-byte sectors: .iso). */
#ifndef DISC_H
#define DISC_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#define DISC_SECTOR_DATA 2048
#define DISC_SECTOR_RAW 2352

typedef struct {
    FILE *f;
    uint32_t sector_size; /* bytes per sector in the image file */
    uint32_t data_offset; /* offset of the 2048 user bytes within a sector */
    uint32_t sectors;
} Disc;

typedef struct {
    uint32_t lba;
    uint32_t size;
    bool is_dir;
} DiscEntry;

/* Returns 0 on success. Fails if the file is not an ISO 9660 image. */
int disc_open(Disc *d, const char *path);
void disc_close(Disc *d);

/* True if sectors keep their headers; streamed video and audio need this. */
bool disc_is_raw(const Disc *d);

/* Reads the 2048 user bytes of `count` sectors starting at `lba`. */
int disc_read(Disc *d, uint32_t lba, uint32_t count, void *dst);

/* Reads one sector as the drive delivers it in whole-sector mode: 2340
 * bytes starting at the header (address and mode, subheader, data, error
 * correction). Cooked images get a synthesised Mode 2 Form 1 header. */
#define DISC_SECTOR_WHOLE 2340
int disc_read_whole(Disc *d, uint32_t lba, uint8_t *dst);

/* Looks up a path such as "BIN/FIELD.BIN" (either slash, any case). */
int disc_find(Disc *d, const char *path, DiscEntry *out);

#endif
