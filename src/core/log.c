#include "log.h"

#include <stdarg.h>
#include <stdio.h>

static FILE *s_file;
static uint64_t (*s_clock)(void);
static uint64_t s_clock_start;

void log_set_clock(uint64_t (*now_us)(void))
{
    s_clock = now_us;
    s_clock_start = now_us();
}

void log_open(const char *dir)
{
    char path[512], prev[512];

    snprintf(path, sizeof path, "%s/log.txt", dir);
    snprintf(prev, sizeof prev, "%s/log.prev.txt", dir);
    remove(prev);
    rename(path, prev);
    s_file = fopen(path, "w");
}

void log_line(const char *fmt, ...)
{
    va_list ap;

    if (s_file) {
        if (s_clock) {
            uint32_t ms = (uint32_t)((s_clock() - s_clock_start) / 1000);

            fprintf(s_file, "%3u.%03u  ", (unsigned)(ms / 1000), (unsigned)(ms % 1000));
        }
        va_start(ap, fmt);
        vfprintf(s_file, fmt, ap);
        va_end(ap);
        fputc('\n', s_file);
        fflush(s_file);
    }
#ifndef __3DS__
    va_start(ap, fmt);
    fputs("lom3ds: ", stderr);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
#endif
}
