#include "rom.h"

#include "disc.h"
#include "sha1.h"

#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <string.h>

#define MAX_EXE_SIZE (4u << 20)

/* Hash of the boot executable, as published by the decompilation. The game
 * code is compiled for one release, so only that release is accepted; the
 * Japanese one (SLPS-02170) needs a build of its own. */
const RomVersion g_rom_versions[] = {
    {"SLUS-01013", "North America", "SLUS_010.13", "d11dfdd50d412ac3fa3e2eb80fbde138da118f27"},
    {NULL, NULL, NULL, NULL},
};

const RomVersion *rom_identify(const char *exe_name, const char *sha1_hex)
{
    const RomVersion *v;

    for (v = g_rom_versions; v->serial; v++) {
        if (!strcmp(v->exe_name, exe_name) && !strcmp(v->exe_sha1, sha1_hex)) {
            return v;
        }
    }
    return NULL;
}

static bool has_image_extension(const char *name)
{
    static const char *const exts[] = {".bin", ".img", ".iso"};
    const char *dot = strrchr(name, '.');
    size_t i, j;

    if (!dot) {
        return false;
    }
    for (i = 0; i < sizeof exts / sizeof exts[0]; i++) {
        for (j = 0; exts[i][j] && tolower((unsigned char)dot[j]) == exts[i][j]; j++) {
        }
        if (!exts[i][j] && !dot[j]) {
            return true;
        }
    }
    return false;
}

/* Extracts the executable name from SYSTEM.CNF's "BOOT = cdrom:\NAME;1". */
static bool parse_boot_name(const char *cnf, char *out, size_t out_size)
{
    const char *p = strstr(cnf, "BOOT");
    size_t n;

    if (!p || !(p = strchr(p, '='))) {
        return false;
    }
    p++;
    p += strspn(p, " \t");
    if (!strncmp(p, "cdrom:", 6)) {
        p += 6;
    }
    p += strspn(p, "\\/");
    n = strcspn(p, ";\r\n \t");
    if (!n || n >= out_size) {
        return false;
    }
    memcpy(out, p, n);
    out[n] = 0;
    return true;
}

bool rom_read_boot_hash(const char *image_path, char exe_name[32], char sha1_hex[41], bool *raw_sectors)
{
    static uint8_t buf[16 * DISC_SECTOR_DATA];
    bool ok = false;
    Disc disc;
    DiscEntry e;
    Sha1 sha;
    uint32_t left, lba;

    if (disc_open(&disc, image_path)) {
        return false;
    }
    *raw_sectors = disc_is_raw(&disc);

    if (disc_find(&disc, "SYSTEM.CNF", &e) || e.is_dir || e.size >= DISC_SECTOR_DATA || disc_read(&disc, e.lba, 1, buf)) {
        goto done;
    }
    buf[e.size] = 0;
    if (!parse_boot_name((const char *)buf, exe_name, 32) || disc_find(&disc, exe_name, &e) || e.is_dir ||
        e.size > MAX_EXE_SIZE) {
        goto done;
    }
    sha1_init(&sha);
    for (left = e.size, lba = e.lba; left;) {
        uint32_t bytes = left < sizeof buf ? left : sizeof buf;
        uint32_t secs = (bytes + DISC_SECTOR_DATA - 1) / DISC_SECTOR_DATA;

        if (disc_read(&disc, lba, secs, buf)) {
            goto done;
        }
        sha1_update(&sha, buf, bytes);
        left -= bytes;
        lba += secs;
    }
    sha1_final_hex(&sha, sha1_hex);
    ok = true;

done:
    disc_close(&disc);
    return ok;
}

void rom_scan(const char *dir, RomInfo *out)
{
    DIR *dp = opendir(dir);
    struct dirent *de;
    char exe[32], hex[41];

    memset(out, 0, sizeof *out);
    out->status = ROM_MISSING;
    if (!dp) {
        return;
    }
    /* A raw image of the game wins over a cooked one, and either wins over
     * an image that is not the game. */
    while ((de = readdir(dp)) != NULL && !(out->status == ROM_OK && out->raw_sectors)) {
        const RomVersion *version;
        char path[sizeof out->path];
        bool raw = false;

        if (!has_image_extension(de->d_name)) {
            continue;
        }
        snprintf(path, sizeof path, "%s/%s", dir, de->d_name);
        version = rom_read_boot_hash(path, exe, hex, &raw) ? rom_identify(exe, hex) : NULL;
        if (!version && out->status == ROM_OK) {
            continue;
        }
        out->status = version ? ROM_OK : ROM_UNRECOGNIZED;
        out->version = version;
        out->raw_sectors = raw;
        memcpy(out->path, path, sizeof path);
        snprintf(out->file, sizeof out->file, "%.*s", (int)sizeof out->file - 1, de->d_name);
    }
    closedir(dp);
}
