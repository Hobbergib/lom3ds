/* Host tests for the disc reader, disc identification and settings.
 * Usage: test_core DIR   (DIR as written by tools/mktestiso.py) */
#include "../src/core/config.h"
#include "../src/core/disc.h"
#include "../src/core/rom.h"
#include "../src/core/sha1.h"
#include "../src/core/sha256.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int s_failures;

#define CHECK(cond)                                                  \
    do {                                                             \
        if (!(cond)) {                                               \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);   \
            s_failures++;                                            \
        }                                                            \
    } while (0)

static void test_sha256(void)
{
    static char block[1000];
    char hex[65];
    Sha256 s;
    int i;

    sha256_init(&s);
    sha256_final_hex(&s, hex);
    CHECK(!strcmp(hex, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));
    sha256_init(&s);
    sha256_update(&s, "abc", 3);
    sha256_final_hex(&s, hex);
    CHECK(!strcmp(hex, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));

    /* One million 'a', fed in uneven pieces to exercise buffering. */
    memset(block, 'a', sizeof block);
    sha256_init(&s);
    sha256_update(&s, block, 1);
    sha256_update(&s, block, 999);
    for (i = 1; i < 1000; i++) {
        sha256_update(&s, block, sizeof block);
    }
    sha256_final_hex(&s, hex);
    CHECK(!strcmp(hex, "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0"));
}

static void test_sha1(void)
{
    static char block[1000];
    char hex[41];
    Sha1 s;
    int i;

    sha1_init(&s);
    sha1_update(&s, "abc", 3);
    sha1_final_hex(&s, hex);
    CHECK(!strcmp(hex, "a9993e364706816aba3e25717850c26c9cd0d89d"));

    /* One million 'a', fed in uneven pieces to exercise buffering. */
    memset(block, 'a', sizeof block);
    sha1_init(&s);
    sha1_update(&s, block, 1);
    sha1_update(&s, block, 999);
    for (i = 1; i < 1000; i++) {
        sha1_update(&s, block, sizeof block);
    }
    sha1_final_hex(&s, hex);
    CHECK(!strcmp(hex, "34aa973cd4c4daa4f61eeb2bdbad27316534016f"));
}

static void test_identify(void)
{
    const RomVersion *v = rom_identify("SLUS_010.13", "d11dfdd50d412ac3fa3e2eb80fbde138da118f27");

    CHECK(v && !strcmp(v->serial, "SLUS-01013"));
    /* The Japanese release is a real disc, but this build is not for it. */
    CHECK(!rom_identify("SLPS_021.70", "b067188a92e4de9a4db7bb7e5343c757e9884bfa"));
    /* Right hash under the wrong name, and an unknown hash. */
    CHECK(!rom_identify("SLPS_021.70", "d11dfdd50d412ac3fa3e2eb80fbde138da118f27"));
    CHECK(!rom_identify("SLUS_010.13", "0000000000000000000000000000000000000000"));
}

static void test_image(const char *path, bool raw, const char *expected_sha1)
{
    static uint8_t buf[4 * DISC_SECTOR_DATA];
    char exe[32], hex[41];
    bool is_raw = !raw;
    DiscEntry e;
    Disc d;

    CHECK(disc_open(&d, path) == 0);
    if (!d.f) {
        return;
    }
    CHECK(disc_is_raw(&d) == raw);

    CHECK(disc_find(&d, "SYSTEM.CNF", &e) == 0 && !e.is_dir);
    CHECK(disc_read(&d, e.lba, 1, buf) == 0 && !memcmp(buf, "BOOT = cdrom:", 13));

    CHECK(disc_find(&d, "bin\\field.bin", &e) == 0 && e.size == 22 * 200);
    CHECK(disc_read(&d, e.lba, 3, buf) == 0);
    CHECK(!memcmp(buf, "field overlay stand-in", 22) && !memcmp(buf + 22 * 199, "field overlay stand-in", 22));
    CHECK(disc_find(&d, "BIN", &e) == 0 && e.is_dir);
    CHECK(disc_find(&d, "", &e) == 0 && e.is_dir);

    CHECK(disc_find(&d, "MISSING.BIN", &e) != 0);
    CHECK(disc_find(&d, "BIN/FIELD", &e) != 0);
    CHECK(disc_find(&d, "SYSTEM.CNF/X", &e) != 0);
    CHECK(disc_read(&d, d.sectors, 1, buf) != 0);
    CHECK(disc_read(&d, d.sectors - 1, 2, buf) != 0);
    disc_close(&d);

    CHECK(rom_read_boot_hash(path, exe, hex, &is_raw));
    CHECK(is_raw == raw);
    CHECK(!strcmp(exe, "SLUS_010.13"));
    CHECK(!strcmp(hex, expected_sha1));
}

static void test_scan(const char *root)
{
    char dir[512];
    RomInfo info;

    /* A well-formed disc whose executable is not the game's. */
    snprintf(dir, sizeof dir, "%s/raw", root);
    rom_scan(dir, &info);
    CHECK(info.status == ROM_UNRECOGNIZED && !info.version && !strcmp(info.file, "test.bin"));

    snprintf(dir, sizeof dir, "%s/junk", root);
    rom_scan(dir, &info);
    CHECK(info.status == ROM_UNRECOGNIZED);

    snprintf(dir, sizeof dir, "%s/empty", root);
    rom_scan(dir, &info);
    CHECK(info.status == ROM_MISSING && !info.path[0]);

    snprintf(dir, sizeof dir, "%s/does-not-exist", root);
    rom_scan(dir, &info);
    CHECK(info.status == ROM_MISSING);
}

static void test_config(const char *root)
{
    int per_tab[TAB_COUNT] = {0};
    char path[512];
    FILE *f;
    int i;

    config_reset();
    for (i = 0; i < OPT_COUNT; i++) {
        CHECK(g_opt_defs[i].key && g_opt_defs[i].count >= 2 && g_opt_defs[i].def < g_opt_defs[i].count);
        CHECK(g_opt_defs[i].tab < TAB_COUNT);
        per_tab[g_opt_defs[i].tab % TAB_COUNT]++;
    }
    /* The menu assumes no tab is empty and that a tab fits its panel: six
     * rows, or twelve in two columns. */
    for (i = 0; i < TAB_COUNT; i++) {
        CHECK(per_tab[i] >= 1 && per_tab[i] <= 12);
    }

    g_opt[OPT_SCREEN_FIT] = FIT_STRETCH;
    g_opt[OPT_VOLUME] = 3;
    CHECK(config_save(root));
    config_reset();
    config_load(root);
    CHECK(g_opt[OPT_SCREEN_FIT] == FIT_STRETCH && g_opt[OPT_VOLUME] == 3);
    CHECK(g_opt[OPT_REVERB] == g_opt_defs[OPT_REVERB].def);

    /* Out-of-range values, unknown keys and garbage keep the defaults. */
    snprintf(path, sizeof path, "%s/config.ini", root);
    f = fopen(path, "w");
    CHECK(f != NULL);
    if (f) {
        fputs("volume=99\nframe_skip=-1\nnonsense\nunknown=1\nreverb=x\ndithering=0\n", f);
        fclose(f);
    }
    config_load(root);
    CHECK(g_opt[OPT_VOLUME] == g_opt_defs[OPT_VOLUME].def);
    CHECK(g_opt[OPT_FRAME_SKIP] == g_opt_defs[OPT_FRAME_SKIP].def);
    CHECK(g_opt[OPT_REVERB] == g_opt_defs[OPT_REVERB].def);
    CHECK(g_opt[OPT_DITHERING] == 0);
    remove(path);

    CHECK(!config_save("/nonexistent-dir/for-test"));
}

int main(int argc, char **argv)
{
    char path[512], expected[64] = "";
    FILE *f;

    if (argc != 2) {
        fprintf(stderr, "usage: %s DIR\n", argv[0]);
        return 2;
    }
    snprintf(path, sizeof path, "%s/expected.txt", argv[1]);
    f = fopen(path, "r");
    if (!f || !fgets(expected, sizeof expected, f)) {
        fprintf(stderr, "cannot read %s\n", path);
        return 2;
    }
    fclose(f);
    expected[strcspn(expected, "\n")] = 0;

    test_sha1();
    test_sha256();
    test_identify();
    snprintf(path, sizeof path, "%s/raw/test.bin", argv[1]);
    test_image(path, true, expected);
    snprintf(path, sizeof path, "%s/cooked/test.iso", argv[1]);
    test_image(path, false, expected);
    test_scan(argv[1]);
    test_config(argv[1]);

    printf(s_failures ? "%d check(s) failed\n" : "all core tests passed\n", s_failures);
    return s_failures != 0;
}
