/* Finds and identifies the user's own disc image in the data directory. */
#ifndef ROM_H
#define ROM_H

#include <stdbool.h>

typedef enum {
    ROM_MISSING,      /* no disc image in the data directory */
    ROM_UNRECOGNIZED, /* an image is there, but it is not a supported release */
    ROM_OK
} RomStatus;

typedef struct {
    const char *serial;   /* "SLUS-01013" */
    const char *region;   /* "North America" */
    const char *exe_name; /* boot executable named by SYSTEM.CNF */
    const char *exe_sha1;
} RomVersion;

typedef struct {
    RomStatus status;
    const RomVersion *version; /* set when status is ROM_OK */
    char path[512];            /* image examined last (empty if ROM_MISSING) */
    char file[64];             /* its file name, shortened for display */
    bool raw_sectors;          /* false for .iso: no streamed video or audio */
} RomInfo;

/* The supported releases, terminated by an entry with a NULL serial. */
extern const RomVersion g_rom_versions[];

/* Returns the release with this boot executable name and hash, or NULL. */
const RomVersion *rom_identify(const char *exe_name, const char *sha1_hex);

/* Reads the boot executable named by the image's SYSTEM.CNF and hashes it.
 * Returns false if the file is not a readable PlayStation disc image. */
bool rom_read_boot_hash(const char *image_path, char exe_name[32], char sha1_hex[41], bool *raw_sectors);

/* Examines every disc image in `dir`, stopping at the first supported one. */
void rom_scan(const char *dir, RomInfo *out);

#endif
