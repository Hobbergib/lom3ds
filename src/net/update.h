/* Fetches new builds of the port from its GitHub releases page.
 *
 * A release carries two files: the program (lom3ds.3dsx) and version.txt,
 * which names the build and gives the program's size and SHA-256. Both are
 * fetched over verified HTTPS, and the program is checked against
 * version.txt before it replaces the one on the SD card. */
#ifndef UPDATE_H
#define UPDATE_H

#include <stdbool.h>
#include <stddef.h>

typedef struct {
    char build[24];   /* build stamp, YYYYMMDD-HHMMSS; later ones compare greater */
    char sha256[65];  /* of lom3ds.3dsx */
    unsigned long size;
    char cia_sha256[65]; /* of lom3ds.cia; empty if the release has none */
    unsigned long cia_size;
} UpdateInfo;

/* Called during a download; `total` is the size the release announced. */
typedef void (*UpdateProgress)(unsigned long done, unsigned long total);

/* Reads what the newest release is. On failure writes the reason to err. */
bool update_check(UpdateInfo *info, char *err, size_t err_size);

/* Downloads the release file `name`, which must have the given size and
 * SHA-256, and puts it in place of the file `target`. The old file stays
 * untouched unless the download is complete and matches. */
bool update_download(const char *name, unsigned long size, const char *sha256, const char *target, UpdateProgress progress,
                     char *err, size_t err_size);

#endif
