#include "core/config.h"
#include "core/log.h"
#include "core/rom.h"
#include "plat/plat.h"
#include "port/game.h"
#include "ui/launcher.h"

#include <stdio.h>
#include <stdlib.h>

/* Set by the Makefile when anything is recompiled; names this build in the
 * log and to the updater. */
char g_build_id[24] = BUILD_ID;

int main(int argc, char **argv)
{
    RomInfo rom;

#ifndef __3DS__
    /* Testing aid: pose as another build, to try the updater. */
    if (getenv("LOM3DS_BUILD_ID")) {
        snprintf(g_build_id, sizeof g_build_id, "%s", getenv("LOM3DS_BUILD_ID"));
    }
#endif

    plat_init(argc, argv);
    log_open(plat_data_dir());
    log_set_clock(plat_time_us);
    plat_load_script();
    log_line("Legend of Mana 3DS, build %s, %s", g_build_id, plat_is_new3ds() ? "New 3DS" : "3DS");
    g_config_has_zl_zr = plat_is_new3ds();
    config_load(plat_data_dir());
    plat_set_speedup(g_opt[OPT_N3DS_BOOST]);

    if (launcher_run(&rom)) {
        int i;

        log_line("disc %s (%s, %s sectors)", rom.file, rom.version->serial, rom.raw_sectors ? "raw" : "cooked");
        for (i = 0; i < OPT_COUNT; i++) {
            log_line("option %s=%d", g_opt_defs[i].key, g_opt[i]);
        }
        game_run(&rom);
    }

    plat_exit();
    return 0;
}
